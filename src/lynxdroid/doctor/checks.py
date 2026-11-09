"""The checks behind ``lynxdroid doctor``.

Each check answers three questions the specification demands (stage 3):

    what did we look at, what did we actually observe, what should the user do

Missing component, refused access and "cannot determine" are three distinct
statuses - never merged into a single "unsupported".
"""

from __future__ import annotations

from pathlib import Path

from lynxdroid.core.diagnostics import Check, Report, Status
from lynxdroid.doctor import platform as pf
from lynxdroid.version import SCHEMA_VERSIONS, __version__

MB = 1024 * 1024

#: Check ids that must be OK for the container path to boot Android.
CONTAINER_PREREQUISITES: tuple[str, ...] = (
    "os.arch",
    "session.wayland",
    "kernel.binder",
    "container.namespaces",
    "container.memfd",
    "waydroid.binary",
    "waydroid.images",
    "disk.space",
)

_ARCH_SUPPORTED = {"x86_64", "aarch64", "armv8l", "i686", "i386"}


# --------------------------------------------------------------------------
# Environment / OS
# --------------------------------------------------------------------------


def environment_report() -> Report:
    report = Report(area="environment")
    info = pf.os_info()
    session = pf.session_info()

    report.meta.update(
        {
            "lynxdroid_version": __version__,
            "schema_versions": SCHEMA_VERSIONS,
            "collected_by": "lynxdroid doctor (read-only)",
        }
    )

    report.add(
        Check(
            id="os.distro",
            title="Linux distribution",
            status=Status.OK if info.is_arch_family else Status.WARNING,
            detail=f"{info.pretty_name}",
            observed=f"ID={info.distro_id} kernel={info.kernel_release} source={info.source}",
            next_step=""
            if info.is_arch_family
            else "this build targets CachyOS/Arch; other distributions need different package and"
            " service handling and are not covered by the documented tests",
            requirement="P1-OS",
            data={"distro_id": info.distro_id, "is_cachyos": info.is_cachyos},
        )
    )

    report.add(
        Check(
            id="os.arch",
            title="CPU architecture",
            status=Status.OK if info.machine in _ARCH_SUPPORTED else Status.ERROR,
            detail=info.machine,
            observed=info.machine,
            next_step=""
            if info.machine in _ARCH_SUPPORTED
            else "Android images from the Waydroid OTA channel exist for x86_64 and arm64 only",
            requirement="P0-ARCH",
        )
    )

    report.add(
        Check(
            id="os.kernel",
            title="Running kernel",
            status=Status.OK,
            detail=info.kernel_release,
            observed=info.kernel_release,
            requirement="P0-KERNEL",
        )
    )

    pm = pf.package_manager()
    report.add(
        Check(
            id="os.pkgmanager",
            title="Package manager",
            status=Status.OK if pm == "pacman" else (Status.WARNING if pm else Status.UNKNOWN),
            detail=pm or "not detected",
            observed=pm or "",
            next_step=""
            if pm == "pacman"
            else "LynxDroid documents commands for pacman (CachyOS/Arch) only",
            requirement="P1-PKG",
        )
    )

    report.add(
        Check(
            id="session.privileges",
            title="Privilege level of this process",
            status=Status.OK if not session.running_as_root else Status.WARNING,
            detail="regular user" if not session.running_as_root else "running as root",
            observed=f"euid={0 if session.running_as_root else 'non-root'}",
            next_step=""
            if not session.running_as_root
            else "LynxDroid is designed to run as a normal user; only the documented backend"
            " helpers may request elevated privileges",
            requirement="P0-NONROOT",
        )
    )

    in_container_status = Status.WARNING if session.in_container else Status.OK
    report.add(
        Check(
            id="session.container",
            title="Are we inside a container",
            status=in_container_status,
            detail=session.container_hint,
            observed=session.container_hint,
            next_step="a container host cannot provide the user's GPU, session or binder devices;"
            " hardware and session checks are therefore not representative"
            if session.in_container
            else "",
            requirement="P1-REALHOST",
        )
    )
    return report


