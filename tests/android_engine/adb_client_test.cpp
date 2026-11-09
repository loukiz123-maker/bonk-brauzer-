#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "bonk/android/adb_client.hpp"
#include "bonk/android/engine_session.hpp"
#include "test_framework.hpp"

using namespace bonk;

namespace {

std::string read_binary(const std::filesystem::path& path) {
  std::ifstream file{path, std::ios::binary};
  if (!file) return {};
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void write_png_u32(std::string& bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned int i = 0; i < 4; ++i) {
    bytes[offset + i] = static_cast<char>((value >> (24 - 8 * i)) & 0xFF);
  }
}

std::uint32_t png_crc32(std::string_view data) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const char byte : data) {
    crc ^= static_cast<std::uint8_t>(byte);
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc ^ 0xFFFFFFFFu;
}

void refresh_fixture_ihdr_crc(std::string& png) { write_png_u32(png, 29, png_crc32(std::string_view(png).substr(12, 17))); }

std::string shell_quote(std::string_view value) {
  std::string quoted;
  quoted.push_back(static_cast<char>(39));
  for (const char character : value) {
    if (character == static_cast<char>(39)) {
      quoted.push_back(static_cast<char>(39));
      quoted.push_back(static_cast<char>(92));
      quoted.push_back(static_cast<char>(39));
      quoted.push_back(static_cast<char>(39));
    } else {
      quoted.push_back(character);
    }
  }
  quoted.push_back(static_cast<char>(39));
  return quoted;
}

class FakeAdb final {
 public:
  FakeAdb() {
    std::string pattern = (std::filesystem::temp_directory_path() / "bonk-fake-adb-XXXXXX").string();
    std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
    mutable_pattern.push_back(0);
    const char* directory = ::mkdtemp(mutable_pattern.data());
    if (directory == nullptr) return;
    directory_ = directory;
    executable_ = directory_ / "adb-fake";
    arguments_file_ = directory_ / "argv.txt";

    const std::filesystem::path fixture = std::filesystem::absolute("tests/fixtures/logo.png");
    std::ofstream script(executable_);
    if (!script) return;
    script << "#!/bin/sh" << std::endl
           << R"(printf '%s ' "$@" > )" << shell_quote(arguments_file_.string()) << std::endl
           << R"(if [ "${3-}" = "exec-out" ]; then exec /bin/cat )" << shell_quote(fixture.string()) << "; fi"
           << std::endl
           << "exit 0" << std::endl;
    script.close();
    if (!script || ::chmod(executable_.c_str(), S_IRUSR | S_IWUSR | S_IXUSR) != 0) return;
    ready_ = true;
  }

  ~FakeAdb() {
    std::error_code error;
    if (!directory_.empty()) std::filesystem::remove_all(directory_, error);
  }

  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] android::AdbConfig config() const { return {executable_, "emulator-5554", true}; }
  [[nodiscard]] std::string arguments() const { return read_binary(arguments_file_); }

 private:
  std::filesystem::path directory_;
  std::filesystem::path executable_;
  std::filesystem::path arguments_file_;
  bool ready_ = false;
};

}  // namespace

BONK_TEST(android_adb, builds_exact_devices_and_boot_property_argv) {
  const android::AdbConfig config{"/opt/android-sdk/platform-tools/adb", "emulator-5554"};
  const Result<android::RuntimeCommand> devices = android::make_adb_devices_command(config);
  REQUIRE(devices.is_ok());
  CHECK_EQ(devices.value().executable, config.executable.string());
  REQUIRE(devices.value().arguments.size() == 2);
  CHECK_EQ(devices.value().arguments[0], std::string("devices"));
  CHECK_EQ(devices.value().arguments[1], std::string("-l"));

  const Result<android::RuntimeCommand> property = android::make_adb_boot_property_command(config);
  REQUIRE(property.is_ok());
  REQUIRE(property.value().arguments.size() == 5);
  CHECK_EQ(property.value().arguments[0], std::string("-s"));
  CHECK_EQ(property.value().arguments[1], config.serial);
  CHECK_EQ(property.value().arguments[2], std::string("shell"));
  CHECK_EQ(property.value().arguments[3], std::string("getprop"));
  CHECK_EQ(property.value().arguments[4], std::string("sys.boot_completed"));
}

