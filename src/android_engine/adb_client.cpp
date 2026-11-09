#include "bonk/android/adb_client.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "bonk/android/engine_session.hpp"
#include "bonk/android/process_runner.hpp"

namespace bonk::android {
namespace {

constexpr std::size_t kDefaultAdbCaptureBytes = 64 * 1024;
constexpr std::size_t kMaxScreenshotPngBytes = 32 * 1024 * 1024;
constexpr std::size_t kMaxScreenshotPixels = 16 * 1024 * 1024;
constexpr std::uint32_t kMaxViewportDimension = 16384;
constexpr std::uint32_t kMaxAndroidKeyCode = 65535;
constexpr std::size_t kMaxAndroidPackageNameBytes = 255;
constexpr auto kMaxAdbCommandTimeout = std::chrono::minutes{1};
constexpr auto kMaxAdbSwipeDuration = std::chrono::seconds{30};
constexpr auto kMaxGuestBootWaitTimeout = std::chrono::minutes{5};
constexpr auto kMaxGuestBootPollInterval = std::chrono::seconds{30};

bool ascii_alpha(unsigned char character) {
  return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

bool valid_package_name(std::string_view package_name) {
  if (package_name.empty() || package_name.size() > kMaxAndroidPackageNameBytes) return false;
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

bool valid_serial(std::string_view serial) {
  if (serial.empty() || serial.front() == '-') return false;
  for (const unsigned char character : serial) {
    const bool alphanumeric = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                              (character >= '0' && character <= '9');
    const bool accepted = alphanumeric || character == '_' || character == '-' || character == '.' || character == ':';
    if (!accepted) return false;
  }
  return true;
}

bool has_nul(const std::filesystem::path& path) { return path.native().find('\0') != std::filesystem::path::string_type::npos; }

Status validate_config(const AdbConfig& config) {
  if (config.executable.empty() || has_nul(config.executable) || !config.executable.is_absolute()) {
    return Status::failure(Error::make("android.invalid_adb_config", "ADB executable must be an explicit absolute path"));
  }
  if (!valid_serial(config.serial) || config.serial.find('\0') != std::string::npos) {
    return Status::failure(Error::make("android.invalid_adb_config", "ADB serial is empty or contains unsupported characters"));
  }
  return Status::success();
}

Status validate_apk_path(const std::filesystem::path& apk_path) {
  if (apk_path.empty() || has_nul(apk_path) || !apk_path.is_absolute()) {
    return Status::failure(
        Error::make("android.invalid_apk_path", "APK path must be an explicit absolute path without NUL bytes"));
  }
  return Status::success();
}

Status validate_timeout(std::chrono::milliseconds timeout) {
  if (timeout.count() <= 0 || timeout > kMaxAdbCommandTimeout) {
    return Status::failure(
        Error::make("android.invalid_timeout", "ADB command timeout must be between 1 ms and 60 seconds"));
  }
  return Status::success();
}

Status require_server_consent(const AdbConfig& config) {
  if (!config.allow_adb_server_autostart) {
    return Status::failure(Error::make(
        "android.adb_server_consent_required",
        "using ADB may start a user-level ADB server or create an ADB key; explicit user consent is required"));
  }
  return Status::success();
}

Status validate_swipe_duration(std::chrono::milliseconds duration) {
  if (duration.count() <= 0 || duration > kMaxAdbSwipeDuration) {
    return Status::failure(
        Error::make("android.invalid_gesture_duration", "touch gesture duration must be between 1 ms and 30 seconds"));
  }
  return Status::success();
}

std::string_view trim_ascii(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) value.remove_prefix(1);
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) value.remove_suffix(1);
  return value;
}

std::uint32_t read_png_u32(std::string_view data, std::size_t offset) {
  return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset])) << 24) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 1])) << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 2])) << 8) |
         static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 3]));
}

std::uint32_t png_crc32(std::string_view data) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const char byte : data) {
    crc ^= static_cast<std::uint8_t>(byte);
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc ^ 0xFFFFFFFFu;
}

