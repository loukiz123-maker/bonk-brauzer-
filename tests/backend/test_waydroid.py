"""Waydroid adapter: parsing, consent gates, capability honesty.

The CLI is never executed here: a fake runner is injected so the tests assert
*our* logic (parsing, gating, error mapping) without needing Waydroid.
"""

from __future__ import annotations

import pytest

from lynxdroid.backend.contract import BackendState, Readiness, Support
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
    instance = WaydroidBackend(BackendConfig(binary="/usr/bin/waydroid"))
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


class TestConsent:
    def test_set_prop_without_consent_raises(self, backend: WaydroidBackend) -> None:
        from lynxdroid.backend.contract import BackendError

        with pytest.raises(BackendError, match="consent"):
            backend.set_prop("persist.waydroid.width", "1280")

    def test_set_prop_rejects_bad_names(self, backend: WaydroidBackend) -> None:
        from lynxdroid.backend.contract import BackendError

        for name in ("", "persist;rm -rf /", "a b", "../x"):
            with pytest.raises(BackendError):
                backend.set_prop(name, "1", consent=True)

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
        steps = backend.apply_display(1920, 1080, 320)
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