BONK_TEST(android_adb, builds_screenshot_tap_and_discrete_key_argv) {
  const android::AdbConfig config{"/opt/android-sdk/platform-tools/adb", "emulator-5554"};

  const Result<android::RuntimeCommand> screenshot = android::make_adb_screenshot_command(config);
  REQUIRE(screenshot.is_ok());
  CHECK_EQ(screenshot.value().arguments.size(), std::size_t{5});
  CHECK_EQ(screenshot.value().arguments[0], std::string("-s"));
  CHECK_EQ(screenshot.value().arguments[1], config.serial);
  CHECK_EQ(screenshot.value().arguments[2], std::string("exec-out"));
  CHECK_EQ(screenshot.value().arguments[3], std::string("screencap"));
  CHECK_EQ(screenshot.value().arguments[4], std::string("-p"));

  const Result<android::RuntimeCommand> tap = android::make_adb_tap_command(config, 123, 456);
  REQUIRE(tap.is_ok());
  CHECK_EQ(tap.value().arguments.size(), std::size_t{7});
  CHECK_EQ(tap.value().arguments[2], std::string("shell"));
  CHECK_EQ(tap.value().arguments[3], std::string("input"));
  CHECK_EQ(tap.value().arguments[4], std::string("tap"));
  CHECK_EQ(tap.value().arguments[5], std::string("123"));
  CHECK_EQ(tap.value().arguments[6], std::string("456"));

  const Result<android::RuntimeCommand> key = android::make_adb_key_command(config, 4);
  REQUIRE(key.is_ok());
  CHECK_EQ(key.value().arguments.size(), std::size_t{6});
  CHECK_EQ(key.value().arguments[4], std::string("keyevent"));
  CHECK_EQ(key.value().arguments[5], std::string("4"));

  const Result<android::RuntimeCommand> launch = android::make_adb_launch_package_command(config, "org.example.game");
  REQUIRE(launch.is_ok());
  CHECK_EQ(launch.value().arguments.size(), std::size_t{12});
  CHECK_EQ(launch.value().arguments[4], std::string("start"));
  CHECK_EQ(launch.value().arguments[7], std::string("android.intent.action.MAIN"));
  CHECK_EQ(launch.value().arguments[9], std::string("android.intent.category.LAUNCHER"));
  CHECK_EQ(launch.value().arguments[11], std::string("org.example.game"));
}

BONK_TEST(android_adb, rejects_relative_executable_invalid_serial_and_out_of_range_key_code) {
  CHECK(android::make_adb_devices_command({"adb", "emulator-5554"}).is_error());
  CHECK(android::make_adb_devices_command({"/sdk/platform-tools/adb", ""}).is_error());
  CHECK(android::make_adb_devices_command({"/sdk/platform-tools/adb", "-s;bad"}).is_error());
  CHECK(android::make_adb_key_command({"/sdk/platform-tools/adb", "emulator-5554"}, 0).is_error());
  CHECK(android::make_adb_key_command({"/sdk/platform-tools/adb", "emulator-5554"}, 65536).is_error());
  CHECK(android::make_adb_launch_package_command({"/sdk/platform-tools/adb", "emulator-5554"}, "com..game").is_error());
  CHECK(android::make_adb_launch_package_command({"/sdk/platform-tools/adb", "emulator-5554"}, "com.game;evil").is_error());
  CHECK(android::make_adb_launch_package_command({"/sdk/platform-tools/adb", "emulator-5554"}, "single").is_error());
}

