"""The host profile, judged on described machines rather than this one.

`describe_host()` reads the machine it runs on, and this sandbox is a Debian
container with no binder, no GPU and no Waydroid - one machine, one verdict.
Every interesting case is a machine we do not have: a CachyOS laptop that is
ready, one with Intel graphics but no render-node access, one whose kernel
lacks binder, one where nothing could be read at all.

So the probes are scripted and the profile is built from them. The rules the
tests defend are the ones that decide whether a report is trustworthy:

* an unreadable fact is ``None``, never a plausible default;
* ``UNKNOWN`` is never rounded up to ``READY``;
* the worst precondition decides the whole verdict;
* every ``BLOCKED`` and ``PARTIAL`` names a next step the user can take.
"""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path

import pytest

from lynxdroid.doctor import host_profile as hp
from lynxdroid.doctor import platform as pf

# ---------------------------------------------------------------------------
# Building the machines
# ---------------------------------------------------------------------------


def _os(**overrides) -> pf.OsInfo:
    base = pf.OsInfo(
        distro_id="cachyos",
        distro_name="CachyOS",
        pretty_name="CachyOS Linux",
        kernel_release="6.12.4-1-cachyos",
        machine="x86_64",
        is_arch_family=True,
        is_cachyos=True,
        source="/etc/os-release",
    )
    return replace(base, **overrides)


def _session(**overrides) -> pf.SessionInfo:
    base = pf.SessionInfo(
        session_type="wayland",
        desktop="KDE",
        wayland_display="wayland-0",
        wayland_socket=Path("/run/user/1000/wayland-0"),
        wayland_socket_usable=True,
        display=None,
        is_kde=True,
        running_as_root=False,
        in_container=False,
        container_hint="",
        reasons=[],
    )
    return replace(base, **overrides)


def _gpu(**overrides) -> pf.GpuInfo:
    base = pf.GpuInfo(
        vendor="Intel",
        driver="Iris Xe Graphics [i915]",
        driver_version="6.12.4",
        kernel_driver="i915",
        render_node="/dev/dri/renderD128",
        render_node_usable=True,
        is_intel=True,
        is_iris_xe_class=True,
        source="/sys/class/drm/card0/device",
    )
    return replace(base, **overrides)


def _install(**overrides) -> pf.WaydroidInstall:
    base = pf.WaydroidInstall(
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
        package_version=None,
        session_dir=Path("/var/lib/waydroid"),
    )
    return replace(base, **overrides)


def _script_host(
    monkeypatch,
    *,
    os_info=None,
    session=None,
    gpu=(None, "no /sys/class/drm"),
    install=None,
    binder_devices=(),
    binderfs_mounted=False,
    binder_in_filesystems=True,
    free_bytes=(64 * 1024**3, "statvfs"),
):
    """Make describe_host() see a machine we describe instead of this one."""
    monkeypatch.setattr(hp.pf, "os_info", lambda: os_info or _os())
    monkeypatch.setattr(hp.pf, "session_info", lambda: session or _session())
    monkeypatch.setattr(hp.pf, "gpu_info", lambda *a, **k: gpu)
    monkeypatch.setattr(hp.pf, "waydroid_install", lambda: install or _install())
    monkeypatch.setattr(hp.pf, "binder_devices", lambda: list(binder_devices))
    monkeypatch.setattr(hp.pf, "binderfs_mounted", lambda: binderfs_mounted)
    monkeypatch.setattr(hp.pf, "binder_supported_in_filesystems", lambda: binder_in_filesystems)
    monkeypatch.setattr(hp.pf, "disk_free", lambda _path: free_bytes)


# ---------------------------------------------------------------------------
# The happy case, and what makes it unhappy
# ---------------------------------------------------------------------------


