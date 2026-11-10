"""Plan a Linux/x86 bzImage layout offline, without running any guest code.

This mirrors the experimental C staging helpers: it recognizes only the
narrow subset the prototype would stage and never claims bootability.
Nothing is written, no VM is created and no kernel is executed.
"""

from __future__ import annotations

import argparse
import json
import os
import stat
import struct
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

HEADER_BYTES = 0x264
MAX_IMAGE_BYTES = 1024 * 1024 * 1024
SETUP_ADDRESS = 0x10000
KERNEL_ADDRESS = 0x100000
MAX_SETUP_BYTES = 0x8000
BOOT_PARAMS_ADDRESS = 0x7000
BOOT_PARAMS_BYTES = 4096
CMDLINE_ADDRESS = 0x80000
GDT_ADDRESS = 0xA00
TABLES_START = 0x1000
TABLES_END = 0x7000
ENTRY_OFFSET = 0x200
MIN_PROTOCOL = 0x020C


class LinuxImageError(ValueError):
    """The selected file is not a supported x86 Linux boot image."""


@dataclass(frozen=True)
class LinuxBootPlan:
    size_bytes: int
    protocol: str
    setup_bytes: int
    kernel_bytes: int
    init_size: int
    kernel_alignment: int
    relocatable: bool
    preferred_address: int
    runtime_address: int
    entry_address: int
    minimum_ram_bytes: int
    ram_bytes: int
    fits: bool
    problems: tuple[str, ...]
    bootability: str = "not verified: no vCPU, devices or Android runtime"


