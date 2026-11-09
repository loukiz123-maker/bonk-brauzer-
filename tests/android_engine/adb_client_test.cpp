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
  explicit FakeAdb(bool simulate_boot_sequence = false, std::string_view launch_output = "Status: ok",
                   std::string_view install_output = "Success", int exit_code = 0,
                   std::string_view launch_stderr_output = {}, std::string_view install_stderr_output = {},
                   bool include_devices_output = false,
                   std::string_view package_path_output = "package:/data/app/org.example.game/base.apk",
                   std::string_view package_path_stderr_output = {})
      : simulate_boot_sequence_(simulate_boot_sequence) {
    std::string pattern = (std::filesystem::temp_directory_path() / "bonk-fake-adb-XXXXXX").string();
    std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
    mutable_pattern.push_back(0);
    const char* directory = ::mkdtemp(mutable_pattern.data());
    if (directory == nullptr) return;
    directory_ = directory;
    executable_ = directory_ / "adb-fake";
    arguments_file_ = directory_ / "argv.txt";
    boot_query_count_file_ = directory_ / "boot-query-count";

    const std::filesystem::path fixture = std::filesystem::absolute("tests/fixtures/logo.png");
    std::ofstream script(executable_);
    if (!script) return;
    script << "#!/bin/sh" << std::endl
           << R"(printf '%s ' "$@" > )" << shell_quote(arguments_file_.string()) << std::endl;
    if (simulate_boot_sequence_) {
      script << R"(if [ "${1-}" = "devices" ]; then
  printf 'List of devices attached\nemulator-5554\tdevice\n'
elif [ "${4-}" = "getprop" ]; then
  count=0
  if [ -f )"
             << shell_quote(boot_query_count_file_.string())
             << R"( ]; then
    count=$(/bin/cat )"
             << shell_quote(boot_query_count_file_.string()) << ")" << R"(
  fi
  count=$((count + 1))
  printf '%s' "$count" > )"
             << shell_quote(boot_query_count_file_.string())
             << R"(
  if [ "$count" -eq 1 ]; then printf '0\n'; else printf '1\n'; fi
fi
)";
    } else {
      if (include_devices_output) {
        script << R"(if [ "${1-}" = "devices" ]; then
  printf 'List of devices attached\nemulator-5554\tdevice product:sdk_gphone64_x86_64 model:Pixel_8\nusb-serial-1\toffline transport_id:4\nunauthorized-serial\tunauthorized\n'
fi
)";
      }
      script << R"(if [ "${3-}" = "exec-out" ]; then exec /bin/cat )" << shell_quote(fixture.string())
             << R"(; fi)" << std::endl;
    }
    script << R"(if [ "${4-}" = "am" ]; then)" << std::endl;
    if (!launch_output.empty()) {
      script << "  printf '%s\\n' " << shell_quote(launch_output) << std::endl;
    }
    if (!launch_stderr_output.empty()) {
      script << "  printf '%s\\n' " << shell_quote(launch_stderr_output) << " >&2" << std::endl;
    }
    script << "fi" << std::endl;
    script << R"(if [ "${3-}" = "install" ]; then)" << std::endl;
    if (!install_output.empty()) {
      script << "  printf '%s\\n' " << shell_quote(install_output) << std::endl;
    }
    if (!install_stderr_output.empty()) {
      script << "  printf '%s\\n' " << shell_quote(install_stderr_output) << " >&2" << std::endl;
    }
    script << "fi" << std::endl;
    script << R"(if [ "${4-}" = "pm" ] && [ "${5-}" = "path" ]; then)" << std::endl;
    if (!package_path_output.empty()) {
      script << "  printf '%s\\n' " << shell_quote(package_path_output) << std::endl;
    }
    if (!package_path_stderr_output.empty()) {
      script << "  printf '%s\\n' " << shell_quote(package_path_stderr_output) << " >&2" << std::endl;
    }
    script << "fi" << std::endl;
    script << "exit " << exit_code << std::endl;
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
  [[nodiscard]] std::string boot_query_count() const { return read_binary(boot_query_count_file_); }
  [[nodiscard]] std::filesystem::path selected_apk_path() const { return directory_ / "selected-game.apk"; }

 private:
  std::filesystem::path directory_;
  std::filesystem::path executable_;
  std::filesystem::path arguments_file_;
  std::filesystem::path boot_query_count_file_;
  bool simulate_boot_sequence_ = false;
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

  const Result<android::RuntimeCommand> package_path =
      android::make_adb_package_path_command(config, "org.example.game");
  REQUIRE(package_path.is_ok());
  REQUIRE(package_path.value().arguments.size() == 6);
  CHECK_EQ(package_path.value().arguments[0], std::string("-s"));
  CHECK_EQ(package_path.value().arguments[1], config.serial);
  CHECK_EQ(package_path.value().arguments[2], std::string("shell"));
  CHECK_EQ(package_path.value().arguments[3], std::string("pm"));
  CHECK_EQ(package_path.value().arguments[4], std::string("path"));
  CHECK_EQ(package_path.value().arguments[5], std::string("org.example.game"));
}