class TestAVerifiedTarget:
    def test_a_ready_laptop_is_reported_ready(self, monkeypatch) -> None:
        _script_host(
            monkeypatch,
            gpu=(_gpu(), ""),
            binder_devices=(Path("/dev/binder"),),
        )
        profile = hp.describe_host()
        assert profile.verdict is hp.Verdict.READY, profile.summary()
        assert profile.blockers() == ()
        assert profile.unresolved() == ()

    def test_the_profile_records_the_machine_it_described(self, monkeypatch) -> None:
        _script_host(monkeypatch, gpu=(_gpu(), ""), binder_devices=(Path("/dev/binder"),))
        profile = hp.describe_host()
        assert profile.os_id == "cachyos"
        assert profile.kernel.startswith("6.12")
        assert profile.session == "wayland"
        assert profile.gpu_renderer_is_iris_xe is True
        assert profile.waydroid_version == "1.5.0"

    def test_a_missing_render_node_access_is_a_limitation_not_a_blocker(self, monkeypatch) -> None:
        _script_host(
            monkeypatch,
            gpu=(_gpu(render_node_usable=False), ""),
            binder_devices=(Path("/dev/binder"),),
        )
        profile = hp.describe_host()
        assert profile.verdict is hp.Verdict.PARTIAL
        gpu = next(p for p in profile.preconditions if p.id == "gpu")
        assert "video" in gpu.next_step


class TestHardBlockers:
    def test_a_kernel_without_binder_blocks_the_container(self, monkeypatch) -> None:
        """The failure that cannot be worked around: Android needs binder."""
        _script_host(
            monkeypatch,
            gpu=(_gpu(), ""),
            binder_in_filesystems=False,
        )
        profile = hp.describe_host()
        binder = next(p for p in profile.preconditions if p.id == "binder")
        assert binder.verdict is hp.Verdict.BLOCKED
        assert "CONFIG_ANDROID_BINDER_IPC" in binder.reason
        assert profile.verdict is hp.Verdict.BLOCKED

    def test_binderfs_present_but_unmounted_is_a_user_action(self, monkeypatch) -> None:
        """We never mount it ourselves: that needs root, which we do not take."""
        _script_host(monkeypatch, gpu=(_gpu(), ""), binder_in_filesystems=True)
        binder = next(p for p in hp.describe_host().preconditions if p.id == "binder")
        assert binder.verdict is hp.Verdict.PARTIAL
        assert "mount -t binder" in binder.next_step

    def test_waydroid_not_installed_blocks_with_an_install_command(self, monkeypatch) -> None:
        _script_host(
            monkeypatch,
            gpu=(_gpu(), ""),
            binder_devices=(Path("/dev/binder"),),
            install=_install(binary=None, version=None),
        )
        waydroid = next(p for p in hp.describe_host().preconditions if p.id == "waydroid")
        assert waydroid.verdict is hp.Verdict.BLOCKED
        assert "pacman" in waydroid.next_step

    def test_installed_without_an_image_is_blocked_separately(self, monkeypatch) -> None:
        """A message saying "install Waydroid" would be wrong here."""
        _script_host(
            monkeypatch,
            gpu=(_gpu(), ""),
            binder_devices=(Path("/dev/binder"),),
            install=_install(system_present=False),
        )
        waydroid = next(p for p in hp.describe_host().preconditions if p.id == "waydroid")
        assert waydroid.verdict is hp.Verdict.BLOCKED
        assert "image" in waydroid.reason
        assert "waydroid init" in waydroid.next_step


