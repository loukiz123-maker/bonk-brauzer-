"""Waydroid adapter: parsing, consent gates, capability honesty.

The CLI is never executed here: a fake runner is injected so the tests assert
*our* logic (parsing, gating, error mapping) without needing Waydroid.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from lynxdroid.backend.contract import BackendError, BackendState, Consent, Readiness, Support
from lynxdroid.backend.waydroid import BackendConfig, WaydroidBackend
from lynxdroid.core import proc

STATUS_RUNNING = """\
Session:        RUNNING
Container:      RUNNING
Vendor type:    MAINLINE
Session user:   user (10000)
Wayland display:        wayland-0
IP address:     192.168.240.112
"""

STATUS_STOPPED = """\
Session:        STOPPED
Container:      STOPPED
Vendor type:    MAINLINE
"""

STATUS_EMPTY = ""


class FakeProc:
    """Replace ``proc.run`` with a scripted table."""

    def __init__(self, responses: dict[tuple[str, ...], proc.RunResult]) -> None:
        self.responses = responses
        self.calls: list[tuple[str, ...]] = []

    def run(self, argv, *, timeout=15.0, **kwargs):
        key = tuple(argv)
        self.calls.append(key)
        for pattern, result in self.responses.items():
            if key[: len(pattern)] == pattern:
                return proc.RunResult(
                    argv=key,
                    returncode=result.returncode,
                    stdout=result.stdout,
                    stderr=result.stderr,
                )
        return proc.RunResult(argv=key, returncode=1, stderr="unexpected command")


def ok(text: str = "") -> proc.RunResult:
    return proc.RunResult(argv=(), returncode=0, stdout=text)


@pytest.fixture()
def backend(monkeypatch) -> WaydroidBackend:
    """A backend whose two opt-ins are ON, so each test states one thing.

    The defaults are off - that is the product's promise - and the default
    path has its own tests. Enabling them here keeps the other 40 tests about
    parsing, not about consent.
    """
    instance = WaydroidBackend(
        BackendConfig(
            binary="/usr/bin/waydroid", allow_service_control=True, allow_prop_writes=True
        )
    )
    monkeypatch.setattr("lynxdroid.backend.waydroid.pf.waydroid_install", lambda: _install_stub())
    monkeypatch.setattr(
        "lynxdroid.backend.waydroid.pf.systemd_state", lambda _unit: ("active", "running")
    )
    return instance


def _install_stub():
    from pathlib import Path

    from lynxdroid.doctor.platform import WaydroidInstall

    return WaydroidInstall(
        binary="/usr/bin/waydroid",
        version="1.5.0",
        service_present=True,
        image_dir=Path("/var/lib/waydroid/images"),
        system_image=Path("/var/lib/waydroid/images/system.img"),
        vendor_image=Path("/var/lib/waydroid/images/vendor.img"),
        system_present=True,
        vendor_present=True,
        config_file=Path("/var/lib/waydroid/waydroid.cfg"),
        config_present=True,
        package_version="1.5.0-1",
        session_dir=Path("/var/lib/waydroid"),
    )


class TestStatusParsing:
    def test_running_status(self, backend: WaydroidBackend, monkeypatch) -> None:
        monkeypatch.setattr(
            proc,
            "run",
            FakeProc(
                {
                    ("/usr/bin/waydroid", "status"): ok(STATUS_RUNNING),
                }
            ).run,
        )
        parsed = backend.read_status()
        assert parsed["session"] == "RUNNING"
        assert parsed["container"] == "RUNNING"
        assert parsed["ip_address"] == "192.168.240.112"

    def test_stopped_status(self, backend: WaydroidBackend, monkeypatch) -> None:
        monkeypatch.setattr(
            proc,
            "run",
            FakeProc(
                {
                    ("/usr/bin/waydroid", "status"): ok(STATUS_STOPPED),
                }
            ).run,
        )
        assert backend.snapshot().state is BackendState.STOPPED

    def test_session_running_without_boot_is_only_starting(
        self, backend: WaydroidBackend, monkeypatch
    ) -> None:
        """A running session process is not proof that Android booted."""
        monkeypatch.setattr(
            proc,
            "run",
            FakeProc(
                {
                    ("/usr/bin/waydroid", "status"): ok(STATUS_RUNNING),
                    ("/usr/bin/waydroid", "shell"): ok("0\n"),
                }
            ).run,
        )
        snapshot = backend.snapshot()
        assert snapshot.state is BackendState.STARTING
        assert snapshot.readiness["container"] is Readiness.READY
        assert snapshot.readiness["android"] is Readiness.NOT_READY
        assert "not reported boot" in snapshot.detail

    def test_booted_android_marks_every_layer_ready(
        self, backend: WaydroidBackend, monkeypatch
    ) -> None:
        monkeypatch.setattr(
            proc,
            "run",
            FakeProc(
                {
                    ("/usr/bin/waydroid", "status"): ok(STATUS_RUNNING),
                    ("/usr/bin/waydroid", "shell"): ok("1\n"),
                }
            ).run,
        )
        snapshot = backend.snapshot()
        assert snapshot.state is BackendState.RUNNING
        assert snapshot.readiness["android"] is Readiness.READY

    def test_unparseable_output_does_not_crash(self, backend: WaydroidBackend, monkeypatch) -> None:
        monkeypatch.setattr(
            proc,
            "run",
            FakeProc(
                {
                    ("/usr/bin/waydroid", "status"): ok("garbage without colons"),
                }
            ).run,
        )
        snapshot = backend.snapshot()
        assert snapshot.state in (BackendState.STOPPED, BackendState.NOT_CONFIGURED)

    def test_missing_binary_is_reported(self, monkeypatch) -> None:
        instance = WaydroidBackend(BackendConfig(binary=None))
        monkeypatch.setattr("lynxdroid.backend.waydroid.proc.which", lambda _n: None)
        assert instance.read_status() == {}

    def test_status_error_is_visible(self, backend: WaydroidBackend, monkeypatch) -> None:
        monkeypatch.setattr(
            proc,
            "run",
            FakeProc(
                {
                    ("/usr/bin/waydroid", "status"): proc.RunResult(
                        argv=(), returncode=1, stderr="binder missing"
                    ),
                }
            ).run,
        )
        parsed = backend.read_status()
        assert "__error__" in parsed
        assert "binder" in parsed["__error__"]


class TestCapabilities:
    def test_max_touches_matches_the_channel_not_the_dream(self) -> None:
        """The CLI path carries one contact at a time; saying 10 would be a lie.

        Multitouch needs the Android-side agent; while the backend can only
        use `waydroid shell`, the honest number is 1 (and 0 when the binary is
        not installed at all, because then there is no path whatsoever).
        """
        missing = WaydroidBackend(BackendConfig(binary=None))
        missing.binary = lambda: None  # type: ignore[method-assign]
        assert missing.capabilities().max_touches == 0

        installed = WaydroidBackend(BackendConfig(binary="/usr/bin/waydroid"))
        installed.binary = lambda: "/usr/bin/waydroid"  # type: ignore[method-assign]
        caps = installed.capabilities()
        assert caps.max_touches == 1
        # ... and the multitouch feature itself must not be advertised as ready.
        multi = caps.feature("multitouch")
        assert multi is None or multi.support is not Support.SUPPORTED

    def test_reports_unsupported_features_honestly(self, backend: WaydroidBackend) -> None:
        caps = backend.capabilities()
        assert caps.status_of("split-apk") == Support.UNSUPPORTED.value
        assert caps.status_of("multi-instance") == Support.UNTESTED.value
        assert caps.status_of("arm-translation") == Support.UNTESTED.value

    def test_conditional_features_need_consent(self, backend: WaydroidBackend) -> None:
        caps = backend.capabilities()
        props = caps.feature("display-props")
        assert props is not None
        assert props.needs_consent

    def test_cli_path_reports_single_touch(self, backend: WaydroidBackend) -> None:
        # Honesty requirement: the CLI alone is not multitouch.
        assert backend.capabilities().max_touches == 1

    def test_notes_explain_wayland_limits(self, backend: WaydroidBackend) -> None:
        notes = " ".join(backend.capabilities().notes)
        assert "foreign" in notes or "surface" in notes
        assert "container" in notes

    def test_missing_binary_capabilities(self, monkeypatch) -> None:
        monkeypatch.setattr("lynxdroid.backend.waydroid.proc.which", lambda _n: None)
        monkeypatch.setattr(
            "lynxdroid.backend.waydroid.pf.waydroid_install", lambda: _install_stub()
        )
        caps = WaydroidBackend().capabilities()
        assert caps.version == "not-installed"
        assert not caps.supports("session")


def _consent() -> Consent:
    """Stand-in for a user who said yes; only the CLI and GUI can create one."""
    from lynxdroid.backend.contract import user_consented

    return user_consented("test: change the viewport")


class TestConsent:
    def test_set_prop_without_consent_raises(self, backend: WaydroidBackend) -> None:
        from lynxdroid.backend.contract import BackendError

        with pytest.raises(BackendError, match="consent"):
            backend.set_prop("persist.waydroid.width", "1280")

    def test_set_prop_rejects_bad_names(self, backend: WaydroidBackend) -> None:
        from lynxdroid.backend.contract import BackendError

        for name in ("", "persist;rm -rf /", "a b", "../x"):
            with pytest.raises(BackendError):
                backend.set_prop(name, "1", consent=_consent())

    def test_apply_display_records_previous_values(
        self, backend: WaydroidBackend, monkeypatch
    ) -> None:
        fake = FakeProc(
            {
                ("/usr/bin/waydroid", "prop", "get"): ok("1280\n"),
                ("/usr/bin/waydroid", "prop", "set"): ok(),
                ("/usr/bin/waydroid", "shell", "wm", "density"): ok(),
            }
        )
        monkeypatch.setattr(proc, "run", fake.run)
        steps = backend.apply_display(1920, 1080, 320, consent=_consent())
        assert any("1280 -> 1920" in step for step in steps)
        assert any("persist.waydroid.height" in step for step in steps)
        assert any("session start" in step or "restart" in step for step in steps)

    def test_apply_display_rejects_non_positive(self, backend: WaydroidBackend) -> None:
        from lynxdroid.backend.contract import BackendError

        with pytest.raises(BackendError):
            backend.apply_display(0, 1080, 320)


class TestApps:
    def test_list_apps_parses_package_lines(self, backend: WaydroidBackend, monkeypatch) -> None:
        monkeypatch.setattr(
            proc,
            "run",
            FakeProc(
                {
                    ("/usr/bin/waydroid", "app", "list"): ok(
                        "com.android.settings Settings\ncom.example.game My Game\n\nnot-a-package\n"
                    ),
                }
            ).run,
        )
        apps = backend.list_apps()
        assert {a["package"] for a in apps} == {"com.android.settings", "com.example.game"}
        assert apps[1]["label"] == "My Game"

    def test_launch_validates_package(self, backend: WaydroidBackend) -> None:
        from lynxdroid.backend.contract import BackendError

        for package in ("bad", "", "com/x", "com..x", "; rm -rf /"):
            with pytest.raises(BackendError):
                backend.launch_app(package)

    def test_install_rejects_non_apk(self, backend: WaydroidBackend, tmp_path) -> None:
        target = tmp_path / "file.zip"
        target.write_bytes(b"PK\x03\x04")
        ok_flag, message = backend.install_apk(str(target))
        assert not ok_flag
        assert ".apk" in message

    def test_install_rejects_symlink(self, backend: WaydroidBackend, tmp_path) -> None:
        real = tmp_path / "real.apk"
        real.write_bytes(b"PK\x03\x04")
        link = tmp_path / "link.apk"
        link.symlink_to(real)
        ok_flag, message = backend.install_apk(str(link))
        assert not ok_flag
        assert "symlink" in message

    def test_install_missing_file(self, backend: WaydroidBackend) -> None:
        ok_flag, message = backend.install_apk("/nonexistent/game.apk")
        assert not ok_flag
        assert "not a file" in message


class TestProperties:
    def test_get_prop_validates_name(self, backend: WaydroidBackend) -> None:
        from lynxdroid.backend.contract import BackendError

        with pytest.raises(BackendError):
            backend.get_prop("bad name")

    def test_get_prop_returns_none_when_missing(
        self, backend: WaydroidBackend, monkeypatch
    ) -> None:
        monkeypatch.setattr(proc, "run", FakeProc({}).run)
        assert backend.get_prop("persist.waydroid.width") is None

    def test_get_prop_strips_whitespace(self, backend: WaydroidBackend, monkeypatch) -> None:
        monkeypatch.setattr(
            proc,
            "run",
            FakeProc(
                {
                    ("/usr/bin/waydroid", "prop", "get"): ok("  1920  \n"),
                }
            ).run,
        )
        assert backend.get_prop("persist.waydroid.width") == "1920"


class TestIntents:
    def test_launch_intent_validates_action(self, backend: WaydroidBackend) -> None:
        from lynxdroid.backend.contract import BackendError

        with pytest.raises(BackendError):
            backend.launch_intent("id.lynxdroid; rm -rf /")

    def test_launch_intent_ok(self, backend: WaydroidBackend, monkeypatch) -> None:
        monkeypatch.setattr(
            proc,
            "run",
            FakeProc(
                {
                    ("/usr/bin/waydroid", "shell", "am"): ok(),
                }
            ).run,
        )
        assert backend.launch_intent("id.lynxdroid.agent.START")


class TestStartStop:
    def test_start_requires_a_binary(self, monkeypatch) -> None:
        from lynxdroid.backend.contract import BackendError

        monkeypatch.setattr("lynxdroid.backend.waydroid.proc.which", lambda _n: None)
        with pytest.raises(BackendError, match="not installed"):
            WaydroidBackend(BackendConfig(binary=None)).start()

    def test_start_times_out_without_boot_completed(
        self, backend: WaydroidBackend, monkeypatch
    ) -> None:
        from lynxdroid.backend.contract import BackendError

        monkeypatch.setattr(
            proc,
            "run",
            FakeProc(
                {
                    ("/usr/bin/waydroid", "status"): ok(STATUS_RUNNING),
                    ("/usr/bin/waydroid", "session", "stop"): ok(),
                    ("/usr/bin/waydroid", "shell"): ok("0\n"),
                }
            ).run,
        )
        with pytest.raises(BackendError, match="boot completion"):
            backend.start(timeout_s=0.2)
        assert backend.lifecycle.state is BackendState.ERROR
        assert "boot completion" in (backend.lifecycle.snapshot().error or "")


# ---------------------------------------------------------------------------
# A start is silent for up to a minute. What the window shows during that
# minute is the difference between "working" and "hung".
# ---------------------------------------------------------------------------


class TestStartReportsProgress:
    def test_the_detail_changes_while_boot_is_awaited(self, backend, monkeypatch) -> None:
        """Observers must see progress, not one frozen message.

        A cold boot takes tens of seconds. The lifecycle published "user
        requested start" and then said nothing until RUNNING, which is
        indistinguishable from a start that hung - so this detail was the
        only thing the user could steer by, and nothing exercised it.
        """
        details: list[str] = []
        backend.lifecycle.subscribe(lambda snap: details.append(snap.detail))

        # Poll 1: session not up yet. Poll 2: up, still booting. Poll 3: booted.
        polls = iter([{"session": "STOPPED"}, {"session": "RUNNING"}, {"session": "RUNNING"}])
        monkeypatch.setattr(backend, "read_status", lambda: next(polls, {"session": "RUNNING"}))
        booted = iter([False, True])
        monkeypatch.setattr(backend, "boot_completed", lambda **_: next(booted, True))
        monkeypatch.setattr("lynxdroid.backend.waydroid.time.sleep", lambda _s: None)

        backend.start(timeout_s=5.0)

        waiting = [d for d in details if "waiting" in d]
        assert len(set(waiting)) >= 2, (
            f"the start never announced a second phase; details seen: {details}"
        )
        assert any("session" in d for d in waiting), details
        assert any("Android" in d for d in waiting), details

    def test_a_finished_start_stops_saying_it_is_waiting(self, backend, monkeypatch) -> None:
        """The detail must describe now, not a wait that is over.

        ``transition`` overwrites the detail with the reason it is given, so
        this holds today; the test exists because that is a property of a
        different module and could change without anyone noticing.
        """
        monkeypatch.setattr(backend, "read_status", lambda: {"session": "RUNNING"})
        monkeypatch.setattr(backend, "boot_completed", lambda **_: True)
        monkeypatch.setattr("lynxdroid.backend.waydroid.time.sleep", lambda _s: None)
        spawns: list[list[str]] = []
        monkeypatch.setattr(
            "lynxdroid.backend.waydroid.proc.spawn",
            lambda argv, **kw: spawns.append(argv) or _FakeSessionProc(),
        )

        snap = backend.start(timeout_s=5.0)
        # The session was already running, so nothing was spawned; the point
        # here is only what the snapshot says once the start is over.
        assert spawns == []

        assert "waiting" not in snap.detail, snap.detail


class _FakeSessionProc:
    """Stands in for a spawned ``waydroid session start`` process."""

    pid = 4242


class TestSessionLogLocation:
    """The session log is ours to write, so it must live where we may write.

    ``/var/lib/waydroid`` is root-owned. Writing the log there made every
    start fail with "cannot spawn waydroid session: [Errno 2] No such file or
    directory", which reads like Waydroid is missing rather than like a
    permission problem - and this app runs as a normal user by design.
    """

    def test_the_log_is_written_under_the_xdg_state_directory(
        self, backend, monkeypatch, tmp_path
    ) -> None:
        monkeypatch.setenv("XDG_STATE_HOME", str(tmp_path))
        captured: list[Path] = []

        def fake_spawn(argv, *, stdout_path=None, **kwargs):
            captured.append(stdout_path)
            return _FakeSessionProc()

        monkeypatch.setattr("lynxdroid.backend.waydroid.proc.spawn", fake_spawn)
        # Stopped at first (so the session is started, which is when the log
        # path is chosen), running afterwards. A counter rather than an
        # iterator: snapshot() reads the status too, so the number of looks
        # before the spawn is not part of the contract and must not matter.
        looks = {"n": 0}

        def status() -> dict[str, str]:
            looks["n"] += 1
            return {"session": "STOPPED" if looks["n"] <= 2 else "RUNNING"}

        monkeypatch.setattr(backend, "read_status", status)
        monkeypatch.setattr(backend, "boot_completed", lambda **_: True)
        monkeypatch.setattr("lynxdroid.backend.waydroid.time.sleep", lambda _s: None)

        backend.start(timeout_s=5.0)

        assert captured, "no session log path was used"
        log_path = captured[0]
        assert log_path is not None
        assert str(tmp_path) in str(log_path), (
            f"the session log went outside XDG_STATE_HOME: {log_path}"
        )
        assert "/var/lib/waydroid" not in str(log_path), (
            "the session log is written to Waydroid's root-owned directory"
        )

    def test_the_config_no_longer_offers_a_root_owned_state_dir(self) -> None:
        """A setting that can only fail is worse than no setting."""
        assert not hasattr(BackendConfig(), "state_dir")


class TestConsentCannotBeFaked:
    """The old signature took ``consent: bool = False``.

    A caller passing ``True`` was indistinguishable from a caller that had
    asked the user, so the guarantee lived in the discipline of four call
    sites. ``apply_display`` did exactly that: it passed ``consent=True`` to
    its own ``set_prop`` calls, which proves nothing at all.
    """

    def test_a_bare_true_is_not_consent(self, backend: WaydroidBackend) -> None:
        """A bool must not satisfy the parameter, even a truthy one."""
        with pytest.raises(BackendError):
            backend.set_prop("persist.waydroid.width", "1280", consent=True)  # type: ignore[arg-type]

    def test_apply_display_without_consent_writes_nothing(self, backend, monkeypatch) -> None:
        calls: list[tuple[str, ...]] = []
        monkeypatch.setattr(proc, "run", lambda argv, **kw: calls.append(tuple(argv)) or ok(""))
        with pytest.raises(BackendError, match="consent"):
            backend.apply_display(1920, 1080, 320)
        assert calls == [], f"properties were written without consent: {calls}"

    def test_apply_display_passes_the_consent_through(self, backend, monkeypatch) -> None:
        """It must forward the user's agreement, not invent a new one."""
        seen: list[Consent | None] = []
        original = backend.set_prop

        def spy(name, value, *, consent=None):
            seen.append(consent)
            return True

        backend.set_prop = spy  # type: ignore[method-assign]
        _ = original
        monkeypatch.setattr(proc, "run", lambda argv, **kw: ok(""))
        mine = _consent()
        backend.apply_display(1920, 1080, 320, consent=mine)
        assert seen, "set_prop was never called"
        assert all(item is mine for item in seen), (
            "apply_display used a different consent object than the caller's"
        )

    def test_consent_names_what_was_agreed_to(self) -> None:
        from lynxdroid.backend.contract import user_consented

        with pytest.raises(ValueError):
            user_consented("   ")
        assert "viewport" in user_consented("viewport to 1920x1080").describe()


