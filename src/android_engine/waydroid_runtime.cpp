#include "bonk/android/waydroid_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "bonk/android/process_runner.hpp"

namespace bonk::android {
namespace {

constexpr std::size_t kMaxWaydroidOutputBytes = 64 * 1024;
constexpr auto kMaxWaydroidCommandTimeout = std::chrono::minutes{5};
constexpr std::size_t kMaxAndroidPackageNameBytes = 255;

bool contains_nul(std::string_view value) noexcept { return value.find('\0') != std::string_view::npos; }
bool path_contains_nul(const std::filesystem::path& path) {
  return path.native().find('\0') != std::filesystem::path::string_type::npos;
}

bool ascii_alpha(unsigned char character) noexcept {
  return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

bool valid_package_name(std::string_view package_name) noexcept {
  if (package_name.empty() || package_name.size() > kMaxAndroidPackageNameBytes || contains_nul(package_name)) return false;
  bool segment_start = true;
  bool has_separator = false;
  for (const unsigned char character : package_name) {
    if (character == '.') {
      if (segment_start) return false;
      segment_start = true;
      has_separator = true;
    } else if (segment_start) {
      if (!ascii_alpha(character)) return false;
      segment_start = false;
    } else if (!ascii_alpha(character) && !(character >= '0' && character <= '9') && character != '_') {
      return false;
    }
  }
  return has_separator && !segment_start;
}

Status validate_config(const WaydroidConfig& config) {
  if (config.executable.empty() || contains_nul(config.executable) || config.executable.front() != '/') {
    return Status::failure(Error::make(
        "android.invalid_waydroid_config", "Waydroid executable must be an explicit absolute path"));
  }
  return Status::success();
}

Status validate_timeout(std::chrono::milliseconds timeout) {
  if (timeout.count() <= 0 || timeout > kMaxWaydroidCommandTimeout) {
    return Status::failure(
        Error::make("android.invalid_timeout", "Waydroid command timeout must be between 1 ms and 5 minutes"));
  }
  return Status::success();
}

Status validate_apk_path(const std::filesystem::path& apk_path) {
  if (apk_path.empty() || path_contains_nul(apk_path) || !apk_path.is_absolute()) {
    return Status::failure(
        Error::make("android.invalid_apk_path", "APK path must be an explicit absolute path without NUL bytes"));
  }
  return Status::success();
}

std::string_view trim_ascii(std::string_view value) noexcept {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r' ||
                            value.front() == '\n' || value.front() == '\f' || value.front() == '\v')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' ||
                            value.back() == '\n' || value.back() == '\f' || value.back() == '\v')) {
    value.remove_suffix(1);
  }
  return value;
}

bool ascii_equal_case_insensitive(std::string_view left, std::string_view right) noexcept {
  if (left.size() != right.size()) return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    char a = left[i];
    char b = right[i];
    if (a >= 'a' && a <= 'z') a = static_cast<char>(a - 'a' + 'A');
    if (b >= 'a' && b <= 'z') b = static_cast<char>(b - 'a' + 'A');
    if (a != b) return false;
  }
  return true;
}

WaydroidState parse_state(std::string_view value) noexcept {
  if (ascii_equal_case_insensitive(value, "RUNNING")) return WaydroidState::Running;
  if (ascii_equal_case_insensitive(value, "STOPPED")) return WaydroidState::Stopped;
  return WaydroidState::Unknown;
}

Result<RuntimeCommand> make_command(const WaydroidConfig& config, std::vector<std::string> arguments) {
  const Status valid = validate_config(config);
  if (valid.is_error()) return Result<RuntimeCommand>::failure(valid.error());
  return Result<RuntimeCommand>::success({config.executable, std::move(arguments)});
}

