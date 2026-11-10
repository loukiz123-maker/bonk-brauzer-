#include <chrono>
#include <string>
#include <string_view>

#include "bonk/android/engine_state.hpp"
#include "test_framework.hpp"

using namespace bonk;
using namespace bonk::android;

namespace {

EngineSnapshot booted_snapshot() {
  RuntimeStatus status;
  status.session = RuntimeState::Running;
  status.container = RuntimeState::Running;
  GuestStatus guest;
  guest.state = GuestState::BootComplete;
  guest.serial = "192.168.240.112:5555";
  EngineSnapshot snapshot = make_engine_snapshot(status, true, guest);
  return snapshot;
}

}  // namespace

BONK_TEST(engine_state, runtime_running_is_not_guest_ready) {
  RuntimeStatus status;
  status.session = RuntimeState::Running;
  status.container = RuntimeState::Running;

  const EngineSnapshot snapshot = make_engine_snapshot(status, /*owned=*/false);

  CHECK(snapshot.runtime.session_running());
  CHECK_EQ(snapshot.runtime.owned, false);
  CHECK_EQ(snapshot.guest.observed, false);
  CHECK(!guest_ready_for_app_actions(snapshot));
  CHECK(!app_is_running(snapshot));
}

BONK_TEST(engine_state, unobserved_guest_never_renders_as_a_state) {
  RuntimeStatus status;
  status.session = RuntimeState::Running;
  EngineSnapshot snapshot = make_engine_snapshot(status, false);
  // Even a caller that sets a state must not have it rendered as observed.
  snapshot.guest.state = GuestState::BootComplete;

  const std::string rendered = render_engine_snapshot(snapshot);

  CHECK(rendered.find("engine guest=unknown observed=no") != std::string::npos);
  CHECK(rendered.find("boot-complete") == std::string::npos);
  CHECK(!guest_ready_for_app_actions(snapshot));
}

BONK_TEST(engine_state, boot_complete_guest_is_ready_for_app_actions) {
  const EngineSnapshot snapshot = booted_snapshot();

  CHECK(guest_ready_for_app_actions(snapshot));
  CHECK_EQ(snapshot.guest.serial, std::string("192.168.240.112:5555"));
}

BONK_TEST(engine_state, guest_ready_requires_a_running_owned_session_state) {
  EngineSnapshot snapshot = booted_snapshot();
  snapshot.runtime.session = RuntimeState::Stopped;

  CHECK(snapshot.guest.observed);
  CHECK(!guest_ready_for_app_actions(snapshot));
}

BONK_TEST(engine_state, successful_launch_command_alone_is_not_a_running_app) {
  EngineSnapshot snapshot = booted_snapshot();
  snapshot.app.package_id = "com.example.game";
  snapshot.app.phase = AppPhase::LaunchRequested;
  CHECK(!app_is_running(snapshot));

  snapshot.app.phase = AppPhase::LaunchCommandSucceeded;
  // The command returned 0, but nothing inside Android witnessed the app.
  CHECK(!app_is_running(snapshot));

  snapshot.app.foreground_confirmed = true;
  CHECK(app_is_running(snapshot));
}

BONK_TEST(engine_state, install_success_is_not_a_running_app) {
  EngineSnapshot snapshot = booted_snapshot();
  snapshot.app.package_id = "com.example.game";
  snapshot.app.phase = AppPhase::InstallCommandSucceeded;
  snapshot.app.foreground_confirmed = true;  // even if a caller over-sets this

  CHECK(!app_is_running(snapshot));
}

BONK_TEST(engine_state, app_is_running_requires_booted_guest) {
  EngineSnapshot snapshot = booted_snapshot();
  snapshot.app.phase = AppPhase::LaunchCommandSucceeded;
  snapshot.app.foreground_confirmed = true;
  CHECK(app_is_running(snapshot));

  snapshot.guest.state = GuestState::Booting;
  CHECK(!app_is_running(snapshot));
}

BONK_TEST(engine_state, input_command_success_is_not_guest_observation) {
  EngineSnapshot snapshot = booted_snapshot();
  snapshot.input.command_sent = true;

  const std::string rendered = render_engine_snapshot(snapshot);

  CHECK(rendered.find("engine input sent=yes observed-in-guest=no") != std::string::npos);
}

BONK_TEST(engine_state, frames_are_never_implied_by_a_running_guest) {
  const EngineSnapshot snapshot = booted_snapshot();

  CHECK(snapshot.frames == FrameKind::Unavailable);
  CHECK(render_engine_snapshot(snapshot).find("engine frames=unavailable") != std::string::npos);
}

BONK_TEST(engine_state, rendered_snapshot_is_line_oriented_and_greppable) {
  const std::string rendered = render_engine_snapshot(booted_snapshot());

  CHECK(rendered.find("engine runtime=running container=running owned=yes\n") != std::string::npos);
  CHECK(rendered.find("engine guest=boot-complete observed=yes serial=192.168.240.112:5555\n") !=
        std::string::npos);
  CHECK(rendered.find("engine app=not-requested package=- foreground-confirmed=no\n") != std::string::npos);
  CHECK(rendered.find("engine frames=unavailable\n") != std::string::npos);
  // Every line starts with the stable prefix used by the runbook's grep steps.
  std::size_t line_start = 0;
  while (line_start < rendered.size()) {
    CHECK_EQ(rendered.compare(line_start, 7, "engine "), 0);
    const std::size_t newline = rendered.find('\n', line_start);
    if (newline == std::string::npos) break;
    line_start = newline + 1;
  }
}

BONK_TEST(engine_state, unknown_values_render_without_blank_tokens) {
  RuntimeStatus status;  // both unknown
  EngineSnapshot snapshot = make_engine_snapshot(status, false);

  const std::string rendered = render_engine_snapshot(snapshot);

  CHECK(rendered.find("engine runtime=unknown container=unknown owned=no\n") != std::string::npos);
  CHECK(rendered.find("serial= \n") == std::string::npos);
  CHECK(rendered.find("serial=-\n") != std::string::npos);
}

BONK_TEST(engine_state, detail_lines_are_kept_out_of_state_tokens) {
  RuntimeStatus status;
  status.session = RuntimeState::Running;
  GuestStatus guest;
  guest.state = GuestState::BootComplete;
  guest.detail = "Android reports sys.boot_completed=1; app/game readiness is not checked";
  EngineSnapshot snapshot = make_engine_snapshot(status, false, guest);

  CHECK(render_engine_snapshot(snapshot).find(
            "engine detail guest=Android reports sys.boot_completed=1; app/game readiness is not checked\n") !=
        std::string::npos);
}

BONK_TEST(engine_state, app_phase_names_are_stable) {
  CHECK_EQ(to_string(AppPhase::NotRequested), std::string_view("not-requested"));
  CHECK_EQ(to_string(AppPhase::InstallCommandSucceeded), std::string_view("install-command-succeeded"));
  CHECK_EQ(to_string(AppPhase::LaunchCommandSucceeded), std::string_view("launch-command-succeeded"));
  CHECK_EQ(to_string(AppPhase::LaunchCommandFailed), std::string_view("launch-command-failed"));
  CHECK_EQ(to_string(FrameKind::StillSnapshot), std::string_view("still-snapshot"));
  CHECK_EQ(to_string(FrameKind::Streaming), std::string_view("streaming"));
}
