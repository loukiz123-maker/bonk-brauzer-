"""KVM allocation sequencing with fake ioctls; no VM is created during tests."""

from __future__ import annotations

import errno
import mmap

import pytest

from lynxdroid.backend import kvm_smoke as smoke


class FakeOps(smoke.KvmOperations):
    def __init__(self, failure: str = "") -> None:
        self.failure = failure
        self.calls: list[object] = []
        self.version = 12
        self.mapping_size = mmap.PAGESIZE
        self.character = True

    def _check(self, step: str) -> None:
        self.calls.append(step)
        if self.failure == step:
            raise OSError(errno.EIO, f"failed {step}")

    def open_kvm(self) -> int:
        self._check("open")
        return 10

    def is_character_device(self, fd: int) -> bool:
        self._check("stat")
        return self.character

    def ioctl(self, fd: int, request: int, argument: int = 0) -> int:
        names = {
            smoke.KVM_GET_API_VERSION: ("version", self.version),
            smoke.KVM_GET_VCPU_MMAP_SIZE: ("size", self.mapping_size),
            smoke.KVM_CREATE_VM: ("vm", 11),
            smoke.KVM_CREATE_VCPU: ("vcpu", 12),
        }
        step, value = names[request]
        self._check(step)
        self.calls.append((fd, request, argument))
        return value

    def non_inheritable(self, fd: int) -> None:
        self._check(f"cloexec-{fd}")

    def map_vcpu(self, fd: int, size: int) -> object:
        self._check("map")
        self.calls.append(("map-args", fd, size))
        return self

    def close(self, fd: int | None = None) -> None:
        self.calls.append(("close", fd))


def test_success_closes_mapping_then_vcpu_vm_kvm(monkeypatch) -> None:
    monkeypatch.setattr(smoke.platform, "system", lambda: "Linux")
    ops = FakeOps()
    result = smoke.run_smoke(ops)
    assert result.success and result.stage == "complete"
    assert "not run" in result.scope
    assert ops.calls[-4:] == [("close", None), ("close", 12), ("close", 11), ("close", 10)]
    assert (11, smoke.KVM_CREATE_VCPU, 0) in ops.calls
    assert ("map-args", 12, mmap.PAGESIZE) in ops.calls


@pytest.mark.parametrize(
    ("failure", "stage", "closed"),
    [
        ("open", "open-kvm", []),
        ("stat", "open-kvm", [10]),
        ("version", "api-version", [10]),
        ("size", "run-mapping-size", [10]),
        ("vm", "create-vm", [10]),
        ("cloexec-11", "create-vm", [11, 10]),
        ("vcpu", "create-vcpu", [11, 10]),
        ("cloexec-12", "create-vcpu", [12, 11, 10]),
        ("map", "map-vcpu", [12, 11, 10]),
    ],
)
def test_failures_close_all_allocated_descriptors(monkeypatch, failure, stage, closed) -> None:
    monkeypatch.setattr(smoke.platform, "system", lambda: "Linux")
    ops = FakeOps(failure)
    result = smoke.run_smoke(ops)
    assert not result.success and result.stage == stage
    assert [
        call[1] for call in ops.calls if isinstance(call, tuple) and call[0] == "close"
    ] == closed


def test_rejects_invalid_version_mapping_and_non_device(monkeypatch) -> None:
    monkeypatch.setattr(smoke.platform, "system", lambda: "Linux")
    ops = FakeOps()
    ops.version = 99
    assert smoke.run_smoke(ops).stage == "api-version"
    assert "vm" not in ops.calls
    ops = FakeOps()
    ops.mapping_size = smoke.MAX_RUN_MAPPING + 1
    assert smoke.run_smoke(ops).stage == "run-mapping-size"
    assert "vm" not in ops.calls
    ops = FakeOps()
    ops.character = False
    assert smoke.run_smoke(ops).stage == "open-kvm"
    assert "version" not in ops.calls


def test_non_linux_refuses_to_open(monkeypatch) -> None:
    monkeypatch.setattr(smoke.platform, "system", lambda: "Darwin")
    ops = FakeOps()
    assert smoke.run_smoke(ops).stage == "platform"
    assert ops.calls == []


def test_cli_requires_explicit_confirmation(monkeypatch, capsys) -> None:
    monkeypatch.setattr(smoke.sys, "argv", ["kvm_smoke"])
    with pytest.raises(SystemExit) as exc:
        smoke.main()
    assert exc.value.code == 2
    assert "--confirm" in capsys.readouterr().err