def session_report() -> Report:
    report = Report(area="session")
    session = pf.session_info()

    wayland_ok = session.session_type == "wayland" and session.wayland_socket_usable
    if wayland_ok:
        detail = f"Wayland ({session.wayland_display})"
        status = Status.OK
    elif session.session_type == "wayland" and not session.wayland_socket_usable:
        detail = "Wayland session declared but its socket is not usable"
        status = Status.ERROR
    elif session.session_type == "x11":
        detail = "X11 session - Waydroid needs a Wayland compositor"
        status = Status.ERROR
    elif session.session_type is None:
        detail = "no graphical session detected"
        status = Status.UNKNOWN
    else:
        detail = f"{session.session_type} session"
        status = Status.WARNING

    report.add(
        Check(
            id="session.wayland",
            title="Wayland compositor",
            status=status,
            detail=detail,
            observed="; ".join(session.reasons) or f"WAYLAND_DISPLAY={session.wayland_display}",
            next_step=""
            if wayland_ok
            else "log into a KDE Plasma Wayland session; on X11 use a nested compositor such as"
            " cage only as a documented fallback",
            requirement="P0-WAYLAND",
            data={
                "session_type": session.session_type,
                "wayland_display": session.wayland_display,
                "socket": str(session.wayland_socket) if session.wayland_socket else None,
            },
        )
    )

    report.add(
        Check(
            id="session.desktop",
            title="Desktop environment",
            status=Status.OK
            if session.is_kde
            else (Status.WARNING if session.desktop else Status.UNKNOWN),
            detail=session.desktop or "not detected",
            observed=f"XDG_CURRENT_DESKTOP={session.desktop}",
            next_step=""
            if session.is_kde
            else "window embedding and focus behaviour in this project are tested on KWin only",
            requirement="P1-KDE",
        )
    )

    runtime = Path("/run/user") / str(_uid())
    report.add(
        Check(
            id="session.runtime_dir",
            title="XDG runtime directory",
            status=Status.OK if runtime.is_dir() else Status.ERROR,
            detail=str(runtime) if runtime.is_dir() else "missing",
            observed=str(runtime),
            next_step=""
            if runtime.is_dir()
            else "the runtime directory is created by logind for a real session; its absence"
            " means this shell is not attached to a graphical session",
            requirement="P0-WAYLAND",
        )
    )
    return report


def _uid() -> int:
    import os

    return os.getuid()


# --------------------------------------------------------------------------
# Kernel features
# --------------------------------------------------------------------------


