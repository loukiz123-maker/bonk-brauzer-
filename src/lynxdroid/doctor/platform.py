"""Read-only discovery of the host platform.

Every function here only *reads*: files under /proc and /sys, ``uname``,
package-manager queries and version flags of installed binaries. Nothing is
mounted, loaded, enabled or written. That constraint is what makes ``doctor``
safe to run first (specification stage 3).

Any value we cannot establish is returned as ``None`` together with the reason,
so the report can say "could not determine" instead of guessing.
"""

from __future__ import annotations

import gzip
import os
import platform as _platform
import re
from dataclasses import dataclass, field
from pathlib import Path

from lynxdroid.core import proc

# --------------------------------------------------------------------------
# Small read helpers
# --------------------------------------------------------------------------


def read_text(path: str | Path, *, limit: int = 65536) -> str | None:
    """Read a small text file, returning ``None`` when unreadable."""
    try:
        with Path(path).open(encoding="utf-8", errors="replace") as handle:
            return handle.read(limit)
    except OSError:
        return None


def read_first_line(path: str | Path) -> str | None:
    text = read_text(path, limit=4096)
    if not text:
        return None
    return text.splitlines()[0].strip()


def read_symlink(path: str | Path) -> str | None:
    try:
        return str(Path(path).readlink())
    except OSError:
        return None


def path_exists(path: str | Path) -> bool:
    return Path(path).exists()


# --------------------------------------------------------------------------
# Hardware / OS
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class OsInfo:
    distro_id: str
    distro_name: str
    pretty_name: str
    kernel_release: str
    machine: str
    is_arch_family: bool
    is_cachyos: bool
    source: str


def os_release() -> dict[str, str]:
    text = read_text("/etc/os-release") or read_text("/usr/lib/os-release") or ""
    data: dict[str, str] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        data[key.strip()] = value.strip().strip('"').strip("'")
    return data


def os_info() -> OsInfo:
    release = os_release()
    distro_id = release.get("ID", "").lower()
    id_like = release.get("ID_LIKE", "").lower().split()
    is_arch = distro_id in {"arch", "cachyos", "endeavouros", "manjaro"} or "arch" in id_like
    return OsInfo(
        distro_id=distro_id or "unknown",
        distro_name=release.get("NAME", "unknown"),
        pretty_name=release.get("PRETTY_NAME", release.get("NAME", "unknown")),
        kernel_release=_platform.release(),
        machine=_platform.machine(),
        is_arch_family=is_arch,
        is_cachyos="cachyos" in distro_id or "cachy" in release.get("PRETTY_NAME", "").lower(),
        source="/etc/os-release" if Path("/etc/os-release").exists() else "unavailable",
    )


def package_manager() -> str | None:
    """Which package manager the host actually uses (Arch family only)."""
    for candidate in ("pacman", "paru", "yay"):
        if proc.have(candidate):
            return candidate
    if proc.have("apt"):
        return "apt"
    if proc.have("dnf"):
        return "dnf"
    return None


def package_version(package: str) -> str | None:
    """Installed version of an Arch package, or ``None`` if unknown/absent."""
    if not proc.have("pacman"):
        return None
    result = proc.run(["pacman", "-Q", package], timeout=8.0)
    if not result.ok:
        return None
    parts = result.stdout.strip().split()
    return parts[1] if len(parts) >= 2 else None


# --------------------------------------------------------------------------
# Session / desktop
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class SessionInfo:
    session_type: str | None  # "wayland" | "x11" | None
    desktop: str | None  # XDG_CURRENT_DESKTOP, e.g. "KDE"
    wayland_display: str | None
    wayland_socket: Path | None
    wayland_socket_usable: bool
    display: str | None
    is_kde: bool
    running_as_root: bool
    in_container: bool
    container_hint: str
    reasons: list[str] = field(default_factory=list)


