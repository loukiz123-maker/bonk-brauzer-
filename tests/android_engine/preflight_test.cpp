#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include <unistd.h>

#include "bonk/android/preflight.hpp"
#include "test_framework.hpp"

using namespace bonk;
namespace fs = std::filesystem;

namespace {

class TempDirectory {
 public:
  TempDirectory() {
    std::error_code error;
    const fs::path base = fs::temp_directory_path(error);
    if (error) return;

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ = base / ("bonk-android-preflight-" + std::to_string(::getpid()) + "-" + std::to_string(stamp) + "-" +
                      std::to_string(attempt));
      if (fs::create_directory(path_, error)) {
        valid_ = true;
        return;
      }
      if (error && error != std::errc::file_exists) return;
      error.clear();
    }
  }

  ~TempDirectory() {
    if (!valid_) return;
    std::error_code error;
    (void)fs::remove_all(path_, error);
  }

  [[nodiscard]] bool valid() const noexcept { return valid_; }
  [[nodiscard]] const fs::path& path() const noexcept { return path_; }

 private:
  fs::path path_;
  bool valid_ = false;
};

bool write_text(const fs::path& path, std::string_view text) {
  std::ofstream output(path, std::ios::binary);
  if (!output) return false;
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  return static_cast<bool>(output);
}

struct AvdFixture {
  fs::path sdk_root;
  fs::path avd_home;
  fs::path avd_data;
  fs::path system_image;
  std::string avd_name = "Pixel_API_35";
};

bool create_avd_fixture(const fs::path& root, AvdFixture& fixture, std::string_view image_sysdir) {
  fixture.sdk_root = root / "sdk";
  fixture.avd_home = root / "avd-home";
  fixture.avd_data = fixture.avd_home / (fixture.avd_name + ".avd");
  const fs::path image_directory = fixture.sdk_root / std::string(image_sysdir);
  fixture.system_image = image_directory / "system.img";

  std::error_code error;
  fs::create_directories(fixture.avd_data, error);
  if (error) return false;
  fs::create_directories(image_directory, error);
  if (error) return false;

  // These tiny files exercise path/INI checks only. They are not Android images
  // and are never passed to an emulator in this test.
  const std::string avd_ini = "path=" + fixture.avd_data.string() + "\n";
  const std::string config_ini = "image.sysdir.1=" + std::string(image_sysdir) + "\n";
  return write_text(fixture.avd_home / (fixture.avd_name + ".ini"), avd_ini) &&
         write_text(fixture.avd_data / "config.ini", config_ini) &&
         write_text(fixture.system_image, "synthetic unit-test placeholder; not an Android system image");
}

android::RuntimePreflightRequest make_request(const AvdFixture& fixture, const fs::path& missing_kvm) {
  android::RuntimePreflightRequest request;
  request.emulator_executable = "/bin/true";  // inspected, never executed by preflight
  request.adb_executable = "/bin/true";       // inspected, never executed by preflight
  request.android_sdk_root = fixture.sdk_root;
  request.avd_home = fixture.avd_home;
  request.avd_name = fixture.avd_name;
  request.kvm_device = missing_kvm;
  return request;
}

}  // namespace

BONK_TEST(android_preflight, missing_paths_are_reported_without_starting_or_installing_anything) {
  android::RuntimePreflightRequest request;
  request.emulator_executable = "/definitely/missing/android-sdk/emulator/emulator";
  request.adb_executable = "/definitely/missing/android-sdk/platform-tools/adb";
  request.android_sdk_root = "/definitely/missing/android-sdk";
  request.avd_home = "/definitely/missing/avd-home";
  request.avd_name = "Pixel_API_35";
  request.kvm_device = "/definitely/missing/kvm";

  const android::RuntimePreflightReport report = android::preflight_runtime(request);
  CHECK(report.emulator.state == android::ProbeState::Missing);
  CHECK(report.adb.state == android::ProbeState::Missing);
  CHECK(report.android_sdk_root.state == android::ProbeState::Missing);
  CHECK(report.avd_home.state == android::ProbeState::Missing);
  CHECK(report.kvm_device.state == android::ProbeState::Missing);
  CHECK(!report.static_inputs_present());
  CHECK(!report.guest_control_tool_available());
  CHECK(!report.kvm_device_accessible());
}

BONK_TEST(android_preflight, reports_selected_avd_files_but_does_not_claim_android_is_ready) {
  TempDirectory temp;
  REQUIRE(temp.valid());
  AvdFixture fixture;
  REQUIRE(create_avd_fixture(temp.path(), fixture, "system-images/android-35/google_apis/x86_64"));

  const android::RuntimePreflightReport report =
      android::preflight_runtime(make_request(fixture, temp.path() / "missing-kvm"));
  CHECK(report.emulator.available());
  CHECK(report.adb.available());
  CHECK(report.android_sdk_root.available());
  CHECK(report.avd_home.available());
  CHECK(report.avd_definition.available());
  CHECK(report.avd_data.available());
  CHECK(report.avd_config.available());
  CHECK(report.system_image.available());
  CHECK(report.static_inputs_present());
  CHECK(report.guest_control_tool_available());
  CHECK(!report.kvm_device_accessible());
  CHECK(report.system_image.message.find("bootability were not checked") != std::string::npos);
}

BONK_TEST(android_preflight, rejects_system_image_paths_outside_the_selected_sdk_root) {
  TempDirectory temp;
  REQUIRE(temp.valid());
  AvdFixture fixture;
  REQUIRE(create_avd_fixture(temp.path(), fixture, "../../outside-image"));

  const android::RuntimePreflightReport report =
      android::preflight_runtime(make_request(fixture, temp.path() / "missing-kvm"));
  CHECK(report.avd_config.available());
  CHECK(report.system_image.state == android::ProbeState::Invalid);
  CHECK(report.system_image.message.find("outside the selected SDK root") != std::string::npos);
  CHECK(!report.static_inputs_present());
}

BONK_TEST(android_preflight, rejects_relative_paths) {
  android::RuntimePreflightRequest request;
  request.emulator_executable = "emulator";
  request.adb_executable = "adb";
  request.android_sdk_root = "sdk";
  request.avd_home = "avd";
  request.avd_name = "Pixel_API_35";
  request.kvm_device = "dev/kvm";

  const android::RuntimePreflightReport report = android::preflight_runtime(request);
  CHECK(report.emulator.state == android::ProbeState::Invalid);
  CHECK(report.adb.state == android::ProbeState::Invalid);
  CHECK(report.android_sdk_root.state == android::ProbeState::Invalid);
  CHECK(report.avd_home.state == android::ProbeState::Invalid);
  CHECK(report.kvm_device.state == android::ProbeState::Invalid);
}

BONK_TEST(android_preflight, rejects_invalid_avd_names_before_resolving_a_definition) {
  TempDirectory temp;
  REQUIRE(temp.valid());

  android::RuntimePreflightRequest request;
  request.emulator_executable = "/bin/true";
  request.adb_executable = "/bin/true";
  request.android_sdk_root = temp.path();
  request.avd_home = temp.path();
  request.avd_name = "../Pixel";
  request.kvm_device = temp.path() / "missing-kvm";

  const android::RuntimePreflightReport report = android::preflight_runtime(request);
  CHECK(report.avd_definition.state == android::ProbeState::Invalid);
  CHECK(!report.static_inputs_present());
}
