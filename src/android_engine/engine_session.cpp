#include "bonk/android/engine_session.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <spawn.h>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace bonk::android {
namespace {

bool has_embedded_nul(std::string_view value) noexcept { return value.find('\0') != std::string_view::npos; }

std::string system_error_message(std::string_view action, int error_number) {
  std::string message(action);
  message += ": ";
  message += std::strerror(error_number);
  return message;
}

/// True once the process-group leader has terminated, without reaping it.
///
/// WNOWAIT leaves the leader as a zombie, so its PID -- which is also the ID of
/// the process group this session created -- cannot be recycled yet. That keeps
/// a follow-up kill(-pid, ...) aimed at our own group and nobody else's.
/// A wait error also returns true so the caller's poll() reports it.
bool leader_exit_is_pending(int process_id) noexcept {
  siginfo_t info{};
  int result = -1;
  do {
    info.si_pid = 0;
    result = ::waitid(P_PID, static_cast<id_t>(process_id), &info, WEXITED | WNOHANG | WNOWAIT);
  } while (result < 0 && errno == EINTR);
  return result < 0 || info.si_pid != 0;
}

}  // namespace

Result<RuntimeCommand> make_emulator_command(const EmulatorConfig& config) {
  if (config.emulator_executable.empty() || has_embedded_nul(config.emulator_executable) ||
      config.emulator_executable.front() != '/') {
    return Result<RuntimeCommand>::failure(
        Error::make("android.invalid_emulator_config", "emulator executable must be an explicit absolute path"));
  }
  if (config.avd_name.empty() || has_embedded_nul(config.avd_name) || config.avd_name.front() == '-' ||
      config.avd_name.find('/') != std::string::npos || config.avd_name.find('\\') != std::string::npos) {
    return Result<RuntimeCommand>::failure(
        Error::make("android.invalid_emulator_config", "AVD name must be a non-empty name, not an option or path"));
  }

  RuntimeCommand command;
  command.executable = config.emulator_executable;
  command.arguments = {"-avd", config.avd_name};
  if (config.headless) command.arguments.emplace_back("-no-window");
  return Result<RuntimeCommand>::success(std::move(command));
}

std::string_view to_string(ProcessState state) noexcept {
  switch (state) {
    case ProcessState::Stopped:
      return "stopped";
    case ProcessState::Running:
      return "running";
    case ProcessState::Exited:
      return "exited";
    case ProcessState::Failed:
      return "failed";
  }
  return "failed";
}

EngineSession::~EngineSession() noexcept {
  if (child_pid_ <= 0) return;

  (void)stop(std::chrono::milliseconds{200});
  if (child_pid_ <= 0) return;

  // Last-resort cleanup for an owned child. The negative PID targets only the
  // process group created by posix_spawn for this session.
  (void)::kill(-child_pid_, SIGKILL);
  (void)::kill(child_pid_, SIGKILL);
  int wait_status = 0;
  pid_t result = -1;
  do {
    result = ::waitpid(static_cast<pid_t>(child_pid_), &wait_status, 0);
  } while (result < 0 && errno == EINTR);
  child_pid_ = -1;
}

Status EngineSession::fail(std::string code, std::string message) {
  status_.state = ProcessState::Failed;
  status_.process_id = child_pid_;
  status_.exit_code.reset();
  status_.termination_signal.reset();
  status_.message = message;
  return Status::failure(Error::make(code, message));
}

Status EngineSession::start(const RuntimeCommand& command) {
  if (child_pid_ > 0) {
    (void)poll();
    if (child_pid_ > 0) {
      return Status::failure(
          Error::make("android.already_running", "this session already owns a runtime process"));
    }
  }

  if (command.executable.empty() || has_embedded_nul(command.executable)) {
    return fail("android.invalid_command", "runtime executable must be a non-empty C string");
  }
  for (const std::string& argument : command.arguments) {
    if (has_embedded_nul(argument)) {
      return fail("android.invalid_command", "runtime argument contains an embedded NUL byte");
    }
  }

  std::vector<char*> argv;
  argv.reserve(command.arguments.size() + 2);
  argv.push_back(const_cast<char*>(command.executable.c_str()));
  for (const std::string& argument : command.arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);

  posix_spawnattr_t attributes;
  int error_number = ::posix_spawnattr_init(&attributes);
  if (error_number != 0) {
    return fail("android.spawn_setup_failed", system_error_message("cannot initialize process attributes", error_number));
  }

  error_number = ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  if (error_number == 0) error_number = ::posix_spawnattr_setpgroup(&attributes, 0);
  if (error_number != 0) {
    (void)::posix_spawnattr_destroy(&attributes);
    return fail("android.spawn_setup_failed", system_error_message("cannot isolate runtime process group", error_number));
  }

  pid_t child = -1;
  error_number = ::posix_spawnp(&child, command.executable.c_str(), nullptr, &attributes, argv.data(), environ);
  (void)::posix_spawnattr_destroy(&attributes);
  if (error_number != 0) {
    return fail("android.spawn_failed", system_error_message("cannot start runtime executable", error_number));
  }

  child_pid_ = static_cast<int>(child);
  stop_requested_ = false;
  status_ = SessionStatus{};
  status_.state = ProcessState::Running;
  status_.process_id = child_pid_;
  return Status::success();
}