struct ScreenshotPngInfo {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

Result<ScreenshotPngInfo> inspect_screenshot_png(std::string_view bytes) {
  if (bytes.size() > kMaxScreenshotPngBytes) {
    return Result<ScreenshotPngInfo>::failure(
        Error::make("android.screenshot_too_large", "ADB screenshot PNG exceeds the 32 MiB limit"));
  }
  constexpr char kSignature[8] = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};
  if (bytes.size() < sizeof(kSignature) || std::memcmp(bytes.data(), kSignature, sizeof(kSignature)) != 0) {
    return Result<ScreenshotPngInfo>::failure(Error::make("android.invalid_screenshot", "ADB output is not a PNG file"));
  }

  ScreenshotPngInfo info;
  bool header_seen = false;
  bool image_data_chunk_seen = false;
  bool image_data_seen = false;
  bool image_data_ended = false;
  std::size_t position = sizeof(kSignature);
  while (position < bytes.size()) {
    if (bytes.size() - position < 12) {
      return Result<ScreenshotPngInfo>::failure(
          Error::make("android.invalid_screenshot", "truncated PNG chunk header or checksum"));
    }
    const std::uint32_t chunk_length = read_png_u32(bytes, position);
    const std::size_t available_payload_and_crc = bytes.size() - position - 8;
    if (chunk_length > available_payload_and_crc - 4) {
      return Result<ScreenshotPngInfo>::failure(Error::make("android.invalid_screenshot", "PNG chunk runs past the output"));
    }
    const std::string_view type = bytes.substr(position + 4, 4);
    const std::string_view payload = bytes.substr(position + 8, chunk_length);
    const std::size_t crc_offset = position + 8 + chunk_length;
    const std::uint32_t stored_crc = read_png_u32(bytes, crc_offset);
    const std::uint32_t computed_crc = png_crc32(bytes.substr(position + 4, 4 + chunk_length));
    if (stored_crc != computed_crc) {
      return Result<ScreenshotPngInfo>::failure(Error::make("android.invalid_screenshot", "PNG chunk checksum mismatch"));
    }

    if (type == "IHDR") {
      if (header_seen || position != sizeof(kSignature) || chunk_length != 13) {
        return Result<ScreenshotPngInfo>::failure(Error::make("android.invalid_screenshot", "invalid PNG IHDR chunk"));
      }
      info.width = read_png_u32(payload, 0);
      info.height = read_png_u32(payload, 4);
      const auto bit_depth = static_cast<std::uint8_t>(payload[8]);
      const auto color_type = static_cast<std::uint8_t>(payload[9]);
      const auto compression_method = static_cast<std::uint8_t>(payload[10]);
      const auto filter_method = static_cast<std::uint8_t>(payload[11]);
      const auto interlace_method = static_cast<std::uint8_t>(payload[12]);
      if (info.width == 0 || info.height == 0 || info.width > kMaxViewportDimension ||
          info.height > kMaxViewportDimension || static_cast<std::uint64_t>(info.width) * info.height >
                                                     kMaxScreenshotPixels) {
        return Result<ScreenshotPngInfo>::failure(
            Error::make("android.screenshot_too_large", "ADB screenshot dimensions exceed the 16,777,216-pixel limit"));
      }
      if (bit_depth != 8 || (color_type != 0 && color_type != 2 && color_type != 4 && color_type != 6) ||
          compression_method != 0 || filter_method != 0 || interlace_method > 1) {
        return Result<ScreenshotPngInfo>::failure(
            Error::make("android.unsupported_screenshot_format", "ADB screenshot must be an 8-bit PNG image"));
      }
      header_seen = true;
    } else {
      if (!header_seen) {
        return Result<ScreenshotPngInfo>::failure(
            Error::make("android.invalid_screenshot", "PNG IHDR must be the first chunk"));
      }
      if (type == "IDAT") {
        if (image_data_ended) {
          return Result<ScreenshotPngInfo>::failure(
              Error::make("android.invalid_screenshot", "PNG IDAT chunks are not contiguous"));
        }
        image_data_chunk_seen = true;
        image_data_seen = image_data_seen || chunk_length != 0;
      } else {
        if (image_data_chunk_seen) image_data_ended = true;
        if (type == "IEND") {
          if (chunk_length != 0 || !image_data_seen) {
            return Result<ScreenshotPngInfo>::failure(
                Error::make("android.invalid_screenshot", "PNG IEND is malformed or image data is missing"));
          }
          const std::size_t end = crc_offset + 4;
          if (end != bytes.size()) {
            return Result<ScreenshotPngInfo>::failure(
                Error::make("android.invalid_screenshot", "unexpected data follows PNG IEND"));
          }
          return Result<ScreenshotPngInfo>::success(info);
        }
      }
    }
    position = crc_offset + 4;
  }
  return Result<ScreenshotPngInfo>::failure(Error::make("android.invalid_screenshot", "PNG is missing IEND"));
}

