"""Waydroid adapter: our control layer around the installed Waydroid.

What this file is:
    a *client* of Waydroid's public CLI (``waydroid``, ``systemctl``,
    ``waydroid prop``), plus a contract that tells the truth about what each
    call can and cannot do on a given host.

What it is not:
    a re-installer of Waydroid, a fork, or something that silently edits
    ``/var/lib/waydroid``. Every mutating call needs a session that the user
    started, and every persistent property write requires a
    :class:`~lynxdroid.backend.contract.Consent` - an object that only
    :func:`~lynxdroid.backend.contract.user_consented` can create, so a caller
    cannot claim agreement on the user's behalf by passing ``True``.

Parsing rules are deliberately forgiving: Waydroid's ``status`` output has
changed format between releases, so we parse key: value pairs and never depend
on column positions.
"""

from __future__ import annotations

import re
import subprocess
import time
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

from lynxdroid.backend.contract import (
    AndroidBackend,
    BackendCapabilities,
    BackendError,
    BackendState,
    Consent,
    Feature,
    Readiness,
    Snapshot,
    Support,
)
from lynxdroid.backend.lifecycle import Lifecycle
from lynxdroid.core import paths, proc
from lynxdroid.core.diagnostics import Report
from lynxdroid.core.logging_setup import get
from lynxdroid.doctor import checks as doctor_checks
from lynxdroid.doctor import platform as pf

log = get("backend.waydroid")

READY_MARKERS = ("boot_completed=1", "sys.boot_completed")
POLL_INTERVAL_S = 0.5


@dataclass
class BackendConfig:
    """Everything the adapter is allowed to touch, with sane defaults."""

    binary: str | None = None
    service_unit: str = "waydroid-container.service"
    status_timeout_s: float = 10.0
    #: Starting our own *user* session is allowed by default: it runs as the
    #: user and is the normal way a session comes up.
    allow_session_start: bool = True
    #: Starting the root-owned container unit is **not** allowed by default.
    #: Settings has promised this ("root actions require opt-in") since the
    #: beginning while the code called `systemctl start` regardless.
    allow_service_control: bool = False
    #: Whether this process may write persistent Android properties at all.
    #: Settings has promised a separate opt-in for this; a `Consent` says the
    #: user agreed *now*, this says the feature is enabled *at all*.
    allow_prop_writes: bool = False
    image_dir: Path = Path("/var/lib/waydroid/images")


def config_for(settings: object) -> BackendConfig:
    """Build the adapter configuration from the user's settings.

    This lives in ``backend`` because the dependency rule forbids
    ``settings`` from importing ``backend``. Keeping it here also means the
    defaults in :class:`BackendConfig` and the fields in ``Settings`` are read
    in one place instead of each call site inventing its own config - which
    is how ``allow_service_control`` came to be saved and ignored.
    """
    return BackendConfig(
        allow_service_control=bool(getattr(settings, "allow_service_control", False)),
        allow_prop_writes=bool(getattr(settings, "allow_prop_writes", False)),
        allow_session_start=bool(getattr(settings, "allow_session_start", True)),
    )


