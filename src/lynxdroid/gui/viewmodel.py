"""Presentation logic, deliberately free of Qt.

Why this file exists: the interesting UI decisions (what is enabled, what
warning is shown, what the status text says, which control is forbidden) are
logic, not drawing. Keeping them here means they are unit-tested on any
machine, including CI without a display server, while the Qt widgets stay
thin and obvious.

Every value here is derived from the domain objects - nothing is invented.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum

from lynxdroid.backend.compat import FixResult, match_failure
from lynxdroid.backend.contract import (
    BackendCapabilities,
    BackendState,
    Readiness,
    Snapshot,
    Support,
)
from lynxdroid.core.diagnostics import Report, Status
from lynxdroid.display.geometry import ViewportGeometry
from lynxdroid.display.resolution import PRESETS, Orientation, ValidationIssue, validate_display
from lynxdroid.input.channels import Capabilities as ChannelCapabilities
from lynxdroid.profiles.model import Element, ElementType, Profile, key_conflicts
from lynxdroid.settings import Settings


class Tone(str, Enum):
    """Semantic colour intent; the theme maps it to real colours."""

    NEUTRAL = "neutral"
    POSITIVE = "positive"
    WARNING = "warning"
    DANGER = "danger"
    INFO = "info"


@dataclass(frozen=True)
class Banner:
    """A message shown at the top of the window."""

    text: str
    tone: Tone
    detail: str = ""
    dismissible: bool = True


@dataclass(frozen=True)
class ControlState:
    """Whether a control may be used, and why not."""

    enabled: bool
    reason: str = ""
    needs_consent: bool = False


@dataclass
class StatusPanel:
    """Everything the status area renders."""

    headline: str
    tone: Tone
    layers: list[tuple[str, Readiness, str]] = field(default_factory=list)
    detail: str = ""
    backend_label: str = ""


_STATE_TEXT: dict[BackendState, tuple[str, Tone]] = {
    BackendState.NOT_CONFIGURED: ("Android is not set up yet", Tone.WARNING),
    BackendState.STOPPED: ("Android is stopped", Tone.NEUTRAL),
    BackendState.STARTING: ("Android is starting…", Tone.INFO),
    BackendState.RUNNING: ("Android is running", Tone.POSITIVE),
    BackendState.STOPPING: ("Android is stopping…", Tone.INFO),
    BackendState.ERROR: ("Android failed to start", Tone.DANGER),
}


def status_panel(snapshot: Snapshot, caps: BackendCapabilities) -> StatusPanel:
    """Turn a backend snapshot into a headline, tone and per-layer rows."""
    headline, tone = _STATE_TEXT.get(snapshot.state, ("Unknown state", Tone.WARNING))
    layers: list[tuple[str, Readiness, str]] = []
    labels = {
        "process": "Backend process",
        "container": "Container service",
        "android": "Android system",
        "graphics": "Graphics path",
        "input": "Input channel",
    }
    for key, label in labels.items():
        value = snapshot.readiness.get(key, Readiness.UNKNOWN)
        layers.append((label, value, _readiness_text(value)))
    return StatusPanel(
        headline=headline,
        tone=tone,
        layers=layers,
        detail=snapshot.error or snapshot.detail,
        backend_label=f"{caps.name} {caps.version}",
    )


def _readiness_text(value: Readiness) -> str:
    return {
        Readiness.READY: "ready",
        Readiness.NOT_READY: "not ready",
        Readiness.FAILED: "failed",
        Readiness.UNKNOWN: "not checked",
    }[value]


def start_control(snapshot: Snapshot, caps: BackendCapabilities) -> ControlState:
    """Should the Start button be clickable?"""
    if not caps.supports("session"):
        feature = caps.feature("session")
        return ControlState(False, feature.reason if feature else "backend cannot start sessions")
    if snapshot.state is BackendState.RUNNING:
        return ControlState(False, "already running")
    if snapshot.state is BackendState.STARTING:
        return ControlState(False, "a start is already in progress")
    if snapshot.state is BackendState.STOPPING:
        return ControlState(False, "wait for the current stop to finish")
    return ControlState(True)


def stop_control(snapshot: Snapshot, caps: BackendCapabilities) -> ControlState:
    if not caps.supports("session"):
        return ControlState(False, "backend cannot control sessions")
    if snapshot.state is BackendState.STARTING:
        # Honoured, not ignored: the backend stops right after the start ends.
        return ControlState(True, "will stop as soon as the start finishes")
    if snapshot.state in (BackendState.RUNNING,):
        return ControlState(True)
    if snapshot.state is BackendState.STOPPING:
        return ControlState(False, "already stopping")
    return ControlState(False, "nothing is running")


def display_control(caps: BackendCapabilities) -> ControlState:
    feature = caps.feature("display-props")
    if feature is None:
        return ControlState(False, "backend reports no display properties")
    if not feature.enabled:
        return ControlState(False, feature.reason or "not supported by this backend")
    return ControlState(True, feature.reason, needs_consent=feature.needs_consent)


def _feature_control(caps: BackendCapabilities, key: str) -> ControlState:
    feature = caps.feature(key)
    if feature is None:
        return ControlState(False, "unknown capability")
    if not feature.enabled:
        return ControlState(False, feature.reason or "unsupported on this backend")
    return ControlState(True, feature.reason, needs_consent=feature.needs_consent)


def overlay_control(
    caps: BackendCapabilities, channel: ChannelCapabilities | None, profile: Profile | None
) -> ControlState:
    """Is the key overlay usable?

    The overlay is only honest when touches can actually reach Android: a
    single-contact channel cannot hold movement and an action at the same
    time, which is exactly what the overlay is for.
    """
    if profile is None:
        return ControlState(False, "choose a layout profile first")
    if channel is None:
        return ControlState(False, "input channel not established")
    if not channel.available:
        return ControlState(False, channel.reason or "no touch channel")
    if channel.simultaneous_touches < 2 or not channel.real_multitouch:
        return ControlState(
            False,
            "this channel carries one contact at a time, so overlapping "
            "key presses cannot be delivered (movement + action is impossible)",
        )
    return ControlState(True)


def banners(
    *,
    snapshot: Snapshot | None,
    caps: BackendCapabilities | None,
    report: Report | None,
    settings: Settings,
    channel: ChannelCapabilities | None,
) -> list[Banner]:
    """Ordered, de-duplicated warnings the user must see before playing."""
    found: list[Banner] = []

    if caps is not None:
        multi = caps.feature("multi-instance")
        if multi is not None and multi.support in (Support.UNTESTED, Support.UNSUPPORTED):
            found.append(
                Banner(
                    "Instance support is not verified",
                    Tone.WARNING,
                    multi.reason,
                )
            )
        arm = caps.feature("arm-translation")
        if settings.arm_translation_warning and arm is not None and not arm.enabled:
            found.append(
                Banner(
                    "ARM-only apps will not run",
                    Tone.WARNING,
                    "this host is x86_64; apps shipping only arm64 code need a licensed "
                    "translator that is not part of this project",
                )
            )
        note = next((n for n in caps.notes if "foreign surface" in n), None)
        if note:
            found.append(Banner("Android runs in its own window", Tone.INFO, note))

    if snapshot is not None and snapshot.error:
        found.append(Banner("Backend error", Tone.DANGER, snapshot.error))

    if report is not None:
        blockers = report.blockers
        if blockers:
            found.append(
                Banner(
                    f"{len(blockers)} blocking prerequisite(s)",
                    Tone.DANGER,
                    "; ".join(f"{c.id}: {c.detail}" for c in blockers[:4]),
                )
            )
        elif report.by_status(Status.WARNING):
            found.append(
                Banner(
                    f"{len(report.by_status(Status.WARNING))} warning(s) from doctor",
                    Tone.WARNING,
                    "open Diagnostics for the details",
                )
            )

    if channel is not None and channel.available and not channel.real_multitouch:
        found.append(
            Banner(
                "Single-touch input only",
                Tone.WARNING,
                "the active channel cannot deliver simultaneous movement and action",
            )
        )

    return found


# --------------------------------------------------------------------------
# Diagnostics view
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class CheckRow:
    """One row of the diagnostics table."""

    symbol: str
    status: Status
    title: str
    detail: str
    next_step: str
    requires_privilege: bool

    @property
    def tone(self) -> Tone:
        return {
            Status.OK: Tone.POSITIVE,
            Status.WARNING: Tone.WARNING,
            Status.ERROR: Tone.DANGER,
            Status.UNKNOWN: Tone.NEUTRAL,
        }[self.status]


def check_rows(report: Report, *, include_ok: bool = True) -> list[CheckRow]:
    rows: list[CheckRow] = []
    for check in report.checks:
        if check.status is Status.OK and not include_ok:
            continue
        rows.append(
            CheckRow(
                symbol=check.status.symbol,
                status=check.status,
                title=check.title,
                detail=check.detail,
                next_step=check.next_step,
                requires_privilege=check.requires_privilege,
            )
        )
    return rows


def report_summary(report: Report) -> tuple[str, Tone]:
    counts = {status: len(report.by_status(status)) for status in Status}
    if counts[Status.ERROR]:
        return (
            f"{counts[Status.ERROR]} error, {counts[Status.WARNING]} warning, "
            f"{counts[Status.OK]} ok",
            Tone.DANGER,
        )
    if counts[Status.WARNING]:
        return f"{counts[Status.WARNING]} warning, {counts[Status.OK]} ok", Tone.WARNING
    if counts[Status.UNKNOWN]:
        return f"{counts[Status.UNKNOWN]} not determined, {counts[Status.OK]} ok", Tone.INFO
    return f"all {counts[Status.OK]} checks passed", Tone.POSITIVE


# --------------------------------------------------------------------------
# Display view
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class DisplayForm:
    """Validated state of the resolution form."""

    width: str
    height: str
    dpi: str
    preset_name: str = ""
    issues: tuple[ValidationIssue, ...] = ()
    orientation: Orientation | None = None

    @property
    def is_valid(self) -> bool:
        return not any(issue.is_error for issue in self.issues)

    @property
    def warnings(self) -> tuple[ValidationIssue, ...]:
        return tuple(issue for issue in self.issues if issue.severity == "warning")

    def message(self) -> str:
        if not self.issues:
            return ""
        return "; ".join(f"{issue.field}: {issue.message}" for issue in self.issues)

    @classmethod
    def from_settings(cls, settings: Settings) -> DisplayForm:
        return cls(
            width=str(settings.buffer_width),
            height=str(settings.buffer_height),
            dpi=str(settings.dpi),
            orientation=Orientation.of(settings.buffer_width, settings.buffer_height),
        )

    def validated(self) -> DisplayForm:
        request, issues = validate_display(self.width, self.height, self.dpi)
        orientation = Orientation.of(request.width, request.height) if request is not None else None
        preset = next(
            (
                p.name
                for p in PRESETS
                if request is not None
                and p.width == request.width
                and p.height == request.height
                and p.dpi == request.dpi
            ),
            "",
        )
        return DisplayForm(self.width, self.height, self.dpi, preset, tuple(issues), orientation)


def preset_rows() -> list[tuple[str, str, str]]:
    """(name, size text, note) for the preset list."""
    return [(p.name, f"{p.width}x{p.height} @ {p.dpi} dpi", p.note) for p in PRESETS]


def geometry_caption(geometry: ViewportGeometry | None) -> str:
    if geometry is None:
        return "no Android surface yet"
    rect = geometry.content_rect()
    bars = geometry.bars()
    parts = [f"{geometry.buffer.width}x{geometry.buffer.height}"]
    if geometry.rotation.value:
        parts.append(f"rotated {geometry.rotation.value}°")
    if bars["left"] or bars["top"]:
        parts.append(
            f"black bars L{bars['left']} T{bars['top']} R{bars['right']} B{bars['bottom']}"
        )
    parts.append(f"drawn at {int(rect.width)}x{int(rect.height)}")
    return " · ".join(parts)


# --------------------------------------------------------------------------
# Profile / overlay view
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class ProfileRow:
    id: str
    name: str
    package: str
    elements: str
    reference: str
    warning: str = ""

    @property
    def tone(self) -> Tone:
        return Tone.WARNING if self.warning else Tone.NEUTRAL


def profile_rows(
    profiles: list[Profile], *, live_buffer: tuple[int, int] | None = None
) -> list[ProfileRow]:
    rows: list[ProfileRow] = []
    for profile in profiles:
        warning = ""
        if live_buffer is not None:
            live_w, live_h = live_buffer
            ref = profile.reference
            same_shape = (ref.width >= ref.height) == (live_w >= live_h)
            if abs(ref.width / max(ref.height, 1) - live_w / max(live_h, 1)) > 0.05:
                # The model owns the wording: it also tells the user what to
                # do about it, which a bare "different aspect ratio" does not.
                warning = ref.relayout_hint
            elif not same_shape:
                warning = "authored for another orientation"
        conflicts = key_conflicts(profile.elements)
        if conflicts:
            warning = (
                (warning + "; " if warning else "")
                + "key conflict: "
                + ", ".join(sorted(conflicts))
            )
        rows.append(
            ProfileRow(
                id=profile.id,
                name=profile.name,
                package=profile.target.package,
                elements=f"{len(profile.elements)} elements",
                reference=f"{profile.reference.width}x{profile.reference.height}",
                warning=warning,
            )
        )
    return rows


def element_summary(element: Element) -> str:
    """One-line description used in the editor list."""
    if element.type is ElementType.TAP:
        return f"tap → {'/'.join(element.keys)}"
    if element.type is ElementType.HOLD:
        return f"hold → {'/'.join(element.keys)}"
    if element.type is ElementType.DPAD:
        return "stick → " + " ".join(k.upper() for k in element.dpad_keys)
    if element.type is ElementType.SWIPE:
        return f"swipe {element.swipe_duration_ms} ms"
    if element.type is ElementType.MOUSE:
        return f"aim ×{element.sensitivity:g}, exit {element.exit_key}"
    raise AssertionError(f"unhandled element type {element.type!r}")


def editor_hint(element_type: ElementType) -> str:
    """Contextual help shown while placing a new key."""
    return {
        ElementType.TAP: "click on the picture: a press produces one quick tap there",
        ElementType.HOLD: "click and drag to size the touch: the contact stays while held",
        ElementType.DPAD: "click where the stick centre should be, then drag its radius",
        ElementType.SWIPE: "click the start point, then drag to the end point",
        ElementType.MOUSE: "click where the aim cursor rests; press the key to grab",
    }[element_type]


def apply_changes_message(steps: list[str]) -> tuple[str, Tone]:
    if not steps:
        return "nothing to apply", Tone.NEUTRAL
    joined = " · ".join(steps)
    if "takes effect" in joined or "restart" in joined:
        return joined + " — restart the session to apply the buffer size", Tone.INFO
    return joined, Tone.POSITIVE


def empty_state_text(has_backend: bool, has_report: bool) -> str:
    if not has_backend:
        return (
            "Waydroid is not installed. Install it from your distribution, then run "
            "diagnostics again. LynxDroid drives the installed backend; it does not "
            "download Android itself."
        )
    if not has_report:
        return "Run a diagnostic pass to see what this machine supports."
    return "Ready. Start the session when you want Android available."


def consent_text(action: str, detail: str) -> str:
    """Explicit consent wording for privileged or persistent changes."""
    return (
        f"{action}\n\n{detail}\n\n"
        "This changes state that persists on your system. LynxDroid records the previous "
        "value so it can be restored, and will not touch files it does not own."
    )


def game_error_message(message: str) -> str:
    """Present a possible match, without claiming to have diagnosed the cause."""
    if not message.strip():
        return "Enter the error message shown by the game."
    signature = match_failure(message)
    if signature is None:
        return "No known match. The cause is unknown; run Doctor and consult the game vendor."
    text = f"Possible match: {signature.title}\n\n{signature.explanation}"
    if signature.fix == "network":
        text += "\n\nYou can run the network probes below. They do not change settings."
    return text


def game_network_message(result: FixResult) -> str:
    """Display probe output including unsuccessful or unavailable steps."""
    steps = "\n".join(
        f"{step.action}: {'succeeded' if step.done else 'failed'}\n{step.detail}"
        for step in result.steps
    )
    return f"{result.message}\n\n{steps}" if steps else result.message
