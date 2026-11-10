"""Offline bzImage planning tests; synthetic headers only, never boot Linux."""

from __future__ import annotations

import struct

import pytest

from lynxdroid.backend.linux_boot import (
    KERNEL_ADDRESS,
    MIN_PROTOCOL,
    LinuxImageError,
    inspect_linux_image,
    main,
)

SETUP_SECTORS = 4
SETUP_BYTES = (SETUP_SECTORS + 1) * 512


def header(
    *,
    protocol: int = MIN_PROTOCOL,
    loadflags: int = 0x81,
    xloadflags: int = 1,
    setup_sectors: int = SETUP_SECTORS,
    kernel_bytes: int = 4096,
    init_size: int = 1 << 21,
    alignment: int = 0x1000,
    relocatable: bool = True,
    preferred: int = 0,
) -> bytearray:
    image = bytearray(SETUP_BYTES + kernel_bytes)
    struct.pack_into("<I", image, 0x1F4, kernel_bytes // 16 + (1 if kernel_bytes % 16 else 0))
    struct.pack_into("<H", image, 0x1FE, 0xAA55)
    image[0x202:0x206] = b"HdrS"
    struct.pack_into("<H", image, 0x206, protocol)
    image[0x1F1] = setup_sectors
    image[0x211] = loadflags
    struct.pack_into("<H", image, 0x236, xloadflags)
    struct.pack_into("<I", image, 0x230, alignment)
    image[0x234] = 1 if relocatable else 0
    struct.pack_into("<I", image, 0x238, 2048)
    struct.pack_into("<Q", image, 0x258, preferred)
    struct.pack_into("<I", image, 0x260, init_size)
    return image


def write(tmp_path, image: bytes):
    path = tmp_path / "bzImage"
    path.write_bytes(image)
    return path


def test_plans_layout_without_claiming_bootability(tmp_path) -> None:
    plan = inspect_linux_image(write(tmp_path, header()), 64 * 1024 * 1024)
    assert plan.fits and not plan.problems
    assert plan.protocol == "2.0c"
    assert plan.setup_bytes == SETUP_BYTES
    assert plan.entry_address == KERNEL_ADDRESS + 0x200
    assert plan.minimum_ram_bytes > 0
    assert "not verified" in plan.bootability


def test_reports_insufficient_ram(tmp_path) -> None:
    plan = inspect_linux_image(write(tmp_path, header(init_size=1 << 30)), 64 * 1024 * 1024)
    assert not plan.fits
    assert any("does not fit" in problem for problem in plan.problems)


def test_rejects_runtime_window_above_identity_map(tmp_path) -> None:
    plan = inspect_linux_image(
        write(tmp_path, header(relocatable=False, preferred=0xF0000000, init_size=0x20000000)),
        64 * 1024 * 1024 * 1024,
    )
    assert not plan.fits
    assert any("4 GiB identity map" in problem for problem in plan.problems)


@pytest.mark.parametrize(
    ("change", "message"),
    [
        ({"protocol": 0x020B}, "boot protocol"),
        ({"loadflags": 0x80}, "LOAD_HIGH"),
        ({"xloadflags": 0}, "64-bit"),
        ({"alignment": 3}, "power of two"),
        ({"setup_sectors": 200}, "implausible"),
        ({"init_size": 0}, "initialisation memory"),
    ],
)
def test_rejects_unsupported_headers(tmp_path, change, message) -> None:
    with pytest.raises(LinuxImageError, match=message):
        inspect_linux_image(write(tmp_path, header(**change)), 64 * 1024 * 1024)


def test_rejects_declared_kernel_larger_than_file(tmp_path) -> None:
    image = header()
    struct.pack_into("<I", image, 0x1F4, 0xFFFF)
    with pytest.raises(LinuxImageError, match="declared kernel size"):
        inspect_linux_image(write(tmp_path, image), 64 * 1024 * 1024)


def test_rejects_missing_signature_and_truncation(tmp_path) -> None:
    image = header()
    image[0x202:0x206] = b"nope"
    with pytest.raises(LinuxImageError, match="boot signature"):
        inspect_linux_image(write(tmp_path, image), 64 * 1024 * 1024)
    with pytest.raises(LinuxImageError, match="truncated"):
        inspect_linux_image(write(tmp_path, b"short"), 64 * 1024 * 1024)


def test_rejects_symlink_and_directory(tmp_path) -> None:
    real = write(tmp_path, header())
    link = tmp_path / "link"
    link.symlink_to(real)
    with pytest.raises(LinuxImageError):
        inspect_linux_image(link, 64 * 1024 * 1024)
    with pytest.raises(LinuxImageError, match="regular file"):
        inspect_linux_image(tmp_path, 64 * 1024 * 1024)


def test_cli_reports_failure_without_booting(tmp_path, capsys) -> None:
    path = write(tmp_path, header(init_size=1 << 30))
    assert main([str(path), "--ram-mib", "16"]) == 1
    assert "fits: no" in capsys.readouterr().out
    path_ok = write(tmp_path, header())
    assert main([str(path_ok), "--ram-mib", "64", "--json"]) == 0
    assert '"fits": true' in capsys.readouterr().out