def kernel_report() -> Report:
    report = Report(area="kernel")
    config = pf.kernel_config()
    devices = pf.binder_devices()
    binderfs = pf.binderfs_mounted()
    module = pf.binder_module_loaded()
    fs_lists_binder = pf.binder_supported_in_filesystems()

    config_android = config.get("CONFIG_ANDROID")
    config_binder_ipc = config.get("CONFIG_ANDROID_BINDER_IPC")
    config_binderfs = config.get("CONFIG_ANDROID_BINDERFS")

    report.meta["config_source"] = config.source
    report.meta["config_error"] = config.error

    if devices:
        binder_status = Status.OK
        binder_detail = f"{len(devices)} binder device(s) present"
        binder_next = ""
    elif config_binder_ipc in {"y", "m"}:
        binder_status = Status.WARNING
        binder_detail = "kernel supports binder but no binder device node exists yet"
        binder_next = (
            "with binderfs the devices appear after 'mkdir -p /dev/binderfs && "
            "mount -t binder binder /dev/binderfs' (root, requires consent); "
            "starting the waydroid-container service usually does this for you"
        )
    elif config_binder_ipc == "n":
        binder_status = Status.ERROR
        binder_detail = "this kernel was built without CONFIG_ANDROID_BINDER_IPC"
        binder_next = (
            "install a kernel that provides binder (linux-cachyos, linux-zen, or a DKMS module)"
        )
    elif config.source is None:
        binder_status = Status.UNKNOWN
        binder_detail = "no binder device and no kernel config to inspect"
        binder_next = f"enable CONFIG_IKCONFIG_PROC or install a kernel with binder; {config.error}"
    else:
        binder_status = Status.ERROR
        binder_detail = "kernel config does not mention binder"
        binder_next = "install a kernel that provides binder"

    report.add(
        Check(
            id="kernel.binder",
            title="Android Binder IPC",
            status=binder_status,
            detail=binder_detail,
            observed=", ".join(str(p) for p in devices) or (config.source or "config unavailable"),
            next_step=binder_next,
            requires_privilege=binder_status is not Status.OK,
            requirement="P0-BINDER",
            data={
                "devices": [str(p) for p in devices],
                "binderfs_mounted": binderfs,
                "module_loaded": module,
                "CONFIG_ANDROID": config_android,
                "CONFIG_ANDROID_BINDER_IPC": config_binder_ipc,
                "CONFIG_ANDROID_BINDERFS": config_binderfs,
                "CONFIG_ANDROID_BINDER_DEVICES": config.get("CONFIG_ANDROID_BINDER_DEVICES"),
                "config_source": config.source,
            },
        )
    )

    # binderfs as a distinct capability: needed by Waydroid's preferred setup.
    # /proc/filesystems is a second, independent source: distributions often
    # ship a kernel with no readable config (CONFIG_IKCONFIG unset), and this
    # file still says whether binderfs is built in.
    if config_binderfs in {"y", "m"}:
        fs_status = Status.OK
        fs_detail = "binderfs available" + (" and mounted" if binderfs else ", not mounted yet")
    elif config_binderfs == "n" and config.source:
        fs_status = Status.WARNING
        fs_detail = "classic /dev/binder devices without binderfs"
    elif config.source is None and fs_lists_binder:
        fs_status = Status.OK
        fs_detail = (
            "binderfs is in /proc/filesystems"
            + (" and mounted" if binderfs else ", not mounted yet")
            + " (kernel config not readable)"
        )
    elif config.source is None:
        fs_status = Status.UNKNOWN
        fs_detail = "cannot read kernel config; binderfs state unknown"
    else:
        fs_status = Status.UNKNOWN
        fs_detail = "binderfs not mentioned in kernel config"

    report.add(
        Check(
            id="kernel.binderfs",
            title="binderfs filesystem",
            status=fs_status,
            detail=fs_detail,
            observed=(
                f"CONFIG_ANDROID_BINDERFS={config_binderfs or 'unknown'} "
                f"in /proc/filesystems={fs_lists_binder} mounted={binderfs}"
            ),
            next_step=""
            if fs_status is Status.OK
            else "binderfs lets each container get private binder devices; without it the"
            " classic binder,hwbinder,vndbinder nodes are used instead",
            requires_privilege=not binderfs,
            requirement="P1-BINDERFS",
        )
    )

    ok_ns, present_ns = pf.namespaces_available()
    report.add(
        Check(
            id="container.namespaces",
            title="Container namespaces",
            status=Status.OK if ok_ns else Status.ERROR,
            detail=", ".join(present_ns) if present_ns else "none",
            observed=f"present: {', '.join(present_ns)}",
            next_step=""
            if ok_ns
            else (
                "the Android container needs user/pid/net/mnt/ipc/uts "
                "namespaces from the host kernel"
            ),
            requirement="P0-NS",
        )
    )

    memfd_ok, memfd_detail = pf.memfd_available()
    report.add(
        Check(
            id="container.memfd",
            title="memfd_create",
            status=Status.OK if memfd_ok else Status.ERROR,
            detail=memfd_detail,
            observed=memfd_detail,
            next_step=""
            if memfd_ok
            else "memfd is required for shared memory in the graphics path; a seccomp policy"
            " is most likely blocking it",
            requirement="P0-MEMFD",
        )
    )

    psi = config.get("CONFIG_PSI")
    report.add(
        Check(
            id="kernel.psi",
            title="Pressure stall information",
            status=Status.OK
            if Path("/proc/pressure/cpu").exists()
            else (Status.WARNING if psi in {"y", "m"} else Status.UNKNOWN),
            detail="available" if Path("/proc/pressure/cpu").exists() else "not exposed",
            observed=str(Path("/proc/pressure/cpu").exists()),
            next_step="PSI is optional for this project; it only improves the resource"
            " measurement report"
            if not Path("/proc/pressure/cpu").exists()
            else "",
            requirement="P1-PSI",
        )
    )
    return report


# --------------------------------------------------------------------------
# GPU
# --------------------------------------------------------------------------