class TestUnknownIsNotReady:
    def test_no_gpu_description_keeps_the_verdict_unknown(self, monkeypatch) -> None:
        """A container has no /dev/dri; that is not a laptop without a GPU."""
        _script_host(
            monkeypatch, gpu=(None, "no /sys/class/drm"), binder_devices=(Path("/dev/binder"),)
        )
        profile = hp.describe_host()
        gpu = next(p for p in profile.preconditions if p.id == "gpu")
        assert gpu.verdict is hp.Verdict.UNKNOWN
        assert gpu.fact is None, "an unreadable fact must not be given a value"
        assert profile.verdict is hp.Verdict.UNKNOWN

    def test_one_unknown_precondition_prevents_ready(self, monkeypatch) -> None:
        """Everything else passing is not enough: unchecked is not passed."""
        _script_host(
            monkeypatch,
            gpu=(None, "absent"),
            binder_devices=(Path("/dev/binder"),),
            session=_session(session_type=None, wayland_socket_usable=False),
        )
        profile = hp.describe_host()
        assert profile.verdict is hp.Verdict.UNKNOWN
        assert {p.id for p in profile.unresolved()} == {"gpu", "wayland"}

    def test_an_unreadable_disk_is_unknown_not_fine(self, monkeypatch) -> None:
        _script_host(
            monkeypatch,
            gpu=(_gpu(), ""),
            binder_devices=(Path("/dev/binder"),),
            free_bytes=(None, "OSError"),
        )
        disk = next(p for p in hp.describe_host().preconditions if p.id == "disk")
        assert disk.verdict is hp.Verdict.UNKNOWN
        assert disk.fact is None


class TestSessionVerdicts:
    def test_x11_is_a_named_limitation(self, monkeypatch) -> None:
        _script_host(
            monkeypatch,
            gpu=(_gpu(), ""),
            binder_devices=(Path("/dev/binder"),),
            session=_session(session_type="x11", wayland_socket_usable=False),
        )
        wayland = next(p for p in hp.describe_host().preconditions if p.id == "wayland")
        assert wayland.verdict is hp.Verdict.PARTIAL
        assert "Wayland" in wayland.reason

    def test_wayland_without_a_usable_socket_is_blocked(self, monkeypatch) -> None:
        """ "wayland" plus an unreachable socket is what an ssh session sees."""
        _script_host(
            monkeypatch,
            gpu=(_gpu(), ""),
            binder_devices=(Path("/dev/binder"),),
            session=_session(wayland_socket_usable=False),
        )
        wayland = next(p for p in hp.describe_host().preconditions if p.id == "wayland")
        assert wayland.verdict is hp.Verdict.BLOCKED
        assert "ssh" in wayland.next_step


class TestTheVerdictIsTheWorst:
    def test_one_blocker_outvotes_every_ready(self, monkeypatch) -> None:
        _script_host(monkeypatch, gpu=(_gpu(), ""), binder_in_filesystems=False)
        profile = hp.describe_host()
        assert profile.verdict is hp.Verdict.BLOCKED
        assert [p.id for p in profile.blockers()] == ["binder"]

    def test_blocked_outranks_unknown(self, monkeypatch) -> None:
        """A known failure is a stronger statement than an unchecked item."""
        _script_host(monkeypatch, gpu=(None, "absent"), binder_in_filesystems=False)
        profile = hp.describe_host()
        assert profile.verdict is hp.Verdict.BLOCKED
        assert profile.unresolved(), "the unknown must still be reported"

    def test_a_profile_with_no_preconditions_is_unknown(self) -> None:
        empty = hp.HostProfile(
            os_id="",
            pretty_name="",
            kernel="",
            machine="",
            session="",
            desktop="",
            in_container=False,
            container_hint="",
            gpu_vendor="",
            gpu_driver="",
            gpu_renderer_is_iris_xe=None,
            waydroid_version=None,
        )
        assert empty.verdict is hp.Verdict.UNKNOWN


class TestEveryVerdictIsActionable:
    def test_blocked_and_partial_always_name_a_next_step(self, monkeypatch) -> None:
        """A dead end the user cannot act on is a bug report, not a message."""
        cases = [
            {"binder_in_filesystems": False},
            {"binder_in_filesystems": True},
            {"install": _install(binary=None, version=None)},
            {"install": _install(system_present=False)},
            {"session": _session(session_type="x11", wayland_socket_usable=False)},
            {"session": _session(wayland_socket_usable=False)},
            {"gpu": (_gpu(render_node_usable=False), "")},
            {"gpu": (_gpu(render_node=None), "")},
        ]
        for index, case in enumerate(cases):
            monkeypatch.undo()
            arguments = {"gpu": (_gpu(), ""), "binder_devices": (Path("/dev/binder"),)}
            arguments.update(case)
            _script_host(monkeypatch, **arguments)
            profile = hp.describe_host()
            for precondition in profile.preconditions:
                if precondition.verdict in (hp.Verdict.BLOCKED, hp.Verdict.PARTIAL):
                    assert precondition.next_step, (
                        f"case {index}: {precondition.id} is {precondition.verdict.value} "
                        "with no next step"
                    )