def _detect_container() -> tuple[bool, str]:
    # systemd/docker set this variable in lower case by convention.
    env_marker = os.environ.get("container", "").strip()  # noqa: SIM112
    if env_marker:
        return True, f"container env marker: {env_marker}"
    for marker in ("/.dockerenv", "/run/.containerenv"):
        if Path(marker).exists():
            return True, f"marker file {marker} present"
    cgroup = read_text("/proc/1/cgroup", limit=8192) or ""
    for needle in ("docker", "lxc", "kubepods", "podman", "containerd"):
        if needle in cgroup:
            return True, f"/proc/1/cgroup mentions {needle}"
    if proc.have("systemd-detect-virt"):
        result = proc.run(["systemd-detect-virt", "--container"], timeout=5.0)
        virt = result.stdout.strip()
        if result.ok and virt and virt != "none":
            return True, f"systemd-detect-virt reports {virt}"
    return False, "no container markers found"


def session_info() -> SessionInfo:
    reasons: list[str] = []
    session_type = os.environ.get("XDG_SESSION_TYPE", "").strip().lower() or None
    display = os.environ.get("DISPLAY", "").strip() or None
    wayland_display = os.environ.get("WAYLAND_DISPLAY", "").strip() or None

    if session_type is None:
        if wayland_display:
            session_type = "wayland"
            reasons.append("XDG_SESSION_TYPE unset, WAYLAND_DISPLAY present")
        elif display:
            session_type = "x11"
            reasons.append("XDG_SESSION_TYPE unset, DISPLAY present")
        else:
            reasons.append("XDG_SESSION_TYPE unset and no display variable")

    socket: Path | None = None
    usable = False
    if wayland_display:
        if Path(wayland_display).is_absolute():
            socket = Path(wayland_display)
        else:
            runtime = os.environ.get("XDG_RUNTIME_DIR", "")
            if runtime:
                socket = Path(runtime) / wayland_display
        if socket is not None:
            usable = socket.exists() and os.access(socket, os.R_OK | os.W_OK)
            if not socket.exists():
                reasons.append(f"Wayland socket {socket} does not exist")
            elif not usable:
                reasons.append(f"Wayland socket {socket} exists but is not writable")

    desktop = os.environ.get("XDG_CURRENT_DESKTOP", "").strip() or None
    desktop_lower = (desktop or "").lower()
    is_kde = (
        "kde" in desktop_lower
        or "plasma" in desktop_lower
        or bool(os.environ.get("KDE_FULL_SESSION"))
    )

    in_container, container_hint = _detect_container()
    running_as_root = os.geteuid() == 0

    return SessionInfo(
        session_type=session_type,
        desktop=desktop,
        wayland_display=wayland_display,
        wayland_socket=socket,
        wayland_socket_usable=usable,
        display=display,
        is_kde=is_kde,
        running_as_root=running_as_root,
        in_container=in_container,
        container_hint=container_hint,
        reasons=reasons,
    )


# --------------------------------------------------------------------------
# Kernel features: binder, memfd, namespaces
# --------------------------------------------------------------------------

_CONFIG_PATHS = ("/proc/config.gz", "/boot/config-{release}", "/usr/lib/modules/{release}/config")

_BINDER_KEYS = (
    "CONFIG_ANDROID",
    "CONFIG_ANDROID_BINDER_IPC",
    "CONFIG_ANDROID_BINDERFS",
    "CONFIG_ANDROID_BINDER_DEVICES",
    "CONFIG_ASHMEM",
    "CONFIG_PSI",
)


@dataclass(frozen=True)
class KernelConfig:
    values: dict[str, str]
    source: str | None
    error: str | None

    def get(self, key: str) -> str | None:
        return self.values.get(key)

    @property
    def available(self) -> bool:
        return self.source is not None


def kernel_config(release: str | None = None) -> KernelConfig:
    """Read the running kernel's config from whichever source exists.

    Order: /proc/config.gz (CONFIG_IKCONFIG_PROC), /boot/config-*, then
    /usr/lib/modules/*/config. If none is present we report UNKNOWN with the
    reason instead of claiming "unsupported" - a kernel can perfectly well
    support binder without exposing its config.
    """
    release = release or _platform.release()
    tried: list[str] = []
    for template in _CONFIG_PATHS:
        candidate = template.format(release=release)
        tried.append(candidate)
        if not Path(candidate).exists():
            continue
        text: str | None
        if candidate.endswith(".gz"):
            try:
                with gzip.open(candidate, "rt", encoding="utf-8", errors="replace") as handle:
                    text = handle.read(1_000_000)
            except OSError as exc:
                return KernelConfig({}, None, f"{candidate}: {exc.strerror}")
        else:
            text = read_text(candidate, limit=1_000_000)
        if text is None:
            continue
        values: dict[str, str] = {}
        for line in text.splitlines():
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                # "# CONFIG_X is not set" is informative
                match = re.match(r"#\s*(CONFIG_\w+)\s+is not set", stripped)
                if match:
                    values[match.group(1)] = "n"
                continue
            key, _, value = stripped.partition("=")
            values[key] = value.strip().strip('"')
        return KernelConfig(values, candidate, None)
    return KernelConfig({}, None, "no kernel config exposed by: " + ", ".join(tried))


