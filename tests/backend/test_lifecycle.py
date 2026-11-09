"""State machine tests: illegal transitions, races, timeouts, error recovery."""

from __future__ import annotations

import pytest

from lynxdroid.backend.contract import BackendState, Consent, Readiness
from lynxdroid.backend.lifecycle import Lifecycle
from lynxdroid.backend.waydroid import FakeBackend


def running_lifecycle() -> Lifecycle:
    machine = Lifecycle()
    machine.transition(BackendState.STOPPED, "configured")
    machine.transition(BackendState.STARTING, "start")
    machine.transition(BackendState.RUNNING, "boot completed")
    return machine


class TestTransitions:
    def test_normal_start_stop_cycle(self) -> None:
        machine = Lifecycle()
        machine.transition(BackendState.STOPPED, "configured")
        machine.transition(BackendState.STARTING, "start")
        machine.transition(BackendState.RUNNING, "ready")
        machine.transition(BackendState.STOPPING, "stop")
        machine.transition(BackendState.STOPPED, "stopped")
        assert machine.state is BackendState.STOPPED

    def test_illegal_transition_raises(self) -> None:
        machine = Lifecycle()
        machine.transition(BackendState.STOPPED, "configured")
        with pytest.raises(ValueError, match="illegal transition"):
            machine.transition(BackendState.STOPPING, "nothing to stop")

    def test_discovered_running_session_is_accepted_once(self) -> None:
        """A session started from a terminal must not force a restart."""
        machine = Lifecycle()
        machine.transition(BackendState.RUNNING, "discovered an already running session")
        assert machine.state is BackendState.RUNNING
        with pytest.raises(ValueError):
            machine.transition(BackendState.STARTING, "no double start")

    def test_cannot_stop_when_already_stopped_via_wrong_path(self) -> None:
        machine = Lifecycle()
        machine.transition(BackendState.STOPPED, "configured")
        with pytest.raises(ValueError):
            machine.transition(BackendState.STOPPING, "nothing to stop")

    def test_error_is_recoverable(self) -> None:
        machine = Lifecycle()
        machine.transition(BackendState.STOPPED, "configured")
        machine.transition(BackendState.STARTING, "start")
        machine.fail("binder missing")
        assert machine.state is BackendState.ERROR
        assert machine.snapshot().error == "binder missing"
        machine.transition(BackendState.STARTING, "retry")
        machine.transition(BackendState.RUNNING, "ready")
        assert machine.snapshot().error is None

    def test_every_transition_is_recorded(self) -> None:
        machine = running_lifecycle()
        history = machine.history()
        assert [(t.source, t.target) for t in history] == [
            (BackendState.NOT_CONFIGURED, BackendState.STOPPED),
            (BackendState.STOPPED, BackendState.STARTING),
            (BackendState.STARTING, BackendState.RUNNING),
        ]
        assert all(t.reason for t in history)

    def test_history_is_bounded(self) -> None:
        machine = Lifecycle()
        machine.transition(BackendState.STOPPED, "configured")
        for _ in range(150):
            machine.transition(BackendState.STARTING, "x")
            machine.transition(BackendState.ERROR, "y")
            machine.transition(BackendState.STOPPED, "z")
        assert len(machine.history(limit=1000)) <= 200


class TestReadiness:
    def test_layers_are_independent(self) -> None:
        machine = Lifecycle()
        machine.set_readiness("process", Readiness.READY)
        machine.set_readiness("android", Readiness.NOT_READY)
        snapshot = machine.snapshot()
        assert snapshot.ready("process")
        assert not snapshot.ready("android")
        assert not snapshot.ready("graphics")  # unknown -> not ready

    def test_failure_marks_android_failed(self) -> None:
        machine = running_lifecycle()
        machine.fail("crashed")
        assert machine.snapshot().readiness["android"] is Readiness.FAILED


class TestObservers:
    def test_observers_receive_snapshots(self) -> None:
        seen: list[str] = []
        machine = Lifecycle()
        machine.subscribe(lambda snap: seen.append(snap.state.value))
        machine.transition(BackendState.STOPPED, "configured")
        assert seen == ["stopped"]

    def test_failing_observer_does_not_break_the_machine(self) -> None:
        machine = Lifecycle()

        def broken(snapshot):
            raise RuntimeError("observer bug")

        machine.subscribe(broken)
        machine.transition(BackendState.STOPPED, "configured")
        assert machine.state is BackendState.STOPPED

    def test_unsubscribe_stops_notifications(self) -> None:
        seen: list[str] = []
        machine = Lifecycle()
        handler = lambda snap: seen.append(snap.state.value)  # noqa: E731
        machine.subscribe(handler)
        machine.unsubscribe(handler)
        machine.transition(BackendState.STOPPED, "configured")
        assert seen == []


class TestStopDuringStart:
    def test_stop_request_is_remembered_during_start(self) -> None:
        machine = Lifecycle()
        machine.transition(BackendState.STOPPED, "configured")
        machine.transition(BackendState.STARTING, "start")
        machine.request_stop_during_start()
        assert machine.stop_after_start
        machine.transition(BackendState.RUNNING, "ready")
        assert not machine.stop_after_start  # cleared by RUNNING, caller then stops

    def test_a_stop_request_is_visible_in_the_snapshot(self) -> None:
        """Pressing Stop must not look like it did nothing.

        The backend returns this snapshot straight to the caller, so the
        explanation has to be in it.
        """
        machine = Lifecycle()
        machine.transition(BackendState.STOPPED, "configured")
        machine.transition(BackendState.STARTING, "start")
        seen: list[str] = []
        machine.subscribe(lambda snap: seen.append(snap.detail))
        machine.request_stop_during_start()
        assert "stop requested" in machine.snapshot().detail
        assert any("stop requested" in detail for detail in seen), (
            "observers were not told that the stop request was registered"
        )

    def test_stop_request_ignored_when_not_starting(self) -> None:
        machine = running_lifecycle()
        machine.request_stop_during_start()
        assert not machine.stop_after_start


class TestFakeBackend:
    def test_start_and_stop(self) -> None:
        backend = FakeBackend()
        assert backend.start().state is BackendState.RUNNING
        assert backend.stop().state is BackendState.STOPPED

    def test_start_failure_is_reported(self) -> None:
        backend = FakeBackend()
        backend._fail_start = True
        with pytest.raises(Exception, match="start failure"):
            backend.start()

    def test_boot_timeout_is_distinguishable(self) -> None:
        backend = FakeBackend()
        backend._fail_boot = True
        with pytest.raises(Exception, match="boot timeout"):
            backend.start()

    def test_capabilities_declare_unsupported_features(self) -> None:
        caps = FakeBackend().capabilities()
        assert caps.supports("session")
        assert not caps.supports("multi-instance")
        assert caps.status_of("multi-instance") == "unsupported"

    def test_prop_write_requires_consent(self) -> None:
        backend = FakeBackend()
        with pytest.raises(Exception, match="consent"):
            backend.set_prop("persist.waydroid.width", "1280")

    def test_prop_write_with_consent(self) -> None:
        backend = FakeBackend()
        assert backend.set_prop("persist.waydroid.width", "1280", consent=_consent())
        assert backend.get_prop("persist.waydroid.width") == "1280"


def _consent() -> Consent:
    """A user who agreed; only the CLI can create one in the real app."""
    from lynxdroid.backend.contract import user_consented

    return user_consented("test: change the viewport")
