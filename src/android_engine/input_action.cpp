#include "bonk/android/input_action.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace bonk::android {
namespace {

constexpr std::uint32_t kMaxViewportDimension = 16384;
constexpr std::uint32_t kMaxAndroidKeyCode = 65535;

bool finite_in_unit_range(float value) noexcept {
  if (!std::isfinite(value)) return false;
  return value >= 0.0f && value <= 1.0f;
}

}  // namespace

std::string_view to_string(InputActionKind kind) noexcept {
  switch (kind) {
    case InputActionKind::Tap:
      return "tap";
    case InputActionKind::Swipe:
      return "swipe";
    case InputActionKind::Key:
      return "key";
  }
  return "unknown";
}

bool InputViewport::valid() const noexcept {
  return width > 0 && height > 0 && width <= kMaxViewportDimension && height <= kMaxViewportDimension;
}

Status validate_input_action(const InputAction& action) {
  switch (action.kind) {
    case InputActionKind::Tap: {
      if (!action.viewport.valid()) {
        return Status::failure(Error::make(
            "android.invalid_viewport", "a tap requires the guest viewport it was captured against"));
      }
      if (!finite_in_unit_range(action.start_x) || !finite_in_unit_range(action.start_y)) {
        return Status::failure(
            Error::make("android.invalid_coordinates", "tap coordinates must be finite values in [0, 1]"));
      }
      return Status::success();
    }
    case InputActionKind::Swipe: {
      if (!action.viewport.valid()) {
        return Status::failure(Error::make(
            "android.invalid_viewport", "a swipe requires the guest viewport it was captured against"));
      }
      if (!finite_in_unit_range(action.start_x) || !finite_in_unit_range(action.start_y) ||
          !finite_in_unit_range(action.end_x) || !finite_in_unit_range(action.end_y)) {
        return Status::failure(
            Error::make("android.invalid_coordinates", "swipe coordinates must be finite values in [0, 1]"));
      }
      if (action.duration < kMinInputDuration || action.duration > kMaxInputDuration) {
        return Status::failure(Error::make(
            "android.invalid_gesture_duration", "swipe duration must be between 1 ms and 30 seconds"));
      }
      return Status::success();
    }
    case InputActionKind::Key: {
      if (action.key_code == 0 || action.key_code > kMaxAndroidKeyCode) {
        return Status::failure(
            Error::make("android.invalid_key_code", "Android key code must be between 1 and 65535"));
      }
      return Status::success();
    }
  }
  return Status::failure(Error::make("android.invalid_input_action", "unrecognized input action kind"));
}

Result<InputAction> make_tap(InputViewport viewport, float x, float y) {
  InputAction action;
  action.kind = InputActionKind::Tap;
  action.viewport = viewport;
  action.start_x = x;
  action.start_y = y;
  const Status valid = validate_input_action(action);
  if (valid.is_error()) return Result<InputAction>::failure(valid.error());
  return Result<InputAction>::success(action);
}

Result<InputAction> make_swipe(InputViewport viewport, float start_x, float start_y, float end_x, float end_y,
                               std::chrono::milliseconds duration) {
  InputAction action;
  action.kind = InputActionKind::Swipe;
  action.viewport = viewport;
  action.start_x = start_x;
  action.start_y = start_y;
  action.end_x = end_x;
  action.end_y = end_y;
  action.duration = duration;
  const Status valid = validate_input_action(action);
  if (valid.is_error()) return Result<InputAction>::failure(valid.error());
  return Result<InputAction>::success(action);
}

Result<InputAction> make_key(std::uint32_t android_key_code) {
  InputAction action;
  action.kind = InputActionKind::Key;
  action.key_code = android_key_code;
  const Status valid = validate_input_action(action);
  if (valid.is_error()) return Result<InputAction>::failure(valid.error());
  return Result<InputAction>::success(action);
}

Result<RuntimeCommand> build_input_command(const AdbConfig& config, const InputAction& action) {
  const Status valid = validate_input_action(action);
  if (valid.is_error()) return Result<RuntimeCommand>::failure(valid.error());

  switch (action.kind) {
    case InputActionKind::Tap: {
      const Result<PixelPoint> point =
          normalized_touch_to_pixels(action.viewport.width, action.viewport.height, action.start_x, action.start_y);
      if (point.is_error()) return Result<RuntimeCommand>::failure(point.error());
      const Result<RuntimeCommand> command = make_adb_tap_command(config, point.value().x, point.value().y);
      return command;
    }
    case InputActionKind::Swipe: {
      const Result<PixelPoint> start =
          normalized_touch_to_pixels(action.viewport.width, action.viewport.height, action.start_x, action.start_y);
      if (start.is_error()) return Result<RuntimeCommand>::failure(start.error());
      const Result<PixelPoint> end =
          normalized_touch_to_pixels(action.viewport.width, action.viewport.height, action.end_x, action.end_y);
      if (end.is_error()) return Result<RuntimeCommand>::failure(end.error());
      return make_adb_swipe_command(config, start.value().x, start.value().y, end.value().x, end.value().y,
                                    action.duration);
    }
    case InputActionKind::Key:
      return make_adb_key_command(config, action.key_code);
  }
  return Result<RuntimeCommand>::failure(
      Error::make("android.invalid_input_action", "unrecognized input action kind"));
}

Result<InputSendResult> send_input_action(const AdbConfig& config, const InputAction& action,
                                          std::chrono::milliseconds command_timeout) {
  const Result<RuntimeCommand> command = build_input_command(config, action);
  if (command.is_error()) return Result<InputSendResult>::failure(command.error());
  const Status sent = run_adb(config, command.value(), command_timeout);
  if (sent.is_error()) return Result<InputSendResult>::failure(sent.error());

  InputSendResult result;
  result.sequence = action.sequence;
  result.command_sent = true;
  // Transport accepted the request. Nothing inside Android has reported
  // receiving it, so delivery stays unproven.
  result.observed_in_guest = false;
  result.detail = "requested " + std::string(to_string(action.kind)) + " via adb shell input";
  return Result<InputSendResult>::success(std::move(result));
}

}  // namespace bonk::android
