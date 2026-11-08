"""Resolution, DPI and viewport-option validation (stage 7).

The specification is explicit about four *different* concepts that users
conflate, so they are separate types here:

===============  ==========================================================
concept          what it changes
===============  ==========================================================
buffer size      the pixels Android renders into (``persist.waydroid.width``
                 / ``height``, applied by Waydroid on session restart)
wm override      Android's logical viewport (``wm size``); a software-level
                 override that some apps ignore
density          logical density in dpi (``wm density``)
window scale     how big the window is on the desktop; purely ours
===============  ==========================================================
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from enum import Enum

# Hard engineering limits. Not marketing numbers: they exist to reject
# zero/negative values and prevent integer overflow in area/stride maths.
MIN_EDGE = 320
MAX_EDGE = 7680
MIN_DPI = 72
MAX_DPI = 960
MIN_AREA = MIN_EDGE * MIN_EDGE
MAX_AREA = 3840 * 2160  # 8.3 Mpx: 4K is the largest buffer we accept


class ApplyMode(str, Enum):
    """When a setting takes effect, so the UI can tell the truth."""

    LIVE = "live"
    RESTART_SESSION = "restart-session"
    RESTART_CONTAINER = "restart-container"


@dataclass(frozen=True, slots=True)
class DisplayPreset:
    name: str
    width: int
    height: int
    dpi: int
    orientation: str
    note: str = ""


PRESETS: tuple[DisplayPreset, ...] = (
    DisplayPreset(
        "720p landscape",
        1280,
        720,
        240,
        "landscape",
        "lightest option; good for 2D games on Iris Xe",
    ),
    DisplayPreset("720p portrait", 720, 1280, 240, "portrait"),
    DisplayPreset("1080p landscape", 1920, 1080, 320, "landscape", "native for the Vivobook panel"),
    DisplayPreset("1080p portrait", 1080, 1920, 320, "portrait"),
    DisplayPreset(
        "1600x900 landscape",
        1600,
        900,
        280,
        "landscape",
        "middle ground between sharpness and GPU load",
    ),
    DisplayPreset(
        "ancient 4:3", 1024, 768, 160, "landscape", "for old apps that assume a 4:3 screen"
    ),
)


class Orientation(str, Enum):
    PORTRAIT = "portrait"
    LANDSCAPE = "landscape"

    @classmethod
    def of(cls, width: int, height: int) -> Orientation:
        return cls.LANDSCAPE if width >= height else cls.PORTRAIT


@dataclass(frozen=True, slots=True)
class DisplayRequest:
    """Validated request to change what Android renders."""

    width: int
    height: int
    dpi: int
    #: Apply the buffer change through persist props (needs a session restart)
    apply_buffer: bool = True
    #: Also apply an Android-side ``wm size`` override (cosmetic, reversible)
    apply_wm_override: bool = False

    @property
    def orientation(self) -> Orientation:
        return Orientation.of(self.width, self.height)

    @property
    def area(self) -> int:
        return self.width * self.height

    @property
    def aspect(self) -> float:
        return self.width / self.height

    def as_async_clamp_string(self) -> str:  # pragma: no cover - convenience
        return f"{self.width}x{self.height}@{self.dpi}"


@dataclass(frozen=True, slots=True)
class ValidationIssue:
    field: str
    message: str
    severity: str  # "error" | "warning"

    @property
    def is_error(self) -> bool:
        return self.severity == "error"


def validate_display(
    width: object,
    height: object,
    dpi: object,
    *,
    max_area: int = MAX_AREA,
) -> tuple[DisplayRequest | None, list[ValidationIssue]]:
    """Validate user input for resolution/DPI.

    Accepts ``object`` on purpose: GUI text fields, JSON and CLI all arrive as
    strings/ints/floats and must be validated the same way. Returns
    ``(request_or_None, issues)``; ``request`` is ``None`` whenever any issue
    is an error, so callers cannot accidentally apply a half-valid value.
    """
    issues: list[ValidationIssue] = []
    w = _as_int(width, "width", issues)
    h = _as_int(height, "height", issues)
    d = _as_int(dpi, "dpi", issues)

    if w is None or h is None or d is None:
        return None, issues

    for field, value, low, high in (
        ("width", w, MIN_EDGE, MAX_EDGE),
        ("height", h, MIN_EDGE, MAX_EDGE),
    ):
        if value <= 0:
            issues.append(ValidationIssue(field, f"{field} must be positive", "error"))
        elif value < low:
            issues.append(
                ValidationIssue(field, f"{field}={value} is below the safe minimum {low}", "error")
            )
        elif value > high:
            issues.append(
                ValidationIssue(field, f"{field}={value} is above the safe maximum {high}", "error")
            )

    if d <= 0:
        issues.append(ValidationIssue("dpi", "dpi must be positive", "error"))
    elif not MIN_DPI <= d <= MAX_DPI:
        issues.append(
            ValidationIssue("dpi", f"dpi={d} must be between {MIN_DPI} and {MAX_DPI}", "error")
        )

    if not any(issue.is_error for issue in issues):
        area = w * h
        if area > max_area:
            issues.append(
                ValidationIssue(
                    "size",
                    f"{w}x{h} = {area / 1e6:.1f} Mpx exceeds the supported "
                    f"{max_area / 1e6:.1f} Mpx",
                    "error",
                )
            )
        ratio = w / h if h else 0
        if ratio and not (0.3 <= ratio <= 3.5):
            issues.append(
                ValidationIssue(
                    "aspect",
                    f"aspect ratio {ratio:.2f} is extreme; most Android apps assume "
                    "between 0.3 and 3.5",
                    "warning",
                )
            )
        if area * 4 > 2**31 - 1:
            # Guard for 32-bit stride arithmetic inside some Android buffers.
            issues.append(
                ValidationIssue(
                    "size",
                    "RGBA buffer would overflow a signed 32-bit stride",
                    "error",
                )
            )

    if any(issue.is_error for issue in issues):
        return None, issues
    return DisplayRequest(width=w, height=h, dpi=d), issues


def _as_int(value: object, field: str, issues: list[ValidationIssue]) -> int | None:
    if isinstance(value, bool):
        issues.append(ValidationIssue(field, f"{field} must be a number", "error"))
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        if not math.isfinite(value) or value != int(value):
            issues.append(ValidationIssue(field, f"{field} must be a whole number", "error"))
            return None
        return int(value)
    if isinstance(value, str):
        text = value.strip()
        if not text:
            issues.append(ValidationIssue(field, f"{field} is empty", "error"))
            return None
        try:
            return int(text, 10)
        except ValueError:
            issues.append(ValidationIssue(field, f"{field}='{text}' is not an integer", "error"))
            return None
    issues.append(
        ValidationIssue(field, f"{field} has unsupported type {type(value).__name__}", "error")
    )
    return None


def suggest_apply_mode(change: str) -> ApplyMode:
    """Document (in one place) which changes need which restart level.

    Waydroid quirk: ``persist.waydroid.width/height`` are read when the session
    starts, ``wm size`` applies live but is a logical override that many
    full-screen games ignore, and ``persist.waydroid.multi_windows`` needs the
    container restarted. We do not claim otherwise anywhere in the UI.
    """
    return {
        "buffer": ApplyMode.RESTART_SESSION,
        "dpi": ApplyMode.LIVE,
        "wm-size": ApplyMode.LIVE,
        "multi-windows": ApplyMode.RESTART_CONTAINER,
        "fake-touch": ApplyMode.RESTART_SESSION,
    }.get(change, ApplyMode.RESTART_SESSION)


def preset_by_name(name: str) -> DisplayPreset | None:
    for preset in PRESETS:
        if preset.name.lower() == name.strip().lower():
            return preset
    return None