def binder_devices() -> list[Path]:
    """Existing binder char devices, from both the /dev root and binderfs."""
    found: list[Path] = []
    for directory in (Path("/dev"), Path("/dev/binderfs")):
        if not directory.is_dir():
            continue
        try:
            entries = sorted(directory.iterdir())
        except OSError:
            continue
        for entry in entries:
            name = entry.name
            if "binder" not in name:
                continue
            try:
                if entry.is_char_device():
                    found.append(entry)
            except OSError:
                continue
    return found


def binderfs_mounted() -> bool:
    mounts = read_text("/proc/mounts", limit=262144) or ""
    return "binder" in mounts and "/dev/binderfs" in mounts


def binder_module_loaded() -> bool:
    for name in ("binder_linux", "binder", "binder_ndk"):
        if Path(f"/sys/module/{name}").is_dir():
            return True
    return False


def binder_supported_in_filesystems() -> bool:
    filesystems = read_text("/proc/filesystems", limit=65536) or ""
    return any(line.split() and line.split()[-1] == "binder" for line in filesystems.splitlines())


def memfd_available() -> tuple[bool, str]:
    """Probe ``memfd_create`` through libc without creating visible files."""
    import ctypes
    import ctypes.util

    libc_name = ctypes.util.find_library("c")
    if libc_name is None:
        return False, "libc not resolvable"
    try:
        libc = ctypes.CDLL(libc_name, use_errno=True)
    except OSError as exc:  # pragma: no cover - unusual
        return False, f"cannot load libc: {exc}"
    func = getattr(libc, "memfd_create", None)
    if func is None:
        return False, "libc has no memfd_create symbol"
    func.argtypes = [ctypes.c_char_p, ctypes.c_uint]
    func.restype = ctypes.c_int
    fd = func(b"lynxdroid-probe", 0)
    if fd < 0:
        import errno

        code = ctypes.get_errno()
        # EPERM/EACCES = blocked by seccomp (e.g. inside a restricted container)
        return False, f"memfd_create failed: {errno.errorcode.get(code, code)}"
    os.close(fd)
    return True, "memfd_create works"


def namespaces_available() -> tuple[bool, list[str]]:
    """Which namespaces the running kernel exposes (container prerequisite)."""
    wanted = ("user", "pid", "net", "mnt", "ipc", "uts", "cgroup")
    present = [name for name in wanted if Path(f"/proc/self/ns/{name}").exists()]
    return len(present) == len(wanted), present


# --------------------------------------------------------------------------
# Graphics
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class GpuInfo:
    vendor: str
    driver: str
    driver_version: str
    kernel_driver: str
    render_node: str | None
    render_node_usable: bool
    is_intel: bool
    is_iris_xe_class: bool
    source: str


_INTEL_GEN12_PLUS = re.compile(
    r"^(Alder Lake|Raptor Lake|Meteor Lake|Lunar Lake|Arrow Lake|"
    r"Tiger Lake|Rocket Lake|DG1|DG2|Arc)",
    re.IGNORECASE,
)
_IRIS_XE_RE = re.compile(r"\bIris Xe\b", re.IGNORECASE)


def _pci_vendor_name(vendor_id: str) -> str:
    return {
        "0x8086": "Intel",
        "0x1002": "AMD",
        "0x10de": "NVIDIA",
        "0x1af4": "Virtio",
        "0x1234": "QEMU/Bochs",
    }.get(vendor_id.lower(), vendor_id)