std::string command_failure_message(const CommandOutput& output) {
  std::string detail = output.stderr_data.empty() ? output.stdout_data : output.stderr_data;
  const std::string_view trimmed = trim_ascii(detail);
  detail.assign(trimmed);
  constexpr std::size_t kMaxMessageBytes = 512;
  if (detail.size() > kMaxMessageBytes) {
    detail.resize(kMaxMessageBytes);
    detail += "…";
  }
  if (detail.empty()) detail = "ADB command exited unsuccessfully without diagnostic output";
  return detail;
}

Result<CommandOutput> run_adb(const RuntimeCommand& command, std::chrono::milliseconds timeout,
                              std::size_t max_capture_bytes = kDefaultAdbCaptureBytes) {
  const Result<CommandOutput> result = run_command(command, timeout, max_capture_bytes);
  if (result.is_error()) return Result<CommandOutput>::failure(result.error());
  if (result.value().timed_out) {
    return Result<CommandOutput>::failure(Error::make("android.adb_timeout", "ADB command timed out"));
  }
  if (result.value().stdout_truncated || result.value().stderr_truncated) {
    return Result<CommandOutput>::failure(Error::make(
        "android.adb_output_truncated",
        "ADB command output exceeded the " + std::to_string(max_capture_bytes) + " byte per-stream safety limit"));
  }
  if (!result.value().exit_code.has_value() || *result.value().exit_code != 0) {
    return Result<CommandOutput>::failure(
        Error::make("android.adb_command_failed", command_failure_message(result.value())));
  }
  return result;
}

Result<GuestStatus> guest_boot_timeout(const GuestStatus* last_status) {
  std::string message = "timed out waiting for sys.boot_completed=1";
  if (last_status != nullptr) {
    message += "; last observed state was ";
    message += to_string(last_status->state);
    if (!last_status->detail.empty()) {
      message += ": ";
      constexpr std::size_t kMaxDetailBytes = 256;
      message += last_status->detail.substr(0, kMaxDetailBytes);
      if (last_status->detail.size() > kMaxDetailBytes) message += "…";
    }
  }
  return Result<GuestStatus>::failure(Error::make("android.guest_boot_timeout", std::move(message)));
}

}  // namespace

std::string_view to_string(GuestState state) noexcept {
  switch (state) {
    case GuestState::NotConnected:
      return "not-connected";
    case GuestState::Offline:
      return "offline";
    case GuestState::Unauthorized:
      return "unauthorized";
    case GuestState::AdbOnline:
      return "adb-online";
    case GuestState::Booting:
      return "booting";
    case GuestState::BootComplete:
      return "boot-complete";
    case GuestState::Unknown:
      return "unknown";
  }
  return "unknown";
}

Result<RuntimeCommand> make_adb_devices_command(const AdbConfig& config) {
  const Status valid = validate_config(config);
  if (valid.is_error()) return Result<RuntimeCommand>::failure(valid.error());
  return Result<RuntimeCommand>::success({config.executable.string(), {"devices", "-l"}});
}

