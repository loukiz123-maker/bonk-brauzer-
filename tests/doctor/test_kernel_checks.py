"""The kernel half of ``lynxdroid doctor``, tested without the host's kernel.

These checks decide whether Android can boot at all, and they were reached by
no test - a regression in the branch logic would have shipped silently.
Everything the check reads from the machine is patched, so the expectations
hold on any host.
"""

from __future__ import annotations

import pytest

from lynxdroid.core.diagnostics import Status
from lynxdroid.doctor import checks
from lynxdroid.doctor.platform import KernelConfig


def _report(
    monkeypatch, *, config: KernelConfig, devices=(), mounted=False, module=False, listed=False
):
    monkeypatch.setattr(checks.pf, "kernel_config", lambda release=None: config)
    monkeypatch.setattr(checks.pf, "binder_devices", lambda: list(devices))
    monkeypatch.setattr(checks.pf, "binderfs_mounted", lambda: mounted)
    monkeypatch.setattr(checks.pf, "binder_module_loaded", lambda: module)
    monkeypatch.setattr(checks.pf, "binder_supported_in_filesystems", lambda: listed)
    return checks.kernel_report()


def _check(report, check_id: str):
    found = [c for c in report.checks if c.id == check_id]
    assert len(found) == 1, f"{check_id} reported {len(found)} times"
    return found[0]


CONFIG_WITH_BINDER = KernelConfig({"CONFIG_ANDROID_BINDER_IPC": "y"}, "/proc/config.gz", None)
NO_CONFIG = KernelConfig({}, None, "no config source found")


class TestBinderDevice:
    def test_a_present_device_is_ok(self, monkeypatch) -> None:
        report = _report(monkeypatch, config=CONFIG_WITH_BINDER, devices=["/dev/binder"])
        check = _check(report, "kernel.binder")
        assert check.status is Status.OK
        assert "/dev/binder" in check.observed
        assert check.requires_privilege is False

    def test_kernel_support_without_a_node_is_a_warning_with_instructions(
        self, monkeypatch
    ) -> None:
        report = _report(monkeypatch, config=CONFIG_WITH_BINDER)
        check = _check(report, "kernel.binder")
        assert check.status is Status.WARNING
        # The user must be told what to do, and that it needs root.
        assert "binderfs" in check.next_step
        assert check.requires_privilege is True

    def test_a_kernel_built_without_binder_is_an_error(self, monkeypatch) -> None:
        config = KernelConfig({"CONFIG_ANDROID_BINDER_IPC": "n"}, "/proc/config.gz", None)
        check = _check(_report(monkeypatch, config=config), "kernel.binder")
        assert check.status is Status.ERROR
        assert "kernel" in check.next_step

    def test_no_config_and_no_device_is_unknown_not_unsupported(self, monkeypatch) -> None:
        """ "Cannot tell" must never be reported as "not supported"."""
        check = _check(_report(monkeypatch, config=NO_CONFIG), "kernel.binder")
        assert check.status is Status.UNKNOWN
        assert check.detail != ""


class TestBinderfs:
    def test_built_in_and_mounted_is_ok(self, monkeypatch) -> None:
        config = KernelConfig({"CONFIG_ANDROID_BINDERFS": "y"}, "/proc/config.gz", None)
        check = _check(_report(monkeypatch, config=config, mounted=True), "kernel.binderfs")
        assert check.status is Status.OK
        assert check.requires_privilege is False

    def test_built_in_but_not_mounted_says_so(self, monkeypatch) -> None:
        config = KernelConfig({"CONFIG_ANDROID_BINDERFS": "y"}, "/proc/config.gz", None)
        check = _check(_report(monkeypatch, config=config), "kernel.binderfs")
        assert check.status is Status.OK
        assert "not mounted" in check.detail

    def test_proc_filesystems_is_used_when_the_config_is_unreadable(self, monkeypatch) -> None:
        """Distributions often ship a kernel with no readable config; binderfs
        can still be proven present from /proc/filesystems."""
        check = _check(_report(monkeypatch, config=NO_CONFIG, listed=True), "kernel.binderfs")
        assert check.status is Status.OK
        assert "/proc/filesystems" in check.detail

    def test_nothing_to_go_on_stays_unknown(self, monkeypatch) -> None:
        check = _check(_report(monkeypatch, config=NO_CONFIG), "kernel.binderfs")
        assert check.status is Status.UNKNOWN

    def test_the_observed_field_names_both_sources(self, monkeypatch) -> None:
        check = _check(_report(monkeypatch, config=NO_CONFIG, listed=True), "kernel.binderfs")
        assert "/proc/filesystems" in check.observed
        assert "mounted=" in check.observed


class TestBinderFilesystemProbe:
    @pytest.mark.parametrize(
        ("content", "expected"),
        [
            ("nodev\tsysfs\n\text4\nnodev\tbinder\n", True),
            ("nodev\tsysfs\n\text4\n", False),
            ("", False),
        ],
    )
    def test_detects_the_binder_line(self, monkeypatch, content, expected) -> None:
        monkeypatch.setattr(checks.pf, "read_text", lambda path, limit=65536: content)
        assert checks.pf.binder_supported_in_filesystems() is expected

    def test_an_unreadable_file_is_not_a_support_claim(self, monkeypatch) -> None:
        monkeypatch.setattr(checks.pf, "read_text", lambda path, limit=65536: None)
        assert checks.pf.binder_supported_in_filesystems() is False