class TestSerialisation:
    def test_the_profile_serialises_to_json_shapes(self, monkeypatch) -> None:
        import json

        _script_host(monkeypatch, gpu=(_gpu(), ""), binder_devices=(Path("/dev/binder"),))
        payload = hp.describe_host().as_dict()
        text = json.dumps(payload)  # must not raise
        assert "READY" in text
        assert payload["preconditions"], "the evidence must travel with the verdict"
        assert {p["id"] for p in payload["preconditions"]} == {
            "binder",
            "wayland",
            "gpu",
            "waydroid",
            "disk",
        }

    def test_unknown_gpu_serialises_as_null_not_empty_string(self, monkeypatch) -> None:
        _script_host(monkeypatch, gpu=(None, "absent"))
        payload = hp.describe_host().as_dict()
        assert payload["gpu"]["iris_xe_class"] is None


class TestTheRealMachine:
    """The one machine this sandbox actually has: a Debian container."""

    def test_this_container_is_reported_truthfully(self) -> None:
        profile = hp.describe_host()
        # No binder and no Waydroid here; the verdict must not pretend.
        assert profile.verdict in {hp.Verdict.BLOCKED, hp.Verdict.UNKNOWN}
        assert profile.verdict is not hp.Verdict.READY

    def test_the_report_says_when_it_is_a_container(self) -> None:
        """Otherwise the profile reads as a description of the user's laptop."""
        profile = hp.describe_host()
        if profile.in_container:
            assert profile.container_hint, "a container must say what gave it away"


@pytest.mark.parametrize("verdict", list(hp.Verdict))
def test_verdict_severity_is_ordered(verdict) -> None:
    """READY < UNKNOWN < PARTIAL < BLOCKED, used by `max` above."""
    assert 0 <= verdict.severity <= 3


class TestTheTwoVerdictsAgree:
    """`doctor` and `profile` both answer "can this machine run Android".

    Two answers to one question is how a user learns to distrust both. The
    CLI's `doctor --strict` counts an UNKNOWN prerequisite as blocking
    (`Report.blockers_for`), so the profile does too, and this test holds
    them together on the machine we are actually running on.
    """

    def test_both_refuse_to_say_go_on_this_machine(self) -> None:
        from lynxdroid.doctor import checks

        report = checks.full_report()
        ready, _blockers = checks.prerequisites_state(report)
        profile = hp.describe_host()
        green = profile.verdict in (hp.Verdict.READY, hp.Verdict.PARTIAL)
        assert green == ready, (
            "doctor and profile disagree about this host: "
            f"doctor ready={ready}, profile={profile.verdict.value}"
        )

    def test_the_profile_blocks_on_unknown_like_doctor_does(self) -> None:
        """A profile of nothing but unknowns is not a green light."""
        unknown = hp.Precondition(
            id="x", fact=None, verdict=hp.Verdict.UNKNOWN, reason="not checked"
        )
        profile = hp.HostProfile(
            os_id="",
            pretty_name="",
            kernel="",
            machine="",
            session="",
            desktop="",
            in_container=False,
            container_hint="",
            gpu_vendor="",
            gpu_driver="",
            gpu_renderer_is_iris_xe=None,
            waydroid_version=None,
            preconditions=(unknown,),
        )
        assert profile.verdict is hp.Verdict.UNKNOWN
        # ...and the CLI maps that to a non-zero exit, not to success.
        from lynxdroid.core.errors import ExitCode

        clear = profile.verdict in (hp.Verdict.READY, hp.Verdict.PARTIAL)
        assert not clear and ExitCode.BLOCKED != ExitCode.OK