Result<RuntimeCommand> make_adb_boot_property_command(const AdbConfig& config) {
  const Status valid = validate_config(config);
  if (valid.is_error()) return Result<RuntimeCommand>::failure(valid.error());
  return Result<RuntimeCommand>::success(
      {config.executable.string(), {"-s", config.serial, "shell", "getprop", "sys.boot_completed"}});
}

Result<RuntimeCommand> make_adb_screenshot_command(const AdbConfig& config) {
  const Status valid = validate_config(config);
  if (valid.is_error()) return Result<RuntimeCommand>::failure(valid.error());
  return Result<RuntimeCommand>::success(
      {config.executable.string(), {"-s", config.serial, "exec-out", "screencap", "-p"}});
}

Result<RuntimeCommand> make_adb_tap_command(const AdbConfig& config, std::uint32_t x, std::uint32_t y) {
  const Status valid = validate_config(config);
  if (valid.is_error()) return Result<RuntimeCommand>::failure(valid.error());
  return Result<RuntimeCommand>::success({config.executable.string(),
                                          {"-s", config.serial, "shell", "input", "tap", std::to_string(x),
                                           std::to_string(y)}});
}

Result<RuntimeCommand> make_adb_swipe_command(const AdbConfig& config, std::uint32_t start_x, std::uint32_t start_y,
                                              std::uint32_t end_x, std::uint32_t end_y,
                                              std::chrono::milliseconds duration) {
  const Status valid = validate_config(config);
  if (valid.is_error()) return Result<RuntimeCommand>::failure(valid.error());
  const Status duration_valid = validate_swipe_duration(duration);
  if (duration_valid.is_error()) return Result<RuntimeCommand>::failure(duration_valid.error());
  return Result<RuntimeCommand>::success(
      {config.executable.string(),
       {"-s", config.serial, "shell", "input", "swipe", std::to_string(start_x), std::to_string(start_y),
        std::to_string(end_x), std::to_string(end_y), std::to_string(duration.count())}});
}

Result<RuntimeCommand> make_adb_key_command(const AdbConfig& config, std::uint32_t android_key_code) {
  const Status valid = validate_config(config);
  if (valid.is_error()) return Result<RuntimeCommand>::failure(valid.error());
  if (android_key_code == 0 || android_key_code > kMaxAndroidKeyCode) {
    return Result<RuntimeCommand>::failure(
        Error::make("android.invalid_key_code", "Android key code must be between 1 and 65535"));
  }
  return Result<RuntimeCommand>::success(
      {config.executable.string(), {"-s", config.serial, "shell", "input", "keyevent", std::to_string(android_key_code)}});
}

Result<RuntimeCommand> make_adb_launch_package_command(const AdbConfig& config, std::string_view package_name) {
  const Status valid = validate_config(config);
  if (valid.is_error()) return Result<RuntimeCommand>::failure(valid.error());
  if (!valid_package_name(package_name)) {
    return Result<RuntimeCommand>::failure(
        Error::make("android.invalid_package_name", "Android package name must be a valid dot-separated identifier"));
  }
  return Result<RuntimeCommand>::success(
      {config.executable.string(),
       {"-s", config.serial, "shell", "am", "start", "-W", "-a", "android.intent.action.MAIN", "-c",
        "android.intent.category.LAUNCHER", "-p", std::string(package_name)}});
}

Result<RuntimeCommand> make_adb_install_apk_command(const AdbConfig& config, const std::filesystem::path& apk_path) {
  const Status valid = validate_config(config);
  if (valid.is_error()) return Result<RuntimeCommand>::failure(valid.error());
  const Status apk_path_valid = validate_apk_path(apk_path);
  if (apk_path_valid.is_error()) return Result<RuntimeCommand>::failure(apk_path_valid.error());
  return Result<RuntimeCommand>::success(
      {config.executable.string(), {"-s", config.serial, "install", apk_path.string()}});
}