std::string process_failure_detail(const CommandOutput& output, std::string_view action) {
  std::string detail = "waydroid ";
  detail.append(action);
  detail += " command failed";
  if (output.exit_code.has_value()) {
    detail += " with exit code " + std::to_string(*output.exit_code);
  } else if (output.termination_signal.has_value()) {
    detail += " after signal " + std::to_string(*output.termination_signal);
  }
  std::string_view stderr_text = trim_ascii(output.stderr_data);
  if (!stderr_text.empty()) {
    constexpr std::size_t kMaxDetailBytes = 512;
    detail += ": ";
    detail.append(stderr_text.substr(0, kMaxDetailBytes));
    if (stderr_text.size() > kMaxDetailBytes) detail += "…";
  }
  return detail;
}

Result<CommandOutput> run_waydroid_operation(const RuntimeCommand& command, std::chrono::milliseconds timeout,
                                             std::string_view action) {
  const Status timeout_valid = validate_timeout(timeout);
  if (timeout_valid.is_error()) return Result<CommandOutput>::failure(timeout_valid.error());
  const Result<CommandOutput> output = run_command(command, timeout, kMaxWaydroidOutputBytes);
  if (output.is_error()) return Result<CommandOutput>::failure(output.error());
  if (output.value().timed_out) {
    return Result<CommandOutput>::failure(Error::make(
        "android.waydroid_command_timeout", "Waydroid " + std::string(action) + " command timed out"));
  }
  if (output.value().stdout_truncated || output.value().stderr_truncated) {
    return Result<CommandOutput>::failure(Error::make(
        "android.waydroid_output_truncated", "Waydroid command output exceeded the 64 KiB safety limit"));
  }
  if (!output.value().exit_code.has_value() || *output.value().exit_code != 0) {
    return Result<CommandOutput>::failure(Error::make(
        "android.waydroid_command_failed", process_failure_detail(output.value(), action)));
  }
  return output;
}

Status inspect_operation_output(const CommandOutput& output, std::string_view operation) {
  for (const std::string* stream : {&output.stdout_data, &output.stderr_data}) {
    std::istringstream lines{*stream};
    std::string line;
    while (std::getline(lines, line)) {
      const std::string_view text = trim_ascii(line);
      if (text.empty()) continue;
      if (ascii_equal_case_insensitive(text.substr(0, std::min<std::size_t>(text.size(), 6)), "ERROR:") ||
          ascii_equal_case_insensitive(text.substr(0, std::min<std::size_t>(text.size(), 7)), "FAILED ") ||
          text.find("WayDroid session is stopped") != std::string_view::npos ||
          text.find("Failed to access IPlatform service") != std::string_view::npos) {
        constexpr std::size_t kMaxDetailBytes = 512;
        std::string detail(text.substr(0, kMaxDetailBytes));
        if (text.size() > kMaxDetailBytes) detail += "…";
        return Status::failure(Error::make(
            "android.waydroid_" + std::string(operation) + "_reported_failure",
            "Waydroid " + std::string(operation) + " reported: " + detail));
      }
    }
  }
  return Status::success();
}

Status require_running_session(const WaydroidConfig& config, std::chrono::milliseconds timeout) {
  const Result<WaydroidStatus> status = query_waydroid_status(config, timeout);
  if (status.is_error()) return Status::failure(status.error());
  if (status.value().session == WaydroidState::Running) return Status::success();
  if (status.value().session == WaydroidState::Stopped) {
    return Status::failure(Error::make(
        "android.waydroid_session_required", "start the Waydroid session explicitly before using Android UI or apps"));
  }
  return Status::failure(
      Error::make("android.waydroid_state_unknown", "cannot verify that a Waydroid session is running"));
}

Result<WaydroidStatus> waydroid_session_exited(const SessionStatus& runtime) {
  std::string message = "Waydroid session process exited before the session became ready (state: ";
  message += to_string(runtime.state);
  if (runtime.exit_code.has_value()) message += ", exit code: " + std::to_string(*runtime.exit_code);
  if (runtime.termination_signal.has_value()) {
    message += ", signal: " + std::to_string(*runtime.termination_signal);
  }
  if (!runtime.message.empty()) message += ", detail: " + runtime.message;
  message += ")";
  return Result<WaydroidStatus>::failure(Error::make("android.waydroid_session_exited", std::move(message)));
}

}  // namespace

