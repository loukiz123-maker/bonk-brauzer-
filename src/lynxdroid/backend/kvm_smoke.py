"""Explicit KVM VM/vCPU allocation test; it never boots or runs guest code.

The test creates a temporary empty VM, one vCPU and its shared run mapping,
then closes all resources. It is not a VMM, Android runtime or compatibility test.
"""

from __future__ import annotations

import argparse
import contextlib
import fcntl
import json
import mmap
import os
import platform
import stat
import sys
from dataclasses import asdict, dataclass
from typing import Any

KVM_GET_API_VERSION = 0xAE00
KVM_CREATE_VM = 0xAE01
KVM_GET_VCPU_MMAP_SIZE = 0xAE04
KVM_CREATE_VCPU = 0xAE41
KVM_API_VERSION = 12
MAX_RUN_MAPPING = 16 * 1024 * 1024


@dataclass(frozen=True)
class SmokeResult:
    success: bool
    stage: str
    detail: str
    scope: str = "Empty VM allocation only; Android and guest code were not run"


class KvmOperations:
    """Operating-system boundary; replaceable in tests without patching globals."""

    def open_kvm(self) -> int:
        return os.open("/dev/kvm", os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW)

    def is_character_device(self, fd: int) -> bool:
        return stat.S_ISCHR(os.fstat(fd).st_mode)

    def ioctl(self, fd: int, request: int, argument: int = 0) -> int:
        return int(fcntl.ioctl(fd, request, argument))

    def non_inheritable(self, fd: int) -> None:
        os.set_inheritable(fd, False)

    def map_vcpu(self, fd: int, size: int) -> Any:
        return mmap.mmap(fd, size, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ | mmap.PROT_WRITE)

    def close(self, fd: int) -> None:
        os.close(fd)


def run_smoke(ops: KvmOperations | None = None) -> SmokeResult:
    """Create and dispose of one empty VM and vCPU, without KVM_RUN."""
    if platform.system() != "Linux":
        return SmokeResult(False, "platform", "KVM requires Linux")
    ops = ops or KvmOperations()
    stage = "open-kvm"
    try:
        with contextlib.ExitStack() as cleanup:
            kvm_fd = ops.open_kvm()
            cleanup.callback(ops.close, kvm_fd)
            if not ops.is_character_device(kvm_fd):
                return SmokeResult(False, stage, "/dev/kvm is not a character device")
            stage = "api-version"
            version = ops.ioctl(kvm_fd, KVM_GET_API_VERSION)
            if version != KVM_API_VERSION:
                return SmokeResult(False, stage, f"unexpected KVM API version: {version}")
            stage = "run-mapping-size"
            mapping_size = ops.ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE)
            if not mmap.PAGESIZE <= mapping_size <= MAX_RUN_MAPPING:
                return SmokeResult(False, stage, f"invalid KVM run mapping size: {mapping_size}")
            stage = "create-vm"
            vm_fd = ops.ioctl(kvm_fd, KVM_CREATE_VM)
            cleanup.callback(ops.close, vm_fd)
            ops.non_inheritable(vm_fd)
            stage = "create-vcpu"
            vcpu_fd = ops.ioctl(vm_fd, KVM_CREATE_VCPU, 0)
            cleanup.callback(ops.close, vcpu_fd)
            ops.non_inheritable(vcpu_fd)
            stage = "map-vcpu"
            mapping = ops.map_vcpu(vcpu_fd, mapping_size)
            cleanup.callback(mapping.close)
            return SmokeResult(True, "complete", "empty VM and vCPU created and released")
    except (OSError, ValueError) as exc:
        return SmokeResult(False, stage, str(exc))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--confirm", action="store_true", help="explicitly allow temporary VM/vCPU allocation"
    )
    args = parser.parse_args()
    if not args.confirm:
        parser.error("this test allocates a temporary KVM VM: pass --confirm to run it")
    result = run_smoke()
    sys.stdout.write(json.dumps(asdict(result), indent=2, ensure_ascii=False) + "\n")
    return 0 if result.success else 1


if __name__ == "__main__":
    raise SystemExit(main())