def _open_regular(path: str | Path) -> tuple[int, int]:
    try:
        fd = os.open(os.fspath(path), os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
    except OSError as exc:
        raise LinuxImageError(f"cannot open image: {exc.strerror or exc}") from exc
    metadata = os.fstat(fd)
    if not stat.S_ISREG(metadata.st_mode):
        os.close(fd)
        raise LinuxImageError("image must be a regular file, not a link or device")
    if metadata.st_size > MAX_IMAGE_BYTES:
        os.close(fd)
        raise LinuxImageError("image is larger than the supported limit")
    return fd, metadata.st_size


def _runtime_address(header: bytes, relocatable: bool, alignment: int) -> int:
    preferred = int(struct.unpack_from("<Q", header, 0x258)[0])
    runtime = preferred
    if relocatable:
        if runtime < KERNEL_ADDRESS:
            runtime = KERNEL_ADDRESS
        if runtime > (1 << 64) - 1 - (alignment - 1):
            raise LinuxImageError("kernel runtime address overflows")
        runtime = (runtime + alignment - 1) & ~(alignment - 1)
    return runtime


def inspect_linux_image(path: str | Path, ram_bytes: int) -> LinuxBootPlan:
    """Read only the fixed header and describe the layout this prototype would need."""
    fd, size = _open_regular(path)
    try:
        header = os.read(fd, HEADER_BYTES)
    finally:
        os.close(fd)
    if len(header) < HEADER_BYTES:
        raise LinuxImageError("truncated kernel header")
    if struct.unpack_from("<H", header, 0x1FE)[0] != 0xAA55 or header[0x202:0x206] != b"HdrS":
        raise LinuxImageError("not an x86 Linux boot image (missing boot signature)")
    protocol = struct.unpack_from("<H", header, 0x206)[0]
    loadflags = header[0x211]
    xloadflags = struct.unpack_from("<H", header, 0x236)[0]
    if protocol < MIN_PROTOCOL:
        raise LinuxImageError(f"unsupported boot protocol 0x{protocol:04x}; need 2.12 or newer")
    if not loadflags & 0x01:
        raise LinuxImageError("only LOAD_HIGH (bzImage) images are supported")
    if not xloadflags & 0x01:
        raise LinuxImageError("only 64-bit kernels are supported")
    setup_sectors = header[0x1F1] or 4
    setup_bytes = (setup_sectors + 1) * 512
    kernel_bytes = struct.unpack_from("<I", header, 0x1F4)[0] * 16
    if setup_bytes > MAX_SETUP_BYTES or setup_bytes > size:
        raise LinuxImageError("kernel setup size is implausible for this file")
    if kernel_bytes == 0 or kernel_bytes > size - setup_bytes:
        raise LinuxImageError("declared kernel size does not fit the selected file")
    init_size = struct.unpack_from("<I", header, 0x260)[0]
    alignment = struct.unpack_from("<I", header, 0x230)[0]
    relocatable = bool(header[0x234])
    if not init_size:
        raise LinuxImageError("kernel reports no initialisation memory")
    if relocatable and (alignment == 0 or alignment & (alignment - 1)):
        raise LinuxImageError("kernel alignment is not a power of two")
    runtime = _runtime_address(header, relocatable, alignment or 1)
    if runtime < KERNEL_ADDRESS:
        raise LinuxImageError("kernel runtime address is below 1 MiB")

    requirements = {
        "page tables": TABLES_END,
        "GDT": GDT_ADDRESS + 24,
        "boot_params": BOOT_PARAMS_ADDRESS + BOOT_PARAMS_BYTES,
        "kernel setup": SETUP_ADDRESS + setup_bytes,
        "boot command line": CMDLINE_ADDRESS + 1,
        "kernel image": KERNEL_ADDRESS + kernel_bytes,
        "kernel runtime window": runtime + init_size,
    }
    minimum = max(requirements.values())
    problems: list[str] = []
    if runtime + init_size > 1 << 32:
        problems.append("kernel runtime window is above the 4 GiB identity map")
    if ram_bytes < minimum:
        problems.append(f"guest RAM {ram_bytes} is below the required {minimum} bytes")
    for name, end in requirements.items():
        if end > ram_bytes:
            problems.append(f"{name} does not fit in {ram_bytes} bytes of guest RAM")
    return LinuxBootPlan(
        size_bytes=size,
        protocol=f"{protocol >> 8}.{protocol & 0xFF:02x}",
        setup_bytes=setup_bytes,
        kernel_bytes=kernel_bytes,
        init_size=init_size,
        kernel_alignment=alignment,
        relocatable=relocatable,
        preferred_address=struct.unpack_from("<Q", header, 0x258)[0],
        runtime_address=runtime,
        entry_address=KERNEL_ADDRESS + ENTRY_OFFSET,
        minimum_ram_bytes=minimum,
        ram_bytes=ram_bytes,
        fits=not problems,
        problems=tuple(problems),
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Inspect an x86 bzImage offline; never boots it.")
    parser.add_argument("image")
    parser.add_argument("--ram-mib", type=int, default=2048)
    parser.add_argument("--json", action="store_true")
    arguments = parser.parse_args(argv)
    if arguments.ram_mib <= 0:
        parser.error("--ram-mib must be positive")
    try:
        plan = inspect_linux_image(arguments.image, arguments.ram_mib * 1024 * 1024)
    except LinuxImageError as exc:
        sys.stderr.write(f"error: {exc}\n")
        return 1
    if arguments.json:
        sys.stdout.write(json.dumps(asdict(plan), indent=2) + "\n")
    else:
        lines = [
            f"protocol: {plan.protocol}",
            f"setup: {plan.setup_bytes} bytes; kernel: {plan.kernel_bytes} bytes",
            f"runtime: 0x{plan.runtime_address:x}; entry: 0x{plan.entry_address:x}",
            f"minimum RAM: {plan.minimum_ram_bytes} bytes",
            f"fits: {'yes' if plan.fits else 'no'}",
            *(f"  - {problem}" for problem in plan.problems),
            plan.bootability,
        ]
        sys.stdout.write("\n".join(lines) + "\n")
    return 0 if plan.fits else 1


if __name__ == "__main__":
    raise SystemExit(main())