def gpu_info(
    drm_root: Path | None = None, module_root: Path | None = None
) -> tuple[GpuInfo | None, str]:
    """Identify the active DRM device and its kernel driver.

    Returns ``(info, reason_when_missing)``. Only /sys is read.

    The two roots are parameters so a test can describe a machine (one GPU,
    two GPUs, a missing subsystem) without the hardware being present. They
    default to the real sysfs.
    """
    drm_root = Path(drm_root) if drm_root is not None else Path("/sys/class/drm")
    module_root = Path(module_root) if module_root is not None else Path("/sys/module")
    if not drm_root.is_dir():
        return None, "/sys/class/drm is absent (no DRM subsystem)"
    cards = sorted(p for p in drm_root.iterdir() if re.fullmatch(r"card\d+", p.name))
    render_nodes = sorted(p for p in drm_root.iterdir() if re.fullmatch(r"renderD\d+", p.name))
    if not cards:
        return None, "no DRM card devices under /sys/class/drm"

    chosen = cards[0]
    device_dir = chosen / "device"
    vendor_id = read_first_line(device_dir / "vendor") or ""
    device_id = read_first_line(device_dir / "device") or ""
    kernel_driver = read_symlink(device_dir / "driver") or ""
    driver_name = Path(kernel_driver).name if kernel_driver else "unknown"
    drm_driver = read_first_line(device_dir / "uevent") or ""
    driver_version = ""
    for line in (read_text(device_dir / "uevent", limit=8192) or "").splitlines():
        if line.startswith("DRIVER="):
            driver_name = line.split("=", 1)[1].strip()
    if not drm_driver:
        drm_driver = f"{driver_name} (uevent unavailable)"
    version_file = module_root / driver_name / "version"
    if version_file.exists():
        driver_version = read_first_line(version_file) or ""

    node_name: str | None = None
    usable = False
    # Link a render node to the card by PCI address. The previous test was
    # ``link.endswith("0")`` - the card index, not part of a PCI address -
    # which picks an arbitrary node on a machine with two GPUs and made the
    # report blame the wrong driver.
    card_address = _pci_address(device_dir)
    for node in render_nodes:
        link = read_symlink(node / "device")
        if card_address and link and _pci_address_from_link(link) == card_address:
            node_name = f"/dev/dri/{node.name}"
            break
    if node_name is None:
        for node in render_nodes:
            link = read_symlink(node / "device")
            if link and _pci_address_from_link(link) is None:
                # A virtual driver (virtio, vkms) exposes no PCI address.
                node_name = f"/dev/dri/{node.name}"
                break
    if node_name is None and len(render_nodes) == 1:
        # Fall back to the single render node: with one GPU there is no
        # ambiguity about which device backs the card.
        node_name = f"/dev/dri/{render_nodes[0].name}"
    if node_name:
        usable = Path(node_name).exists() and os.access(node_name, os.R_OK | os.W_OK)

    model = _gpu_model_name(device_dir) or f"{_pci_vendor_name(vendor_id)} {device_id}"
    vendor = _pci_vendor_name(vendor_id)
    is_intel = vendor_id.lower() == "0x8086"
    info = GpuInfo(
        vendor=vendor,
        driver=f"{model} [{driver_name}]",
        driver_version=driver_version or "unknown",
        kernel_driver=driver_name,
        render_node=node_name,
        render_node_usable=usable,
        is_intel=is_intel,
        is_iris_xe_class=bool(_IRIS_XE_RE.search(model)) or bool(_INTEL_GEN12_PLUS.search(model)),
        source=str(device_dir),
    )
    return info, ""


_PCI_ADDRESS_RE = re.compile(r"[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-9a-fA-F]")


def _pci_address(device_dir: Path) -> str | None:
    """The PCI address of a card's ``device`` directory, e.g. ``0000:00:02.0``.

    The directory name itself is the address; the symlink target is used as
    a fallback for older layouts where it is not.
    """
    if _PCI_ADDRESS_RE.fullmatch(device_dir.name):
        return device_dir.name.lower()
    return _pci_address_from_link(read_symlink(device_dir) or "")


