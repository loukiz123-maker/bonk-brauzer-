#include "bonk/android/launcher_plan.hpp"

#include <string>
#include <utility>

namespace bonk::android {
namespace {

constexpr std::size_t kMaxAndroidPackageNameBytes = 255;

bool ascii_alpha(unsigned char character) noexcept {
  return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

}  // namespace

std::string_view to_string(LauncherStepKind step) noexcept {
  switch (step) {
    case LauncherStepKind::StartSession:
      return "start-session";
    case LauncherStepKind::WaitForSession:
      return "wait-for-session";
    case LauncherStepKind::ProbeGuest:
      return "probe-guest";
    case LauncherStepKind::OpenUi:
      return "open-ui";
    case LauncherStepKind::InstallApk:
      return "install-apk";
    case LauncherStepKind::LaunchPackage:
      return "launch-package";
    case LauncherStepKind::AwaitOwnedSession:
      return "await-owned-session";
  }
  return "unknown";
}

bool valid_android_package_id(std::string_view package_id) noexcept {
  if (package_id.empty() || package_id.size() > kMaxAndroidPackageNameBytes) return false;
  if (package_id.find('\0') != std::string_view::npos) return false;
  bool segment_start = true;
  bool has_separator = false;
  for (const unsigned char character : package_id) {
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

bool launcher_request_has_action(const LauncherRequest& request) noexcept {
  return request.start_confirmed || request.open_ui || !request.apk_path.empty() || !request.package_id.empty() ||
         request.probe_guest;
}

namespace {

/// Actions that need a running Waydroid session. The ADB guest probe is
/// deliberately excluded: it is read-only and reports honestly even when the
/// session is stopped (`not-connected`), so it must not force a session start.
bool needs_running_session(const LauncherRequest& request) noexcept {
  return request.start_confirmed || request.open_ui || !request.apk_path.empty() || !request.package_id.empty();
}

}  // namespace

Result<LauncherPlan> plan_launcher(const WaydroidStatus& current, const LauncherRequest& request) {
  // Validation runs before any runtime interaction, so a malformed request can
  // never start a session as a side effect of being rejected.
  if (!request.package_id.empty() && !valid_android_package_id(request.package_id)) {
    return Result<LauncherPlan>::failure(
        Error::make("android.invalid_package_name", "launch requires a valid Android package ID"));
  }
  if (!request.apk_path.empty() && !request.install_confirmed) {
    return Result<LauncherPlan>::failure(Error::make(
        "android.apk_install_consent_required",
        "installing this APK requires explicit per-file user confirmation"));
  }

  LauncherPlan plan;
  if (!launcher_request_has_action(request)) return Result<LauncherPlan>::success(std::move(plan));

  // A probe-only request never inspects or changes the runtime state, so it is
  // planned without consulting it.
  if (!needs_running_session(request)) {
    plan.steps.push_back({LauncherStepKind::ProbeGuest, {}});
    return Result<LauncherPlan>::success(std::move(plan));
  }

  if (current.session == WaydroidState::Unknown) {
    return Result<LauncherPlan>::failure(Error::make(
        "android.waydroid_state_unknown", "cannot plan Waydroid actions while its session state is unknown"));
  }

  const bool session_is_running = current.session == WaydroidState::Running;
  if (!session_is_running) {
    if (!request.start_confirmed) {
      return Result<LauncherPlan>::failure(Error::make(
          "android.waydroid_start_consent_required",
          "Waydroid is stopped; starting it requires explicit --confirm-start consent"));
    }
    plan.steps.push_back({LauncherStepKind::StartSession, {}});
    plan.steps.push_back({LauncherStepKind::WaitForSession, {}});
    plan.owns_session = true;
  } else if (request.start_confirmed) {
    // An already-running session belongs to someone else. Never adopt it.
    plan.declined_ownership = true;
  }

  // Guest observation is read-only and runs before the launcher displays any
  // app result, so the UI can distinguish "booted guest" from "session up".
  if (request.probe_guest) {
    plan.steps.push_back({LauncherStepKind::ProbeGuest, {}});
  }

  // The Waydroid UI is the initial supported display surface; apps are only
  // launched once it is up.
  const bool app_action = !request.apk_path.empty() || !request.package_id.empty();
  if (request.open_ui || request.start_confirmed || app_action) {
    plan.steps.push_back({LauncherStepKind::OpenUi, {}});
  }

  if (!request.apk_path.empty()) {
    plan.steps.push_back({LauncherStepKind::InstallApk, request.apk_path.string()});
  }
  if (!request.package_id.empty()) {
    plan.steps.push_back({LauncherStepKind::LaunchPackage, request.package_id});
  }

  // Only the owner of the foreground session keeps supervising it.
  if (plan.owns_session) plan.steps.push_back({LauncherStepKind::AwaitOwnedSession, {}});

  return Result<LauncherPlan>::success(std::move(plan));
}

}  // namespace bonk::android
