#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "bonk/android/input_action.hpp"
#include "test_framework.hpp"

using namespace bonk;
using namespace bonk::android;

namespace {

InputViewport viewport() { return InputViewport{1080, 1920}; }

AdbConfig adb_config() {
  AdbConfig config;
  config.executable = "/usr/bin/adb";
  config.serial = "192.168.240.112:5555";
  config.allow_adb_server_autostart = true;
  return config;
}

std::string join_arguments(const RuntimeCommand& command) {
  std::string joined;
  for (const std::string& argument : command.arguments) {
    if (!joined.empty()) joined.push_back(' ');
    joined += argument;
  }
  return joined;
}

}  // namespace

BONK_TEST(input_action, tap_is_built_from_a_captured_viewport) {
  const Result<InputAction> tap = make_tap(viewport(), 0.5f, 0.25f);
  REQUIRE(tap.is_ok());

  const Result<RuntimeCommand> command = build_input_command(adb_config(), tap.value());
  REQUIRE(command.is_ok());
  CHECK_EQ(join_arguments(command.value()),
           std::string("-s 192.168.240.112:5555 shell input tap 540 480"));
}

BONK_TEST(input_action, tap_on_a_missing_viewport_is_rejected) {
  const Result<InputAction> tap = make_tap(InputViewport{0, 0}, 0.5f, 0.5f);
  REQUIRE(tap.is_error());
  CHECK_EQ(tap.error().code, std::string("android.invalid_viewport"));

  // Half-specified viewports are just as unusable.
  CHECK(make_tap(InputViewport{1080, 0}, 0.5f, 0.5f).is_error());
  CHECK(make_tap(InputViewport{0, 1920}, 0.5f, 0.5f).is_error());
}

BONK_TEST(input_action, coordinates_outside_the_unit_square_are_rejected) {
  for (const float bad : {-0.01f, 1.01f, std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity()}) {
    CHECK(make_tap(viewport(), bad, 0.5f).is_error());
    CHECK(make_tap(viewport(), 0.5f, bad).is_error());
  }
  const Result<InputAction> edge = make_tap(viewport(), 1.0f, 0.0f);
  REQUIRE(edge.is_ok());
}

BONK_TEST(input_action, swipe_duration_is_bounded) {
  const Result<InputAction> zero = make_swipe(viewport(), 0.1f, 0.1f, 0.9f, 0.9f, std::chrono::milliseconds{0});
  REQUIRE(zero.is_error());
  CHECK_EQ(zero.error().code, std::string("android.invalid_gesture_duration"));

  const Result<InputAction> too_long =
      make_swipe(viewport(), 0.1f, 0.1f, 0.9f, 0.9f, std::chrono::milliseconds{30001});
  REQUIRE(too_long.is_error());
  CHECK_EQ(too_long.error().code, std::string("android.invalid_gesture_duration"));

  const Result<InputAction> ok = make_swipe(viewport(), 0.1f, 0.1f, 0.9f, 0.9f, std::chrono::milliseconds{300});
  REQUIRE(ok.is_ok());
  const Result<RuntimeCommand> command = build_input_command(adb_config(), ok.value());
  REQUIRE(command.is_ok());
  CHECK_EQ(join_arguments(command.value()),
           std::string("-s 192.168.240.112:5555 shell input swipe 108 192 971 1727 300"));
}

BONK_TEST(input_action, key_codes_are_range_checked) {
  CHECK(make_key(0).is_error());
  CHECK(make_key(65536).is_error());
  const Result<InputAction> key = make_key(4);
  REQUIRE(key.is_ok());
  const Result<RuntimeCommand> command = build_input_command(adb_config(), key.value());
  REQUIRE(command.is_ok());
  CHECK_EQ(join_arguments(command.value()), std::string("-s 192.168.240.112:5555 shell input keyevent 4"));
}

BONK_TEST(input_action, command_builder_refuses_an_invalid_action) {
  InputAction action;  // Tap kind with a zero viewport
  const Result<RuntimeCommand> command = build_input_command(adb_config(), action);
  REQUIRE(command.is_error());
  CHECK_EQ(command.error().code, std::string("android.invalid_viewport"));
}

