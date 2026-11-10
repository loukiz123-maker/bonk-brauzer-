#include "bonk/android/engine_state.hpp"

#include <string>

namespace bonk::android {
namespace {

constexpr std::string_view kAbsentToken = "-";

std::string_view token_or_absent(std::string_view value) noexcept {
  return value.empty() ? kAbsentToken : value;
}

}  // namespace

std::string_view to_string(AppPhase phase) noexcept {
  switch (phase) {
    case AppPhase::NotRequested:
      return "not-requested";
    case AppPhase::InstallRequested:
      return "install-requested";
    case AppPhase::InstallCommandSucceeded:
      return "install-command-succeeded";
    case AppPhase::InstallCommandFailed:
      return "install-command-failed";
    case AppPhase::LaunchRequested:
      return "launch-requested";
    case AppPhase::LaunchCommandSucceeded:
      return "launch-command-succeeded";
    case AppPhase::LaunchCommandFailed:
      return "launch-command-failed";
  }
  return "unknown";
}

std::string_view to_string(FrameKind kind) noexcept {
  switch (kind) {
    case FrameKind::Unavailable:
      return "unavailable";
    case FrameKind::StillSnapshot:
      return "still-snapshot";
    case FrameKind::Streaming:
      return "streaming";
  }
  return "unavailable";
}

bool guest_ready_for_app_actions(const EngineSnapshot& snapshot) noexcept {
  return snapshot.runtime.session_running() && snapshot.guest.observed &&
         snapshot.guest.state == GuestState::BootComplete;
}

bool app_is_running(const EngineSnapshot& snapshot) noexcept {
  if (!guest_ready_for_app_actions(snapshot)) return false;
  if (snapshot.app.phase != AppPhase::LaunchCommandSucceeded) return false;
  return snapshot.app.foreground_confirmed;
}

EngineSnapshot make_engine_snapshot(const WaydroidStatus& status, bool owned) {
  EngineSnapshot snapshot;
  snapshot.runtime.session = status.session;
  snapshot.runtime.container = status.container;
  snapshot.runtime.owned = owned;
  return snapshot;
}

EngineSnapshot make_engine_snapshot(const WaydroidStatus& status, bool owned, const GuestStatus& guest) {
  EngineSnapshot snapshot = make_engine_snapshot(status, owned);
  snapshot.guest.state = guest.state;
  snapshot.guest.observed = true;
  snapshot.guest.serial = guest.serial;
  snapshot.guest.detail = guest.detail;
  return snapshot;
}

std::string render_engine_snapshot(const EngineSnapshot& snapshot) {
  std::string out;

  out += "engine runtime=";
  out += to_string(snapshot.runtime.session);
  out += " container=";
  out += to_string(snapshot.runtime.container);
  out += " owned=";
  out += snapshot.runtime.owned ? "yes" : "no";
  out += '\n';

  // An unobserved guest is reported as unknown even if a caller left a
  // non-default state in the struct: "not queried" must never read as a fact.
  out += "engine guest=";
  out += snapshot.guest.observed ? to_string(snapshot.guest.state) : "unknown";
  out += " observed=";
  out += snapshot.guest.observed ? "yes" : "no";
  out += " serial=";
  out += token_or_absent(snapshot.guest.serial);
  out += '\n';

  out += "engine app=";
  out += to_string(snapshot.app.phase);
  out += " package=";
  out += token_or_absent(snapshot.app.package_id);
  out += " foreground-confirmed=";
  out += snapshot.app.foreground_confirmed ? "yes" : "no";
  out += '\n';

  out += "engine input sent=";
  out += snapshot.input.command_sent ? "yes" : "no";
  out += " observed-in-guest=";
  out += snapshot.input.observed_in_guest ? "yes" : "no";
  out += '\n';

  out += "engine frames=";
  out += to_string(snapshot.frames);
  out += '\n';

  if (!snapshot.runtime.detail.empty()) out += "engine detail runtime=" + snapshot.runtime.detail + '\n';
  if (!snapshot.guest.detail.empty()) out += "engine detail guest=" + snapshot.guest.detail + '\n';
  if (!snapshot.input.detail.empty()) out += "engine detail input=" + snapshot.input.detail + '\n';
  return out;
}

}  // namespace bonk::android