class TestOptInsAreOffByDefault:
    """SECURITY.md promised opt-ins that the code never checked.

    `allow_service_control` and `allow_prop_writes` were saved, clamped and
    never read, so `systemctl start` ran and properties were written as long
    as some caller passed consent. The settings said "off"; the code said
    "yes, please".
    """

    def _strict(self) -> WaydroidBackend:
        return WaydroidBackend(BackendConfig(binary="/usr/bin/waydroid"))

    def test_starting_the_container_unit_requires_the_opt_in(self, monkeypatch) -> None:
        backend = self._strict()
        monkeypatch.setattr(
            "lynxdroid.backend.waydroid.pf.waydroid_install", lambda: _install_stub()
        )
        monkeypatch.setattr(
            "lynxdroid.backend.waydroid.pf.systemd_state", lambda _unit: ("inactive", "dead")
        )
        ran: list[tuple[str, ...]] = []
        monkeypatch.setattr(
            "lynxdroid.backend.waydroid.proc.run",
            lambda argv, **kw: ran.append(tuple(argv)) or ok(""),
        )

        with pytest.raises(BackendError, match="needs root") as caught:
            backend._ensure_container()

        assert caught.value.code == "service-control-disabled"
        assert "systemctl start waydroid-container.service" in (caught.value.hint or "")
        assert ran == [], f"a root command was attempted without the opt-in: {ran}"

    def test_writing_a_property_requires_the_opt_in(self, monkeypatch) -> None:
        backend = self._strict()
        ran: list[tuple[str, ...]] = []
        monkeypatch.setattr(
            "lynxdroid.backend.waydroid.proc.run",
            lambda argv, **kw: ran.append(tuple(argv)) or ok(""),
        )

        with pytest.raises(BackendError) as caught:
            backend.set_prop("persist.waydroid.width", "1280", consent=_consent())

        assert caught.value.code == "prop-writes-disabled"
        assert ran == [], f"a property was written without the opt-in: {ran}"

    def test_the_defaults_match_what_the_settings_say(self) -> None:
        """The two defaults must not drift apart again."""
        from lynxdroid.backend.waydroid import config_for
        from lynxdroid.settings import Settings

        settings = Settings()
        config = config_for(settings)
        assert settings.allow_service_control is False
        assert settings.allow_prop_writes is False
        assert config.allow_service_control is settings.allow_service_control
        assert config.allow_prop_writes is settings.allow_prop_writes

    def test_turning_it_on_in_the_settings_reaches_the_backend(self) -> None:
        from lynxdroid.backend.waydroid import config_for
        from lynxdroid.settings import Settings

        config = config_for(Settings(allow_service_control=True, allow_prop_writes=True))
        assert config.allow_service_control is True
        assert config.allow_prop_writes is True
