#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "bonk/android/waydroid_runtime.hpp"
#include "test_framework.hpp"

using namespace bonk;

namespace {

std::string shell_quote(std::string_view value) {
  std::string quoted;
  quoted.push_back('\'');
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(character);
    }
  }
  quoted.push_back('\'');
  return quoted;
}

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream file{path};
  if (!file) return {};
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

class FakeWaydroid final {
 public:
  explicit FakeWaydroid(std::string_view status_output = "Session:\tSTOPPED\n", int status_exit_code = 0,
                        std::string_view operation_stdout = {}, std::string_view operation_stderr = {},
                        int operation_exit_code = 0) {
    std::string pattern = (std::filesystem::temp_directory_path() / "bonk-fake-waydroid-XXXXXX").string();
    std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
    mutable_pattern.push_back('\0');
    const char* directory = ::mkdtemp(mutable_pattern.data());
    if (directory == nullptr) return;
    directory_ = directory;
    executable_ = directory_ / "waydroid-fake";
    arguments_file_ = directory_ / "arguments.txt";
    selected_apk_path_ = directory_ / "selected-game.apk";
    std::ofstream apk{selected_apk_path_, std::ios::binary};
    apk << "synthetic test payload, not an installable APK";
    apk.close();
    if (!apk) return;

    std::ofstream script(executable_);
    if (!script) return;
    script << "#!/bin/sh\nprintf '%s\\n' \"$*\" >> " << shell_quote(arguments_file_.string()) << "\n"
           << "if [ \"${1-}\" = \"status\" ]; then\n"
           << "  printf '%s' " << shell_quote(status_output) << "\n"
           << "  exit " << status_exit_code << "\n"
           << "fi\n"
           << "if [ \"${1-}\" = \"session\" ] && [ \"${2-}\" = \"start\" ]; then\n"
           << "  exec /bin/sleep 30\n"
           << "fi\n"
           << "if [ \"${1-}\" = \"app\" ]; then\n"
           << "  printf '%s' " << shell_quote(operation_stdout) << "\n"
           << "  printf '%s' " << shell_quote(operation_stderr) << " >&2\n"
           << "  exit " << operation_exit_code << "\n"
           << "fi\n"
           << "if [ \"${1-}\" = \"show-full-ui\" ]; then\n"
           << "  printf '%s' " << shell_quote(operation_stdout) << "\n"
           << "  printf '%s' " << shell_quote(operation_stderr) << " >&2\n"
           << "  exit " << operation_exit_code << "\n"
           << "fi\n"
           << "exit 23\n";
    script.close();
    if (!script || ::chmod(executable_.c_str(), S_IRUSR | S_IWUSR | S_IXUSR) != 0) return;
    ready_ = true;
  }

  ~FakeWaydroid() {
    std::error_code error;
    if (!directory_.empty()) std::filesystem::remove_all(directory_, error);
  }

  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] android::WaydroidConfig config() const { return {executable_.string()}; }
  [[nodiscard]] std::string arguments() const { return read_text_file(arguments_file_); }
  [[nodiscard]] std::filesystem::path selected_apk_path() const { return selected_apk_path_; }

 private:
  std::filesystem::path directory_;
  std::filesystem::path executable_;
  std::filesystem::path arguments_file_;
  std::filesystem::path selected_apk_path_;
  bool ready_ = false;
};

}  // namespace

BONK_TEST(android_waydroid, builds_explicit_status_and_session_commands) {
  const android::WaydroidConfig config{"/usr/bin/waydroid"};
  const Result<android::RuntimeCommand> status = android::make_waydroid_status_command(config);
  REQUIRE(status.is_ok());
  CHECK_EQ(status.value().executable, config.executable);
  REQUIRE(status.value().arguments.size() == 1);
  CHECK_EQ(status.value().arguments[0], std::string("status"));

  const Result<android::RuntimeCommand> session = android::make_waydroid_session_command(config);
  REQUIRE(session.is_ok());
  CHECK_EQ(session.value().executable, config.executable);
  REQUIRE(session.value().arguments.size() == 2);
  CHECK_EQ(session.value().arguments[0], std::string("session"));
  CHECK_EQ(session.value().arguments[1], std::string("start"));

  const Result<android::RuntimeCommand> full_ui = android::make_waydroid_full_ui_command(config);
  REQUIRE(full_ui.is_ok());
  CHECK_EQ(full_ui.value().arguments, std::vector<std::string>{"show-full-ui"});

  const Result<android::RuntimeCommand> install =
      android::make_waydroid_install_apk_command(config, "/home/user/downloads/game.apk");
  REQUIRE(install.is_ok());
  CHECK_EQ(install.value().arguments,
           (std::vector<std::string>{"app", "install", "/home/user/downloads/game.apk"}));

  const Result<android::RuntimeCommand> launch =
      android::make_waydroid_launch_package_command(config, "org.example.game");
  REQUIRE(launch.is_ok());
  CHECK_EQ(launch.value().arguments, (std::vector<std::string>{"app", "launch", "org.example.game"}));
  CHECK(android::make_waydroid_install_apk_command(config, "relative.apk").is_error());
  CHECK(android::make_waydroid_launch_package_command(config, "org..game").is_error());
  CHECK(android::make_waydroid_launch_package_command(config, "org.game;evil").is_error());

  CHECK(android::make_waydroid_status_command({"waydroid"}).is_error());
  const std::string nul_path("/bin/waydroid\0bad", 18);
  CHECK(android::make_waydroid_session_command({nul_path}).is_error());
}