std::string_view to_string(WaydroidState state) noexcept {
  switch (state) {
    case WaydroidState::Unknown:
      return "unknown";
    case WaydroidState::Stopped:
      return "stopped";
    case WaydroidState::Running:
      return "running";
  }
  return "unknown";
}

Result<RuntimeCommand> make_waydroid_status_command(const WaydroidConfig& config) {
  return make_command(config, {"status"});
}

Result<RuntimeCommand> make_waydroid_session_command(const WaydroidConfig& config) {
  return make_command(config, {"session", "start"});
}

Result<RuntimeCommand> make_waydroid_full_ui_command(const WaydroidConfig& config) {
  return make_command(config, {"show-full-ui"});
}

Result<RuntimeCommand> make_waydroid_install_apk_command(const WaydroidConfig& config,
                                                        const std::filesystem::path& apk_path) {
  const Status path_valid = validate_apk_path(apk_path);
  if (path_valid.is_error()) return Result<RuntimeCommand>::failure(path_valid.error());
  return make_command(config, {"app", "install", apk_path.string()});
}

Result<RuntimeCommand> make_waydroid_launch_package_command(const WaydroidConfig& config,
                                                            std::string_view package_name) {
  if (!valid_package_name(package_name)) {
    return Result<RuntimeCommand>::failure(
        Error::make("android.invalid_package_name", "Android package ID is invalid"));
  }
  return make_command(config, {"app", "launch", std::string(package_name)});
}

Result<WaydroidStatus> parse_waydroid_status_output(std::string_view output) {
  if (output.size() > kMaxWaydroidOutputBytes) {
    return Result<WaydroidStatus>::failure(
        Error::make("android.waydroid_output_too_large", "Waydroid status output exceeds 64 KiB"));
  }

  WaydroidStatus status;
  bool found_session = false;
  bool found_container = false;
  std::istringstream lines{std::string(output)};
  std::string line;
  while (std::getline(lines, line)) {
    const std::string_view trimmed = trim_ascii(line);
    const std::size_t separator = trimmed.find(':');
    if (separator == std::string_view::npos) continue;
    const std::string_view key = trim_ascii(trimmed.substr(0, separator));
    const std::string_view value = trim_ascii(trimmed.substr(separator + 1));

    if (ascii_equal_case_insensitive(key, "Session")) {
      if (found_session) {
        return Result<WaydroidStatus>::failure(
            Error::make("android.ambiguous_waydroid_status", "Waydroid status contains duplicate Session fields"));
      }
      status.session = parse_state(value);
      found_session = true;
    } else if (ascii_equal_case_insensitive(key, "Container")) {
      if (found_container) {
        return Result<WaydroidStatus>::failure(Error::make(
            "android.ambiguous_waydroid_status", "Waydroid status contains duplicate Container fields"));
      }
      status.container = parse_state(value);
      found_container = true;
    }
  }

  if (!found_session) {
    return Result<WaydroidStatus>::failure(
        Error::make("android.invalid_waydroid_status", "Waydroid status output has no Session field"));
  }
  return Result<WaydroidStatus>::success(status);
}

