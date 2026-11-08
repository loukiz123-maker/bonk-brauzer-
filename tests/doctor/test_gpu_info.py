"""GPU detection against described machines, not the one we happen to run on.

``gpu_info`` reads sysfs, which in the sandbox has no DRM subsystem at all, so
it was never exercised: 125 lines with no test behind them. The roots are now
parameters, and these tests build a sysfs tree for machines we cannot buy -
one Intel GPU, two GPUs with the discrete card alphabetically first, a
virtual card with no PCI address, a card with no render node.
"""

from __future__ import annotations

from pathlib import Path

from lynxdroid.doctor.platform import gpu_info


def _pci_address(link_target: str) -> str | None:
    import re

    match = re.search(r"\d{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9a-f]", link_target, re.I)
    return match.group(0) if match else None


def _card(
    drm: Path,
    *,
    name: str,
    address: str | None,
    vendor: str,
    device: str,
    uevent_driver: str,
) -> None:
    """Create one card the way the kernel lays it out under /sys/class/drm.

    With a PCI address, ``cardN/device`` is a symlink into ``drm/devices``;
    without one (virtio), it is a real directory, as on the real kernel.
    """
    card = drm / name
    card.mkdir(parents=True)
    if address is None:
        device_dir = card / "device"
        device_dir.mkdir()
    else:
        device_dir = drm / "devices" / address
        device_dir.mkdir(parents=True)
        (card / "device").symlink_to(device_dir)
    (device_dir / "vendor").write_text(f"{vendor}\n", encoding="utf-8")
    (device_dir / "device").write_text(f"{device}\n", encoding="utf-8")
    (device_dir / "uevent").write_text(f"DRIVER={uevent_driver}\n", encoding="utf-8")


def _render_node(drm: Path, *, name: str, address: str | None) -> None:
    """``renderDN/device`` points at the same PCI device the card does."""
    node = drm / name
    node.mkdir(parents=True)
    if address is None:
        target = drm / "devices" / "virtual"
        target.mkdir(parents=True)
    else:
        target = drm / "devices" / address
        target.mkdir(parents=True, exist_ok=True)
    (node / "device").symlink_to(target)


class TestGpuDetection:
    def test_a_machine_with_no_drm_says_so(self, tmp_path: Path) -> None:
        info, reason = gpu_info(drm_root=tmp_path / "missing", module_root=tmp_path)
        assert info is None
        assert "absent" in reason

    def test_a_machine_with_no_cards_says_so(self, tmp_path: Path) -> None:
        drm = tmp_path / "drm"
        drm.mkdir()
        info, reason = gpu_info(drm_root=drm, module_root=tmp_path)
        assert info is None
        assert "no DRM card" in reason

    def test_an_intel_card_is_identified_as_intel(self, tmp_path: Path) -> None:
        drm = tmp_path / "drm"
        _card(
            drm,
            name="card0",
            address="0000:00:02.0",
            vendor="0x8086",
            device="0x9a49",
            uevent_driver="i915",
        )
        _render_node(drm, name="renderD128", address="0000:00:02.0")
        module = tmp_path / "module" / "i915"
        module.mkdir(parents=True)
        (module / "version").write_text("6.1.0-cachyos\n", encoding="utf-8")

        info, reason = gpu_info(drm_root=drm, module_root=tmp_path / "module")

        assert reason == ""
        assert info is not None
        assert info.is_intel
        assert info.kernel_driver == "i915"
        assert info.driver_version == "6.1.0-cachyos"

    def test_two_gpus_pick_the_render_node_of_the_card_being_reported(self, tmp_path: Path) -> None:
        """The bug this file was written for.

        Matching a render node by ``endswith("0")`` - the card index - ties
        renderD128 to card0 on one machine and to whichever node sorts first
        on another, so the report pairs an Intel card with the NVIDIA driver.
        The link must be made by PCI address.
        """
        drm = tmp_path / "drm"
        # Function .1 on the discrete card: with ".0" both addresses happen
        # to end in "0", which is exactly the suffix the broken code matched,
        # so the bug would stay invisible.
        _card(
            drm,
            name="card0",
            address="0000:01:00.1",
            vendor="0x10de",
            device="0x25a0",
            uevent_driver="nvidia",
        )
        _card(
            drm,
            name="card1",
            address="0000:00:02.0",
            vendor="0x8086",
            device="0x9a49",
            uevent_driver="i915",
        )
        _render_node(drm, name="renderD128", address="0000:00:02.0")
        _render_node(drm, name="renderD129", address="0000:01:00.1")

        info, reason = gpu_info(drm_root=drm, module_root=tmp_path)

        assert reason == ""
        assert info is not None
        # card0 is the discrete card and is what gets reported...
        assert info.vendor == "NVIDIA"
        # ...so its render node must be the one paired with 0000:01:00.1,
        # which is renderD129 - not renderD128, the node that merely sorts first.
        assert info.render_node == "/dev/dri/renderD129", (
            f"the NVIDIA card was paired with {info.render_node}"
        )

    def test_a_virtual_card_without_a_pci_address_still_finds_a_node(self, tmp_path: Path) -> None:
        drm = tmp_path / "drm"
        _card(
            drm,
            name="card0",
            address=None,
            vendor="0x1af4",
            device="0x1050",
            uevent_driver="virtio_gpu",
        )
        _render_node(drm, name="renderD128", address=None)

        info, reason = gpu_info(drm_root=drm, module_root=tmp_path)

        assert reason == ""
        assert info is not None
        assert info.vendor == "Virtio"
        assert info.render_node == "/dev/dri/renderD128"

    def test_a_card_without_a_render_node_reports_none(self, tmp_path: Path) -> None:
        drm = tmp_path / "drm"
        _card(
            drm,
            name="card0",
            address="0000:00:02.0",
            vendor="0x8086",
            device="0x9a49",
            uevent_driver="i915",
        )

        info, _reason = gpu_info(drm_root=drm, module_root=tmp_path)

        assert info is not None
        assert info.render_node is None
        assert info.render_node_usable is False

    def test_an_unknown_driver_does_not_claim_a_version(self, tmp_path: Path) -> None:
        drm = tmp_path / "drm"
        _card(
            drm,
            name="card0",
            address="0000:00:02.0",
            vendor="0x8086",
            device="0x9a49",
            uevent_driver="i915",
        )

        info, _reason = gpu_info(drm_root=drm, module_root=tmp_path / "no-modules")

        assert info is not None
        assert info.driver_version == "unknown"