class WaydroidBackend(AndroidBackend):
    """Concrete adapter. Construct once, drive from a worker thread."""

    def __init__(self, config: BackendConfig | None = None) -> None:
        self._config = config or BackendConfig()
        self._lifecycle = Lifecycle()
        self._capabilities: BackendCapabilities | None = None
        #: Set when we started the session ourselves, so stop() can reap it.
        self._session_proc: subprocess.Popen[bytes] | None = None
        #: Cache of the last boot probe, so a 2 s status poll does not spawn a
        #: subprocess every time. ``(value, monotonic_timestamp)``.
        self._boot_cache: tuple[bool, float] = (False, 0.0)
        self._boot_cache_ttl_s = 1.0
        #: True only while this instance is performing its own start sequence.
        self._start_in_progress = False

    # ------------------------------------------------------------------
    # Identity / capabilities
    # ------------------------------------------------------------------
    @property
    def name(self) -> str:
        return "waydroid"

    @property
    def lifecycle(self) -> Lifecycle:
        return self._lifecycle

    def binary(self) -> str | None:
        return self._config.binary or proc.which("waydroid")

    def capabilities(self) -> BackendCapabilities:
        if self._capabilities is not None:
            return self._capabilities
        binary = self.binary()
        install = pf.waydroid_install()
        version = install.version or install.package_version or "unknown"

        features: list[Feature] = []
        notes: list[str] = []

        if binary is None:
            features.append(
                Feature(
                    "binary",
                    "Waydroid CLI",
                    Support.UNSUPPORTED,
                    "waydroid is not installed on this host",
                )
            )
            self._capabilities = BackendCapabilities(
                "waydroid", "not-installed", tuple(features), 0
            )
            return self._capabilities

        features.append(Feature("binary", "Waydroid CLI", Support.SUPPORTED, version))
        features.append(
            Feature(
                "session",
                "Start/stop Android session",
                Support.SUPPORTED,
                "uses 'waydroid session start|stop'",
            )
        )
        features.append(
            Feature(
                "container-service",
                "Container service control",
                Support.CONDITIONAL,
                "stopping/starting waydroid-container.service needs root; "
                "unless the user allows it, an already running unit is reused",
                needs_consent=True,
            )
        )
        features.append(
            Feature(
                "display-props",
                "Viewport resolution",
                Support.CONDITIONAL,
                "persist.waydroid.width/height are applied by Waydroid when the "
                "session restarts; 'wm size' is a live logical override that some "
                "apps ignore",
                needs_consent=True,
            )
        )
        features.append(
            Feature(
                "density",
                "DPI override",
                Support.CONDITIONAL,
                "applied live through 'waydroid shell wm density'; a logical override, "
                "not a change of the video buffer",
                needs_consent=True,
            )
        )
        features.append(
            Feature(
                "multi-windows",
                "Per-app windows",
                Support.CONDITIONAL,
                "persist.waydroid.multi_windows requires a container restart and is "
                "NOT several independent containers",
                needs_consent=True,
            )
        )
        features.append(
            Feature(
                "fake-touch",
                "Mouse-as-touch helper",
                Support.CONDITIONAL,
                "persist.waydroid.fake_touch turns mouse input into a single touch for "
                "listed packages; it does not provide simultaneous multitouch",
                needs_consent=True,
            )
        )
        features.append(
            Feature("app-list", "Installed app list", Support.SUPPORTED, "'waydroid app list'")
        )
        features.append(
            Feature(
                "app-launch", "Launch app", Support.SUPPORTED, "'waydroid app launch <package>'"
            )
        )
        features.append(
            Feature(
                "apk-install",
                "Install APK",
                Support.SUPPORTED,
                "'waydroid app install <file>' (single APK only)",
            )
        )
        features.append(
            Feature(
                "split-apk",
                "Split APK / AAB",
                Support.UNSUPPORTED,
                "not implemented: requires its own installer path and tests",
            )
        )
        features.append(
            Feature(
                "multi-instance",
                "Independent instances",
                Support.UNTESTED,
                "Waydroid does not document isolated multi-container operation; "
                "multiple windows are not multiple instances",
            )
        )
        features.append(
            Feature(
                "arm-translation",
                "ARM (arm64) apps on x86_64",
                Support.UNTESTED,
                "depends on the image and a licensed translator; not bundled here",
            )
        )
        features.append(
            Feature(
                "networking",
                "Container networking",
                Support.CONDITIONAL,
                "Waydroid uses its own NAT bridge; toggling it needs root",
                needs_consent=True,
            )
        )

        notes.append(
            "Waydroid renders into a Wayland surface it owns. Embedding that foreign "
            "surface inside a Qt window is not possible on KWin; LynxDroid opens the "
            "Android window itself and applies geometry/input transforms to it."
        )
        notes.append(
            "Android runs in a container sharing the host kernel: isolation is weaker "
            "than a VM and privileged services need explicit consent."
        )
        self._capabilities = BackendCapabilities(
            name="waydroid",
            version=version,
            features=tuple(features),
            max_touches=1,  # honest: the CLI path is single-contact
            notes=tuple(notes),
        )
        return self._capabilities

    def diagnose(self) -> Report:
        report = doctor_checks.waydroid_report()
        report.meta["backend"] = "waydroid"
        report.meta["version"] = self.capabilities().version
        return report

    # ------------------------------------------------------------------
    # CLI helpers
    # ------------------------------------------------------------------
    def _run(self, args: list[str], *, timeout: float | None = None) -> proc.RunResult:
        binary = self.binary()
        if binary is None:
            raise BackendError(
                "waydroid is not installed",
                code="not-installed",
                hint="install with 'pacman -S waydroid'",
            )
        return proc.run([binary, *args], timeout=timeout or self._config.status_timeout_s)

    def _shell(self, command: str, *, timeout: float = 20.0) -> proc.RunResult:
        """Run one command inside Android. ``command`` is a single trusted literal."""
        if not command or "\x00" in command:
            raise BackendError("invalid shell command", code="invalid-command")
        return self._run(["shell", *command.split()], timeout=timeout)

    def shell_probe(self, command: Sequence[str], *, timeout: float = 10.0) -> tuple[int, str]:
        """Run one command inside Android; return ``(exit code, output)``.

        Part of :class:`lynxdroid.backend.compat.CompatTarget`. The arguments
        arrive as a list so that no caller can smuggle a shell expression in a
        string: Waydroid's own ``shell`` passes them to ``sh`` and we do not
        add a second interpretation on top.
        """
        if not command or any("\x00" in part for part in command):
            raise BackendError("invalid shell command", code="invalid-command")
        result = self._run(["shell", *command], timeout=timeout)
        output = result.stdout.strip() or result.stderr.strip() or result.error or ""
        return result.returncode, output

    def _systemctl(self, *args: str, timeout: float = 20.0) -> proc.RunResult:
        return proc.run(["systemctl", *args], timeout=timeout)

    # ------------------------------------------------------------------
    # Status
    # ------------------------------------------------------------------
    def read_status(self) -> dict[str, str]:
        """Parse ``waydroid status`` into a dict. Empty when unavailable."""
        binary = self.binary()
        if binary is None:
            return {}
        result = proc.run([binary, "status"], timeout=self._config.status_timeout_s)
        if result.error:
            return {"__error__": result.error}
        parsed: dict[str, str] = {}
        for line in result.stdout.splitlines():
            match = re.match(r"^\s*([A-Za-z][\w \-]*?)\s*:\s*(.+?)\s*$", line)
            if match:
                parsed[match.group(1).strip().lower().replace(" ", "_")] = match.group(2)
        if result.returncode != 0:
            parsed.setdefault(
                "__error__", result.stderr.strip() or f"waydroid status exited {result.returncode}"
            )
        return parsed

    def snapshot(self) -> Snapshot:
        status = self.read_status()
        self._lifecycle.set_raw(status)

        session_running = _norm(status.get("session")) == "running"
        container_running = _norm(status.get("container")) == "running"

        # A running session process is NOT proof that Android booted (stage 4).
        # We only claim the Android layer is ready when the guest says so.
        android_ready = session_running and self.boot_completed()

        state = self._derive_state(status, android_ready=android_ready)
        current = self._lifecycle.state
        if current is not state:
            self._move_to(state, self._explain(state, status, android_ready=android_ready))

        self._lifecycle.set_readiness("process", Readiness.READY if status else Readiness.FAILED)
        self._lifecycle.set_readiness(
            "container", Readiness.READY if container_running else Readiness.NOT_READY
        )
        if android_ready:
            self._lifecycle.set_readiness("android", Readiness.READY)
        elif session_running:
            # Booting, not failed: this is the state the UI shows as "starting".
            self._lifecycle.set_readiness("android", Readiness.NOT_READY)
        else:
            self._lifecycle.set_readiness("android", Readiness.NOT_READY)
        return self._lifecycle.snapshot()

    def _derive_state(self, status: dict[str, str], *, android_ready: bool) -> BackendState:
        if "__error__" in status and not status.get("session"):
            return BackendState.ERROR
        if not status:
            return BackendState.NOT_CONFIGURED if self.binary() is None else BackendState.STOPPED
        session = _norm(status.get("session"))
        container = _norm(status.get("container"))
        if session == "running":
            # Session process is up; Android counts as running only after it
            # reports boot completion, otherwise we are still starting.
            return BackendState.RUNNING if android_ready else BackendState.STARTING
        if session in ("starting",) or container == "starting":
            return BackendState.STARTING
        if session in ("stopping",):
            return BackendState.STOPPING
        return BackendState.STOPPED

    @staticmethod
    def _explain(state: BackendState, status: dict[str, str], *, android_ready: bool = True) -> str:
        if state is BackendState.RUNNING:
            user = status.get("session_user", "")
            return f"session running and Android booted{', user ' + user if user else ''}"
        if state is BackendState.STARTING and not android_ready and status.get("session"):
            return "session is up, Android has not reported boot completion yet"
        if state is BackendState.ERROR:
            return status.get("__error__", "backend reported an error")
        return (
            f"session {status.get('session', 'unknown')}, "
            f"container {status.get('container', 'unknown')}"
        )

    def _move_to(self, state: BackendState, reason: str) -> None:
        try:
            self._lifecycle.transition(state, reason)
        except ValueError:
            # Status polls can race a manual transition; log instead of crashing
            # the poller, and keep the machine's own state.
            log.debug("ignoring out-of-band state %s: %s", state.value, reason)

    # ------------------------------------------------------------------
    # Lifecycle
    # ------------------------------------------------------------------
    def start(self, *, timeout_s: float = 90.0) -> Snapshot:
        if self.binary() is None:
            raise BackendError("waydroid is not installed", code="not-installed")
        snap = self.snapshot()
        if snap.state is BackendState.RUNNING:
            return snap
        if snap.state is BackendState.STARTING and self._start_in_progress:
            raise BackendError("a start is already in progress", code="already-starting")

        # A session that is only booting is an external state, not our start:
        # recording it as STARTING here is correct and idempotent.
        self._start_in_progress = True
        self._lifecycle.transition(BackendState.STARTING, "user requested start")
        self._lifecycle.set_readiness("process", Readiness.READY)
        self._lifecycle.set_readiness("container", Readiness.NOT_READY)
        self._lifecycle.set_readiness("android", Readiness.NOT_READY)

        deadline = time.monotonic() + timeout_s
        try:
            self._ensure_container()
            self._ensure_session()
            ready = self._wait_boot(deadline)
        except BackendError as exc:
            self._start_in_progress = False
            self._lifecycle.fail(str(exc))
            raise

        if not ready:
            message = (
                f"Android did not report boot completion within {timeout_s:.0f}s "
                "(Waydroid session may be running while Android is still booting)"
            )
            self._start_in_progress = False
            self._lifecycle.fail(message)
            raise BackendError(
                message,
                code="boot-timeout",
                hint="check 'journalctl -u waydroid-container.service'",
            )

        self._boot_cache = (True, time.monotonic())
        self._start_in_progress = False
        self._lifecycle.set_readiness("android", Readiness.READY)
        self._lifecycle.transition(BackendState.RUNNING, "Android reported boot completed")

        if self._lifecycle.stop_after_start:
            self._lifecycle.request_stop_during_start()
            return self.stop()
        return self._lifecycle.snapshot()

    def _ensure_container(self) -> None:
        state, detail = pf.systemd_state(self._config.service_unit)
        if state == "active":
            self._lifecycle.set_readiness("container", Readiness.READY)
            return
        if state == "unknown":
            raise BackendError(
                f"cannot determine container service state ({detail})",
                code="service-unknown",
            )
        # Starting the unit needs root, and the user has to have said yes:
        # `allow_service_control` was a setting nothing read, so the promise
        # in SECURITY.md ("root actions require opt-in") was not kept by the
        # code. Without the opt-in the user is told the exact command instead.
        if not self._config.allow_service_control:
            raise BackendError(
                f"container service is {state}; starting it needs root and is not enabled",
                code="service-control-disabled",
                hint=f"run 'systemctl start {self._config.service_unit}' yourself, "
                "or enable allow_service_control in the settings",
            )
        # We do not escalate silently: we try the non-interactive path and
        # report precisely why it failed.
        result = self._systemctl("start", self._config.service_unit)
        if result.ok:
            self._lifecycle.set_readiness("container", Readiness.READY)
            return
        raise BackendError(
            f"container service is {state} and could not be started "
            f"({result.stderr.strip() or result.returncode})",
            code="container-start-failed",
            hint="run 'systemctl start waydroid-container.service' with the required "
            "privileges, then retry; LynxDroid never changes the unit file",
        )

    def _ensure_session(self) -> None:
        """Start *our own* user session if none is running."""
        status = self.read_status()
        if _norm(status.get("session")) == "running":
            return
        if not self._config.allow_session_start:
            raise BackendError(
                "session startup is disabled by configuration", code="session-start-disabled"
            )
        binary = self.binary()
        if binary is None:  # pragma: no cover - guarded by start()
            raise BackendError("waydroid is not installed", code="not-installed")
        # The session log belongs to *us*, not to Waydroid: /var/lib/waydroid
        # is root-owned, so writing there fails for the normal user this app
        # runs as (stage 14: nothing is written outside the XDG directories).
        # The failure was a start that could never work, reported as
        # "cannot spawn waydroid session".
        log_path = paths.state_home() / "waydroid-session.log"
        try:
            log_path.parent.mkdir(parents=True, exist_ok=True)
            self._session_proc = proc.spawn([binary, "session", "start"], stdout_path=log_path)
        except OSError as exc:
            raise BackendError(
                f"cannot spawn waydroid session: {exc}", code="session-spawn-failed"
            ) from exc
        self._lifecycle.set_readiness("container", Readiness.READY)

    def _wait_boot(self, deadline: float) -> bool:
        """Poll until Android reports boot completion or the deadline passes.

        Waiting is the longest silent stretch of a start: a cold boot takes
        tens of seconds, and without a running commentary the window shows
        "user requested start" the whole time, which is indistinguishable
        from a hung start. Each phase names what is being waited for.
        """
        phase = "waiting for the Waydroid session to start"
        self._lifecycle.set_detail(phase)
        while time.monotonic() < deadline:
            status = self.read_status()
            self._lifecycle.set_raw(status)
            if _norm(status.get("session")) == "running":
                self._lifecycle.set_readiness("container", Readiness.READY)
                if phase != "waiting for Android to report boot completed":
                    phase = "waiting for Android to report boot completed"
                    self._lifecycle.set_detail(phase)
                if self.boot_completed(use_cache=False):
                    return True
            time.sleep(POLL_INTERVAL_S)
        return False

    def boot_completed(self, *, use_cache: bool = True) -> bool:
        """Ask Android whether it finished booting. Cached for a short period.

        Two properties are checked because images differ: ``sys.boot_completed``
        (standard) and ``dev.bootcomplete`` (older images).
        """
        now = time.monotonic()
        cached_value, cached_at = self._boot_cache
        if use_cache and now - cached_at < self._boot_cache_ttl_s:
            return cached_value
        value = False
        result = self._shell("getprop sys.boot_completed", timeout=10.0)
        if result.ok and result.stdout.strip() == "1":
            value = True
        else:
            fallback = self._shell("getprop dev.bootcomplete", timeout=10.0)
            value = bool(fallback.ok and fallback.stdout.strip() == "1")
        self._boot_cache = (value, now)
        return value

    def stop(self, *, timeout_s: float = 45.0) -> Snapshot:
        snap = self.snapshot()
        if snap.state in (BackendState.STOPPED, BackendState.NOT_CONFIGURED):
            return snap
        if snap.state is BackendState.STARTING:
            self._lifecycle.request_stop_during_start()
            return self._lifecycle.snapshot()

        self._lifecycle.transition(BackendState.STOPPING, "user requested stop")
        self._boot_cache = (False, 0.0)  # the guest is going away
        try:
            result = self._run(["session", "stop"], timeout=timeout_s)
            if not result.ok and result.returncode not in (0,):
                log.warning("waydroid session stop: %s", result.describe())
        finally:
            if self._session_proc is not None:
                proc.terminate_tree(self._session_proc)
                self._session_proc = None

        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            status = self.read_status()
            if _norm(status.get("session")) in ("stopped", "unknown", ""):
                self._lifecycle.set_readiness("android", Readiness.NOT_READY)
                self._lifecycle.set_readiness("container", Readiness.NOT_READY)
                self._lifecycle.transition(BackendState.STOPPED, "session stopped")
                return self._lifecycle.snapshot()
            time.sleep(POLL_INTERVAL_S)

        message = f"session did not stop within {timeout_s:.0f}s"
        self._lifecycle.fail(message)
        raise BackendError(message, code="stop-timeout")

    # ------------------------------------------------------------------
    # Display
    # ------------------------------------------------------------------
    def apply_display(
        self, width: int, height: int, dpi: int, *, consent: Consent | None = None
    ) -> list[str]:
        """Apply resolution/DPI through the documented Waydroid properties.

        The order matters and is reported to the user verbatim:

        1. remember current values (so we can roll back);
        2. set ``persist.waydroid.width`` / ``height`` (session-level);
        3. set density through ``wm density`` (live);
        4. tell the caller a session restart is required for the buffer size.
        """
        if width <= 0 or height <= 0 or dpi <= 0:
            raise BackendError("width, height and dpi must be positive", code="invalid-display")
        # Refuse before reading anything. Checking at the first set_prop (the
        # previous behaviour) meant an unauthorised call still ran `prop get`
        # twice and put "1280 -> 1920" in the steps it hands the user, which
        # reads like a completed change.
        if not isinstance(consent, Consent):
            raise BackendError(
                "changing the viewport writes persistent Android properties",
                code="consent-required",
                hint="ask the user first, then pass what user_consented returned",
            )
        steps: list[str] = []

        for key, value in (
            ("persist.waydroid.width", str(width)),
            ("persist.waydroid.height", str(height)),
        ):
            previous = self.get_prop(key)
            steps.append(f"{key}: {previous or '<unset>'} -> {value}")
            if not self.set_prop(key, value, consent=consent):
                raise BackendError(f"failed to set {key}", code="prop-failed")

        density = self._shell(f"wm density {dpi}")
        if density.ok:
            steps.append(f"wm density -> {dpi}")
        else:
            steps.append(f"wm density failed: {density.output.strip()}")

        steps.append("buffer size takes effect on the next Android session start")
        return steps

    def read_display(self) -> dict[str, str]:
        values: dict[str, str] = {}
        for key in (
            "persist.waydroid.width",
            "persist.waydroid.height",
            "persist.waydroid.multi_windows",
        ):
            value = self.get_prop(key)
            if value is not None:
                values[key] = value
        size = self._shell("wm size", timeout=10.0)
        if size.ok:
            values["wm size"] = size.stdout.strip().splitlines()[-1] if size.stdout.strip() else ""
        density = self._shell("wm density", timeout=10.0)
        if density.ok and density.stdout.strip():
            values["wm density"] = density.stdout.strip().splitlines()[-1]
        return values

    # ------------------------------------------------------------------
    # Applications
    # ------------------------------------------------------------------
    def list_apps(self) -> list[dict[str, str]]:
        result = self._run(["app", "list"], timeout=30.0)
        if not result.ok:
            raise BackendError(f"cannot list apps: {result.output.strip()}", code="app-list-failed")
        apps: list[dict[str, str]] = []
        for line in result.stdout.splitlines():
            parts = line.split()
            if not parts:
                continue
            # Waydroid prints package lines; extra columns vary by version.
            package = parts[0]
            if not re.match(r"^[A-Za-z][\w.]*$", package):
                continue
            apps.append({"package": package, "label": " ".join(parts[1:]) or package})
        return apps

    def launch_app(self, package: str) -> bool:
        if not re.match(r"^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+$", package):
            raise BackendError(f"'{package}' is not a valid package name", code="invalid-package")
        result = self._run(["app", "launch", package], timeout=30.0)
        if not result.ok:
            raise BackendError(f"launch failed: {result.output.strip()}", code="launch-failed")
        return True

    def install_apk(self, path: str) -> tuple[bool, str]:
        apk = Path(path)
        if apk.suffix.lower() != ".apk":
            return False, "only .apk files can be installed (split APK/AAB unsupported)"
        if not apk.is_file():
            return False, f"{apk} is not a file"
        if apk.is_symlink():
            return False, "refusing to install through a symlink; copy the file first"
        size_mb = apk.stat().st_size / 1024**2
        if size_mb > 4096:
            return False, f"{apk.name} is {size_mb:.0f} MiB, larger than the 4 GiB limit"
        result = self._run(["app", "install", str(apk)], timeout=180.0)
        if result.ok:
            return True, f"installed {apk.name} ({size_mb:.1f} MiB)"
        return False, result.output.strip() or "install failed"

    # ------------------------------------------------------------------
    # Properties
    # ------------------------------------------------------------------
    def get_prop(self, name: str) -> str | None:
        if not _PROP_RE.match(name):
            raise BackendError(f"invalid property name '{name}'", code="invalid-prop")
        result = self._run(["prop", "get", name], timeout=10.0)
        if not result.ok:
            return None
        value = result.stdout.strip()
        return value or None

    def set_prop(self, name: str, value: str, *, consent: Consent | None = None) -> bool:
        """Write a property. Persistent writes need a :class:`Consent`.

        The parameter is an object rather than a bool so that a caller cannot
        assert the user's agreement by passing ``True``: the only way to get a
        Consent is to call :func:`user_consented` after asking.
        """
        if not _PROP_RE.match(name):
            raise BackendError(f"invalid property name '{name}'", code="invalid-prop")
        # isinstance, not "is None": the annotation is not enforced at run
        # time, so a caller passing the old `consent=True` must still be
        # refused rather than silently believed.
        if not self._config.allow_prop_writes:
            raise BackendError(
                f"writing {name} is disabled",
                code="prop-writes-disabled",
                hint="enable allow_prop_writes in the settings; a per-change "
                "confirmation is not a substitute for enabling the feature",
            )
        if not isinstance(consent, Consent):
            raise BackendError(
                f"refusing to change {name} without explicit consent",
                code="consent-required",
                hint="persistent Android properties change system behaviour and need a yes",
            )
        result = self._run(["prop", "set", name, value], timeout=15.0)
        if not result.ok:
            log.warning("prop set %s failed: %s", name, result.describe())
        return result.ok

    # ------------------------------------------------------------------
    # Windowing
    # ------------------------------------------------------------------
    def show_full_ui(self) -> bool:
        result = self._run(["show-full-ui"], timeout=20.0)
        return result.ok

    def launch_intent(self, action: str) -> bool:
        """Send a broadcast intent (used for the touch agent handshake)."""
        if not re.match(r"^[a-z]+(\.[a-zA-Z0-9_]+)+$", action):
            raise BackendError(f"invalid intent action '{action}'", code="invalid-intent")
        result = self._shell(f"am broadcast -a {action}", timeout=20.0)
        return result.ok

    def close(self) -> None:
        if self._session_proc is not None:
            proc.terminate_tree(self._session_proc)
            self._session_proc = None


