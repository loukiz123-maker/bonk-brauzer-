"""The VM host probe makes no changes and never claims an Android boot."""

from __future__ import annotations

import errno
import stat
from types import SimpleNamespace

import pytest

from lynxdroid.doctor import vm_probe


def test_cpu_virtualization_hint_is_only_a_hint() -> None:
    assert vm_probe.cpu_virtualization_hint("flags : fpu vmx sse\n", "x86_64") == "present"
    assert vm_probe.cpu_virtualization_hint("flags : fpu sse\n", "x86_64") == "not-reported"
    assert vm_probe.cpu_virtualization_hint("", "x86_64") == "unknown"
    assert vm_probe.cpu_virtualization_hint("flags : vmx\n", "aarch64") == "unknown"


@pytest.mark.parametrize(
    ("error", "expected"),
    [(errno.ENOENT, "missing"), (errno.EACCES, "permission-denied"), (errno.EIO, "open-error")],
)
def test_unavailable_kvm_is_not_reported_as_ready(monkeypatch, error, expected) -> None:
    monkeypatch.setattr(vm_probe.platform, "system", lambda: "Linux")

    def fail(*args):
        raise OSError(error, "test error")

    monkeypatch.setattr(vm_probe.os, "open", fail)
    result = vm_probe.probe()
    assert result["kvm_status"] == expected
    assert result["kvm_api_version"] is None
    assert "not implemented" in result["scope"]


def test_kvm_api_version_checked_and_descriptor_closed(monkeypatch) -> None:
    monkeypatch.setattr(vm_probe.platform, "system", lambda: "Linux")
    original_open, original_fstat, original_close = (
        vm_probe.os.open,
        vm_probe.os.fstat,
        vm_probe.os.close,
    )
    monkeypatch.setattr(
        vm_probe.os,
        "open",
        lambda path, *args: 73 if path == "/dev/kvm" else original_open(path, *args),
    )
    monkeypatch.setattr(
        vm_probe.os,
        "fstat",
        lambda fd: SimpleNamespace(st_mode=stat.S_IFCHR) if fd == 73 else original_fstat(fd),
    )
    monkeypatch.setattr(vm_probe.fcntl, "ioctl", lambda fd, op: 12)
    closed = []
    monkeypatch.setattr(
        vm_probe.os, "close", lambda fd: closed.append(fd) if fd == 73 else original_close(fd)
    )
    result = vm_probe.probe()
    assert result["kvm_status"] == "available" and result["kvm_api_version"] == 12
    assert closed == [73]


def test_kvm_ioctl_failure_closes_descriptor(monkeypatch) -> None:
    monkeypatch.setattr(vm_probe.platform, "system", lambda: "Linux")
    original_open, original_fstat, original_close = (
        vm_probe.os.open,
        vm_probe.os.fstat,
        vm_probe.os.close,
    )
    monkeypatch.setattr(
        vm_probe.os,
        "open",
        lambda path, *args: 73 if path == "/dev/kvm" else original_open(path, *args),
    )
    monkeypatch.setattr(
        vm_probe.os,
        "fstat",
        lambda fd: SimpleNamespace(st_mode=stat.S_IFCHR) if fd == 73 else original_fstat(fd),
    )

    def fail(*args):
        raise OSError(errno.EIO, "test failure")

    monkeypatch.setattr(vm_probe.fcntl, "ioctl", fail)
    closed = []
    monkeypatch.setattr(
        vm_probe.os, "close", lambda fd: closed.append(fd) if fd == 73 else original_close(fd)
    )
    assert vm_probe.probe()["kvm_status"] == "ioctl-error"
    assert closed == [73]
