// Bonk Brauzer — process-per-tab rendering with an IPC channel.
//
// A tab that crashes must not take the browser down, so the renderer can run in
// a child process: the parent sends a small request over a pipe, the child runs
// the engine (HTML → CSS → layout → paint) and sends back a framed PNG. The
// protocol is length-prefixed structs plus payload bytes — no serialisation
// library, no shared memory, and a truncated reply is reported as a crash
// instead of being half-decoded.
//
// POSIX uses `fork()`; other platforms get `isolation_supported() == false` and
// the caller renders in-process (documented, not silently degraded).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bonk::shell {

struct IsolatedRequest {
  std::string target;                 // file path or URL, same syntax as `Session::navigate`
  int width = 1024;
  int height = 768;
  std::string font_path;
  float scroll = 0.0f;
  std::string profile_dir;
  bool simulate_crash = false;        // child aborts on purpose (crash containment test)
};

struct IsolatedResult {
  bool ok = false;
  bool crashed = false;               // the child died before sending a frame
  int exit_code = 0;
  std::string title;
  std::string error;
  std::vector<std::uint8_t> png;      // rendered frame, ready to be written to disk
  double duration_ms = 0.0;
};

/// True when this build can actually isolate a tab in a child process.
[[nodiscard]] bool isolation_supported() noexcept;

/// Renders `request` in a child process and returns its frame or the crash report.
[[nodiscard]] IsolatedResult render_isolated(const IsolatedRequest& request);

}  // namespace bonk::shell