_PROP_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_.]{1,63}$")


def _norm(value: str | None) -> str:
    return (value or "").strip().lower()


@dataclass
class FakeBackend(AndroidBackend):
    """Deterministic in-memory backend for tests and GUI demos.

    It exists so the UI and the state machine can be exercised without a
    Waydroid install. It is *never* presented as a working Android.
    """

    name: str = "fake"
    _state: BackendState = BackendState.STOPPED
    _apps: list[dict[str, str]] = field(default_factory=list)
    _props: dict[str, str] = field(default_factory=dict)
    _fail_start: bool = False
    _fail_boot: bool = False
    _snapshot_calls: int = 0

    def capabilities(self) -> BackendCapabilities:
        return BackendCapabilities(
            name="fake",
            version="test",
            features=(
                Feature("session", "Session", Support.SUPPORTED),
                Feature("app-list", "App list", Support.SUPPORTED),
                Feature("apk-install", "APK install", Support.SUPPORTED),
                Feature(
                    "multi-instance",
                    "Instances",
                    Support.UNSUPPORTED,
                    "test backend has no containers",
                ),
            ),
            max_touches=10,
            notes=("This is a test backend, not Android.",),
        )

    def diagnose(self) -> Report:
        return doctor_checks.environment_report()

    def snapshot(self) -> Snapshot:
        self._snapshot_calls += 1
        return Snapshot(state=self._state, readiness={}, detail="fake backend")

    def start(self, *, timeout_s: float = 90.0) -> Snapshot:
        if self._fail_start:
            self._state = BackendState.ERROR
            raise BackendError("fake backend start failure", code="fake-failure")
        if self._fail_boot:
            self._state = BackendState.ERROR
            raise BackendError("fake backend boot timeout", code="boot-timeout")
        self._state = BackendState.RUNNING
        return self.snapshot()

    def stop(self, *, timeout_s: float = 45.0) -> Snapshot:
        self._state = BackendState.STOPPED
        return self.snapshot()

    def apply_display(
        self, width: int, height: int, dpi: int, *, consent: Consent | None = None
    ) -> list[str]:
        if not isinstance(consent, Consent):
            raise BackendError(
                "changing the viewport needs explicit consent", code="consent-required"
            )
        self._props["persist.waydroid.width"] = str(width)
        self._props["persist.waydroid.height"] = str(height)
        return [f"fake: set {width}x{height}@{dpi}"]

    def read_display(self) -> dict[str, str]:
        return dict(self._props)

    def list_apps(self) -> list[dict[str, str]]:
        return list(self._apps)

    def launch_app(self, package: str) -> bool:
        return True

    def install_apk(self, path: str) -> tuple[bool, str]:
        return True, f"fake: installed {path}"

    def get_prop(self, name: str) -> str | None:
        return self._props.get(name)

    def set_prop(self, name: str, value: str, *, consent: Consent | None = None) -> bool:
        if not isinstance(consent, Consent):
            raise BackendError("consent required", code="consent-required")
        self._props[name] = value
        return True

    def shell_probe(self, command: Sequence[str], *, timeout: float = 10.0) -> tuple[int, str]:
        """No Android here, so the honest answer is "could not run it"."""
        raise BackendError(
            "the test backend has no Android shell",
            code="no-shell",
            hint="use a real backend to test the container's network",
        )

    def show_full_ui(self) -> bool:
        return True
