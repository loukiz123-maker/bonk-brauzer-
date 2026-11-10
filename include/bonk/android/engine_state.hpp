// LynxDroid engine observation model.
//
// The launcher must never collapse "the runtime process exists" into "the game
// is running". Each layer is observed independently and reported with an
// explicit `observed` marker, so a synthetic/CLI result cannot be mistaken for
// live Android evidence.
//
// Layers:
//   runtime : host process/service state (Waydroid session + container)
//   guest   : Android guest reachability (ADB transport + sys.boot_completed)
//   app     : request/command result for one package ID (never app readiness)
//   input   : whether an input command was sent, and whether the guest witnessed it
//   frames  : unavailable, one still snapshot, or a real stream
//
// This header is pure data plus pure functions: no process is spawned here.
#pragma once

#include <string>
#include <string_view>

#include "bonk/android/adb_client.hpp"
#include "bonk/android/waydroid_runtime.hpp"

namespace bonk::android {

/// Host-side runtime observation. `owned` is true only when this launcher
/// process started the foreground session and is responsible for stopping it.
struct RuntimeObservation {
  WaydroidState session = WaydroidState::Unknown;
  WaydroidState container = WaydroidState::Unknown;
  bool owned = false;
  std::string detail;

  /// True only when the session reports running. A running session does not
  /// mean the guest booted and says nothing about the app.
  [[nodiscard]] bool session_running() const noexcept { return session == WaydroidState::Running; }
};

/// Android guest observation. `observed` is false until an ADB query actually
/// succeeded; an unobserved guest is never reported as anything but unknown.
struct GuestObservation {
  GuestState state = GuestState::Unknown;
  bool observed = false;
  std::string serial;
  std::string detail;
};

/// Command-level app state. None of these imply a visible or responsive app.
enum class AppPhase {
  NotRequested,
  InstallRequested,
  InstallCommandSucceeded,
  InstallCommandFailed,
  LaunchRequested,
  LaunchCommandSucceeded,
  LaunchCommandFailed,
};

[[nodiscard]] std::string_view to_string(AppPhase phase) noexcept;

struct AppObservation {
  AppPhase phase = AppPhase::NotRequested;
  std::string package_id;
  /// Requires an in-guest witness (foreground activity check or app-side
  /// signal). It is never inferred from a successful command.
  bool foreground_confirmed = false;
};

/// Input transport result. `observed_in_guest` requires a witness inside
/// Android, so a successful `adb shell input` alone keeps it false.
struct InputObservation {
  bool command_sent = false;
  bool observed_in_guest = false;
  std::string detail;
};

enum class FrameKind { Unavailable, StillSnapshot, Streaming };

[[nodiscard]] std::string_view to_string(FrameKind kind) noexcept;

struct EngineSnapshot {
  RuntimeObservation runtime;
  GuestObservation guest;
  AppObservation app;
  InputObservation input;
  FrameKind frames = FrameKind::Unavailable;
};

/// True only when the guest itself reported sys.boot_completed=1.
[[nodiscard]] bool guest_ready_for_app_actions(const EngineSnapshot& snapshot) noexcept;

/// True when runtime, guest and app are all verified to the level they claim:
/// session running, guest boot-complete, and the app's foreground witnessed.
/// Used to decide whether a "game is running" claim may be shown.
[[nodiscard]] bool app_is_running(const EngineSnapshot& snapshot) noexcept;

/// Stable, line-oriented rendering used by the CLI and by evidence capture:
///
///   engine runtime=stopped container=stopped owned=no
///   engine guest=unknown observed=no serial=-
///   engine app=not-requested package=- foreground-confirmed=no
///   engine input sent=no observed-in-guest=no
///   engine frames=unavailable
///
/// Tokens never contain spaces, so the output can be grepped by host smoke
/// tests. The detail fields are rendered on separate `engine detail ...`
/// lines so that a value can never be confused with a state token.
[[nodiscard]] std::string render_engine_snapshot(const EngineSnapshot& snapshot);

/// Builds a snapshot from the observations that are actually available.
/// `guest` and `app` default to the honest "not observed / not requested".
[[nodiscard]] EngineSnapshot make_engine_snapshot(const WaydroidStatus& status, bool owned);
[[nodiscard]] EngineSnapshot make_engine_snapshot(const WaydroidStatus& status, bool owned,
                                                 const GuestStatus& guest);

}  // namespace bonk::android