BONK_TEST(input_action, viewport_resolution_changes_move_the_same_normalized_tap) {
  const Result<InputAction> tap = make_tap(viewport(), 0.5f, 0.5f);
  REQUIRE(tap.is_ok());

  const Result<RuntimeCommand> portrait = build_input_command(adb_config(), tap.value());
  REQUIRE(portrait.is_ok());
  CHECK_EQ(join_arguments(portrait.value()), std::string("-s 192.168.240.112:5555 shell input tap 540 960"));

  // Re-targeting the same normalized tap at a rotated viewport needs a new
  // action: the action carries the viewport it was captured against.
  InputAction rotated = tap.value();
  rotated.viewport = InputViewport{1920, 1080};
  const Result<RuntimeCommand> landscape = build_input_command(adb_config(), rotated);
  REQUIRE(landscape.is_ok());
  CHECK_EQ(join_arguments(landscape.value()), std::string("-s 192.168.240.112:5555 shell input tap 960 540"));
}

BONK_TEST(input_action, oversized_viewport_is_rejected) {
  CHECK(make_tap(InputViewport{20000, 1080}, 0.5f, 0.5f).is_error());
  CHECK(make_tap(InputViewport{1080, 20000}, 0.5f, 0.5f).is_error());
}

BONK_TEST(input_action, kind_names_are_stable) {
  CHECK_EQ(to_string(InputActionKind::Tap), std::string_view("tap"));
  CHECK_EQ(to_string(InputActionKind::Swipe), std::string_view("swipe"));
  CHECK_EQ(to_string(InputActionKind::Key), std::string_view("key"));
}

namespace {

class FakeInputAdb final {
 public:
  FakeInputAdb() {
    std::string pattern = (std::filesystem::temp_directory_path() / "bonk-input-adb-XXXXXX").string();
    std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
    mutable_pattern.push_back('\0');
    const char* directory = ::mkdtemp(mutable_pattern.data());
    if (directory == nullptr) return;
    directory_ = directory;
    executable_ = directory_ / "adb-fake";
    log_ = directory_ / "argv.txt";
    std::ofstream script(executable_);
    if (!script) return;
    script << "#!/bin/sh\nprintf '%s\\n' \"$*\" >> " << log_.string() << "\nexit 0\n";
    script.close();
    ::chmod(executable_.c_str(), 0755);
    ready_ = true;
  }
  ~FakeInputAdb() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] AdbConfig config(bool consent) const {
    AdbConfig config;
    config.executable = executable_;
    config.serial = "192.168.240.112:5555";
    config.allow_adb_server_autostart = consent;
    return config;
  }
  [[nodiscard]] std::string issued() const {
    std::ifstream file{log_};
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }

 private:
  std::filesystem::path directory_;
  std::filesystem::path executable_;
  std::filesystem::path log_;
  bool ready_ = false;
};

}  // namespace

BONK_TEST(input_action, send_requires_adb_consent_before_any_process) {
  FakeInputAdb fake;
  REQUIRE(fake.ready());
  const Result<InputAction> tap = make_tap(viewport(), 0.5f, 0.5f);
  REQUIRE(tap.is_ok());

  const Result<InputSendResult> denied = send_input_action(fake.config(false), tap.value());
  REQUIRE(denied.is_error());
  CHECK_EQ(denied.error().code, std::string("android.adb_server_consent_required"));
  CHECK(fake.issued().empty());
}

BONK_TEST(input_action, send_reports_transport_acceptance_but_not_guest_delivery) {
  FakeInputAdb fake;
  REQUIRE(fake.ready());
  const Result<InputAction> created = make_tap(viewport(), 0.5f, 0.5f);
  REQUIRE(created.is_ok());
  InputAction tap = created.value();
  tap.sequence = 42;

  const Result<InputSendResult> sent = send_input_action(fake.config(true), tap);
  REQUIRE(sent.is_ok());
  CHECK(sent.value().command_sent);
  // Nothing inside Android witnessed the tap, so this must stay false.
  CHECK(!sent.value().observed_in_guest);
  CHECK_EQ(sent.value().sequence, std::uint64_t{42});
  CHECK(fake.issued().find("shell input tap 540 960") != std::string::npos);
}

BONK_TEST(input_action, send_refuses_an_invalid_action_without_running_adb) {
  FakeInputAdb fake;
  REQUIRE(fake.ready());

  const Result<InputSendResult> sent = send_input_action(fake.config(true), InputAction{});
  REQUIRE(sent.is_error());
  CHECK_EQ(sent.error().code, std::string("android.invalid_viewport"));
  CHECK(fake.issued().empty());
}
