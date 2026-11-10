// LynxDroid launcher plan.
//
// The order in which a launcher may touch a Waydroid host is a safety
// contract, not an implementation detail:
//
//   validate the request -> observe the runtime -> (consent) start session
//   -> wait for running -> open the runtime UI -> install one confirmed APK
//   -> launch one exact package -> supervise the session we own
//
// Encoding the order as data makes it testable without a Waydroid host and
// keeps `bonk waydroid` from drifting into "start something because the user
// asked for a game".
//
// Nothing here spawns a process; every function is pure.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "bonk/android/waydroid_runtime.hpp"
#include "bonk/core/result.hpp"

namespace bonk::android {

/// One executable step of the launcher flow, in contract order.
enum class LauncherStepKind {
  StartSession,
  WaitForSession,
  ProbeGuest,
  OpenUi,
  InstallApk,
  LaunchPackage,
  AwaitOwnedSession,
};

[[nodiscard]] std::string_view to_string(LauncherStepKind step) noexcept;

struct LauncherStep {
  LauncherStepKind kind = LauncherStepKind::OpenUi;
  std::string detail;  // package id / APK path, empty when not applicable
};

/// What the user asked for. Consent flags must only be set from an explicit
/// user decision, never inferred from the presence of the other fields.
struct LauncherRequest {
  bool start_confirmed = false;    // `--confirm-start`
  bool open_ui = false;            // `--show-ui`
  std::filesystem::path apk_path;  // empty = no install
  bool install_confirmed = false;  // `--confirm-install`
  std::string package_id;          // empty = no launch
  bool probe_guest = false;        // read-only ADB guest observation
};

struct LauncherPlan {
  std::vector<LauncherStep> steps;
  /// True only when the plan starts the session, i.e. this launcher is
  /// responsible for stopping the process group it created.
  bool owns_session = false;
  /// Honoured only for an already-running session that the user asked to
  /// start; surfaced so the CLI can say it declined to take ownership.
  bool declined_ownership = false;
};

/// Returns true for a syntactically valid Android package ID
/// (`a.b`, letter-initial segments, ASCII letters/digits/underscore).
[[nodiscard]] bool valid_android_package_id(std::string_view package_id) noexcept;

/// Builds the ordered plan for `request` given the observed runtime state.
///
/// Refusals (no step is ever produced alongside an error):
///   android.invalid_package_name        - malformed package ID
///   android.apk_install_consent_required - APK without per-file consent
///   android.waydroid_start_consent_required - start needed but not confirmed
///   android.waydroid_state_unknown      - runtime state cannot be trusted
///
/// A request with no action at all produces an empty plan, which is how a
/// status-only invocation is expressed.
[[nodiscard]] Result<LauncherPlan> plan_launcher(const WaydroidStatus& current,
                                                 const LauncherRequest& request);

/// True when the request asks for at least one runtime-touching action.
[[nodiscard]] bool launcher_request_has_action(const LauncherRequest& request) noexcept;

}  // namespace bonk::android
