"""Inspect Android boot image headers without loading or executing the image.

Only AOSP boot header versions 3 and 4 are recognized. Header consistency is
not an integrity, architecture, provenance, licensing, or bootability check.
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

BOOT_MAGIC = b"ANDROID!"
PAGE_SIZE = 4096
HEADER_SIZES = {3: 1580, 4: 1584}


class ImageInspectionError(ValueError):
    """The selected file could not be inspected as a supported boot image."""


@dataclass(frozen=True)
class BootImageInfo:
    size_bytes: int
    header_version: int
    kernel_bytes: int
    ramdisk_bytes: int
    signature_bytes: int
    architecture: str = "unknown"
    integrity: str = "not verified"
    bootability: str = "not verified"


def _align(size: int) -> int:
    return (size + PAGE_SIZE - 1) // PAGE_SIZE * PAGE_SIZE


def inspect_boot_image(path: str | Path) -> BootImageInfo:
    """Inspect a user-selected regular file; read at most the fixed v4 header."""
    try:
        fd = os.open(os.fspath(path), os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
    except OSError as exc:
        raise ImageInspectionError(f"cannot open image: {exc.strerror or exc}") from exc
    try:
        metadata = os.fstat(fd)
        if not stat.S_ISREG(metadata.st_mode):
            raise ImageInspectionError("image must be a regular file, not a link or device")
        header = os.read(fd, HEADER_SIZES[4])
        if len(header) < 44:
            raise ImageInspectionError("truncated image header")
        if header[:8] != BOOT_MAGIC:
            raise ImageInspectionError("not an Android boot image (missing ANDROID! magic)")
        version = struct.unpack_from("<I", header, 40)[0]
        if version not in HEADER_SIZES:
            raise ImageInspectionError(f"unsupported boot header version: {version}")
        expected_header = HEADER_SIZES[version]
        if len(header) < expected_header:
            raise ImageInspectionError("truncated image header")
        kernel, ramdisk, _os_version, header_size = struct.unpack_from("<4I", header, 8)
        if header_size != expected_header:
            raise ImageInspectionError(f"unexpected v{version} header size: {header_size}")
        signature = struct.unpack_from("<I", header, HEADER_SIZES[3])[0] if version == 4 else 0
        kernel_start = PAGE_SIZE
        ramdisk_start = kernel_start + _align(kernel)
        signature_start = ramdisk_start + _align(ramdisk)
        # The last component need not include trailing page padding.
        required = expected_header
        if kernel:
            required = max(required, kernel_start + kernel)
        if ramdisk:
            required = max(required, ramdisk_start + ramdisk)
        if signature:
            required = max(required, signature_start + signature)
        if metadata.st_size < required:
            raise ImageInspectionError(
                "image is truncated according to its declared component sizes"
            )
        return BootImageInfo(metadata.st_size, version, kernel, ramdisk, signature)
    except OSError as exc:
        raise ImageInspectionError(f"cannot inspect image: {exc.strerror or exc}") from exc
    finally:
        os.close(fd)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", help="local Android boot image (header v3 or v4)")
    args = parser.parse_args()
    try:
        result = inspect_boot_image(Path(args.path))
    except ImageInspectionError as exc:
        sys.stderr.write(f"Image inspection failed: {exc}\n")
        return 1
    sys.stdout.write(json.dumps(asdict(result), indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
