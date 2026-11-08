"""Lifecycle state machine shared by every backend.

The point of a separate machine is that invalid transitions are impossible by
construction and every transition is logged, so a failed start can never be
mistaken for a running session (stage 4/5 requirement).

    not-configured --configure--> stopped
    stopped -------start-------> starting --> running
                          \\--> error          |
    running -------stop--------> stopping --> stopped
                          \\--> error

Rules enforced:

* ``start`` while ``starting`` is refused (no double starts);
* ``stop`` during ``starting`` is queued as "stop after start", so a user
  clicking Stop mid-boot is honoured instead of ignored;
* every transition carries a reason, recorded in a bounded history;
* ``error`` remembers the failure text until the next successful transition.
"""

from __future__ import annotations

import time
from collections.abc import Callable
from dataclasses import dataclass, field

from lynxdroid.backend.contract import BackendState, Readiness, Snapshot
from lynxdroid.core.logging_setup import get

log = get("backend.lifecycle")

_ALLOWED: dict[BackendState, set[BackendState]] = {
    # NOT_CONFIGURED -> RUNNING is legal on purpose: a session the user started
    # from a terminal is discovered by polling, and refusing that would force a
    # restart just to satisfy the state machine.
    BackendState.NOT_CONFIGURED: {
        BackendState.STOPPED,
        BackendState.STARTING,
        BackendState.RUNNING,
        BackendState.ERROR,
    },
    BackendState.STOPPED: {BackendState.STARTING, BackendState.ERROR, BackendState.NOT_CONFIGURED},
    BackendState.STARTING: {
        BackendState.RUNNING,
        BackendState.ERROR,
        BackendState.STOPPING,
        BackendState.STOPPED,
    },
    BackendState.RUNNING: {BackendState.STOPPING, BackendState.ERROR, BackendState.STOPPED},
    BackendState.STOPPING: {BackendState.STOPPED, BackendState.ERROR, BackendState.RUNNING},
    BackendState.ERROR: {
        BackendState.STARTING,
        BackendState.STOPPING,
        BackendState.STOPPED,
        BackendState.NOT_CONFIGURED,
    },
}


@dataclass(frozen=True)
class Transition:
    at_ms: int
    source: BackendState
    target: BackendState
    reason: str


@dataclass
class Lifecycle:
    """Owns the current state; the backend calls into it, the UI reads it."""

    _state: BackendState = BackendState.NOT_CONFIGURED
    _since_ms: int = field(default_factory=lambda: _now_ms())
    _error: str | None = None
    _detail: str = "backend not configured"
    _readiness: dict[str, Readiness] = field(default_factory=dict)
    _history: list[Transition] = field(default_factory=list)
    _observers: list[Callable[[Snapshot], None]] = field(default_factory=list)
    _stop_after_start: bool = False
    _raw: dict[str, str] = field(default_factory=dict)

    # -- inspection ------------------------------------------------------
    @property
    def state(self) -> BackendState:
        return self._state

    @property
    def stop_after_start(self) -> bool:
        return self._stop_after_start

    def history(self, limit: int = 32) -> list[Transition]:
        return self._history[-limit:]

    def snapshot(self) -> Snapshot:
        return Snapshot(
            state=self._state,
            readiness=dict(self._readiness),
            detail=self._detail,
            since_ms=self._since_ms,
            error=self._error,
            raw_status=dict(self._raw),
        )

    def subscribe(self, observer: Callable[[Snapshot], None]) -> None:
        self._observers.append(observer)

    def unsubscribe(self, observer: Callable[[Snapshot], None]) -> None:
        if observer in self._observers:
            self._observers.remove(observer)

    # -- mutation --------------------------------------------------------
    def transition(self, target: BackendState, reason: str) -> Snapshot:
        """Move to ``target`` if allowed; otherwise raise ``ValueError``."""
        if target is self._state:
            self._detail = reason or self._detail
            return self._publish()
        if target not in _ALLOWED[self._state]:
            raise ValueError(f"illegal transition {self._state.value} -> {target.value} ({reason})")
        previous = self._state
        self._state = target
        self._since_ms = _now_ms()
        self._detail = reason or target.value
        if target is BackendState.ERROR:
            self._error = reason or "unknown backend error"
        elif target in (BackendState.RUNNING, BackendState.STOPPED, BackendState.STOPPED):
            self._error = None
        self._history.append(
            Transition(at_ms=self._since_ms, source=previous, target=target, reason=reason)
        )
        if len(self._history) > 200:
            del self._history[:-200]
        if target is not BackendState.STARTING:
            self._stop_after_start = False
        log.info("state %s -> %s (%s)", previous.value, target.value, reason)
        return self._publish()

    def fail(self, message: str) -> Snapshot:
        """Convenience: move to ERROR carrying the message."""
        self.set_readiness("android", Readiness.FAILED)
        try:
            return self.transition(BackendState.ERROR, message)
        except ValueError:
            # Already in ERROR: keep the newest explanation.
            self._error = message
            self._detail = message
            return self._publish()

    def request_stop_during_start(self) -> None:
        """User pressed Stop while starting: honour it once the start finishes."""
        if self._state is BackendState.STARTING:
            self._stop_after_start = True
            self._detail = "stop requested; will stop after the start attempt finishes"

    def set_readiness(self, layer: str, value: Readiness) -> None:
        if self._readiness.get(layer) is not value:
            self._readiness[layer] = value
            self._publish()

    def set_raw(self, values: dict[str, str]) -> None:
        self._raw = dict(values)

    def set_detail(self, detail: str) -> None:
        if detail != self._detail:
            self._detail = detail
            self._publish()

    def _publish(self) -> Snapshot:
        snap = self.snapshot()
        for observer in list(self._observers):
            try:
                observer(snap)
            except Exception:
                log.exception("lifecycle observer failed")
        return snap


def _now_ms() -> int:
    return int(time.monotonic() * 1000)