SessionStatus EngineSession::poll() {
  if (child_pid_ <= 0) return status_;

  int wait_status = 0;
  pid_t result = -1;
  do {
    result = ::waitpid(static_cast<pid_t>(child_pid_), &wait_status, WNOHANG);
  } while (result < 0 && errno == EINTR);

  if (result == 0) {
    status_.state = ProcessState::Running;
    status_.process_id = child_pid_;
    status_.exit_code.reset();
    status_.termination_signal.reset();
    status_.message.clear();
    return status_;
  }
  if (result == static_cast<pid_t>(child_pid_)) {
    child_pid_ = -1;
    record_exit(wait_status);
    return status_;
  }

  const int error_number = errno;
  if (error_number == ECHILD) child_pid_ = -1;
  (void)fail("android.wait_failed", system_error_message("cannot inspect runtime process", error_number));
  return status_;
}

void EngineSession::record_exit(int wait_status) noexcept {
  status_.process_id = -1;
  status_.exit_code.reset();
  status_.termination_signal.reset();
  status_.message.clear();

  if (WIFEXITED(wait_status)) {
    status_.exit_code = WEXITSTATUS(wait_status);
  } else if (WIFSIGNALED(wait_status)) {
    status_.termination_signal = WTERMSIG(wait_status);
  } else {
    status_.state = ProcessState::Failed;
    status_.message = "runtime process ended with an unrecognized wait status";
    stop_requested_ = false;
    return;
  }

  status_.state = stop_requested_ ? ProcessState::Stopped : ProcessState::Exited;
  stop_requested_ = false;
}

Status EngineSession::stop(std::chrono::milliseconds grace_period) {
  if (grace_period.count() < 0) {
    return Status::failure(Error::make("android.invalid_timeout", "stop grace period cannot be negative"));
  }

  if (child_pid_ <= 0) return Status::success();
  (void)poll();
  if (child_pid_ <= 0) {
    return status_.state == ProcessState::Failed
               ? Status::failure(Error::make("android.wait_failed", status_.message))
               : Status::success();
  }

  stop_requested_ = true;
  const int process_id = child_pid_;
  const auto send_to_process_group = [process_id](int signal_number) {
    if (::kill(-process_id, signal_number) == 0) return 0;
    const int group_error = errno;
    if (group_error != ESRCH) return group_error;
    if (::kill(process_id, signal_number) == 0 || errno == ESRCH) return 0;
    return errno;
  };

  const int term_error = send_to_process_group(SIGTERM);
  const auto term_deadline = std::chrono::steady_clock::now() + grace_period;

  // Wait for the leader without reaping it (see leader_exit_is_pending).
  bool leader_exited = leader_exit_is_pending(process_id);
  while (!leader_exited && std::chrono::steady_clock::now() < term_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    leader_exited = leader_exit_is_pending(process_id);
  }

  // Nothing in the owned group may outlive stop(). Either the grace period ran
  // out, or the leader is gone and only helpers that ignored SIGTERM (or were
  // still shutting down) can remain. Returning "stopped" while they keep
  // running would leak runtime processes that nothing owns any more.
  const int kill_error = send_to_process_group(SIGKILL);
  if (kill_error != 0 && !leader_exited) {
    const std::string message = system_error_message("cannot force-stop runtime process group", kill_error);
    (void)fail("android.stop_failed", message);
    return Status::failure(Error::make("android.stop_failed", message));
  }

  const auto kill_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  (void)poll();
  while (child_pid_ > 0 && std::chrono::steady_clock::now() < kill_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    (void)poll();
  }

  if (child_pid_ <= 0) {
    if (status_.state == ProcessState::Failed) {
      return Status::failure(Error::make("android.wait_failed", status_.message));
    }
    if (kill_error != 0) {
      const std::string message =
          system_error_message("runtime exited, but the rest of its process group could not be stopped", kill_error);
      (void)fail("android.stop_failed", message);
      return Status::failure(Error::make("android.stop_failed", message));
    }
    return Status::success();
  }

  std::string message = "runtime process did not exit within 2 seconds after SIGKILL";
  if (term_error != 0) {
    message += "; SIGTERM also failed: ";
    message += std::strerror(term_error);
  }
  (void)fail("android.stop_timeout", message);
  return Status::failure(Error::make("android.stop_timeout", std::move(message)));
}

}  // namespace bonk::android
