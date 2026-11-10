"""Read-only KVM prerequisite probe for the future standalone Android runtime.

This does not launch a VM, boot Android, install software or change host settings.
"""

from __future__ import annotations

import argparse
import errno
import fcntl
import json
import os
import platform
import stat
import sys
from pathlib import Path
from typing import Any

# Linux KVM_GET_API_VERSION: _IO(0xAE, 0x00). A successful KVM returns 12.
_KVM_GET_API_VERSION = 0xAE00
_KVM_API_VERSION = 12


def cpu_virtualization_hint(cpuinfo: str, machine: str) -> str:
    """An x86 flag is a hint, never proof that KVM is available to this process."""
    if machine not in ("x86_64", "amd64", "i386", "i686"):
        return "unknown"
    for line in cpuinfo.splitlines():
        key, separator, value = line.partition(":")
        if separator and key.strip() == "flags":
            flags = set(value.split())
            return "present" if {"vmx", "svm"} & flags else "not-reported"
    return "unknown"


def probe() -> dict[str, Any]:
    machine = platform.machine().lower()
    try:
        cpuinfo = Path("/proc/cpuinfo").read_text(encoding="utf-8", errors="replace")
    except OSError:
        cpuinfo = ""
    result: dict[str, Any] = {
        "scope": "KVM prerequisites only; Android runtime is not implemented",
        "machine": machine,
        "cpu_virtualization_flag": cpu_virtualization_hint(cpuinfo, machine),
        "kvm_status": "unknown",
        "kvm_api_version": None,
        "detail": "",
    }
    if platform.system() != "Linux":
        result.update(kvm_status="unsupported-os", detail="KVM requires Linux")
        return result
    try:
        fd = os.open("/dev/kvm", os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW)
    except OSError as exc:
        if exc.errno == errno.ENOENT:
            status = "missing"
        elif exc.errno in (errno.EACCES, errno.EPERM):
            status = "permission-denied"
        else:
            status = "open-error"
        result.update(kvm_status=status, detail=str(exc))
        return result
    try:
        if not stat.S_ISCHR(os.fstat(fd).st_mode):
            result.update(
                kvm_status="not-character-device", detail="/dev/kvm is not a character device"
            )
            return result
        try:
            version = fcntl.ioctl(fd, _KVM_GET_API_VERSION)
        except OSError as exc:
            result.update(kvm_status="ioctl-error", detail=str(exc))
            return result
        result["kvm_api_version"] = version
        if version == _KVM_API_VERSION:
            result.update(kvm_status="available", detail="KVM API 12 responded to this user")
        else:
            result.update(
                kvm_status="unexpected-api", detail=f"unexpected KVM API version: {version}"
            )
        return result
    finally:
        os.close(fd)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="print machine-readable observations")
    args = parser.parse_args()
    result = probe()
    if args.json:
        output = json.dumps(result, ensure_ascii=False, indent=2)
    else:
        output = "\n".join(
            [
                "Standalone VM host check (does not start Android)",
                f"CPU architecture: {result['machine']}",
                f"CPU virtualization flag: {result['cpu_virtualization_flag']}",
                f"KVM: {result['kvm_status']} — {result['detail']}",
                "A positive KVM check does not verify an Android image, GPU, games or performance.",
            ]
        )
    sys.stdout.write(output + "\n")
    return 0 if result["kvm_status"] == "available" else 1


if __name__ == "__main__":
    raise SystemExit(main())