def gpu_report() -> Report:
    report = Report(area="gpu")
    info, reason = pf.gpu_info()

    if info is None:
        report.add(
            Check(
                id="gpu.device",
                title="DRM GPU device",
                status=Status.UNKNOWN if "absent" in reason else Status.ERROR,
                detail=reason,
                observed=reason,
                next_step="run this on the target laptop; a container without /sys/class/drm"
                " cannot describe the real GPU",
                requirement="P0-GPU",
            )
        )
        return report

    report.add(
        Check(
            id="gpu.device",
            title="DRM GPU device",
            status=Status.OK if info.is_intel else Status.WARNING,
            detail=info.driver,
            observed=f"{info.source} driver={info.kernel_driver} version={info.driver_version}",
            next_step=""
            if info.is_intel
            else "this project targets Intel integrated graphics; other vendors are untested",
            requirement="P0-GPU",
            data={
                "vendor": info.vendor,
                "kernel_driver": info.kernel_driver,
                "driver_version": info.driver_version,
                "iris_xe_class": info.is_iris_xe_class,
            },
        )
    )

    if info.render_node is None:
        node_status = Status.ERROR
        node_detail = "no DRM render node"
    elif not info.render_node_usable:
        node_status = Status.ERROR
        node_detail = f"{info.render_node} exists but is not readable/writable"
    else:
        node_status = Status.OK
        node_detail = info.render_node
    report.add(
        Check(
            id="gpu.render_node",
            title="DRM render node",
            status=node_status,
            detail=node_detail,
            observed=info.render_node or "none",
            next_step=""
            if node_status is Status.OK
            else "the user must be able to open the render node; membership in the 'render'"
            " group is the Arch default and grants access to all DRM render nodes"
            " (kernel DRM drivers, no keyboard access)",
            requires_privilege=node_status is not Status.OK,
            requirement="P0-RENDERNODE",
        )
    )

    mesa, mesa_reason = pf.mesa_driver_hint()
    report.add(
        Check(
            id="gpu.mesa",
            title="Mesa driver for Intel",
            status=Status.OK if mesa else Status.UNKNOWN,
            detail=mesa or "not determined",
            observed=mesa_reason,
            next_step="" if mesa else "install mesa (pacman -S mesa) so the OpenGL driver exists",
            requirement="P1-MESA",
        )
    )

    vulkan, vulkan_reason = pf.vulkan_driver_hint()
    report.add(
        Check(
            id="gpu.vulkan",
            title="Vulkan ICD (anv)",
            status=Status.OK if vulkan else Status.WARNING,
            detail=vulkan or "not found",
            observed=vulkan_reason,
            next_step=""
            if vulkan
            else (
                "vulkan-intel is only required by apps that use Vulkan; OpenGL apps work without it"
            ),
            requirement="P1-VULKAN",
        )
    )

    # Honest note: host GL says nothing about the guest renderer.
    report.add(
        Check(
            id="gpu.guest_renderer",
            title="Android-side renderer",
            status=Status.UNKNOWN,
            detail="cannot be determined without a running Android session",
            observed="",
            next_step="after the session starts, run 'lynxdroid doctor --session' to read the"
            " renderer that Android actually uses (dumpsys SurfaceFlinger / GLES)",
            requirement="P0-HWRENDER",
        )
    )
    return report


# --------------------------------------------------------------------------
# Waydroid + storage
# --------------------------------------------------------------------------


def waydroid_report() -> Report:
    report = Report(area="waydroid")
    install = pf.waydroid_install()
    report.meta.update(
        {
            "binary": install.binary,
            "version": install.version,
            "package_version": install.package_version,
        }
    )

    if install.binary:
        binary_status = Status.OK
        binary_detail = install.version or "installed (version not reported)"
    elif pf.package_manager() == "pacman":
        binary_status = Status.ERROR
        binary_detail = "waydroid is not installed"
    else:
        binary_status = Status.UNKNOWN
        binary_detail = "waydroid not found and the package manager is unknown"

    report.add(
        Check(
            id="waydroid.binary",
            title="waydroid CLI",
            status=binary_status,
            detail=binary_detail,
            observed=install.binary or "not on PATH",
            next_step=""
            if install.binary
            else "install the upstream package (pacman -S waydroid) - LynxDroid drives the"
            " installed backend, it does not bundle Android itself",
            requires_privilege=not install.binary,
            requirement="P0-WAYDROID",
            data={"package_version": install.package_version},
        )
    )

    images_ok = install.system_present and install.vendor_present
    if images_ok:
        images_detail = f"{install.system_image.name} + {install.vendor_image.name}"
        images_status = Status.OK
    elif install.system_present or install.vendor_present:
        images_detail = "only one of system/vendor image is present"
        images_status = Status.ERROR
    else:
        images_detail = "no Android images installed yet"
        images_status = Status.ERROR
    report.add(
        Check(
            id="waydroid.images",
            title="Android images",
            status=images_status,
            detail=images_detail,
            observed=str(install.image_dir),
            next_step=""
            if images_ok
            else (
                "prepare images through 'lynxdroid image plan' (then "
                "'image prepare --yes' after reviewing source, licence and size)"
            ),
            requires_privilege=not images_ok,
            requirement="P0-IMAGES",
        )
    )

    report.add(
        Check(
            id="waydroid.service",
            title="Container service unit",
            status=Status.OK if install.service_present else Status.WARNING,
            detail="waydroid-container.service present"
            if install.service_present
            else "systemd unit not found",
            observed="present" if install.service_present else "absent",
            next_step=""
            if install.service_present
            else "the container service is normally shipped with the waydroid package; without"
            " it the session can only be started manually",
            requirement="P1-SERVICE",
        )
    )

    state, state_detail = pf.systemd_state("waydroid-container.service")
    report.add(
        Check(
            id="waydroid.service_state",
            title="Container service state",
            status=Status.OK if state in {"active", "inactive"} else Status.UNKNOWN,
            detail=state,
            observed=state_detail,
            next_step=""
            if state == "active"
            else "LynxDroid starts and stops the service through the backend contract; it never"
            " changes the unit file",
            requirement="P1-SERVICE",
        )
    )

    if not install.config_present:
        config_status = Status.WARNING
        config_detail = "waydroid.cfg not found (images not initialised)"
    else:
        text = pf.read_text(install.config_file, limit=65536) or ""
        config_status = Status.OK
        config_detail = "waydroid.cfg readable"
        report.meta["waydroid_cfg"] = _extract_cfg_summary(text)
    report.add(
        Check(
            id="waydroid.config",
            title="Backend configuration",
            status=config_status,
            detail=config_detail,
            observed=str(install.config_file),
            next_step=""
            if config_status is Status.OK
            else "'waydroid init' creates this file; LynxDroid will not rewrite it without consent",
            requires_privilege=True,
            requirement="P1-CFG",
        )
    )
    return report


