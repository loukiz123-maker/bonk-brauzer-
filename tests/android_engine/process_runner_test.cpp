#include <chrono>
#include <string>

#include "bonk/android/process_runner.hpp"
#include "test_framework.hpp"

using namespace bonk;
using namespace std::chrono_literals;

BONK_TEST(android_process_runner, captures_stdout_without_a_shell) {
  const Result<android::CommandOutput> result =
      android::run_command({"/bin/printf", {"%s:%s", "one", "two"}}, 1s);
  REQUIRE(result.is_ok());
  CHECK_EQ(result.value().stdout_data, std::string("one:two"));
  CHECK(result.value().stderr_data.empty());
  REQUIRE(result.value().exit_code.has_value());
  CHECK_EQ(*result.value().exit_code, 0);
  CHECK(!result.value().timed_out);
}

BONK_TEST(android_process_runner, captures_stderr_and_nonzero_exit) {
  const Result<android::CommandOutput> result =
      android::run_command({"/bin/ls", {"/definitely/missing/lynxdroid-test-path"}}, 1s);
  REQUIRE(result.is_ok());
  REQUIRE(result.value().exit_code.has_value());
  CHECK_NE(*result.value().exit_code, 0);
  CHECK(!result.value().stderr_data.empty());
  CHECK(result.value().stdout_data.empty());
}

BONK_TEST(android_process_runner, bounds_captured_output_but_keeps_draining) {
  const Result<android::CommandOutput> result =
      android::run_command({"/bin/printf", {"%s", "0123456789"}}, 1s, 4);
  REQUIRE(result.is_ok());
  CHECK_EQ(result.value().stdout_data, std::string("0123"));
  CHECK(result.value().stdout_truncated);
  REQUIRE(result.value().exit_code.has_value());
  CHECK_EQ(*result.value().exit_code, 0);
}

BONK_TEST(android_process_runner, kills_and_reaps_a_command_after_timeout) {
  const Result<android::CommandOutput> result =
      android::run_command({"/bin/sleep", {"2"}}, 25ms);
  REQUIRE(result.is_ok());
  CHECK(result.value().timed_out);
  CHECK(result.value().termination_signal.has_value());
  CHECK(!result.value().exit_code.has_value());
}

BONK_TEST(android_process_runner, reports_spawn_failure_as_a_dependency_error) {
  const Result<android::CommandOutput> result =
      android::run_command({"/definitely/missing/lynxdroid-command", {}}, 1s);
  REQUIRE(result.is_error());
  CHECK_EQ(result.error().code, std::string("android.spawn_failed"));
}

BONK_TEST(android_process_runner, rejects_invalid_limits_and_nul_arguments) {
  CHECK(android::run_command({"/bin/true", {}}, 0ms).is_error());
  CHECK(android::run_command({"/bin/true", {}}, 1s, 0).is_error());
  CHECK(android::run_command({"true", {}}, 1s).is_error());
  const std::string argument("bad\0arg", 7);
  const Result<android::CommandOutput> result = android::run_command({"/bin/true", {argument}}, 1s);
  REQUIRE(result.is_error());
  CHECK_EQ(result.error().code, std::string("android.invalid_command"));
}
