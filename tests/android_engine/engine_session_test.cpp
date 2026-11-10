#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include <unistd.h>

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

namespace {

/// Starts a runtime whose leader dies on SIGTERM while a helper in the same
/// process group ignores it, and returns the helper's PID (or -1).
int start_runtime_with_a_stubborn_helper(android::EngineSession& session, const std::string& directory) {
  const std::string pid_file = directory + "/helper.pid";
  const std::string script = "sh -c 'trap \"\" TERM; echo $$ > \"$0\"; while :; do sleep 1; done' \"$0\" & exec sleep 300";
  if (session.start({"/bin/sh", {"-c", script, pid_file}}).is_error()) return -1;

  int helper = -1;
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    std::ifstream in(pid_file);
    if (in >> helper && helper > 0) break;
    helper = -1;
    std::this_thread::sleep_for(5ms);
  }
  // Give the helper a moment to install its SIGTERM trap after writing the PID.
  std::this_thread::sleep_for(100ms);
  return helper;
}

/// A process that was killed but not yet reaped by init still answers kill(pid, 0).
bool process_disappears(int process_id) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (::kill(process_id, 0) != 0 && errno == ESRCH) return true;
    std::this_thread::sleep_for(5ms);
  }
  return false;
}

struct TemporaryDirectory {
  std::string path;
  TemporaryDirectory() {
    char pattern[] = "/tmp/bonk-engine-session-XXXXXX";
    if (::mkdtemp(pattern) != nullptr) path = pattern;
  }
  ~TemporaryDirectory() {
    if (path.empty()) return;
    (void)::unlink((path + "/helper.pid").c_str());
    (void)::rmdir(path.c_str());
  }
};

}  // namespace

// Regression: stop() used to return "stopped" as soon as the leader was reaped,
// leaving every other member of the owned process group running.
BONK_TEST(android_engine, stop_leaves_no_member_of_the_process_group_behind) {
  const TemporaryDirectory directory;
  REQUIRE(!directory.path.empty());

  android::EngineSession session;
  const int helper = start_runtime_with_a_stubborn_helper(session, directory.path);
  REQUIRE(helper > 0);
  REQUIRE(::kill(helper, 0) == 0);

  const Status stopped = session.stop(500ms);
  const bool helper_gone = process_disappears(helper);
  if (!helper_gone) (void)::kill(helper, SIGKILL);  // never leak from a failing test

  REQUIRE(stopped.is_ok());
  CHECK(session.snapshot().state == android::ProcessState::Stopped);
  CHECK_EQ(session.snapshot().process_id, -1);
  REQUIRE(session.snapshot().termination_signal.has_value());
  CHECK_EQ(*session.snapshot().termination_signal, SIGTERM);  // the leader still got its graceful signal
  CHECK(helper_gone);
}

BONK_TEST(android_engine, destructor_leaves_no_member_of_the_process_group_behind) {
  const TemporaryDirectory directory;
  REQUIRE(!directory.path.empty());

  int helper = -1;
  {
    android::EngineSession session;
    helper = start_runtime_with_a_stubborn_helper(session, directory.path);
  }
  REQUIRE(helper > 0);
  const bool helper_gone = process_disappears(helper);
  if (!helper_gone) (void)::kill(helper, SIGKILL);
  CHECK(helper_gone);
}

BONK_TEST(android_engine, stop_escalates_when_the_runtime_itself_ignores_sigterm) {
  android::EngineSession session;
  REQUIRE(session.start({"/bin/sh", {"-c", "trap '' TERM; while :; do sleep 1; done"}}).is_ok());
  std::this_thread::sleep_for(100ms);

  REQUIRE(session.stop(100ms).is_ok());
  CHECK(session.snapshot().state == android::ProcessState::Stopped);
  REQUIRE(session.snapshot().termination_signal.has_value());
  CHECK_EQ(*session.snapshot().termination_signal, SIGKILL);
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