BONK_TEST(android_waydroid, parses_bounded_status_fields_and_rejects_ambiguity) {
  const Result<android::WaydroidStatus> running = android::parse_waydroid_status_output(
      "Session:\tRUNNING\nContainer:\tRUNNING\nVendor type:\tMAINLINE\n");
  REQUIRE(running.is_ok());
  CHECK(running.value().session == android::WaydroidState::Running);
  CHECK(running.value().container == android::WaydroidState::Running);

  const Result<android::WaydroidStatus> stopped =
      android::parse_waydroid_status_output("Session: STOPPED\nVendor type: MAINLINE\n");
  REQUIRE(stopped.is_ok());
  CHECK(stopped.value().session == android::WaydroidState::Stopped);
  CHECK(stopped.value().container == android::WaydroidState::Unknown);

  CHECK(android::parse_waydroid_status_output("Container: RUNNING\n").is_error());
  CHECK(android::parse_waydroid_status_output("Session: RUNNING\nSession: STOPPED\n").is_error());
  CHECK(android::parse_waydroid_status_output(std::string(64 * 1024 + 1, 'x')).is_error());
  CHECK(android::to_string(android::WaydroidState::Unknown) == "unknown");
}

BONK_TEST(android_waydroid, queries_status_read_only_and_checks_process_result) {
  FakeWaydroid fake{"Session:\tRUNNING\nContainer:\tRUNNING\n"};
  REQUIRE(fake.ready());
  const Result<android::WaydroidStatus> status = android::query_waydroid_status(fake.config());
  REQUIRE(status.is_ok());
  CHECK(status.value().session == android::WaydroidState::Running);
  CHECK_EQ(fake.arguments(), std::string("status\n"));

  FakeWaydroid failed{"Session:\tSTOPPED\n", 7};
  REQUIRE(failed.ready());
  const Result<android::WaydroidStatus> failure = android::query_waydroid_status(failed.config());
  REQUIRE(failure.is_error());
  CHECK_EQ(failure.error().code, std::string("android.waydroid_status_failed"));

  CHECK(android::query_waydroid_status(fake.config(), std::chrono::milliseconds{0}).is_error());
}

BONK_TEST(android_waydroid, requires_consent_and_refuses_to_take_over_existing_session) {
  FakeWaydroid fake{"Session:\tRUNNING\nContainer:\tRUNNING\n"};
  REQUIRE(fake.ready());
  android::EngineSession session;

  const Status no_consent = android::start_waydroid_session(session, fake.config(), false);
  REQUIRE(no_consent.is_error());
  CHECK_EQ(no_consent.error().code, std::string("android.waydroid_start_consent_required"));
  CHECK(fake.arguments().empty());

  const Status already_running = android::start_waydroid_session(session, fake.config(), true);
  REQUIRE(already_running.is_error());
  CHECK_EQ(already_running.error().code, std::string("android.waydroid_session_already_running"));
  CHECK_EQ(fake.arguments(), std::string("status\n"));
  CHECK(session.snapshot().state == android::ProcessState::Stopped);
}

BONK_TEST(android_waydroid, starts_only_a_confirmed_stopped_session_under_process_supervision) {
  FakeWaydroid fake{"Session:\tSTOPPED\n"};
  REQUIRE(fake.ready());
  android::EngineSession session;

  REQUIRE(android::start_waydroid_session(session, fake.config(), true).is_ok());
  CHECK(session.poll().state == android::ProcessState::Running);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
  std::string arguments;
  do {
    arguments = fake.arguments();
    if (arguments.find("session start\n") != std::string::npos) break;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  } while (std::chrono::steady_clock::now() < deadline);
  CHECK_EQ(arguments, std::string("status\nsession start\n"));
  REQUIRE(session.stop().is_ok());
  CHECK(session.snapshot().state == android::ProcessState::Stopped);
}