def _pci_address_from_link(link: str) -> str | None:
    """The first PCI address inside a sysfs symlink target."""
    match = _PCI_ADDRESS_RE.search(link)
    return match.group(0).lower() if match else None


def _gpu_model_name(device_dir: Path) -> str | None:
    # Best-effort: /sys does not expose marketing names; lspci does but is
    # optional. We use it only if present, never as a hard requirement.
    if not proc.have("lspci"):
        return None
    result = proc.run(["lspci", "-mm", "-nn"], timeout=8.0)
    if not result.ok:
        return None
    for line in result.stdout.splitlines():
        if re.search(r'"VGA compatible controller"|"Display controller"|"3D controller"', line):
            fields = [f.strip('"') for f in re.split(r'(?<=")\s+(?=")', line)]
            if len(fields) >= 4:
                model = fields[-1]
                return re.sub(r"\s*\[[0-9a-fA-F]{4}:[0-9a-fA-F]{4}\]$", "", model)
    return None


def mesa_driver_hint() -> tuple[str | None, str]:
    """Which Mesa DRI driver is configured for Intel, from env or the system."""
    override = os.environ.get("MESA_LOADER_DRIVER_OVERRIDE")
    if override:
        return override, "MESA_LOADER_DRIVER_OVERRIDE is set"
    for probe in (
        "/usr/lib/dri/iris_dri.so",
        "/usr/lib64/dri/iris_dri.so",
        "/usr/lib/x86_64-linux-gnu/dri/iris_dri.so",
    ):
        if Path(probe).exists():
            return "iris", f"found {probe}"
    return None, "no iris_dri.so found in the usual Mesa paths"


def vulkan_driver_hint() -> tuple[str | None, str]:
    for probe in (
        "/usr/share/vulkan/icd.d/intel_icd.x86_64.json",
        "/usr/share/vulkan/icd.d/intel_icd.json",
    ):
        if Path(probe).exists():
            return "anv", f"found {probe}"
    return None, "no Intel Vulkan ICD json found"


# --------------------------------------------------------------------------
# Waydroid installation
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class WaydroidInstall:
    binary: str | None
    version: str | None
    service_present: bool
    image_dir: Path
    system_image: Path
    vendor_image: Path
    system_present: bool
    vendor_present: bool
    config_file: Path
    config_present: bool
    package_version: str | None
    session_dir: Path


def waydroid_install() -> WaydroidInstall:
    binary = proc.which("waydroid")
    version = None
    if binary:
        result = proc.run([binary, "--version"], timeout=10.0)
        if result.ok:
            version = result.stdout.strip() or result.stderr.strip() or None
    service_paths = (
        "/usr/lib/systemd/system/waydroid-container.service",
        "/etc/systemd/system/waydroid-container.service",
        "/usr/local/lib/systemd/system/waydroid-container.service",
    )
    image_dir = Path("/var/lib/waydroid/images")
    system_image = image_dir / "system.img"
    vendor_image = image_dir / "vendor.img"
    config_file = Path("/var/lib/waydroid/waydroid.cfg")
    return WaydroidInstall(
        binary=binary,
        version=version,
        service_present=any(Path(p).exists() for p in service_paths),
        image_dir=image_dir,
        system_image=system_image,
        vendor_image=vendor_image,
        system_present=system_image.exists(),
        vendor_present=vendor_image.exists(),
        config_file=config_file,
        config_present=config_file.exists(),
        package_version=package_version("waydroid"),
        session_dir=Path("/var/lib/waydroid"),
    )


def systemd_state(unit: str) -> tuple[str, str]:
    """``(state, detail)`` for a systemd unit, read-only."""
    if not proc.have("systemctl"):
        return "unknown", "systemctl not installed"
    result = proc.run(["systemctl", "is-active", unit], timeout=8.0)
    state = result.stdout.strip() or "unknown"
    if result.returncode not in (0, 3):
        return "unknown", result.stderr.strip() or f"systemctl exit {result.returncode}"
    return state, f"systemctl is-active {unit} -> {state}"


def disk_free(path: Path) -> tuple[int | None, str]:
    from lynxdroid.core.paths import free_space_bytes

    value = free_space_bytes(path)
    if value is None:
        return None, f"could not stat filesystem for {path}"
    return value, str(path)