GuestStatus parse_adb_devices_output(std::string_view serial, std::string_view output) {
  std::istringstream lines{std::string(output)};
  std::string line;
  bool found = false;
  std::string device_state;
  while (std::getline(lines, line)) {
    std::istringstream fields{line};
    std::string candidate_serial;
    std::string candidate_state;
    if (!(fields >> candidate_serial >> candidate_state) || candidate_serial != serial) continue;
    if (found) return {GuestState::Unknown, std::string(serial), "ADB listed the selected serial more than once"};
    found = true;
    device_state = std::move(candidate_state);
  }

  if (!found) return {GuestState::NotConnected, std::string(serial), "ADB did not list the selected serial"};
  if (device_state == "device") return {GuestState::AdbOnline, std::string(serial), "ADB transport is online"};
  if (device_state == "offline") return {GuestState::Offline, std::string(serial), "ADB reports the device offline"};
  if (device_state == "unauthorized") {
    return {GuestState::Unauthorized, std::string(serial), "ADB reports the device is unauthorized"};
  }
  return {GuestState::Unknown, std::string(serial), "unrecognized ADB device state: " + device_state};
}

GuestState parse_boot_completed_property(std::string_view output) noexcept {
  const std::string_view value = trim_ascii(output);
  if (value == "1") return GuestState::BootComplete;
  if (value.empty() || value == "0") return GuestState::Booting;
  return GuestState::Unknown;
}

Result<image::Image> decode_screenshot_png(std::string_view png_bytes) {
  const Result<ScreenshotPngInfo> info = inspect_screenshot_png(png_bytes);
  if (info.is_error()) return Result<image::Image>::failure(info.error());
  const Result<image::Image> decoded = image::decode_png(png_bytes, kMaxScreenshotPixels);
  if (decoded.is_error()) return decoded;
  return decoded;
}

Result<PixelPoint> normalized_touch_to_pixels(std::uint32_t width, std::uint32_t height, float x, float y) {
  if (width == 0 || height == 0 || width > kMaxViewportDimension || height > kMaxViewportDimension) {
    return Result<PixelPoint>::failure(
        Error::make("android.invalid_viewport", "viewport dimensions must be between 1 and 16384 pixels"));
  }
  if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0f || y < 0.0f || x > 1.0f || y > 1.0f) {
    return Result<PixelPoint>::failure(
        Error::make("android.invalid_touch_coordinates", "normalized touch coordinates must be finite and in [0, 1]"));
  }
  const auto pixel_x = static_cast<std::uint32_t>(std::round(static_cast<double>(x) * (width - 1)));
  const auto pixel_y = static_cast<std::uint32_t>(std::round(static_cast<double>(y) * (height - 1)));
  return Result<PixelPoint>::success({pixel_x, pixel_y});
}

Result<image::Image> capture_screenshot_rgba(const AdbConfig& config, std::chrono::milliseconds command_timeout) {
  const Status timeout_valid = validate_timeout(command_timeout);
  if (timeout_valid.is_error()) return Result<image::Image>::failure(timeout_valid.error());
  const Result<RuntimeCommand> command = make_adb_screenshot_command(config);
  if (command.is_error()) return Result<image::Image>::failure(command.error());
  const Status consent = require_server_consent(config);
  if (consent.is_error()) return Result<image::Image>::failure(consent.error());
  const Result<CommandOutput> output = run_adb(command.value(), command_timeout, kMaxScreenshotPngBytes);
  if (output.is_error()) return Result<image::Image>::failure(output.error());
  return decode_screenshot_png(output.value().stdout_data);
}

Status send_tap_normalized(const AdbConfig& config, std::uint32_t width, std::uint32_t height, float x, float y,
                           std::chrono::milliseconds command_timeout) {
  const Status timeout_valid = validate_timeout(command_timeout);
  if (timeout_valid.is_error()) return timeout_valid;
  const Result<PixelPoint> point = normalized_touch_to_pixels(width, height, x, y);
  if (point.is_error()) return Status::failure(point.error());
  const Result<RuntimeCommand> command = make_adb_tap_command(config, point.value().x, point.value().y);
  if (command.is_error()) return Status::failure(command.error());
  const Status consent = require_server_consent(config);
  if (consent.is_error()) return consent;
  const Result<CommandOutput> output = run_adb(command.value(), command_timeout);
  return output.status();
}

