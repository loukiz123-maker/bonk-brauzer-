// Bounded, shell-free execution for short Android host-tool commands (for
// example adb). This is not the long-lived runtime supervisor.
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

#include "bonk/android/engine_session.hpp"
#include "bonk/core/result.hpp"

namespace bonk::android {

struct CommandOutput {
  std::string stdout_data;
  std::string stderr_data;
  std::optional<int> exit_code;
  std::optional<int> termination_signal;
  bool timed_out = false;
  bool stdout_truncated = false;
  bool stderr_truncated = false;
};

/// Runs an explicit absolute executable path and argv, captures stdout and
/// stderr separately, and kills/reaps its process group on timeout. No shell or
/// PATH search is involved.
/// max_bytes_per_stream is capped at 64 MiB to bound memory use.
[[nodiscard]] Result<CommandOutput> run_command(
    const RuntimeCommand& command,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{5000},
    std::size_t max_bytes_per_stream = 64 * 1024);

}  // namespace bonk::android
