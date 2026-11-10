// Linux-native Waydroid session integration. This controls a pre-installed
// Waydroid session; it does not initialize images or install system packages.
#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

#include "bonk/android/engine_session.hpp"
#include "bonk/core/result.hpp"

namespace bonk::android {

struct WaydroidConfig {
  std::string executable;  // Explicit absolute path to the user's waydroid CLI.
};

enum class WaydroidState { Unknown, Stopped, Running };

[[nodiscard]] std::string_view to_string(WaydroidState state) noexcept;

struct WaydroidStatus {
  WaydroidState session = WaydroidState::Unknown;
  WaydroidState container = WaydroidState::Unknown;
};

/// Pure command builders. Neither function invokes Waydroid.
[[nodiscard]] Result<RuntimeCommand> make_waydroid_status_command(const WaydroidConfig& config);
[[nodiscard]] Result<RuntimeCommand> make_waydroid_session_command(const WaydroidConfig& config);
[[nodiscard]] Result<RuntimeCommand> make_waydroid_full_ui_command(const WaydroidConfig& config);
[[nodiscard]] Result<RuntimeCommand> make_waydroid_install_apk_command(
    const WaydroidConfig& config, const std::filesystem::path& apk_path);
[[nodiscard]] Result<RuntimeCommand> make_waydroid_launch_package_command(
    const WaydroidConfig& config, std::string_view package_name);

/// Parses the `Session:` and optional `Container:` fields from `waydroid status`.
[[nodiscard]] Result<WaydroidStatus> parse_waydroid_status_output(std::string_view output);

/// Read-only status query. It does not start the container or download images.
[[nodiscard]] Result<WaydroidStatus> query_waydroid_status(
    const WaydroidConfig& config,
    std::chrono::milliseconds command_timeout = std::chrono::milliseconds{5000});

/// Starts a pre-installed Waydroid user session under EngineSession supervision.
/// Explicit confirmation is required because this may start the host's shared
/// Waydroid container; stopping the owned session may stop that container too.
/// The caller must run inside the desktop user's Wayland/D-Bus environment.
[[nodiscard]] Status start_waydroid_session(
    EngineSession& session, const WaydroidConfig& config, bool user_confirmed_start,
    std::chrono::milliseconds status_timeout = std::chrono::milliseconds{5000});

/// Waits for `waydroid status` to report a running session while also checking
/// that the owned foreground session process remains alive. Overall wait is
/// bounded to 5 minutes; it does not start or stop the session.
[[nodiscard]] Result<WaydroidStatus> wait_for_waydroid_session(
    EngineSession& session, const WaydroidConfig& config,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{120000},
    std::chrono::milliseconds poll_interval = std::chrono::milliseconds{250},
    std::chrono::milliseconds status_timeout = std::chrono::milliseconds{5000});

/// Opens Waydroid's own full UI, installs exactly one confirmed APK, or launches
/// an exact package ID. UI/install/launch operations require a running session;
/// they never implicitly start it. A successful command is not an app-readiness
/// check. APK installs require per-file confirmation.
[[nodiscard]] Status show_waydroid_full_ui(
    const WaydroidConfig& config,
    std::chrono::milliseconds command_timeout = std::chrono::milliseconds{15000});
[[nodiscard]] Status install_waydroid_selected_apk(
    const WaydroidConfig& config, const std::filesystem::path& apk_path, bool user_confirmed_install,
    std::chrono::milliseconds command_timeout = std::chrono::milliseconds{120000});
[[nodiscard]] Status launch_waydroid_package(
    const WaydroidConfig& config, std::string_view package_name,
    std::chrono::milliseconds command_timeout = std::chrono::milliseconds{15000});

}  // namespace bonk::android
