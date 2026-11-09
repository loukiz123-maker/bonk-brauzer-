#include "bonk/android/process_runner.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace bonk::android {
namespace {

constexpr std::size_t kMaxCaptureBytes = 64 * 1024 * 1024;
constexpr std::size_t kReadBudgetPerIteration = 256 * 1024;
constexpr std::size_t kFinalDrainBudget = 1024 * 1024;
constexpr auto kTerminateGrace = std::chrono::milliseconds{100};

class FileDescriptor final {
 public:
  explicit FileDescriptor(int value = -1) noexcept : value_(value) {}
  ~FileDescriptor() { reset(); }

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  [[nodiscard]] int get() const noexcept { return value_; }
  void reset(int value = -1) noexcept {
    if (value_ >= 0) (void)::close(value_);
    value_ = value;
  }

 private:
  int value_;
};

struct PipeCapture {
  FileDescriptor read_end;
  FileDescriptor write_end;
  std::string* output = nullptr;
  bool* truncated = nullptr;
  bool open = false;
};

bool has_embedded_nul(std::string_view value) noexcept { return value.find('\0') != std::string_view::npos; }

std::string system_error_message(std::string_view action, int error_number) {
  std::string message(action);
  message += ": ";
  message += std::strerror(error_number);
  return message;
}

int move_above_standard_descriptors(int& descriptor) {
  if (descriptor >= STDERR_FILENO + 1) return 0;
  const int replacement = ::fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
  if (replacement < 0) return errno;
  (void)::close(descriptor);
  descriptor = replacement;
  return 0;
}

int make_pipe(PipeCapture& pipe, std::string& output, bool& truncated) {
  int descriptors[2] = {-1, -1};
  if (::pipe2(descriptors, O_CLOEXEC) != 0) return errno;
  int error = move_above_standard_descriptors(descriptors[0]);
  if (error == 0) error = move_above_standard_descriptors(descriptors[1]);
  if (error != 0) {
    if (descriptors[0] >= 0) (void)::close(descriptors[0]);
    if (descriptors[1] >= 0) (void)::close(descriptors[1]);
    return error;
  }
  pipe.read_end.reset(descriptors[0]);
  pipe.write_end.reset(descriptors[1]);
  pipe.output = &output;
  pipe.truncated = &truncated;

  const int flags = ::fcntl(pipe.read_end.get(), F_GETFL);
  if (flags < 0) return errno;
  if (::fcntl(pipe.read_end.get(), F_SETFL, flags | O_NONBLOCK) < 0) return errno;
  pipe.open = true;
  return 0;
}

int add_pipe_actions(posix_spawn_file_actions_t& actions, const PipeCapture& pipe, int target_fd) {
  int error = ::posix_spawn_file_actions_adddup2(&actions, pipe.write_end.get(), target_fd);
  if (error == 0) error = ::posix_spawn_file_actions_addclose(&actions, pipe.read_end.get());
  if (error == 0) error = ::posix_spawn_file_actions_addclose(&actions, pipe.write_end.get());
  return error;
}

int signal_process_group(int process_id, int signal_number) noexcept {
  if (::kill(-process_id, signal_number) == 0) return 0;
  const int group_error = errno;
  if (group_error != ESRCH) return group_error;
  if (::kill(process_id, signal_number) == 0 || errno == ESRCH) return 0;
  return errno;
}

void kill_and_reap(pid_t process_id) noexcept {
  (void)::kill(-process_id, SIGKILL);
  (void)::kill(process_id, SIGKILL);
  int status = 0;
  pid_t waited = -1;
  do {
    waited = ::waitpid(process_id, &status, 0);
  } while (waited < 0 && errno == EINTR);
}

int drain_pipe(PipeCapture& pipe, std::size_t limit, std::size_t read_budget) {
  if (!pipe.open) return 0;
  std::array<char, 8192> buffer{};
  std::size_t drained = 0;
  while (drained < read_budget) {
    const std::size_t request = std::min(buffer.size(), read_budget - drained);
    const ssize_t count = ::read(pipe.read_end.get(), buffer.data(), request);
    if (count > 0) {
      const std::size_t bytes = static_cast<std::size_t>(count);
      const std::size_t room = limit > pipe.output->size() ? limit - pipe.output->size() : 0;
      const std::size_t keep = bytes < room ? bytes : room;
      if (keep > 0) pipe.output->append(buffer.data(), keep);
      if (keep < bytes) *pipe.truncated = true;
      drained += bytes;
      continue;
    }
    if (count == 0) {
      pipe.read_end.reset();
      pipe.open = false;
      return 0;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return errno;
  }
  return 0;
}

void parse_wait_status(int status, CommandOutput& output) {
  if (WIFEXITED(status)) {
    output.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    output.termination_signal = WTERMSIG(status);
  }
}

}  // namespace

Result<CommandOutput> run_command(const RuntimeCommand& command, std::chrono::milliseconds timeout,
                                  std::size_t max_bytes_per_stream) {
  if (command.executable.empty() || has_embedded_nul(command.executable) || command.executable.front() != '/') {
    return Result<CommandOutput>::failure(
        Error::make("android.invalid_command", "command executable must be an explicit absolute C string path"));
  }
  for (const std::string& argument : command.arguments) {
    if (has_embedded_nul(argument)) {
      return Result<CommandOutput>::failure(
          Error::make("android.invalid_command", "command argument contains an embedded NUL byte"));
    }
  }
  if (timeout.count() <= 0) {
    return Result<CommandOutput>::failure(Error::make("android.invalid_timeout", "command timeout must be positive"));
  }
  if (max_bytes_per_stream == 0 || max_bytes_per_stream > kMaxCaptureBytes) {
    return Result<CommandOutput>::failure(
        Error::make("android.invalid_capture_limit", "capture limit must be between 1 byte and 64 MiB per stream"));
  }

  std::vector<char*> argv;
  argv.reserve(command.arguments.size() + 2);
  argv.push_back(const_cast<char*>(command.executable.c_str()));
  for (const std::string& argument : command.arguments) argv.push_back(const_cast<char*>(argument.c_str()));
  argv.push_back(nullptr);

  CommandOutput output;
  PipeCapture stdout_pipe;
  PipeCapture stderr_pipe;
  int error_number = make_pipe(stdout_pipe, output.stdout_data, output.stdout_truncated);
  if (error_number != 0) {
    return Result<CommandOutput>::failure(
        Error::make("android.capture_setup_failed", system_error_message("cannot create stdout pipe", error_number)));
  }
  error_number = make_pipe(stderr_pipe, output.stderr_data, output.stderr_truncated);
  if (error_number != 0) {
    return Result<CommandOutput>::failure(
        Error::make("android.capture_setup_failed", system_error_message("cannot create stderr pipe", error_number)));
  }

  posix_spawn_file_actions_t actions;
  error_number = ::posix_spawn_file_actions_init(&actions);
  if (error_number != 0) {
    return Result<CommandOutput>::failure(
        Error::make("android.capture_setup_failed", system_error_message("cannot initialize child file actions", error_number)));
  }
  error_number = add_pipe_actions(actions, stdout_pipe, STDOUT_FILENO);
  if (error_number == 0) error_number = add_pipe_actions(actions, stderr_pipe, STDERR_FILENO);
  if (error_number != 0) {
    (void)::posix_spawn_file_actions_destroy(&actions);
    return Result<CommandOutput>::failure(
        Error::make("android.capture_setup_failed", system_error_message("cannot configure child output", error_number)));
  }

  posix_spawnattr_t attributes;
  error_number = ::posix_spawnattr_init(&attributes);
  if (error_number != 0) {
    (void)::posix_spawn_file_actions_destroy(&actions);
    return Result<CommandOutput>::failure(
        Error::make("android.spawn_setup_failed", system_error_message("cannot initialize process attributes", error_number)));
  }
  error_number = ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  if (error_number == 0) error_number = ::posix_spawnattr_setpgroup(&attributes, 0);
  if (error_number != 0) {
    (void)::posix_spawnattr_destroy(&attributes);
    (void)::posix_spawn_file_actions_destroy(&actions);
    return Result<CommandOutput>::failure(
        Error::make("android.spawn_setup_failed", system_error_message("cannot isolate command process group", error_number)));
  }

  pid_t child = -1;
  error_number = ::posix_spawn(&child, command.executable.c_str(), &actions, &attributes, argv.data(), environ);
  (void)::posix_spawnattr_destroy(&attributes);
  (void)::posix_spawn_file_actions_destroy(&actions);
  if (error_number != 0) {
    return Result<CommandOutput>::failure(
        Error::make("android.spawn_failed", system_error_message("cannot start command executable", error_number)));
  }

  stdout_pipe.write_end.reset();
  stderr_pipe.write_end.reset();

  bool child_reaped = false;
  int wait_status = 0;
  bool kill_sent = false;
  auto deadline = std::chrono::steady_clock::now() + timeout;
  auto kill_deadline = deadline;
  auto next_signal_deadline = deadline;

  while (!child_reaped) {
    error_number = drain_pipe(stdout_pipe, max_bytes_per_stream, kReadBudgetPerIteration);
    if (error_number == 0) error_number = drain_pipe(stderr_pipe, max_bytes_per_stream, kReadBudgetPerIteration);
    if (error_number != 0) {
      const std::string message = system_error_message("cannot capture command output", error_number);
      kill_and_reap(child);
      return Result<CommandOutput>::failure(Error::make("android.capture_failed", message));
    }

    pid_t waited = -1;
    do {
      waited = ::waitpid(child, &wait_status, WNOHANG);
    } while (waited < 0 && errno == EINTR);
    if (waited == child) {
      child_reaped = true;
      parse_wait_status(wait_status, output);
      break;
    }
    if (waited < 0) {
      const int wait_error = errno;
      const std::string message = system_error_message("cannot inspect command process", wait_error);
      if (wait_error != ECHILD) kill_and_reap(child);
      return Result<CommandOutput>::failure(Error::make("android.wait_failed", message));
    }

    const auto now = std::chrono::steady_clock::now();
    if (!output.timed_out && now >= deadline) {
      output.timed_out = true;
      (void)signal_process_group(static_cast<int>(child), SIGTERM);
      next_signal_deadline = now + kTerminateGrace;
    } else if (output.timed_out && !kill_sent && now >= next_signal_deadline) {
      (void)signal_process_group(static_cast<int>(child), SIGKILL);
      kill_sent = true;
      kill_deadline = now + std::chrono::seconds{2};
    } else if (kill_sent && now >= kill_deadline) {
      // A SIGKILLed child should be promptly waitable. Use a blocking reap here
      // rather than returning while leaving an owned child or zombie behind.
      do {
        waited = ::waitpid(child, &wait_status, 0);
      } while (waited < 0 && errno == EINTR);
      if (waited == child) {
        child_reaped = true;
        parse_wait_status(wait_status, output);
        break;
      }
      const int wait_error = errno;
      const std::string message = system_error_message("cannot reap timed-out command", wait_error);
      return Result<CommandOutput>::failure(Error::make("android.wait_failed", message));
    }

    std::array<pollfd, 2> descriptors{};
    nfds_t count = 0;
    if (stdout_pipe.open) descriptors[count++] = {stdout_pipe.read_end.get(), POLLIN | POLLHUP, 0};
    if (stderr_pipe.open) descriptors[count++] = {stderr_pipe.read_end.get(), POLLIN | POLLHUP, 0};
    int poll_result = -1;
    do {
      poll_result = ::poll(descriptors.data(), count, 10);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result < 0) {
      const int poll_error = errno;
      const std::string message = system_error_message("cannot wait for command output", poll_error);
      kill_and_reap(child);
      return Result<CommandOutput>::failure(Error::make("android.capture_failed", message));
    }
  }

  // Drain bytes written just before process exit. Close any descriptors held by
  // a detached descendant; this runner supervises the exact foreground command.
  error_number = drain_pipe(stdout_pipe, max_bytes_per_stream, kFinalDrainBudget);
  if (error_number == 0) error_number = drain_pipe(stderr_pipe, max_bytes_per_stream, kFinalDrainBudget);
  if (error_number != 0) {
    return Result<CommandOutput>::failure(
        Error::make("android.capture_failed", system_error_message("cannot finish capturing command output", error_number)));
  }
  stdout_pipe.read_end.reset();
  stderr_pipe.read_end.reset();
  stdout_pipe.open = false;
  stderr_pipe.open = false;
  return Result<CommandOutput>::success(std::move(output));
}

}  // namespace bonk::android
