// Host-side lifecycle adapter for a user-installed Android emulator runtime.
//
// This supervises a runtime process; it deliberately does not claim that an
// Android guest has booted. Guest readiness, video and input are separate
// bridge capabilities documented in docs/ANDROID_ENGINE.md.
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bonk/core/result.hpp"

namespace bonk::android {

/// A process invocation. Arguments are passed as argv entries, never through a shell.
struct RuntimeCommand {
  std::string executable;  // Absolute path or a command resolved through PATH.
  std::vector<std::string> arguments;
};

/// Inputs for launching an already-installed Android SDK Emulator AVD.
/// This does not install the emulator, create an AVD, or download a system image.
struct EmulatorConfig {
  std::string emulator_executable;  // Explicit absolute path; no PATH lookup.
  std::string avd_name;
  bool headless = true;
};

/// Builds `emulator -avd NAME [-no-window]` without invoking or probing the binary.
[[nodiscard]] Result<RuntimeCommand> make_emulator_command(const EmulatorConfig& config);

enum class ProcessState { Stopped, Running, Exited, Failed };

[[nodiscard]] std::string_view to_string(ProcessState state) noexcept;

/// Snapshot of the host process only. Running means the OS created the process;
/// it is not evidence that Android is booted or ready for input.
struct SessionStatus {
  ProcessState state = ProcessState::Stopped;
  int process_id = -1;  // Valid only while the child is still owned by this session.
  std::optional<int> exit_code;
  std::optional<int> termination_signal;
  std::string message;  // Non-empty for a failed operation/state.
};

/// Owns one foreground runtime process and its process group.
///
/// The object is single-threaded: call start/poll/stop from one owner thread.
/// Destruction sends SIGTERM, then SIGKILL if necessary, and reaps the child.
class EngineSession final {
 public:
  EngineSession() = default;
  ~EngineSession() noexcept;

  EngineSession(const EngineSession&) = delete;
  EngineSession& operator=(const EngineSession&) = delete;
  EngineSession(EngineSession&&) = delete;
  EngineSession& operator=(EngineSession&&) = delete;

  /// Starts an executable directly (no shell). A successful return means only
  /// that process creation succeeded; it does not wait for Android boot.
  [[nodiscard]] Status start(const RuntimeCommand& command);

  /// Refreshes the child process state using a non-blocking wait.
  [[nodiscard]] SessionStatus poll();

  /// Gracefully stops the owned process group, escalating to SIGKILL on timeout.
  /// The grace period applies to the runtime process itself; once it is gone,
  /// anything left in its process group is killed, so a successful stop never
  /// leaves helper processes behind. Calling stop when there is no owned child
  /// is a no-op (members of a group whose runtime exited on its own are not
  /// tracked any more).
  [[nodiscard]] Status stop(std::chrono::milliseconds grace_period = std::chrono::milliseconds{1500});

  /// Last observed status; call poll() to refresh a running process.
  [[nodiscard]] const SessionStatus& snapshot() const noexcept { return status_; }

 private:
  [[nodiscard]] Status fail(std::string code, std::string message);
  void record_exit(int wait_status) noexcept;

  int child_pid_ = -1;
  bool stop_requested_ = false;
  SessionStatus status_;
};

}  // namespace bonk::android
