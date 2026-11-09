"""The host, described once: facts separately from the verdict about them.

Stage 2 asks for a *profile of the target machine*, and the distinction that
matters is the one this module is built around:

* a **fact** is something we read (`in_container=True`, `binder="absent"`);
* a **verdict** is what that fact means for running Android in a container
  (`BLOCKED`, with a reason and a way forward).

Keeping them apart is not tidiness. This repository was developed on a Debian
container with no GPU, no binder and no Wayland socket, so a profile that
merely echoed "the machine looks like this" would describe the container and
be read as a description of the user's laptop. The verdict has to be able to
say *why it is unknown here*, which it cannot do if the fact and the
conclusion are the same field.

Three consequences, each of them tested:

1. A fact that cannot be read is ``None``, never a plausible default. "No
   /sys/class/drm" is not "no GPU".
2. A verdict is only ``READY`` when every precondition was *checked* and
   passed. An unverified precondition gives ``UNKNOWN`` - the project's rule
   that "we could not determine" is never reported as "unsupported" or as
   "works".
3. The profile carries the evidence for each precondition, so a report from
   the laptop can be compared with this one line by line.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum

from lynxdroid.doctor import platform as pf


class Verdict(str, Enum):
    """Whether Android can run here, stated as a decision, not a mood.

    ``READY``     - every precondition was checked and passed.
    ``PARTIAL``   - it can run, with a named limitation (single-touch input,
                    no hardware acceleration).
    ``BLOCKED``   - a precondition is known to fail; Android cannot boot.
    ``UNKNOWN``   - a precondition could not be checked. Not the same as
                    BLOCKED, and never rounded up to READY.
    """

    READY = "READY"
    PARTIAL = "PARTIAL"
    BLOCKED = "BLOCKED"
    UNKNOWN = "UNKNOWN"

    @property
    def severity(self) -> int:
        return {"READY": 0, "UNKNOWN": 1, "PARTIAL": 2, "BLOCKED": 3}[self.value]


@dataclass(frozen=True)
class Precondition:
    """One thing Android needs, what we found, and what it means."""

    id: str
    #: What we read. ``None`` means "could not determine" - never a guess.
    fact: str | None
    verdict: Verdict
    #: Why this verdict follows from that fact, in the user's terms.
    reason: str
    #: The exact command or check that settles it on a machine where it is
    #: unknown. Empty when nothing would help.
    next_step: str = ""


@dataclass(frozen=True)
class HostProfile:
    """Everything known about the host, facts and verdicts together."""

    os_id: str
    pretty_name: str
    kernel: str
    machine: str
    session: str
    desktop: str
    in_container: bool
    container_hint: str
    #: Empty when no GPU could be described (not the same as "no GPU").
    gpu_vendor: str
    gpu_driver: str
    gpu_renderer_is_iris_xe: bool | None
    waydroid_version: str | None
    preconditions: tuple[Precondition, ...] = field(default_factory=tuple)

    @property
    def verdict(self) -> Verdict:
        """The worst verdict among the preconditions, not an average.

        One hard failure blocks the whole thing; one unknown keeps the answer
        unknown even if everything else passed, because "we did not check"
        and "it works" are different sentences.

        This matches ``doctor``, which treats an UNKNOWN prerequisite as
        blocking (``Report.blockers_for``). Two answers to "can this machine
        run Android" would be worse than either one being conservative.
        """
        if not self.preconditions:
            return Verdict.UNKNOWN
        worst = max(self.preconditions, key=lambda item: item.verdict.severity)
        return worst.verdict

    def blockers(self) -> tuple[Precondition, ...]:
        return tuple(p for p in self.preconditions if p.verdict is Verdict.BLOCKED)

    def unresolved(self) -> tuple[Precondition, ...]:
        return tuple(p for p in self.preconditions if p.verdict is Verdict.UNKNOWN)

    def limitations(self) -> tuple[Precondition, ...]:
        return tuple(p for p in self.preconditions if p.verdict is Verdict.PARTIAL)

    def summary(self) -> str:
        """One sentence, always naming what is missing rather than a score."""
        verdict = self.verdict
        if verdict is Verdict.READY:
            return "this host meets every precondition Android needs"
        parts: list[str] = []
        if self.blockers():
            parts.append("blocked by " + ", ".join(p.id for p in self.blockers()))
        if self.unresolved():
            parts.append("not checked here: " + ", ".join(p.id for p in self.unresolved()))
        if self.limitations():
            parts.append("limited by " + ", ".join(p.id for p in self.limitations()))
        return "; ".join(parts)

    def as_dict(self) -> dict[str, object]:
        return {
            "os": {"id": self.os_id, "name": self.pretty_name},
            "kernel": self.kernel,
            "machine": self.machine,
            "session": self.session,
            "desktop": self.desktop,
            "container": {"detected": self.in_container, "hint": self.container_hint},
            "gpu": {
                "vendor": self.gpu_vendor,
                "driver": self.gpu_driver,
                "iris_xe_class": self.gpu_renderer_is_iris_xe,
            },
            "waydroid": {"version": self.waydroid_version},
            "verdict": self.verdict.value,
            "summary": self.summary(),
            "preconditions": [
                {
                    "id": p.id,
                    "fact": p.fact,
                    "verdict": p.verdict.value,
                    "reason": p.reason,
                    "next_step": p.next_step,
                }
                for p in self.preconditions
            ],
        }


# ---------------------------------------------------------------------------
# Reading the machine
# ---------------------------------------------------------------------------


def _binder_precondition() -> Precondition:
    """Binder is the one thing that cannot be worked around.

    Without it Android's IPC has nothing to run on: the session starts and
    every app fails, which is the least useful failure mode there is.
    """
    devices = pf.binder_devices()
    if devices:
        return Precondition(
            id="binder",
            fact=", ".join(str(p) for p in devices[:3]),
            verdict=Verdict.READY,
            reason="Android binder devices are present",
        )
    if pf.binderfs_mounted():
        # Mounted but no devices inside: the mount point exists and is empty,
        # which is a different problem from "not mounted at all".
        return Precondition(
            id="binder",
            fact="binderfs mounted, no devices inside",
            verdict=Verdict.BLOCKED,
            reason="/dev/binderfs is mounted but exposes no binder device",
            next_step="ls -l /dev/binderfs; Waydroid creates them at session start",
        )
    if pf.binder_supported_in_filesystems():
        return Precondition(
            id="binder",
            fact="binder registered, not mounted",
            # Mounting needs root, which this project never takes, so this is
            # a limitation the user resolves rather than something we do.
            verdict=Verdict.PARTIAL,
            reason="the kernel supports binderfs but it is not mounted",
            next_step="sudo mount -t binder binder /dev/binderfs  (Waydroid does this itself)",
        )
    return Precondition(
        id="binder",
        fact="no binder devices, no binderfs in /proc/filesystems",
        verdict=Verdict.BLOCKED,
        reason="this kernel cannot run Android: CONFIG_ANDROID_BINDER_IPC is absent",
        next_step="boot a kernel with binder support (CachyOS ships one)",
    )


def _session_precondition(session: pf.SessionInfo) -> Precondition:
    if session.session_type == "wayland" and session.wayland_socket_usable:
        return Precondition(
            id="wayland",
            fact=f"{session.session_type} ({session.wayland_display})",
            verdict=Verdict.READY,
            reason="a Wayland session with a usable socket",
        )
    if session.session_type is None:
        return Precondition(
            id="wayland",
            fact=None,
            verdict=Verdict.UNKNOWN,
            reason="cannot tell which display server is in use from this process",
            next_step="log in to the desktop session and run 'lynxdroid doctor' there",
        )
    if session.session_type == "wayland":
        return Precondition(
            id="wayland",
            fact=f"wayland, socket {session.wayland_socket} unusable",
            verdict=Verdict.BLOCKED,
            reason="Wayland is reported but its socket cannot be reached",
            next_step="start LynxDroid from the graphical session, not over ssh",
        )
    return Precondition(
        id="wayland",
        fact=session.session_type,
        verdict=Verdict.PARTIAL,
        reason="Waydroid needs a Wayland session; X11 is not supported by this build",
        next_step="log in to a Plasma/Wayland session",
    )


def _gpu_precondition(gpu: pf.GpuInfo | None, gpu_reason: str) -> Precondition:
    if gpu is None:
        return Precondition(
            id="gpu",
            fact=None,
            verdict=Verdict.UNKNOWN,
            reason=f"no DRM device could be described ({gpu_reason})",
            next_step="run this on the laptop itself; a container has no /dev/dri",
        )
    if gpu.render_node is None:
        return Precondition(
            id="gpu",
            fact=f"{gpu.vendor} [{gpu.kernel_driver}], no render node",
            verdict=Verdict.PARTIAL,
            reason="the GPU exists but exposes no render node; Android will be slow",
            next_step="check that the i915/xe driver is loaded",
        )
    if not gpu.render_node_usable:
        return Precondition(
            id="gpu",
            fact=f"{gpu.render_node} not readable/writable",
            verdict=Verdict.PARTIAL,
            reason="the render node exists but this user cannot use it",
            next_step=f"add the user to the 'video' group to use {gpu.render_node}",
        )
    return Precondition(
        id="gpu",
        fact=f"{gpu.vendor} [{gpu.kernel_driver}], {gpu.render_node}",
        verdict=Verdict.READY,
        reason=(
            "Intel integrated graphics, the renderer the specification targets"
            if gpu.is_intel
            else "usable render node; this build targets Intel and other vendors are untested"
        ),
    )


def _waydroid_precondition(install: pf.WaydroidInstall) -> Precondition:
    if install.binary is None:
        return Precondition(
            id="waydroid",
            fact=None,
            verdict=Verdict.BLOCKED,
            reason="Waydroid is not installed; LynxDroid drives an installed backend",
            next_step="install 'waydroid' and 'waydroid-image' with pacman",
        )
    if not install.system_present:
        return Precondition(
            id="waydroid",
            fact=f"{install.version or 'unknown version'}, no system image",
            verdict=Verdict.BLOCKED,
            reason="Waydroid is installed but has no Android image to boot",
            next_step="lynxdroid image plan  (then: sudo waydroid init)",
        )
    return Precondition(
        id="waydroid",
        fact=f"{install.version or 'version unknown'}, image present",
        verdict=Verdict.READY,
        reason="an installed backend with an image",
    )


def _disk_precondition(install: pf.WaydroidInstall) -> Precondition:
    free, source = pf.disk_free(install.image_dir)
    if free is None:
        return Precondition(
            id="disk",
            fact=None,
            verdict=Verdict.UNKNOWN,
            reason=f"free space of {install.image_dir} could not be read ({source})",
            next_step="df -h /var/lib/waydroid",
        )
    free_mb = free / (1024 * 1024)
    if free_mb < 1024:
        return Precondition(
            id="disk",
            fact=f"{free_mb:.0f} MiB free on {install.image_dir}",
            verdict=Verdict.BLOCKED,
            reason="Android needs room for its data partition",
            next_step="free space in /var/lib/waydroid or move it to another filesystem",
        )
    return Precondition(
        id="disk",
        fact=f"{free_mb:.0f} MiB free",
        verdict=Verdict.READY,
        reason="enough room for the container's data partition",
    )


def describe_host() -> HostProfile:
    """Read this machine and state a verdict. Read-only, never privileged.

    Every probe here is already used by ``doctor``; this function only puts
    them in one object and adds the judgement the raw checks do not make.
    """
    info = pf.os_info()
    session = pf.session_info()
    gpu, gpu_reason = pf.gpu_info()
    install = pf.waydroid_install()

    preconditions = (
        _binder_precondition(),
        _session_precondition(session),
        _gpu_precondition(gpu, gpu_reason),
        _waydroid_precondition(install),
        _disk_precondition(install),
    )
    return HostProfile(
        os_id=info.distro_id,
        pretty_name=info.pretty_name,
        kernel=info.kernel_release,
        machine=info.machine,
        session=session.session_type or "unknown",
        desktop=session.desktop or "unknown",
        in_container=session.in_container,
        container_hint=session.container_hint,
        gpu_vendor=gpu.vendor if gpu else "",
        gpu_driver=gpu.kernel_driver if gpu else "",
        gpu_renderer_is_iris_xe=gpu.is_iris_xe_class if gpu else None,
        waydroid_version=install.version,
        preconditions=preconditions,
    )