def _extract_cfg_summary(text: str) -> dict[str, str]:
    """Pull the handful of waydroid.cfg keys we display, nothing more."""
    wanted = {
        "arch",
        "vendor_type",
        "system_datetime",
        "vendor_datetime",
        "images_path",
        "multi_windows",
        "binder",
        "vndbinder",
        "hwbinder",
    }
    summary: dict[str, str] = {}
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith(("#", "[")) or "=" not in stripped:
            continue
        key, _, value = stripped.partition("=")
        key = key.strip()
        if key in wanted or key.startswith("persist.waydroid."):
            summary[key] = value.strip()
    return summary


def storage_report() -> Report:
    report = Report(area="storage")
    free, source = pf.disk_free(Path("/var/lib/waydroid"))
    required = 3 * 1024 * MB  # system+vendor images plus working room
    if free is None:
        status = Status.UNKNOWN
        detail = "free space could not be determined"
    elif free >= required:
        status = Status.OK
        detail = f"{free / 1024**3:.1f} GiB free"
    elif free >= 1024 * MB:
        status = Status.WARNING
        detail = f"{free / 1024**3:.1f} GiB free, below the {required // 1024**3} GiB"
    else:
        status = Status.ERROR
        detail = f"only {free / 1024**2:.0f} MiB free"
    report.add(
        Check(
            id="disk.space",
            title="Free space for Android images",
            status=status,
            detail=detail,
            observed=source,
            next_step=""
            if status is Status.OK
            else "Android system+vendor images plus data need roughly 10-15 GiB in total;"
            " free space before downloading",
            requirement="P0-DISK",
            data={"free_bytes": free, "required_bytes": required},
        )
    )
    return report


# --------------------------------------------------------------------------
# Aggregation
# --------------------------------------------------------------------------


def full_report(*, areas: tuple[str, ...] | None = None) -> Report:
    """Combine the requested areas into one report with a stable id set."""
    builders = {
        "environment": environment_report,
        "session": session_report,
        "kernel": kernel_report,
        "gpu": gpu_report,
        "waydroid": waydroid_report,
        "storage": storage_report,
    }
    selected = areas or tuple(builders)
    combined = Report(area="host")
    for name in selected:
        builder = builders.get(name)
        if builder is None:
            continue
        part = builder()
        combined.extend(part.checks)
        combined.meta.setdefault("areas", {})[name] = part.worst.value  # type: ignore[index]
    combined.meta["version"] = __version__
    combined.meta["schema"] = SCHEMA_VERSIONS["doctor-report"]
    combined.meta["required_for_container"] = list(CONTAINER_PREREQUISITES)
    return combined


def prerequisites_state(report: Report) -> tuple[bool, list[Check]]:
    """``(ready, blocking_checks)`` for the container path.

    The predicate itself lives in Report.ok_for() - one definition of "this
    check blocks us", so the CLI and the GUI cannot drift apart.
    """
    blockers = report.blockers_for(CONTAINER_PREREQUISITES)
    return (not blockers, blockers)
