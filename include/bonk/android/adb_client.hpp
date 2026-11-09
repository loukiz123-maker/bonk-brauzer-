// Explicit ADB commands, one-shot screenshot capture, discrete input, and
// Android boot-state inspection. APIs that run ADB require explicit consent
// because the user-level ADB server may auto-start on first use.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "bonk/android/engine_session.hpp"
#include "bonk/core/result.hpp"
#include "bonk/image/image.hpp"

namespace bonk::android {

struct AdbConfig {
  std::filesystem::path executable;  // Explicit absolute path to platform-tools/adb.
  std::string serial;                // For example emulator-5554 or a selected device serial.
  // adb may start a user-level server and create ~/.android/adbkey on first use.
  // Set true only after the user explicitly agrees to that side effect.
  bool allow_adb_server_autostart = false;
};

enum class GuestState { NotConnected, Offline, Unauthorized, AdbOnline, Booting, BootComplete, Unknown };

[[nodiscard]] std::string_view to_string(GuestState state) noexcept;

struct GuestStatus {
  GuestState state = GuestState::Unknown;
  std::string serial;
  std::string detail;
};

/// Command builders are pure; they never invoke ADB. Device enumeration does
/// not need a selected serial, so make_adb_devices_command() ignores config.serial.
[[nodiscard]] Result<RuntimeCommand> make_adb_devices_command(const AdbConfig& config);
[[nodiscard]] Result<RuntimeCommand> make_adb_boot_property_command(const AdbConfig& config);
[[nodiscard]] Result<RuntimeCommand> make_adb_screenshot_command(const AdbConfig& config);
[[nodiscard]] Result<RuntimeCommand> make_adb_tap_command(const AdbConfig& config, std::uint32_t x, std::uint32_t y);
/// Builds one straight-line `adb shell input swipe`; duration must be 1 ms..30 s.
[[nodiscard]] Result<RuntimeCommand> make_adb_swipe_command(
    const AdbConfig& config, std::uint32_t start_x, std::uint32_t start_y,
    std::uint32_t end_x, std::uint32_t end_y, std::chrono::milliseconds duration);
[[nodiscard]] Result<RuntimeCommand> make_adb_key_command(const AdbConfig& config, std::uint32_t android_key_code);
[[nodiscard]] Result<RuntimeCommand> make_adb_launch_package_command(
    const AdbConfig& config, std::string_view package_name);
[[nodiscard]] Result<RuntimeCommand> make_adb_install_apk_command(
    const AdbConfig& config, const std::filesystem::path& apk_path);

/// Pure parsers for ADB output and a single PNG screenshot (not a video stream).
/// Screenshot decoding accepts at most 32 MiB of PNG and 16,777,216 pixels; chunk
/// lengths, CRCs, image format, and terminal IEND are checked before decoding.
[[nodiscard]] GuestStatus parse_adb_devices_output(std::string_view serial, std::string_view output);
/// Parses selectable device rows from `adb devices -l`; duplicate serials are marked Unknown.
[[nodiscard]] std::vector<GuestStatus> parse_adb_devices_list_output(std::string_view output);
[[nodiscard]] GuestState parse_boot_completed_property(std::string_view output) noexcept;
[[nodiscard]] Result<image::Image> decode_screenshot_png(std::string_view png_bytes);

struct PixelPoint {
  std::uint32_t x = 0;
  std::uint32_t y = 0;
};

/// Maps finite normalized viewport coordinates in [0, 1] to guest pixels in
/// [0, width-1] x [0, height-1], rounding to the nearest pixel.
[[nodiscard]] Result<PixelPoint> normalized_touch_to_pixels(
    std::uint32_t width, std::uint32_t height, float x, float y);

/// Captures one ADB screenshot and decodes it to RGBA pixels. The PNG is
/// limited to 32 MiB and 16,777,216 pixels; the default command timeout is 5s and
/// the maximum accepted timeout is 60s. This is a still frame, not a stream.
[[nodiscard]] Result<image::Image> capture_screenshot_rgba(
    const AdbConfig& config,
    std::chrono::milliseconds command_timeout = std::chrono::milliseconds{5000});

/// Sends a tap or one straight-line touch gesture between normalized viewport
/// endpoints. Android's input shell synthesizes the gesture; callers cannot
/// control individual DOWN/MOVE/UP events, pointer IDs, cancellation, multitouch,
/// or joystick state. The gesture duration is 1 ms..30 s and must be shorter
/// than the ADB command timeout. The caller supplies guest-viewport dimensions.
[[nodiscard]] Status send_tap_normalized(
    const AdbConfig& config, std::uint32_t width, std::uint32_t height, float x, float y,
    std::chrono::milliseconds command_timeout = std::chrono::milliseconds{5000});
[[nodiscard]] Status send_swipe_normalized(
    const AdbConfig& config, std::uint32_t width, std::uint32_t height,
    float start_x, float start_y, float end_x, float end_y,
    std::chrono::milliseconds gesture_duration = std::chrono::milliseconds{300},
    std::chrono::milliseconds command_timeout = std::chrono::milliseconds{5000});
[[nodiscard]] Status send_android_key(
    const AdbConfig& config, std::uint32_t android_key_code,
    std::chrono::milliseconds command_timeout = std::chrono::milliseconds{5000});

/// Starts the launchable activity for an already-installed package. This does
/// not install an APK, and a successful `am start` is not an app-readiness check.
/// Explicit `Error:` or non-`ok` `Status:` output in either stream is reported
/// as a launch failure even when ADB exits nonzero; otherwise the exit status is
/// checked normally.
[[nodiscard]] Status launch_installed_package(
    const AdbConfig& config, std::string_view package_name,
    std::chrono::milliseconds command_timeout = std::chrono::milliseconds{15000});

/// Installs exactly the caller-selected APK. `user_confirmed_install` must be
/// true only after explicit confirmation for this path. No replace/downgrade
/// flag is passed. Requires ADB server consent as well; does not launch the app.
/// A `Failure...` line in either output stream is reported as an install failure
/// even when ADB exits nonzero; otherwise the exit status is checked normally.
[[nodiscard]] Status install_selected_apk(
    const AdbConfig& config, const std::filesystem::path& apk_path, bool user_confirmed_install,
    std::chrono::milliseconds command_timeout = std::chrono::milliseconds{60000});

/// Lists ADB transports without requiring a selected serial. The serial field
/// in config is ignored; callers can present the returned rows for explicit
/// selection. This does not query guest boot state. Requires ADB server consent.
[[nodiscard]] Result<std::vector<GuestStatus>> list_adb_devices(
    const AdbConfig& config,
    std::chrono::milliseconds command_timeout = std::chrono::milliseconds{5000});

/// Checks the selected ADB transport, then queries sys.boot_completed only if
/// that serial is online. The timeout applies independently to each command.
/// A result of BootComplete means only that Android reports its boot property;
/// it does not prove that an app/game is installed or usable.
[[nodiscard]] Result<GuestStatus> query_guest_status(
    const AdbConfig& config,
    std::chrono::milliseconds command_timeout = std::chrono::milliseconds{5000});

/// Polls ADB until sys.boot_completed is 1. This does not start an emulator,
/// launch an app, or establish app/game readiness. The overall wait is bounded
/// to 5 minutes; poll interval is 1 ms..30 s and per-command timeout is 1 ms..60 s.
[[nodiscard]] Result<GuestStatus> wait_for_guest_boot(
    const AdbConfig& config,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{120000},
    std::chrono::milliseconds poll_interval = std::chrono::milliseconds{500},
    std::chrono::milliseconds command_timeout = std::chrono::milliseconds{5000});

}  // namespace bonk::android
