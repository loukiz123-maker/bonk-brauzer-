"""The backend contract every Android adapter implements (stage 4).

A backend must be able to answer, *truthfully*:

* what it can do on this host (``Capabilities``), including what it cannot;
* what it is doing right now (``State``) with four independent readiness
  levels - process, container, Android, graphics/input;
* how to start, stop, and recover;
* how to change viewport properties;
* how to install an APK.

Anything the adapter cannot do is reported as ``UNSUPPORTED`` with a reason,
and the GUI must not draw a working control for it.
"""

from __future__ import annotations

import abc
from dataclasses import dataclass, field
from enum import Enum

from lynxdroid.core.diagnostics import Report
from lynxdroid.core.errors import ExitCode, LynxError


class Readiness(str, Enum):
    """Per-layer readiness. A running process is *not* evidence of Android."""

    UNKNOWN = "unknown"
    NOT_READY = "not-ready"
    READY = "ready"
    FAILED = "failed"

    @property
    def is_ready(self) -> bool:
        return self is Readiness.READY


class Support(str, Enum):
    SUPPORTED = "supported"
    CONDITIONAL = "conditional"
    UNSUPPORTED = "unsupported"
    UNTESTED = "untested"


@dataclass(frozen=True, slots=True)
class Feature:
    """One controllable capability of a backend."""

    key: str
    title: str
    support: Support
    reason: str = ""
    #: True when using it changes system state and needs explicit consent.
    needs_consent: bool = False

    @property
    def enabled(self) -> bool:
        return self.support in (Support.SUPPORTED, Support.CONDITIONAL)


@dataclass(frozen=True, slots=True)
class BackendCapabilities:
    """Everything the GUI needs to render honest controls."""

    name: str
    version: str
    features: tuple[Feature, ...] = ()
    #: Maximum simultaneous touch contacts the *backend path* can carry.
    max_touches: int = 0
    #: Platform notes shown verbatim (e.g. Wayland embedding limits).
    notes: tuple[str, ...] = ()

    def feature(self, key: str) -> Feature | None:
        return next((f for f in self.features if f.key == key), None)

    def supports(self, key: str) -> bool:
        found = self.feature(key)
        return bool(found and found.enabled)

    def status_of(self, key: str) -> str:
        found = self.feature(key)
        if found is None:
            return "unknown"
        return found.support.value

    def as_dict(self) -> dict[str, object]:
        return {
            "name": self.name,
            "version": self.version,
            "max_touches": self.max_touches,
            "notes": list(self.notes),
            "features": [
                {
                    "key": f.key,
                    "title": f.title,
                    "support": f.support.value,
                    "reason": f.reason,
                    "needs_consent": f.needs_consent,
                }
                for f in self.features
            ],
        }


class BackendState(str, Enum):
    """Lifecycle states from stage 4: created -> starting -> running -> ..."""

    NOT_CONFIGURED = "not-configured"
    STOPPED = "stopped"
    STARTING = "starting"
    RUNNING = "running"
    STOPPING = "stopping"
    ERROR = "error"

    @property
    def is_transitional(self) -> bool:
        return self in (BackendState.STARTING, BackendState.STOPPING)

    @property
    def is_active(self) -> bool:
        return self in (BackendState.RUNNING, BackendState.STARTING)


@dataclass(frozen=True, slots=True)
class Snapshot:
    """Point-in-time view of the backend, safe to hand to the UI thread."""

    state: BackendState
    readiness: dict[str, Readiness] = field(default_factory=dict)
    detail: str = ""
    since_ms: int = 0
    error: str | None = None
    raw_status: dict[str, str] = field(default_factory=dict)

    def ready(self, layer: str) -> bool:
        return self.readiness.get(layer, Readiness.UNKNOWN).is_ready

    def as_dict(self) -> dict[str, object]:
        return {
            "state": self.state.value,
            "readiness": {k: v.value for k, v in self.readiness.items()},
            "detail": self.detail,
            "since_ms": self.since_ms,
            "error": self.error,
            "raw_status": self.raw_status,
        }


class BackendError(LynxError):
    """A backend operation failed, with the reason the UI should display.

    It derives from ``LynxError`` so the CLI maps it to a documented exit code
    and the JSON error envelope, while still letting the GUI catch exactly
    this class.
    """

    code = "backend-error"
    exit_code = ExitCode.FAIL

    def __init__(
        self, message: str, *, code: str = "backend-error", hint: str | None = None
    ) -> None:
        super().__init__(message, hint=hint)
        if code != "backend-error":
            self.code = code
        if code in ("not-installed", "adb-missing"):
            self.exit_code = ExitCode.MISSING_DEPENDENCY
        elif code in ("consent-required",):
            self.exit_code = ExitCode.BLOCKED
        elif code in ("permission-denied",):
            self.exit_code = ExitCode.PERMISSION


class AndroidBackend(abc.ABC):
    """Interface used by the GUI/CLI; one implementation per backend kind."""

    # -- identity --------------------------------------------------------
    @property
    @abc.abstractmethod
    def name(self) -> str: ...

    @abc.abstractmethod
    def capabilities(self) -> BackendCapabilities:
        """Detect and describe. Must be safe to call without a session."""

    @abc.abstractmethod
    def diagnose(self) -> Report:
        """Backend-specific diagnostics (reuses doctor checks)."""

    # -- lifecycle -------------------------------------------------------
    @abc.abstractmethod
    def snapshot(self) -> Snapshot:
        """Current state. Must never block longer than a few hundred ms."""

    @abc.abstractmethod
    def start(self, *, timeout_s: float = 90.0) -> Snapshot:
        """Bring Android up to RUNNING; raises ``BackendError`` on failure."""

    @abc.abstractmethod
    def stop(self, *, timeout_s: float = 45.0) -> Snapshot:
        """Stop the session and container, releasing input first."""

    # -- viewport --------------------------------------------------------
    @abc.abstractmethod
    def apply_display(self, width: int, height: int, dpi: int) -> list[str]:
        """Apply a resolution; returns the list of human-readable steps."""

    @abc.abstractmethod
    def read_display(self) -> dict[str, str]:
        """Currently effective values, as reported by the backend."""

    # -- applications ----------------------------------------------------
    @abc.abstractmethod
    def list_apps(self) -> list[dict[str, str]]:
        """Installed packages (may be empty when unsupported)."""

    @abc.abstractmethod
    def launch_app(self, package: str) -> bool:
        """Launch one package by name."""

    @abc.abstractmethod
    def install_apk(self, path: str) -> tuple[bool, str]:
        """Install a user-chosen APK; returns ``(ok, message)``."""

    # -- properties ------------------------------------------------------
    @abc.abstractmethod
    def get_prop(self, name: str) -> str | None:
        """Read one backend/Android property."""

    @abc.abstractmethod
    def set_prop(self, name: str, value: str, *, consent: bool = False) -> bool:
        """Write a property. Requires ``consent=True`` for persistent values."""

    # -- windowing -------------------------------------------------------
    @abc.abstractmethod
    def show_full_ui(self) -> bool:
        """Ask the backend to present the Android UI in its own window."""
