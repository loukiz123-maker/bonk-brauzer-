// LynxDroid input actions.
//
// Input is modelled as validated, immutable actions rather than free-form
// coordinates + delays, so the transport layer cannot invent gestures:
//
//   * a viewport-relative action always carries the guest viewport it was
//     captured against, so a later resolution change cannot silently move a tap;
//   * durations are bounded, so a swipe cannot become an unbounded hold;
//   * the action says nothing about delivery - `adb shell input` asks Android
//     to synthesize a gesture, it does not prove an app received it.
//
// Nothing here spawns a process.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "bonk/android/adb_client.hpp"
#include "bonk/core/result.hpp"

namespace bonk::android {

enum class InputActionKind { Tap, Swipe, Key };

[[nodiscard]] std::string_view to_string(InputActionKind kind) noexcept;

/// Guest viewport an action was authored against, in Android pixels.
/// Zero dimensions mean "not captured yet" and reject viewport actions.
struct InputViewport {
  std::uint32_t width = 0;
  std::uint32_t height = 0;

  [[nodiscard]] bool valid() const noexcept;
};

/// Bounds shared with the ADB transport.
inline constexpr std::chrono::milliseconds kMinInputDuration{1};
inline constexpr std::chrono::milliseconds kMaxInputDuration{30000};

struct InputAction {
  InputActionKind kind = InputActionKind::Tap;
  InputViewport viewport;
  // Normalized [0, 1] endpoints; only `start` is used by Tap.
  float start_x = 0.0f;
  float start_y = 0.0f;
  float end_x = 0.0f;
  float end_y = 0.0f;
  std::chrono::milliseconds duration{0};  // Swipe only
  std::uint32_t key_code = 0;             // Key only, 1..65535

  /// Monotonic sequence number assigned by the caller, used to correlate an
  /// accepted action with a guest-side witness. 0 means unassigned.
  std::uint64_t sequence = 0;
};

/// Validation results (all rejections happen before any process is spawned):
///   android.invalid_input_action  - malformed enum/duration/keycode mix
///   android.invalid_viewport      - zero/oversized viewport for a tap/swipe
///   android.invalid_coordinates   - non-finite or outside [0, 1]
///   android.invalid_gesture_duration - swipe outside 1 ms..30 s
///   android.invalid_key_code      - 0 or > 65535
[[nodiscard]] Status validate_input_action(const InputAction& action);

[[nodiscard]] Result<InputAction> make_tap(InputViewport viewport, float x, float y);
[[nodiscard]] Result<InputAction> make_swipe(InputViewport viewport, float start_x, float start_y, float end_x,
                                             float end_y, std::chrono::milliseconds duration);
[[nodiscard]] Result<InputAction> make_key(std::uint32_t android_key_code);

/// Outcome of asking the transport to deliver one action. `command_sent` is a
/// transport result; `observed_in_guest` stays false until an in-guest witness
/// (a test app/agent) reports the action, which is a separate acceptance gate.
struct InputSendResult {
  std::uint64_t sequence = 0;
  bool command_sent = false;
  bool observed_in_guest = false;
  std::string detail;
};

/// Sends one validated action through the ADB transport. Requires ADB server
/// consent (passed via the config) exactly like every other ADB call.
[[nodiscard]] Result<InputSendResult> send_input_action(
    const AdbConfig& config, const InputAction& action,
    std::chrono::milliseconds command_timeout = std::chrono::milliseconds{5000});

/// Translates a validated action into the ADB command that requests it.
/// Returns `android.invalid_input_action` for an invalid action, so callers
/// cannot bypass validation by building a command directly.
[[nodiscard]] Result<RuntimeCommand> build_input_command(const AdbConfig& config, const InputAction& action);

}  // namespace bonk::android