Status send_swipe_normalized(const AdbConfig& config, std::uint32_t width, std::uint32_t height,
                             float start_x, float start_y, float end_x, float end_y,
                             std::chrono::milliseconds gesture_duration,
                             std::chrono::milliseconds command_timeout) {
  const Status timeout_valid = validate_timeout(command_timeout);
  if (timeout_valid.is_error()) return timeout_valid;
  const Status duration_valid = validate_swipe_duration(gesture_duration);
  if (duration_valid.is_error()) return duration_valid;
  if (command_timeout <= gesture_duration) {
    return Status::failure(Error::make(
        "android.gesture_timeout_too_short", "ADB command timeout must exceed the touch gesture duration"));
  }

  const Result<PixelPoint> start = normalized_touch_to_pixels(width, height, start_x, start_y);
  if (start.is_error()) return Status::failure(start.error());
  const Result<PixelPoint> end = normalized_touch_to_pixels(width, height, end_x, end_y);
  if (end.is_error()) return Status::failure(end.error());
  const Result<RuntimeCommand> command =
      make_adb_swipe_command(config, start.value().x, start.value().y, end.value().x, end.value().y, gesture_duration);
  if (command.is_error()) return Status::failure(command.error());
  const Status consent = require_server_consent(config);
  if (consent.is_error()) return consent;
  const Result<CommandOutput> output = run_adb(command.value(), command_timeout);
  return output.status();
}

Status send_android_key(const AdbConfig& config, std::uint32_t android_key_code,
                       std::chrono::milliseconds command_timeout) {
  const Status timeout_valid = validate_timeout(command_timeout);
  if (timeout_valid.is_error()) return timeout_valid;
  const Result<RuntimeCommand> command = make_adb_key_command(config, android_key_code);
  if (command.is_error()) return Status::failure(command.error());
  const Status consent = require_server_consent(config);
  if (consent.is_error()) return consent;
  const Result<CommandOutput> output = run_adb(command.value(), command_timeout);
  return output.status();
}

Status launch_installed_package(const AdbConfig& config, std::string_view package_name,
                                std::chrono::milliseconds command_timeout) {
  const Status timeout_valid = validate_timeout(command_timeout);
  if (timeout_valid.is_error()) return timeout_valid;
  const Result<RuntimeCommand> command = make_adb_launch_package_command(config, package_name);
  if (command.is_error()) return Status::failure(command.error());
  const Status consent = require_server_consent(config);
  if (consent.is_error()) return consent;
  const Result<CommandOutput> output = run_adb(command.value(), command_timeout);
  return output.status();
}

Status install_selected_apk(const AdbConfig& config, const std::filesystem::path& apk_path,
                            bool user_confirmed_install, std::chrono::milliseconds command_timeout) {
  const Status timeout_valid = validate_timeout(command_timeout);
  if (timeout_valid.is_error()) return timeout_valid;
  const Result<RuntimeCommand> command = make_adb_install_apk_command(config, apk_path);
  if (command.is_error()) return Status::failure(command.error());
  if (!user_confirmed_install) {
    return Status::failure(Error::make(
        "android.apk_install_consent_required", "installing this APK requires explicit per-file user confirmation"));
  }
  const Status adb_consent = require_server_consent(config);
  if (adb_consent.is_error()) return adb_consent;

  std::error_code status_error;
  if (!std::filesystem::is_regular_file(apk_path, status_error) || status_error) {
    return Status::failure(
        Error::make("android.apk_not_regular_file", "selected APK path is not an existing regular file"));
  }
  std::ifstream apk_file{apk_path, std::ios::binary};
  if (!apk_file) {
    return Status::failure(Error::make("android.apk_unreadable", "selected APK file is not readable"));
  }

  const Result<CommandOutput> output = run_adb(command.value(), command_timeout);
  return output.status();
}