BONK_TEST(android_adb, builds_a_bounded_straight_line_swipe_argv) {
  const android::AdbConfig config{"/opt/android-sdk/platform-tools/adb", "emulator-5554"};
  const Result<android::RuntimeCommand> swipe =
      android::make_adb_swipe_command(config, 12, 34, 567, 890, std::chrono::milliseconds{750});
  REQUIRE(swipe.is_ok());
  CHECK_EQ(swipe.value().executable, config.executable.string());
  REQUIRE(swipe.value().arguments.size() == 10);
  CHECK_EQ(swipe.value().arguments[0], std::string("-s"));
  CHECK_EQ(swipe.value().arguments[1], config.serial);
  CHECK_EQ(swipe.value().arguments[2], std::string("shell"));
  CHECK_EQ(swipe.value().arguments[3], std::string("input"));
  CHECK_EQ(swipe.value().arguments[4], std::string("swipe"));
  CHECK_EQ(swipe.value().arguments[5], std::string("12"));
  CHECK_EQ(swipe.value().arguments[6], std::string("34"));
  CHECK_EQ(swipe.value().arguments[7], std::string("567"));
  CHECK_EQ(swipe.value().arguments[8], std::string("890"));
  CHECK_EQ(swipe.value().arguments[9], std::string("750"));

  const Result<android::RuntimeCommand> max_duration = android::make_adb_swipe_command(
      config, 0, 0, 1, 1, std::chrono::seconds{30});
  CHECK(max_duration.is_ok());
  CHECK(android::make_adb_swipe_command(config, 0, 0, 1, 1, std::chrono::milliseconds{0}).is_error());
  CHECK(android::make_adb_swipe_command(config, 0, 0, 1, 1, std::chrono::milliseconds{-1}).is_error());
  const Result<android::RuntimeCommand> too_long = android::make_adb_swipe_command(
      config, 0, 0, 1, 1, std::chrono::milliseconds{30001});
  REQUIRE(too_long.is_error());
  CHECK_EQ(too_long.error().code, std::string("android.invalid_gesture_duration"));
}

BONK_TEST(android_adb, builds_install_argv_from_an_explicit_absolute_apk_path) {
  const android::AdbConfig config{"/opt/android-sdk/platform-tools/adb", "emulator-5554"};
  const std::filesystem::path apk_path{"/home/user/downloads/my-game.apk"};
  const Result<android::RuntimeCommand> install = android::make_adb_install_apk_command(config, apk_path);
  REQUIRE(install.is_ok());
  CHECK_EQ(install.value().executable, config.executable.string());
  REQUIRE(install.value().arguments.size() == 4);
  CHECK_EQ(install.value().arguments[0], std::string("-s"));
  CHECK_EQ(install.value().arguments[1], config.serial);
  CHECK_EQ(install.value().arguments[2], std::string("install"));
  CHECK_EQ(install.value().arguments[3], apk_path.string());

  CHECK(android::make_adb_install_apk_command(config, "relative-game.apk").is_error());
}