Result<WaydroidStatus> query_waydroid_status(const WaydroidConfig& config,
                                             std::chrono::milliseconds command_timeout) {
  const Status timeout_valid = validate_timeout(command_timeout);
  if (timeout_valid.is_error()) return Result<WaydroidStatus>::failure(timeout_valid.error());
  const Result<RuntimeCommand> command = make_waydroid_status_command(config);
  if (command.is_error()) return Result<WaydroidStatus>::failure(command.error());

  const Result<CommandOutput> output = run_command(command.value(), command_timeout, kMaxWaydroidOutputBytes);
  if (output.is_error()) return Result<WaydroidStatus>::failure(output.error());
  if (output.value().timed_out) {
    return Result<WaydroidStatus>::failure(Error::make("android.waydroid_status_timeout", "Waydroid status query timed out"));
  }
  if (output.value().stdout_truncated || output.value().stderr_truncated) {
    return Result<WaydroidStatus>::failure(
        Error::make("android.waydroid_output_truncated", "Waydroid status output exceeded the 64 KiB safety limit"));
  }
  if (!output.value().exit_code.has_value() || *output.value().exit_code != 0) {
    return Result<WaydroidStatus>::failure(
        Error::make("android.waydroid_status_failed", process_failure_detail(output.value(), "status")));
  }
  return parse_waydroid_status_output(output.value().stdout_data);
}

Status start_waydroid_session(EngineSession& session, const WaydroidConfig& config, bool user_confirmed_start,
                              std::chrono::milliseconds status_timeout) {
  if (!user_confirmed_start) {
    return Status::failure(Error::make(
        "android.waydroid_start_consent_required",
        "starting Waydroid requires explicit confirmation because it may start the host container"));
  }

  const Result<WaydroidStatus> current = query_waydroid_status(config, status_timeout);
  if (current.is_error()) return Status::failure(current.error());
  if (current.value().session == WaydroidState::Running) {
    return Status::failure(Error::make(
        "android.waydroid_session_already_running",
        "a Waydroid session is already running; refusing to take ownership of an existing session"));
  }
  if (current.value().session != WaydroidState::Stopped) {
    return Status::failure(Error::make(
        "android.waydroid_state_unknown", "Waydroid session state is unknown; refusing to start it"));
  }

  const Result<RuntimeCommand> command = make_waydroid_session_command(config);
  if (command.is_error()) return Status::failure(command.error());
  return session.start(command.value());
}

Result<WaydroidStatus> wait_for_waydroid_session(EngineSession& session, const WaydroidConfig& config,
                                                 std::chrono::milliseconds timeout,
                                                 std::chrono::milliseconds poll_interval,
                                                 std::chrono::milliseconds status_timeout) {
  if (timeout.count() <= 0 || timeout > std::chrono::minutes{5}) {
    return Result<WaydroidStatus>::failure(
        Error::make("android.invalid_timeout", "Waydroid session wait must be between 1 ms and 5 minutes"));
  }
  if (poll_interval.count() <= 0 || poll_interval > std::chrono::seconds{30}) {
    return Result<WaydroidStatus>::failure(
        Error::make("android.invalid_poll_interval", "Waydroid session poll interval must be between 1 ms and 30 seconds"));
  }
  const Status status_timeout_valid = validate_timeout(status_timeout);
  if (status_timeout_valid.is_error()) return Result<WaydroidStatus>::failure(status_timeout_valid.error());

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  WaydroidStatus last_status;
  bool have_last_status = false;
  const auto wait_timeout_error = [&]() {
    std::string message = "timed out waiting for Waydroid session to become running";
    if (have_last_status) {
      message += "; last observed session state was ";
      message += to_string(last_status.session);
    }
    return Result<WaydroidStatus>::failure(Error::make("android.waydroid_session_timeout", std::move(message)));
  };
  while (true) {
    SessionStatus runtime = session.poll();
    if (runtime.state != ProcessState::Running) return waydroid_session_exited(runtime);

    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return wait_timeout_error();
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    const bool query_limited_by_wait_deadline = remaining <= status_timeout;
    const auto query_timeout = std::min(status_timeout, remaining);
    if (query_timeout.count() <= 0) {
      return Result<WaydroidStatus>::failure(
          Error::make("android.waydroid_session_timeout", "timed out waiting for Waydroid session status"));
    }

    const Result<WaydroidStatus> current = query_waydroid_status(config, query_timeout);
    if (current.is_error()) {
      runtime = session.poll();
      if (runtime.state != ProcessState::Running) return waydroid_session_exited(runtime);
      if (query_limited_by_wait_deadline && current.error().code == "android.waydroid_status_timeout") {
        return wait_timeout_error();
      }
      return Result<WaydroidStatus>::failure(current.error());
    }
    runtime = session.poll();
    if (runtime.state != ProcessState::Running) return waydroid_session_exited(runtime);
    last_status = current.value();
    have_last_status = true;
    if (last_status.session == WaydroidState::Running) return Result<WaydroidStatus>::success(last_status);
    if (last_status.session == WaydroidState::Unknown) {
      return Result<WaydroidStatus>::failure(
          Error::make("android.waydroid_state_unknown", "Waydroid returned an unrecognized session state"));
    }

    const auto after_query = std::chrono::steady_clock::now();
    if (after_query >= deadline) return wait_timeout_error();
    const auto sleep_budget = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - after_query);
    if (sleep_budget.count() <= 0) return wait_timeout_error();
    std::this_thread::sleep_for(std::min(poll_interval, sleep_budget));
  }
}

