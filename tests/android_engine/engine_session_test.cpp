#include <chrono>
#include <thread>

#include "bonk/android/engine_session.hpp"
#include "test_framework.hpp"

using namespace bonk;
using namespace std::chrono_literals;

BONK_TEST(android_engine, builds_headless_emulator_arguments_without_a_shell) {
  const android::EmulatorConfig config{"/opt/android-sdk/emulator/emulator", "Pixel API 35", true};
  const Result<android::RuntimeCommand> result = android::make_emulator_command(config);
  REQUIRE(result.is_ok());
  CHECK_EQ(result.value().executable, config.emulator_executable);
  REQUIRE(result.value().arguments.size() == 3);
  CHECK_EQ(result.value().arguments[0], std::string("-avd"));
  CHECK_EQ(result.value().arguments[1], config.avd_name);
  CHECK_EQ(result.value().arguments[2], std::string("-no-window"));
}

BONK_TEST(android_engine, builds_visible_emulator_arguments_when_requested) {
  const android::EmulatorConfig config{"/opt/android-sdk/emulator/emulator", "Pixel_8_API_35", false};
  const Result<android::RuntimeCommand> result = android::make_emulator_command(config);
  REQUIRE(result.is_ok());
  REQUIRE(result.value().arguments.size() == 2);
  CHECK_EQ(result.value().arguments[0], std::string("-avd"));
  CHECK_EQ(result.value().arguments[1], config.avd_name);
}

BONK_TEST(android_engine, rejects_invalid_emulator_configuration) {
  CHECK(android::make_emulator_command({"", "Pixel_8", true}).is_error());
  CHECK(android::make_emulator_command({"emulator", "Pixel_8", true}).is_error());
  CHECK(android::make_emulator_command({"emulator", "", true}).is_error());
  CHECK(android::make_emulator_command({"emulator", "-no-window", true}).is_error());
  CHECK(android::make_emulator_command({"emulator", "folder/avd", true}).is_error());
}

BONK_TEST(android_engine, reports_missing_runtime_binary_clearly) {
  android::EngineSession session;
  const Status status = session.start({"/definitely/missing/lynxdroid-emulator", {"-version"}});
  REQUIRE(status.is_error());
  CHECK_EQ(status.error().code, std::string("android.spawn_failed"));
  CHECK(session.snapshot().state == android::ProcessState::Failed);
  CHECK(session.snapshot().message.find("cannot start runtime executable") != std::string::npos);
  CHECK_EQ(session.snapshot().process_id, -1);
}

BONK_TEST(android_engine, supervises_and_stops_only_its_child_process) {
  android::EngineSession session;
  REQUIRE(session.start({"/bin/sleep", {"30"}}).is_ok());
  const android::SessionStatus running = session.poll();
  CHECK(running.state == android::ProcessState::Running);
  CHECK(running.process_id > 0);

  const Status duplicate_start = session.start({"/bin/sleep", {"30"}});
  REQUIRE(duplicate_start.is_error());
  CHECK_EQ(duplicate_start.error().code, std::string("android.already_running"));
  CHECK(session.snapshot().state == android::ProcessState::Running);

  REQUIRE(session.stop(250ms).is_ok());
  CHECK(session.snapshot().state == android::ProcessState::Stopped);
  CHECK_EQ(session.snapshot().process_id, -1);
  CHECK(session.snapshot().termination_signal.has_value());
  CHECK(session.stop().is_ok());  // stop is idempotent when there is no owned child
}

BONK_TEST(android_engine, observes_a_runtime_that_exits_normally) {
  android::EngineSession session;
  REQUIRE(session.start({"/bin/true", {}}).is_ok());

  android::SessionStatus observed;
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  do {
    observed = session.poll();
    if (observed.state != android::ProcessState::Running) break;
    std::this_thread::sleep_for(5ms);
  } while (std::chrono::steady_clock::now() < deadline);

  CHECK(observed.state == android::ProcessState::Exited);
  REQUIRE(observed.exit_code.has_value());
  CHECK_EQ(*observed.exit_code, 0);
  CHECK_EQ(observed.process_id, -1);
}

BONK_TEST(android_engine, rejects_embedded_nul_arguments) {
  android::EngineSession session;
  const std::string nul_argument("bad\0arg", 7);
  const Status status = session.start({"/bin/true", {nul_argument}});
  REQUIRE(status.is_error());
  CHECK_EQ(status.error().code, std::string("android.invalid_command"));
  CHECK(session.snapshot().state == android::ProcessState::Failed);
}
