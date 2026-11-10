"""Offline boot image inspection; synthetic files only, never boot Android."""

from __future__ import annotations

import os
import struct

import pytest

from lynxdroid.backend.vm_image import ImageInspectionError, inspect_boot_image


def boot_header(version: int, *, kernel: int = 0, ramdisk: int = 0, signature: int = 0) -> bytes:
    header = bytearray(1584 if version == 4 else 1580)
    header[:8] = b"ANDROID!"
    struct.pack_into("<4I", header, 8, kernel, ramdisk, 0, len(header))
    struct.pack_into("<I", header, 40, version)
    if version == 4:
        struct.pack_into("<I", header, 1580, signature)
    return bytes(header)


@pytest.mark.parametrize("version", [3, 4])
def test_recognizes_supported_headers_but_does_not_claim_bootability(tmp_path, version) -> None:
    image = tmp_path / "boot.img"
    image.write_bytes(boot_header(version))
    info = inspect_boot_image(image)
    assert info.header_version == version
    assert info.architecture == "unknown"
    assert info.integrity == "not verified" and info.bootability == "not verified"


def test_v4_components_use_aligned_offsets(tmp_path) -> None:
    image = tmp_path / "boot.img"
    contents = bytearray(12288 + 3)
    contents[:1584] = boot_header(4, kernel=2, ramdisk=2, signature=3)
    image.write_bytes(contents)
    info = inspect_boot_image(image)
    assert (info.kernel_bytes, info.ramdisk_bytes, info.signature_bytes) == (2, 2, 3)
    image.write_bytes(contents[:-1])
    with pytest.raises(ImageInspectionError, match="truncated"):
        inspect_boot_image(image)


@pytest.mark.parametrize("component", ["kernel", "ramdisk", "signature"])
def test_rejects_truncated_declared_payload(tmp_path, component) -> None:
    image = tmp_path / "boot.img"
    image.write_bytes(boot_header(4, **{component: 100}))
    with pytest.raises(ImageInspectionError, match="truncated"):
        inspect_boot_image(image)


def test_rejects_header_size_and_unsupported_version(tmp_path) -> None:
    image = tmp_path / "boot.img"
    header = bytearray(boot_header(3))
    struct.pack_into("<I", header, 20, 1)
    image.write_bytes(header)
    with pytest.raises(ImageInspectionError, match="header size"):
        inspect_boot_image(image)
    struct.pack_into("<I", header, 40, 8)
    image.write_bytes(header)
    with pytest.raises(ImageInspectionError, match="unsupported"):
        inspect_boot_image(image)


def test_refuses_bad_magic_short_file_and_symlink(tmp_path) -> None:
    image = tmp_path / "boot.img"
    image.write_bytes(b"short")
    with pytest.raises(ImageInspectionError, match="truncated"):
        inspect_boot_image(image)
    image.write_bytes(b"not an image" * 100)
    with pytest.raises(ImageInspectionError, match="magic"):
        inspect_boot_image(image)
    link = tmp_path / "link.img"
    link.symlink_to(image)
    with pytest.raises(ImageInspectionError, match="cannot open"):
        inspect_boot_image(link)


def test_refuses_fifo_without_blocking(tmp_path) -> None:
    fifo = tmp_path / "fifo"
    os.mkfifo(fifo)
    with pytest.raises(ImageInspectionError, match="regular file"):
        inspect_boot_image(fifo)