BONK_TEST(android_adb, identifies_selected_transport_state_from_devices_output) {
  constexpr std::string_view output = R"(List of devices attached
emulator-5554	device product:sdk_gphone64_x86_64 model:Pixel_8
usb-serial-1	offline transport_id:4
unauthorized-serial	unauthorized
)";

  CHECK(android::parse_adb_devices_output("emulator-5554", output).state == android::GuestState::AdbOnline);
  CHECK(android::parse_adb_devices_output("usb-serial-1", output).state == android::GuestState::Offline);
  CHECK(android::parse_adb_devices_output("unauthorized-serial", output).state == android::GuestState::Unauthorized);
  CHECK(android::parse_adb_devices_output("missing-serial", output).state == android::GuestState::NotConnected);
}

BONK_TEST(android_adb, handles_ambiguous_or_unrecognized_device_rows) {
  CHECK(android::parse_adb_devices_output("serial", "serial\tno permissions\n").state ==
        android::GuestState::Unknown);
  CHECK(android::parse_adb_devices_output("serial", "serial\tdevice\nserial\toffline\n").state ==
        android::GuestState::Unknown);
}

BONK_TEST(android_adb, parses_boot_property_without_claiming_app_readiness) {
  CHECK(android::parse_boot_completed_property("1\r\n") == android::GuestState::BootComplete);
  CHECK(android::parse_boot_completed_property("0\n") == android::GuestState::Booting);
  CHECK(android::parse_boot_completed_property("\n") == android::GuestState::Booting);
  CHECK(android::parse_boot_completed_property("unexpected") == android::GuestState::Unknown);
}

BONK_TEST(android_adb, decodes_a_bounded_png_screenshot_and_checks_chunk_checksums) {
  const std::string png = read_binary("tests/fixtures/logo.png");
  REQUIRE(!png.empty());
  const Result<image::Image> decoded = android::decode_screenshot_png(png);
  REQUIRE(decoded.is_ok());
  CHECK_EQ(decoded.value().width, 64);
  CHECK_EQ(decoded.value().height, 64);
  CHECK_EQ(decoded.value().rgba.size(), std::size_t{64 * 64 * 4});

  std::string damaged = png;
  REQUIRE(damaged.size() > 29);
  damaged[29] = static_cast<char>(damaged[29] ^ 1);
  const Result<image::Image> rejected = android::decode_screenshot_png(damaged);
  REQUIRE(rejected.is_error());
  CHECK_EQ(rejected.error().code, std::string("android.invalid_screenshot"));

  std::string too_many_pixels = png;
  write_png_u32(too_many_pixels, 16, 5000);
  write_png_u32(too_many_pixels, 20, 4000);
  refresh_fixture_ihdr_crc(too_many_pixels);
  const Result<image::Image> oversized = android::decode_screenshot_png(too_many_pixels);
  REQUIRE(oversized.is_error());
  CHECK_EQ(oversized.error().code, std::string("android.screenshot_too_large"));
  CHECK(android::decode_screenshot_png("not a PNG").is_error());
}

BONK_TEST(android_adb, maps_finite_normalized_coordinates_to_viewport_pixels) {
  const Result<android::PixelPoint> top_left = android::normalized_touch_to_pixels(640, 480, 0.0f, 0.0f);
  REQUIRE(top_left.is_ok());
  CHECK_EQ(top_left.value().x, std::uint32_t{0});
  CHECK_EQ(top_left.value().y, std::uint32_t{0});

  const Result<android::PixelPoint> center = android::normalized_touch_to_pixels(640, 480, 0.5f, 0.5f);
  REQUIRE(center.is_ok());
  CHECK_EQ(center.value().x, std::uint32_t{320});
  CHECK_EQ(center.value().y, std::uint32_t{240});

  const Result<android::PixelPoint> bottom_right = android::normalized_touch_to_pixels(640, 480, 1.0f, 1.0f);
  REQUIRE(bottom_right.is_ok());
  CHECK_EQ(bottom_right.value().x, std::uint32_t{639});
  CHECK_EQ(bottom_right.value().y, std::uint32_t{479});

  CHECK(android::normalized_touch_to_pixels(0, 480, 0.5f, 0.5f).is_error());
  CHECK(android::normalized_touch_to_pixels(16385, 480, 0.5f, 0.5f).is_error());
  CHECK(android::normalized_touch_to_pixels(640, 480, -0.01f, 0.5f).is_error());
  CHECK(android::normalized_touch_to_pixels(640, 480, 0.5f, std::numeric_limits<float>::infinity()).is_error());
  CHECK(android::normalized_touch_to_pixels(640, 480, std::numeric_limits<float>::quiet_NaN(), 0.5f).is_error());
}