Status show_waydroid_full_ui(const WaydroidConfig& config, std::chrono::milliseconds command_timeout) {
  const Result<RuntimeCommand> command = make_waydroid_full_ui_command(config);
  if (command.is_error()) return Status::failure(command.error());
  const Status active_session = require_running_session(config, command_timeout);
  if (active_session.is_error()) return active_session;
  const Result<CommandOutput> output = run_waydroid_operation(command.value(), command_timeout, "show-full-ui");
  if (output.is_error()) return Status::failure(output.error());
  return inspect_operation_output(output.value(), "show_full_ui");
}

Status install_waydroid_selected_apk(const WaydroidConfig& config, const std::filesystem::path& apk_path,
                                     bool user_confirmed_install, std::chrono::milliseconds command_timeout) {
  const Status timeout_valid = validate_timeout(command_timeout);
  if (timeout_valid.is_error()) return timeout_valid;
  const Result<RuntimeCommand> command = make_waydroid_install_apk_command(config, apk_path);
  if (command.is_error()) return Status::failure(command.error());
  if (!user_confirmed_install) {
    return Status::failure(Error::make(
        "android.apk_install_consent_required", "installing this APK requires explicit per-file user confirmation"));
  }

  std::error_code status_error;
  if (!std::filesystem::is_regular_file(apk_path, status_error) || status_error) {
    return Status::failure(
        Error::make("android.apk_not_regular_file", "selected APK path is not an existing regular file"));
  }
  std::ifstream apk_file{apk_path, std::ios::binary};
  if (!apk_file) {
    return Status::failure(Error::make("android.apk_unreadable", "selected APK file is not readable"));
  }

  const Status active_session = require_running_session(config, command_timeout);
  if (active_session.is_error()) return active_session;
  const Result<CommandOutput> output = run_waydroid_operation(command.value(), command_timeout, "app install");
  if (output.is_error()) return Status::failure(output.error());
  return inspect_operation_output(output.value(), "apk_install");
}

Status launch_waydroid_package(const WaydroidConfig& config, std::string_view package_name,
                               std::chrono::milliseconds command_timeout) {
  const Status timeout_valid = validate_timeout(command_timeout);
  if (timeout_valid.is_error()) return timeout_valid;
  const Result<RuntimeCommand> command = make_waydroid_launch_package_command(config, package_name);
  if (command.is_error()) return Status::failure(command.error());
  const Status active_session = require_running_session(config, command_timeout);
  if (active_session.is_error()) return active_session;
  const Result<CommandOutput> output = run_waydroid_operation(command.value(), command_timeout, "app launch");
  if (output.is_error()) return Status::failure(output.error());
  return inspect_operation_output(output.value(), "app_launch");
}

}  // namespace bonk::android
