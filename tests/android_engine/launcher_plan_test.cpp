#include <string>
#include <vector>

#include "bonk/android/launcher_plan.hpp"
#include "test_framework.hpp"

using namespace bonk;
using namespace bonk::android;

namespace {

LauncherRequest game_request() {
  LauncherRequest request;
  request.start_confirmed = true;
  request.apk_path = "/home/user/game.apk";
  request.install_confirmed = true;
  request.package_id = "com.example.game";
  return request;
}

std::vector<LauncherStepKind> kinds_of(const Result<LauncherPlan>& plan) {
  std::vector<LauncherStepKind> kinds;
  if (plan.is_error()) return kinds;
  for (const LauncherStep& step : plan.value().steps) kinds.push_back(step.kind);
  return kinds;
}

bool contains(const std::vector<LauncherStepKind>& kinds, LauncherStepKind kind) {
  for (const LauncherStepKind entry : kinds) {
    if (entry == kind) return true;
  }
  return false;
}

std::size_t index_of(const std::vector<LauncherStepKind>& kinds, LauncherStepKind kind) {
  for (std::size_t i = 0; i < kinds.size(); ++i) {
    if (kinds[i] == kind) return i;
  }
  return kinds.size();
}

RuntimeStatus state(RuntimeState session) {
  RuntimeStatus status;
  status.session = session;
  status.container = session;
  return status;
}

}  // namespace

BONK_TEST(launcher_plan, status_only_request_plans_nothing) {
  const LauncherRequest request;
  const Result<LauncherPlan> plan = plan_launcher(state(RuntimeState::Running), request);

  REQUIRE(plan.is_ok());
  CHECK(plan.value().steps.empty());
  CHECK(!plan.value().owns_session);
  CHECK(!launcher_request_has_action(request));
}

BONK_TEST(launcher_plan, stopped_session_without_consent_is_refused) {
  LauncherRequest request;
  request.package_id = "com.example.game";

  const Result<LauncherPlan> plan = plan_launcher(state(RuntimeState::Stopped), request);

  REQUIRE(plan.is_error());
  CHECK_EQ(plan.error().code, std::string("android.runtime_start_consent_required"));
}

BONK_TEST(launcher_plan, confirmed_start_owns_the_session_and_supervises_it) {
  LauncherRequest request;
  request.start_confirmed = true;

  const Result<LauncherPlan> plan = plan_launcher(state(RuntimeState::Stopped), request);

  REQUIRE(plan.is_ok());
  CHECK(plan.value().owns_session);
  const std::vector<LauncherStepKind> kinds = kinds_of(plan);
  REQUIRE(!kinds.empty());
  CHECK(kinds.front() == LauncherStepKind::StartSession);
  CHECK(kinds.back() == LauncherStepKind::AwaitOwnedSession);
  // Start precedes wait, which precedes any UI/app work.
  CHECK(index_of(kinds, LauncherStepKind::StartSession) < index_of(kinds, LauncherStepKind::WaitForSession));
  CHECK(index_of(kinds, LauncherStepKind::WaitForSession) < index_of(kinds, LauncherStepKind::OpenUi));
}

BONK_TEST(launcher_plan, running_session_is_never_adopted) {
  LauncherRequest request;
  request.start_confirmed = true;
  request.package_id = "com.example.game";

  const Result<LauncherPlan> plan = plan_launcher(state(RuntimeState::Running), request);

  REQUIRE(plan.is_ok());
  CHECK(!plan.value().owns_session);
  CHECK(plan.value().declined_ownership);
  const std::vector<LauncherStepKind> kinds = kinds_of(plan);
  CHECK(!contains(kinds, LauncherStepKind::StartSession));
  CHECK(!contains(kinds, LauncherStepKind::AwaitOwnedSession));
  CHECK(contains(kinds, LauncherStepKind::LaunchPackage));
}

BONK_TEST(launcher_plan, unknown_state_blocks_every_action) {
  LauncherRequest request;
  request.open_ui = true;

  const Result<LauncherPlan> plan = plan_launcher(state(RuntimeState::Unknown), request);

  REQUIRE(plan.is_error());
  CHECK_EQ(plan.error().code, std::string("android.runtime_state_unknown"));
}

BONK_TEST(launcher_plan, invalid_package_is_refused_before_any_runtime_step) {
  LauncherRequest request;
  request.start_confirmed = true;
  request.package_id = "not a package";

  const Result<LauncherPlan> plan = plan_launcher(state(RuntimeState::Stopped), request);

  REQUIRE(plan.is_error());
  CHECK_EQ(plan.error().code, std::string("android.invalid_package_name"));
}