BONK_TEST(android_adb, consent_gates_screenshot_and_input_before_any_process_starts) {
  const android::AdbConfig config{"/definitely/missing/adb", "emulator-5554"};
  const Result<image::Image> screenshot =
      android::capture_screenshot_rgba(config, std::chrono::milliseconds{100});
  REQUIRE(screenshot.is_error());
  CHECK_EQ(screenshot.error().code, std::string("android.adb_server_consent_required"));

  const Status tap = android::send_tap_normalized(config, 640, 480, 0.5f, 0.5f, std::chrono::milliseconds{100});
  REQUIRE(tap.is_error());
  CHECK_EQ(tap.error().code, std::string("android.adb_server_consent_required"));

  const Status key = android::send_android_key(config, 4, std::chrono::milliseconds{100});
  REQUIRE(key.is_error());
  CHECK_EQ(key.error().code, std::string("android.adb_server_consent_required"));

  const Status launch = android::launch_installed_package(config, "org.example.game", std::chrono::milliseconds{100});
  REQUIRE(launch.is_error());
  CHECK_EQ(launch.error().code, std::string("android.adb_server_consent_required"));

  const Result<image::Image> invalid_timeout =
      android::capture_screenshot_rgba(config, std::chrono::seconds{61});
  REQUIRE(invalid_timeout.is_error());
  CHECK_EQ(invalid_timeout.error().code, std::string("android.invalid_timeout"));
}

BONK_TEST(android_adb, fake_adb_exercises_single_capture_tap_and_key_without_an_android_device) {
  FakeAdb fake;
  REQUIRE(fake.ready());
  const android::AdbConfig config = fake.config();

  const Result<image::Image> screenshot = android::capture_screenshot_rgba(config, std::chrono::seconds{1});
  REQUIRE(screenshot.is_ok());
  CHECK_EQ(screenshot.value().width, 64);
  CHECK_EQ(fake.arguments(), std::string("-s emulator-5554 exec-out screencap -p "));

  const Status tap = android::send_tap_normalized(config, 64, 64, 1.0f, 0.0f, std::chrono::seconds{1});
  REQUIRE(tap.is_ok());
  CHECK_EQ(fake.arguments(), std::string("-s emulator-5554 shell input tap 63 0 "));

  const Status key = android::send_android_key(config, 4, std::chrono::seconds{1});
  REQUIRE(key.is_ok());
  CHECK_EQ(fake.arguments(), std::string("-s emulator-5554 shell input keyevent 4 "));

  const Status launch = android::launch_installed_package(config, "org.example.game", std::chrono::seconds{1});
  REQUIRE(launch.is_ok());
  CHECK_EQ(fake.arguments(),
           std::string("-s emulator-5554 shell am start -W -a android.intent.action.MAIN -c "
                        "android.intent.category.LAUNCHER -p org.example.game "));
}

BONK_TEST(android_adb, requires_explicit_consent_before_any_adb_process_is_started) {
  const android::AdbConfig config{"/definitely/missing/adb", "emulator-5554"};
  const Result<android::GuestStatus> result = android::query_guest_status(config, std::chrono::milliseconds{100});
  REQUIRE(result.is_error());
  CHECK_EQ(result.error().code, std::string("android.adb_server_consent_required"));
}

BONK_TEST(android_adb, reports_missing_adb_after_consent_without_faking_a_guest_state) {
  const android::AdbConfig config{"/definitely/missing/adb", "emulator-5554", true};
  const Result<android::GuestStatus> result = android::query_guest_status(config, std::chrono::milliseconds{100});
  REQUIRE(result.is_error());
  CHECK_EQ(result.error().code, std::string("android.spawn_failed"));
}