BONK_TEST(android_adb, rejects_relative_executable_invalid_serial_and_out_of_range_key_code) {
  CHECK(android::make_adb_devices_command({"adb", "emulator-5554"}).is_error());
  CHECK(android::make_adb_devices_command({"/sdk/platform-tools/adb", ""}).is_ok());
  CHECK(android::make_adb_boot_property_command({"/sdk/platform-tools/adb", ""}).is_error());
  CHECK(android::make_adb_boot_property_command({"/sdk/platform-tools/adb", "-s;bad"}).is_error());
  CHECK(android::make_adb_key_command({"/sdk/platform-tools/adb", "emulator-5554"}, 0).is_error());
  CHECK(android::make_adb_key_command({"/sdk/platform-tools/adb", "emulator-5554"}, 65536).is_error());
  CHECK(android::make_adb_launch_package_command({"/sdk/platform-tools/adb", "emulator-5554"}, "com..game").is_error());
  CHECK(android::make_adb_launch_package_command({"/sdk/platform-tools/adb", "emulator-5554"}, "com.game;evil").is_error());
  CHECK(android::make_adb_launch_package_command({"/sdk/platform-tools/adb", "emulator-5554"}, "single").is_error());
  CHECK(android::make_adb_package_path_command({"/sdk/platform-tools/adb", "emulator-5554"}, "com.game;evil").is_error());
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

BONK_TEST(android_adb, parses_full_devices_list_for_explicit_selection) {
  constexpr std::string_view output =
      "List of devices attached\n"
      "emulator-5554\tdevice product:sdk_gphone64_x86_64 model:Pixel_8\n"
      "usb-serial-1\toffline transport_id:4\n"
      "unauthorized-serial\tunauthorized\n"
      "permission-serial\tno permissions (user in plugdev group; are your udev rules wrong?)\n";
  const std::vector<android::GuestStatus> devices = android::parse_adb_devices_list_output(output);
  REQUIRE(devices.size() == 4);
  CHECK_EQ(devices[0].serial, std::string("emulator-5554"));
  CHECK(devices[0].state == android::GuestState::AdbOnline);
  CHECK(devices[1].state == android::GuestState::Offline);
  CHECK(devices[2].state == android::GuestState::Unauthorized);
  CHECK(devices[3].state == android::GuestState::Unknown);

  CHECK(android::parse_adb_devices_list_output("List of devices attached\n").empty());
  const std::vector<android::GuestStatus> duplicate = android::parse_adb_devices_list_output(
      "List of devices attached\nserial-1\tdevice\nserial-1\toffline\n");
  REQUIRE(duplicate.size() == 1);
  CHECK(duplicate[0].state == android::GuestState::Unknown);
  CHECK(duplicate[0].detail.find("more than once") != std::string::npos);
}

BONK_TEST(android_adb, handles_ambiguous_or_unrecognized_device_rows) {
  CHECK(android::parse_adb_devices_output("serial", "serial\tno permissions\n").state ==
        android::GuestState::Unknown);
  CHECK(android::parse_adb_devices_output("serial", "serial\tdevice\nserial\toffline\n").state ==
        android::GuestState::Unknown);
}

BONK_TEST(android_adb, parses_bounded_package_paths_and_reports_missing_packages) {
  const Result<std::vector<std::string>> paths = android::parse_adb_package_paths_output(
      "package:/data/app/org.example.game/base.apk\n"
      "package:/data/app/org.example.game/split_config.arm64.apk\n");
  REQUIRE(paths.is_ok());
  REQUIRE(paths.value().size() == 2);
  CHECK_EQ(paths.value()[0], std::string("/data/app/org.example.game/base.apk"));
  CHECK_EQ(paths.value()[1], std::string("/data/app/org.example.game/split_config.arm64.apk"));

  const Result<std::vector<std::string>> empty = android::parse_adb_package_paths_output("\n");
  REQUIRE(empty.is_ok());
  CHECK(empty.value().empty());

  const Result<std::vector<std::string>> missing =
      android::parse_adb_package_paths_output("Error: package not found\n");
  REQUIRE(missing.is_error());
  CHECK_EQ(missing.error().code, std::string("android.package_not_installed"));
  CHECK(android::parse_adb_package_paths_output("package:relative/base.apk\n").is_error());
  CHECK(android::parse_adb_package_paths_output("unexpected pm output\n").is_error());
  CHECK(android::parse_adb_package_paths_output(std::string(64 * 1024 + 1, 'x')).is_error());
}

BONK_TEST(android_adb, parses_boot_property_without_claiming_app_readiness) {
  CHECK(android::parse_boot_completed_property("1\r\n") == android::GuestState::BootComplete);
  CHECK(android::parse_boot_completed_property("0\n") == android::GuestState::Booting);
  CHECK(android::parse_boot_completed_property("\n") == android::GuestState::Booting);
  CHECK(android::parse_boot_completed_property("unexpected") == android::GuestState::Unknown);
}

BONK_TEST(android_adb, waits_until_guest_boot_property_completes) {
  FakeAdb fake{true};
  REQUIRE(fake.ready());

  const Result<android::GuestStatus> status = android::wait_for_guest_boot(
      fake.config(), std::chrono::seconds{1}, std::chrono::milliseconds{5}, std::chrono::milliseconds{250});
  REQUIRE(status.is_ok());
  CHECK(status.value().state == android::GuestState::BootComplete);
  CHECK_EQ(fake.boot_query_count(), std::string("2"));
  CHECK_EQ(fake.arguments(), std::string("-s emulator-5554 shell getprop sys.boot_completed "));

  const Result<android::GuestStatus> invalid_timeout = android::wait_for_guest_boot(
      fake.config(), std::chrono::milliseconds{0}, std::chrono::milliseconds{5}, std::chrono::milliseconds{250});
  REQUIRE(invalid_timeout.is_error());
  CHECK_EQ(invalid_timeout.error().code, std::string("android.invalid_timeout"));
  const Result<android::GuestStatus> invalid_poll = android::wait_for_guest_boot(
      fake.config(), std::chrono::seconds{1}, std::chrono::milliseconds{0}, std::chrono::milliseconds{250});
  REQUIRE(invalid_poll.is_error());
  CHECK_EQ(invalid_poll.error().code, std::string("android.invalid_poll_interval"));
}

BONK_TEST(android_adb, times_out_with_last_guest_state_when_boot_does_not_complete) {
  FakeAdb fake;
  REQUIRE(fake.ready());

  const Result<android::GuestStatus> status = android::wait_for_guest_boot(
      fake.config(), std::chrono::milliseconds{2000}, std::chrono::milliseconds{5}, std::chrono::milliseconds{2000});
  REQUIRE(status.is_error());
  CHECK_EQ(status.error().code, std::string("android.guest_boot_timeout"));
  CHECK(status.error().message.find("not-connected") != std::string::npos);
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

BONK_TEST(android_adb, sends_normalized_swipe_and_rejects_a_too_short_timeout) {
  FakeAdb fake;
  REQUIRE(fake.ready());
  const android::AdbConfig config = fake.config();

  const Status swipe = android::send_swipe_normalized(config, 640, 480, 0.0f, 0.0f, 1.0f, 1.0f,
                                                       std::chrono::milliseconds{750}, std::chrono::seconds{2});
  REQUIRE(swipe.is_ok());
  CHECK_EQ(fake.arguments(),
           std::string("-s emulator-5554 shell input swipe 0 0 639 479 750 "));

  const Status too_short = android::send_swipe_normalized(
      config, 640, 480, 0.0f, 0.0f, 0.5f, 0.5f, std::chrono::milliseconds{500}, std::chrono::milliseconds{500});
  REQUIRE(too_short.is_error());
  CHECK_EQ(too_short.error().code, std::string("android.gesture_timeout_too_short"));
  CHECK_EQ(fake.arguments(), std::string("-s emulator-5554 shell input swipe 0 0 639 479 750 "));

  const Status invalid_duration = android::send_swipe_normalized(
      config, 640, 480, 0.0f, 0.0f, 0.5f, 0.5f, std::chrono::milliseconds{0}, std::chrono::seconds{1});
  REQUIRE(invalid_duration.is_error());
  CHECK_EQ(invalid_duration.error().code, std::string("android.invalid_gesture_duration"));
}

BONK_TEST(android_adb, queries_guest_package_paths_with_consent_and_reports_absence) {
  FakeAdb fake;
  REQUIRE(fake.ready());
  const android::AdbConfig config = fake.config();

  const android::AdbConfig no_consent{config.executable, config.serial, false};
  const Result<std::vector<std::string>> blocked =
      android::query_installed_package_paths(no_consent, "org.example.game", std::chrono::seconds{1});
  REQUIRE(blocked.is_error());
  CHECK_EQ(blocked.error().code, std::string("android.adb_server_consent_required"));
  CHECK_EQ(fake.arguments(), std::string{});

  const Result<std::vector<std::string>> paths =
      android::query_installed_package_paths(config, "org.example.game", std::chrono::seconds{1});
  REQUIRE(paths.is_ok());
  REQUIRE(paths.value().size() == 1);
  CHECK_EQ(paths.value()[0], std::string("/data/app/org.example.game/base.apk"));
  CHECK_EQ(fake.arguments(), std::string("-s emulator-5554 shell pm path org.example.game "));

  FakeAdb missing{false, "Status: ok", "Success", 1, {}, {}, false, "Error: package not found"};
  REQUIRE(missing.ready());
  const Result<std::vector<std::string>> not_installed = android::query_installed_package_paths(
      missing.config(), "org.example.game", std::chrono::seconds{1});
  REQUIRE(not_installed.is_error());
  CHECK_EQ(not_installed.error().code, std::string("android.package_not_installed"));
  CHECK(not_installed.error().message.find("package not found") != std::string::npos);

  FakeAdb missing_stderr{false, "Status: ok", "Success", 1, {}, {}, false, {}, "Error: package not found"};
  REQUIRE(missing_stderr.ready());
  const Result<std::vector<std::string>> not_installed_stderr = android::query_installed_package_paths(
      missing_stderr.config(), "org.example.game", std::chrono::seconds{1});
  REQUIRE(not_installed_stderr.is_error());
  CHECK_EQ(not_installed_stderr.error().code, std::string("android.package_not_installed"));
}

BONK_TEST(android_adb, installs_only_a_confirmed_readable_apk_with_both_consents) {
  FakeAdb fake;
  REQUIRE(fake.ready());
  const std::filesystem::path apk_path = fake.selected_apk_path();
  std::ofstream apk{apk_path, std::ios::binary};
  REQUIRE(static_cast<bool>(apk));
  apk << "synthetic APK test payload";
  apk.close();
  REQUIRE(static_cast<bool>(apk));

  const android::AdbConfig config = fake.config();
  const Status unconfirmed = android::install_selected_apk(config, apk_path, false, std::chrono::seconds{1});
  REQUIRE(unconfirmed.is_error());
  CHECK_EQ(unconfirmed.error().code, std::string("android.apk_install_consent_required"));
  CHECK_EQ(fake.arguments(), std::string{});

  const android::AdbConfig without_server_consent{config.executable, config.serial, false};
  const Status no_adb_consent =
      android::install_selected_apk(without_server_consent, apk_path, true, std::chrono::seconds{1});
  REQUIRE(no_adb_consent.is_error());
  CHECK_EQ(no_adb_consent.error().code, std::string("android.adb_server_consent_required"));
  CHECK_EQ(fake.arguments(), std::string{});

  const Status missing = android::install_selected_apk(
      config, apk_path.parent_path() / "missing.apk", true, std::chrono::seconds{1});
  REQUIRE(missing.is_error());
  CHECK_EQ(missing.error().code, std::string("android.apk_not_regular_file"));
  CHECK_EQ(fake.arguments(), std::string{});

  const Status installed = android::install_selected_apk(config, apk_path, true, std::chrono::seconds{1});
  REQUIRE(installed.is_ok());
  CHECK_EQ(fake.arguments(), "-s emulator-5554 install " + apk_path.string() + " ");
}

BONK_TEST(android_adb, reports_explicit_apk_install_failure_even_when_adb_exits_zero) {
  FakeAdb fake{false, "Status: ok", "Failure [INSTALL_FAILED_INVALID_APK]"};
  REQUIRE(fake.ready());
  const std::filesystem::path apk_path = fake.selected_apk_path();
  std::ofstream apk{apk_path, std::ios::binary};
  REQUIRE(static_cast<bool>(apk));
  apk << "synthetic APK test payload";
  apk.close();
  REQUIRE(static_cast<bool>(apk));

  const Status install = android::install_selected_apk(fake.config(), apk_path, true, std::chrono::seconds{1});
  REQUIRE(install.is_error());
  CHECK_EQ(install.error().code, std::string("android.apk_install_reported_failure"));
  CHECK(install.error().message.find("INSTALL_FAILED_INVALID_APK") != std::string::npos);
}

BONK_TEST(android_adb, reports_explicit_install_and_launch_failures_before_nonzero_exit_errors) {
  FakeAdb install_stdout{false, "Status: ok", "Failure [INSTALL_FAILED_INVALID_APK]", 23};
  REQUIRE(install_stdout.ready());
  const std::filesystem::path stdout_apk_path = install_stdout.selected_apk_path();
  std::ofstream stdout_apk{stdout_apk_path, std::ios::binary};
  REQUIRE(static_cast<bool>(stdout_apk));
  stdout_apk << "synthetic APK test payload";
  stdout_apk.close();
  REQUIRE(static_cast<bool>(stdout_apk));

  const Status stdout_install = android::install_selected_apk(
      install_stdout.config(), stdout_apk_path, true, std::chrono::seconds{1});
  REQUIRE(stdout_install.is_error());
  CHECK_EQ(stdout_install.error().code, std::string("android.apk_install_reported_failure"));
  CHECK(stdout_install.error().message.find("INSTALL_FAILED_INVALID_APK") != std::string::npos);

  FakeAdb install_stderr{false, "Status: ok", "Success", 23, {}, "Failure [INSTALL_FAILED_VERSION_DOWNGRADE]"};
  REQUIRE(install_stderr.ready());
  const std::filesystem::path stderr_apk_path = install_stderr.selected_apk_path();
  std::ofstream stderr_apk{stderr_apk_path, std::ios::binary};
  REQUIRE(static_cast<bool>(stderr_apk));
  stderr_apk << "synthetic APK test payload";
  stderr_apk.close();
  REQUIRE(static_cast<bool>(stderr_apk));

  const Status stderr_install = android::install_selected_apk(
      install_stderr.config(), stderr_apk_path, true, std::chrono::seconds{1});
  REQUIRE(stderr_install.is_error());
  CHECK_EQ(stderr_install.error().code, std::string("android.apk_install_reported_failure"));
  CHECK(stderr_install.error().message.find("INSTALL_FAILED_VERSION_DOWNGRADE") != std::string::npos);

  FakeAdb launch_stdout{false, "Error: Activity not started, unable to resolve Intent", "Success", 23};
  REQUIRE(launch_stdout.ready());
  const Status stdout_launch = android::launch_installed_package(
      launch_stdout.config(), "org.example.game", std::chrono::seconds{1});
  REQUIRE(stdout_launch.is_error());
  CHECK_EQ(stdout_launch.error().code, std::string("android.app_launch_reported_failure"));
  CHECK(stdout_launch.error().message.find("unable to resolve Intent") != std::string::npos);

  FakeAdb launch_stderr{false, "Status: ok", "Success", 23, "Error: Activity not started, permission denied"};
  REQUIRE(launch_stderr.ready());
  const Status stderr_launch = android::launch_installed_package(
      launch_stderr.config(), "org.example.game", std::chrono::seconds{1});
  REQUIRE(stderr_launch.is_error());
  CHECK_EQ(stderr_launch.error().code, std::string("android.app_launch_reported_failure"));
  CHECK(stderr_launch.error().message.find("permission denied") != std::string::npos);

  FakeAdb unexplained_exit{false, "Status: ok", "Success", 23};
  REQUIRE(unexplained_exit.ready());
  const Status generic_failure = android::launch_installed_package(
      unexplained_exit.config(), "org.example.game", std::chrono::seconds{1});
  REQUIRE(generic_failure.is_error());
  CHECK_EQ(generic_failure.error().code, std::string("android.adb_command_failed"));
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

  const Status swipe = android::send_swipe_normalized(
      config, 640, 480, 0.1f, 0.1f, 0.9f, 0.9f, std::chrono::milliseconds{200}, std::chrono::seconds{1});
  REQUIRE(swipe.is_error());
  CHECK_EQ(swipe.error().code, std::string("android.adb_server_consent_required"));

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

BONK_TEST(android_adb, reports_explicit_am_start_failures_even_when_adb_exits_zero) {
  FakeAdb timeout_adb{false, "Status: timeout"};
  REQUIRE(timeout_adb.ready());
  const Status timeout = android::launch_installed_package(
      timeout_adb.config(), "org.example.game", std::chrono::seconds{1});
  REQUIRE(timeout.is_error());
  CHECK_EQ(timeout.error().code, std::string("android.app_launch_reported_failure"));
  CHECK(timeout.error().message.find("Status: timeout") != std::string::npos);

  FakeAdb unresolved_adb{false, "Error: Activity not started, unable to resolve Intent"};
  REQUIRE(unresolved_adb.ready());
  const Status unresolved = android::launch_installed_package(
      unresolved_adb.config(), "org.example.game", std::chrono::seconds{1});
  REQUIRE(unresolved.is_error());
  CHECK_EQ(unresolved.error().code, std::string("android.app_launch_reported_failure"));
  CHECK(unresolved.error().message.find("unable to resolve Intent") != std::string::npos);
}

BONK_TEST(android_adb, lists_transports_without_a_selected_serial) {
  FakeAdb fake{false, "Status: ok", "Success", 0, {}, {}, true};
  REQUIRE(fake.ready());
  android::AdbConfig config = fake.config();
  config.serial.clear();

  const Result<std::vector<android::GuestStatus>> devices =
      android::list_adb_devices(config, std::chrono::seconds{1});
  REQUIRE(devices.is_ok());
  REQUIRE(devices.value().size() == 3);
  CHECK_EQ(devices.value()[0].serial, std::string("emulator-5554"));
  CHECK(devices.value()[0].state == android::GuestState::AdbOnline);
  CHECK(devices.value()[1].state == android::GuestState::Offline);
  CHECK(devices.value()[2].state == android::GuestState::Unauthorized);
  CHECK_EQ(fake.arguments(), std::string("devices -l "));

  const android::AdbConfig no_consent{config.executable, "", false};
  const Result<std::vector<android::GuestStatus>> blocked =
      android::list_adb_devices(no_consent, std::chrono::seconds{1});
  REQUIRE(blocked.is_error());
  CHECK_EQ(blocked.error().code, std::string("android.adb_server_consent_required"));
  CHECK_EQ(fake.arguments(), std::string("devices -l "));
}

BONK_TEST(android_adb, requires_explicit_consent_before_any_adb_process_is_started) {
  const android::AdbConfig config{"/definitely/missing/adb", "emulator-5554"};
  const Result<android::GuestStatus> result = android::query_guest_status(config, std::chrono::milliseconds{100});
  REQUIRE(result.is_error());
  CHECK_EQ(result.error().code, std::string("android.adb_server_consent_required"));

  const Result<android::GuestStatus> waited = android::wait_for_guest_boot(
      config, std::chrono::seconds{1}, std::chrono::milliseconds{100}, std::chrono::milliseconds{100});
  REQUIRE(waited.is_error());
  CHECK_EQ(waited.error().code, std::string("android.adb_server_consent_required"));

  const android::AdbConfig invalid_serial{"/definitely/missing/adb", "bad;serial", true};
  const Result<android::GuestStatus> invalid = android::query_guest_status(invalid_serial, std::chrono::milliseconds{100});
  REQUIRE(invalid.is_error());
  CHECK_EQ(invalid.error().code, std::string("android.invalid_adb_config"));
}

BONK_TEST(android_adb, reports_missing_adb_after_consent_without_faking_a_guest_state) {
  const android::AdbConfig config{"/definitely/missing/adb", "emulator-5554", true};
  const Result<android::GuestStatus> result = android::query_guest_status(config, std::chrono::milliseconds{100});
  REQUIRE(result.is_error());
  CHECK_EQ(result.error().code, std::string("android.spawn_failed"));
}