Result<GuestStatus> query_guest_status(const AdbConfig& config, std::chrono::milliseconds command_timeout) {
  const Status timeout_valid = validate_timeout(command_timeout);
  if (timeout_valid.is_error()) return Result<GuestStatus>::failure(timeout_valid.error());

  const Result<RuntimeCommand> devices_command = make_adb_devices_command(config);
  if (devices_command.is_error()) return Result<GuestStatus>::failure(devices_command.error());
  const Status consent = require_server_consent(config);
  if (consent.is_error()) return Result<GuestStatus>::failure(consent.error());
  const Result<CommandOutput> devices_output = run_adb(devices_command.value(), command_timeout);
  if (devices_output.is_error()) return Result<GuestStatus>::failure(devices_output.error());

  GuestStatus status = parse_adb_devices_output(config.serial, devices_output.value().stdout_data);
  if (status.state != GuestState::AdbOnline) return Result<GuestStatus>::success(std::move(status));

  const Result<RuntimeCommand> property_command = make_adb_boot_property_command(config);
  if (property_command.is_error()) return Result<GuestStatus>::failure(property_command.error());
  const Result<CommandOutput> property_output = run_adb(property_command.value(), command_timeout);
  if (property_output.is_error()) return Result<GuestStatus>::failure(property_output.error());

  status.state = parse_boot_completed_property(property_output.value().stdout_data);
  if (status.state == GuestState::BootComplete) {
    status.detail = "Android reports sys.boot_completed=1; app/game readiness is not checked";
  } else if (status.state == GuestState::Booting) {
    status.detail = "ADB is online but sys.boot_completed is empty or 0";
  } else {
    status.detail = "ADB returned an unrecognized sys.boot_completed value";
  }
  return Result<GuestStatus>::success(std::move(status));
}

Result<GuestStatus> wait_for_guest_boot(const AdbConfig& config, std::chrono::milliseconds timeout,
                                        std::chrono::milliseconds poll_interval,
                                        std::chrono::milliseconds command_timeout) {
  if (timeout.count() <= 0 || timeout > kMaxGuestBootWaitTimeout) {
    return Result<GuestStatus>::failure(
        Error::make("android.invalid_timeout", "guest boot wait timeout must be between 1 ms and 5 minutes"));
  }
  if (poll_interval.count() <= 0 || poll_interval > kMaxGuestBootPollInterval) {
    return Result<GuestStatus>::failure(
        Error::make("android.invalid_poll_interval", "guest boot poll interval must be between 1 ms and 30 seconds"));
  }
  const Status command_timeout_valid = validate_timeout(command_timeout);
  if (command_timeout_valid.is_error()) return Result<GuestStatus>::failure(command_timeout_valid.error());

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  GuestStatus last_status;
  bool have_last_status = false;
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return guest_boot_timeout(have_last_status ? &last_status : nullptr);

    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    // A guest-state query can use up to two ADB processes (devices + getprop).
    // Halve the remaining budget so both commands fit within the overall wait.
    const auto query_command_timeout = std::min(command_timeout, remaining / 2);
    if (query_command_timeout.count() <= 0) {
      return guest_boot_timeout(have_last_status ? &last_status : nullptr);
    }

    const Result<GuestStatus> current = query_guest_status(config, query_command_timeout);
    if (current.is_error()) return Result<GuestStatus>::failure(current.error());
    last_status = current.value();
    have_last_status = true;
    if (last_status.state == GuestState::BootComplete) return Result<GuestStatus>::success(last_status);

    const auto after_query = std::chrono::steady_clock::now();
    if (after_query >= deadline) return guest_boot_timeout(&last_status);
    const auto sleep_budget = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - after_query);
    if (sleep_budget.count() <= 0) return guest_boot_timeout(&last_status);
    std::this_thread::sleep_for(std::min(poll_interval, sleep_budget));
  }
}

}  // namespace bonk::android