BONK_TEST(android_waydroid, waits_for_running_status_and_monitors_owned_session_process) {
  FakeWaydroid fake{"Session:\tRUNNING\nContainer:\tRUNNING\n"};
  REQUIRE(fake.ready());
  android::EngineSession session;
  REQUIRE(session.start({fake.config().executable, {"session", "start"}}).is_ok());

  const Result<android::WaydroidStatus> status = android::wait_for_waydroid_session(
      session, fake.config(), std::chrono::seconds{1}, std::chrono::milliseconds{5}, std::chrono::milliseconds{250});
  REQUIRE(status.is_ok());
  CHECK(status.value().session == android::WaydroidState::Running);
  CHECK(session.poll().state == android::ProcessState::Running);
  REQUIRE(session.stop().is_ok());
}

BONK_TEST(android_waydroid, boot_wait_fails_when_owned_session_process_exits_or_times_out) {
  FakeWaydroid stopped{"Session:\tSTOPPED\n"};
  REQUIRE(stopped.ready());
  android::EngineSession exited;
  REQUIRE(exited.start({"/bin/true", {}}).is_ok());
  const Result<android::WaydroidStatus> early_exit = android::wait_for_waydroid_session(
      exited, stopped.config(), std::chrono::seconds{1}, std::chrono::milliseconds{1}, std::chrono::milliseconds{100});
  REQUIRE(early_exit.is_error());
  CHECK_EQ(early_exit.error().code, std::string("android.waydroid_session_exited"));

  android::EngineSession stalled;
  REQUIRE(stalled.start({stopped.config().executable, {"session", "start"}}).is_ok());
  const Result<android::WaydroidStatus> timeout = android::wait_for_waydroid_session(
      stalled, stopped.config(), std::chrono::milliseconds{20}, std::chrono::milliseconds{1}, std::chrono::milliseconds{10});
  REQUIRE(timeout.is_error());
  CHECK_EQ(timeout.error().code, std::string("android.waydroid_session_timeout"));
  REQUIRE(stalled.stop().is_ok());
}

BONK_TEST(android_waydroid, app_operations_require_an_explicitly_running_session) {
  FakeWaydroid stopped{"Session:\tSTOPPED\n"};
  REQUIRE(stopped.ready());

  const Status show = android::show_waydroid_full_ui(stopped.config());
  REQUIRE(show.is_error());
  CHECK_EQ(show.error().code, std::string("android.waydroid_session_required"));

  const Status install = android::install_waydroid_selected_apk(stopped.config(), stopped.selected_apk_path(), true);
  REQUIRE(install.is_error());
  CHECK_EQ(install.error().code, std::string("android.waydroid_session_required"));

  const Status launch = android::launch_waydroid_package(stopped.config(), "org.example.game");
  REQUIRE(launch.is_error());
  CHECK_EQ(launch.error().code, std::string("android.waydroid_session_required"));
  CHECK_EQ(stopped.arguments(), std::string("status\nstatus\nstatus\n"));

  FakeWaydroid running{"Session:\tRUNNING\n"};
  REQUIRE(running.ready());
  const Status unconfirmed =
      android::install_waydroid_selected_apk(running.config(), running.selected_apk_path(), false);
  REQUIRE(unconfirmed.is_error());
  CHECK_EQ(unconfirmed.error().code, std::string("android.apk_install_consent_required"));
  CHECK(running.arguments().empty());
  CHECK(android::install_waydroid_selected_apk(running.config(), "/missing/selected.apk", true).is_error());
}

BONK_TEST(android_waydroid, installs_selected_apk_launches_exact_package_and_shows_full_ui) {
  FakeWaydroid fake{"Session:\tRUNNING\nContainer:\tRUNNING\n"};
  REQUIRE(fake.ready());

  REQUIRE(android::install_waydroid_selected_apk(fake.config(), fake.selected_apk_path(), true).is_ok());
  REQUIRE(android::launch_waydroid_package(fake.config(), "org.example.game").is_ok());
  REQUIRE(android::show_waydroid_full_ui(fake.config()).is_ok());

  const std::string expected = "status\napp install " + fake.selected_apk_path().string() +
                               "\nstatus\napp launch org.example.game\nstatus\nshow-full-ui\n";
  CHECK_EQ(fake.arguments(), expected);
}

BONK_TEST(android_waydroid, reports_explicit_operation_failures_even_with_exit_zero) {
  FakeWaydroid fake{"Session:\tRUNNING\n", 0, "ERROR: WayDroid session is stopped\n"};
  REQUIRE(fake.ready());

  const Status launch = android::launch_waydroid_package(fake.config(), "org.example.game");
  REQUIRE(launch.is_error());
  CHECK_EQ(launch.error().code, std::string("android.waydroid_app_launch_reported_failure"));
}
