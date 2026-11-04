#include "bonk/shell/isolate.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

#include "bonk/paint/canvas.hpp"
#include "bonk/shell/shell.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define BONK_HAS_FORK 1
#else
#define BONK_HAS_FORK 0
#endif

namespace bonk::shell {
namespace {

// Wire format: one fixed-size header followed by the variable-length payloads.
// Both sides use the same struct, so a partial read is detectable by length.
struct IpcRequestHeader {
  std::uint32_t magic = 0x424f4e4b;  // "BONK"
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::int32_t scroll_centi = 0;
  std::uint32_t crash = 0;
  std::uint32_t target_length = 0;
  std::uint32_t font_length = 0;
  std::uint32_t profile_length = 0;
};

struct IpcResponseHeader {
  std::uint32_t magic = 0x4652414d;  // "FRAM"
  std::uint32_t ok = 0;
  std::uint32_t title_length = 0;
  std::uint32_t error_length = 0;
  std::uint32_t png_length = 0;
};

#if BONK_HAS_FORK

bool write_all(int fd, const void* data, std::size_t size) {
  const char* bytes = static_cast<const char*>(data);
  std::size_t sent = 0;
  while (sent < size) {
    const ssize_t written = ::write(fd, bytes + sent, size - sent);
    if (written <= 0) return false;
    sent += static_cast<std::size_t>(written);
  }
  return true;
}

bool read_all(int fd, void* data, std::size_t size) {
  char* bytes = static_cast<char*>(data);
  std::size_t got = 0;
  while (got < size) {
    const ssize_t received = ::read(fd, bytes + got, size - got);
    if (received <= 0) return false;
    got += static_cast<std::size_t>(received);
  }
  return true;
}

[[noreturn]] void child_main(int request_fd, int response_fd) {
  IpcRequestHeader header;
  if (!read_all(request_fd, &header, sizeof(header)) || header.magic != 0x424f4e4b) {
    ::_exit(2);
  }
  std::string target(header.target_length, '\0');
  std::string font_path(header.font_length, '\0');
  std::string profile_dir(header.profile_length, '\0');
  if (!target.empty() && !read_all(request_fd, target.data(), target.size())) ::_exit(2);
  if (!font_path.empty() && !read_all(request_fd, font_path.data(), font_path.size())) ::_exit(2);
  if (!profile_dir.empty() && !read_all(request_fd, profile_dir.data(), profile_dir.size())) ::_exit(2);

  if (header.crash != 0) {
    // Simulated renderer crash: no frame, no error, just a dead process. The
    // parent must survive and report it; that is the whole point of the sandbox.
    std::abort();
  }

  Session session(static_cast<float>(header.width), static_cast<float>(header.height), font_path, profile_dir);
  IpcResponseHeader response;
  std::string title;
  std::string error;
  std::string png;

  const Status navigated = session.navigate(target);
  if (navigated.is_error()) {
    error = navigated.error().to_string();
  } else {
    session.set_scroll(static_cast<float>(header.scroll_centi) / 100.0f);
    title = session.tab().title;
    paint::Canvas canvas(header.width, header.height);
    if (!session.render(canvas)) {
      error = "shell.no_layout";
    } else {
      png = canvas.encode_png();
      response.ok = 1;
    }
  }

  response.title_length = static_cast<std::uint32_t>(title.size());
  response.error_length = static_cast<std::uint32_t>(error.size());
  response.png_length = static_cast<std::uint32_t>(png.size());
  const bool sent = write_all(response_fd, &response, sizeof(response)) &&
                    (title.empty() || write_all(response_fd, title.data(), title.size())) &&
                    (error.empty() || write_all(response_fd, error.data(), error.size())) &&
                    (png.empty() || write_all(response_fd, png.data(), png.size()));
  ::close(request_fd);
  ::close(response_fd);
  ::_exit(sent ? 0 : 3);
}

#endif  // BONK_HAS_FORK

}  // namespace

bool isolation_supported() noexcept { return BONK_HAS_FORK != 0; }

IsolatedResult render_isolated(const IsolatedRequest& request) {
  IsolatedResult result;
  const auto started = std::chrono::steady_clock::now();
#if !BONK_HAS_FORK
  // No fork(): render in this process and say so. The caller decides whether
  // that is acceptable; nothing is silently "isolated".
  Session session(static_cast<float>(request.width), static_cast<float>(request.height), request.font_path,
                  request.profile_dir);
  const Status navigated = session.navigate(request.target);
  if (navigated.is_error()) {
    result.error = navigated.error().to_string();
    return result;
  }
  session.set_scroll(request.scroll);
  paint::Canvas canvas(request.width, request.height);
  if (!session.render(canvas)) {
    result.error = "shell.no_layout";
    return result;
  }
  const std::string png = canvas.encode_png();
  result.png.assign(png.begin(), png.end());
  result.title = session.tab().title;
  result.ok = true;
  result.duration_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  return result;
#else
  int request_pipe[2] = {-1, -1};
  int response_pipe[2] = {-1, -1};
  if (::pipe(request_pipe) != 0 || ::pipe(response_pipe) != 0) {
    result.error = "shell.ipc_pipe_failed";
    return result;
  }

  const pid_t child = ::fork();
  if (child < 0) {
    result.error = "shell.ipc_fork_failed";
    ::close(request_pipe[0]);
    ::close(request_pipe[1]);
    ::close(response_pipe[0]);
    ::close(response_pipe[1]);
    return result;
  }
  if (child == 0) {
    ::close(request_pipe[1]);
    ::close(response_pipe[0]);
    child_main(request_pipe[0], response_pipe[1]);
  }

  ::close(request_pipe[0]);
  ::close(response_pipe[1]);

  IpcRequestHeader header;
  header.width = static_cast<std::uint32_t>(request.width < 1 ? 1 : request.width);
  header.height = static_cast<std::uint32_t>(request.height < 1 ? 1 : request.height);
  header.scroll_centi = static_cast<std::int32_t>(request.scroll * 100.0f);
  header.crash = request.simulate_crash ? 1u : 0u;
  header.target_length = static_cast<std::uint32_t>(request.target.size());
  header.font_length = static_cast<std::uint32_t>(request.font_path.size());
  header.profile_length = static_cast<std::uint32_t>(request.profile_dir.size());

  const bool sent = write_all(request_pipe[1], &header, sizeof(header)) &&
                    write_all(request_pipe[1], request.target.data(), request.target.size());
  if (sent && !request.font_path.empty()) {
    (void)write_all(request_pipe[1], request.font_path.data(), request.font_path.size());
  }
  if (sent && !request.profile_dir.empty()) {
    (void)write_all(request_pipe[1], request.profile_dir.data(), request.profile_dir.size());
  }
  ::close(request_pipe[1]);

  IpcResponseHeader response;
  std::string title;
  std::string error;
  std::vector<std::uint8_t> png;
  bool have_response = read_all(response_pipe[0], &response, sizeof(response)) && response.magic == 0x4652414d;
  if (have_response) {
    if (response.title_length > 0) {
      title.resize(response.title_length);
      have_response = read_all(response_pipe[0], title.data(), title.size());
    }
    if (have_response && response.error_length > 0) {
      error.resize(response.error_length);
      have_response = read_all(response_pipe[0], error.data(), error.size());
    }
    if (have_response && response.png_length > 0) {
      png.resize(response.png_length);
      have_response = read_all(response_pipe[0], png.data(), png.size());
    }
  }
  ::close(response_pipe[0]);

  int status = 0;
  (void)::waitpid(child, &status, 0);
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

  if (!have_response) {
    result.crashed = true;
    result.error = WIFSIGNALED(status) ? "shell.tab_crashed (signal " + std::to_string(WTERMSIG(status)) + ")"
                                       : "shell.ipc_truncated_response";
    result.duration_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return result;
  }

  result.title = std::move(title);
  result.error = std::move(error);
  result.png = std::move(png);
  result.ok = response.ok != 0 && !result.png.empty();
  result.duration_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  return result;
#endif
}

}  // namespace bonk::shell