BONK_TEST(launcher_plan, apk_without_per_file_consent_is_refused_before_starting) {
  LauncherRequest request;
  request.start_confirmed = true;
  request.apk_path = "/home/user/game.apk";  // install_confirmed stays false

  const Result<LauncherPlan> plan = plan_launcher(state(RuntimeState::Stopped), request);

  REQUIRE(plan.is_error());
  CHECK_EQ(plan.error().code, std::string("android.apk_install_consent_required"));
}

BONK_TEST(launcher_plan, full_game_flow_is_ordered_and_complete) {
  const Result<LauncherPlan> plan = plan_launcher(state(RuntimeState::Stopped), game_request());

  REQUIRE(plan.is_ok());
  const std::vector<LauncherStepKind> kinds = kinds_of(plan);
  CHECK(index_of(kinds, LauncherStepKind::StartSession) < index_of(kinds, LauncherStepKind::WaitForSession));
  CHECK(index_of(kinds, LauncherStepKind::WaitForSession) < index_of(kinds, LauncherStepKind::OpenUi));
  CHECK(index_of(kinds, LauncherStepKind::OpenUi) < index_of(kinds, LauncherStepKind::InstallApk));
  CHECK(index_of(kinds, LauncherStepKind::InstallApk) < index_of(kinds, LauncherStepKind::LaunchPackage));
  CHECK(index_of(kinds, LauncherStepKind::LaunchPackage) < index_of(kinds, LauncherStepKind::AwaitOwnedSession));

  // Details travel with the step so the CLI cannot swap APK or package IDs.
  for (const LauncherStep& step : plan.value().steps) {
    if (step.kind == LauncherStepKind::InstallApk) CHECK_EQ(step.detail, std::string("/home/user/game.apk"));
    if (step.kind == LauncherStepKind::LaunchPackage) CHECK_EQ(step.detail, std::string("com.example.game"));
  }
}

BONK_TEST(launcher_plan, guest_probe_precedes_ui_and_app_actions) {
  LauncherRequest request = game_request();
  request.probe_guest = true;

  const Result<LauncherPlan> plan = plan_launcher(state(RuntimeState::Running), request);

  REQUIRE(plan.is_ok());
  const std::vector<LauncherStepKind> kinds = kinds_of(plan);
  CHECK(contains(kinds, LauncherStepKind::ProbeGuest));
  CHECK(index_of(kinds, LauncherStepKind::ProbeGuest) < index_of(kinds, LauncherStepKind::OpenUi));
  CHECK(index_of(kinds, LauncherStepKind::ProbeGuest) < index_of(kinds, LauncherStepKind::InstallApk));
}

BONK_TEST(launcher_plan, probe_only_request_does_not_open_the_ui) {
  LauncherRequest request;
  request.probe_guest = true;

  const Result<LauncherPlan> plan = plan_launcher(state(RuntimeState::Running), request);

  REQUIRE(plan.is_ok());
  const std::vector<LauncherStepKind> kinds = kinds_of(plan);
  CHECK(contains(kinds, LauncherStepKind::ProbeGuest));
  CHECK(!contains(kinds, LauncherStepKind::OpenUi));
  CHECK(!plan.value().owns_session);
}

BONK_TEST(launcher_plan, step_names_are_stable) {
  CHECK_EQ(to_string(LauncherStepKind::StartSession), std::string_view("start-session"));
  CHECK_EQ(to_string(LauncherStepKind::WaitForSession), std::string_view("wait-for-session"));
  CHECK_EQ(to_string(LauncherStepKind::ProbeGuest), std::string_view("probe-guest"));
  CHECK_EQ(to_string(LauncherStepKind::OpenUi), std::string_view("open-ui"));
  CHECK_EQ(to_string(LauncherStepKind::InstallApk), std::string_view("install-apk"));
  CHECK_EQ(to_string(LauncherStepKind::LaunchPackage), std::string_view("launch-package"));
  CHECK_EQ(to_string(LauncherStepKind::AwaitOwnedSession), std::string_view("await-owned-session"));
}

BONK_TEST(launcher_plan, package_id_validation) {
  CHECK(valid_android_package_id("com.example.game"));
  CHECK(valid_android_package_id("id.lynxdroid.agent"));
  CHECK(valid_android_package_id("a.b_c.d1"));

  CHECK(!valid_android_package_id(""));
  CHECK(!valid_android_package_id("nodots"));
  CHECK(!valid_android_package_id(".leading"));
  CHECK(!valid_android_package_id("trailing."));
  CHECK(!valid_android_package_id("double..dot"));
  CHECK(!valid_android_package_id("1leading.digit"));
  CHECK(!valid_android_package_id("has-dash.in.name"));
  CHECK(!valid_android_package_id("has space.in.name"));
}
