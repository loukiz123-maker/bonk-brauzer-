"""Viewport geometry: the single source of truth for pointer coordinates.

Two coordinate spaces exist and they are never mixed:

* **widget space** - logical (device independent) pixels inside our Qt window;
* **android space** - pixels inside the Android buffer (``wm size``).

Everything else (letterbox bars, DPI scaling, rotation) is derived here, once,
with tests. The rules encoded below come straight from stages 6 and 7:

1. The content keeps its aspect ratio; leftover area is a black bar
   (letterbox when the window is too wide, pillarbox when too tall).
2. A pointer outside the content rectangle never produces an Android touch.
3. Input and rendering use the *same* transform, so a key drawn at a position
   injects a touch at exactly that position.
4. Geometry is immutable and replaced atomically - a rotation or resize
   creates a new object, so an in-flight event cannot observe a half-updated
   mapping.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from enum import Enum

_EDGE_EPSILON = 1e-3


class FitMode(str, Enum):
    """How the Android buffer fills the window."""

    CONTAIN = "contain"  # fit inside, bars on the short side (default)
    FILL = "fill"  # stretch, aspect ratio not preserved
    INTEGER = "integer"  # integer scale factor, bars as needed (crisp pixels)


class Rotation(int, Enum):
    """Clockwise rotation of the Android content inside the window."""

    NONE = 0
    CW90 = 90
    CW180 = 180
    CW270 = 270

    @property
    def swaps_axes(self) -> bool:
        return self in (Rotation.CW90, Rotation.CW270)


@dataclass(frozen=True, slots=True)
class Size:
    width: int
    height: int

    def __post_init__(self) -> None:
        if self.width < 0 or self.height < 0:
            raise ValueError(f"size components must be >= 0, got {self.width}x{self.height}")

    @property
    def area(self) -> int:
        return self.width * self.height

    @property
    def aspect(self) -> float:
        if self.height == 0:
            return 0.0
        return self.width / self.height

    def is_empty(self) -> bool:
        return self.width == 0 or self.height == 0

    def as_tuple(self) -> tuple[int, int]:
        return (self.width, self.height)


@dataclass(frozen=True, slots=True)
class Point:
    x: float
    y: float


@dataclass(frozen=True, slots=True)
class Rect:
    x: float
    y: float
    width: float
    height: float

    @property
    def right(self) -> float:
        return self.x + self.width

    @property
    def bottom(self) -> float:
        return self.y + self.height

    def contains(self, px: float, py: float) -> bool:
        return self.x <= px < self.right and self.y <= py < self.bottom

    def as_tuple(self) -> tuple[float, float, float, float]:
        return (self.x, self.y, self.width, self.height)


@dataclass(frozen=True, slots=True)
class ViewportGeometry:
    """Immutable description of how the Android buffer maps into the window.

    ``surface`` is in logical pixels; ``buffer`` is the Android size in pixels.
    ``dpr`` is recorded for reporting only - all maths stays in logical pixels,
    which is exactly what Qt delivers from ``QMouseEvent.position()``.
    """

    buffer: Size
    surface: Size
    fit: FitMode = FitMode.CONTAIN
    rotation: Rotation = Rotation.NONE
    dpr: float = 1.0
    #: Android display id/name, for diagnostics only.
    display_name: str = "default"

    # -- derived ---------------------------------------------------------
    @property
    def rotated_buffer(self) -> Size:
        """Buffer size as seen on screen, i.e. after rotation is applied."""
        if self.rotation.swaps_axes:
            return Size(self.buffer.height, self.buffer.width)
        return self.buffer

    def content_rect(self) -> Rect:
        """Where the Android picture is drawn inside the surface."""
        src = self.rotated_buffer
        if src.is_empty() or self.surface.is_empty():
            return Rect(0.0, 0.0, 0.0, 0.0)

        if self.fit is FitMode.FILL:
            return Rect(0.0, 0.0, float(self.surface.width), float(self.surface.height))

        if self.fit is FitMode.INTEGER:
            factor = min(
                self.surface.width // max(src.width, 1), self.surface.height // max(src.height, 1)
            )
            factor = max(factor, 1)
            width = float(src.width * factor)
            height = float(src.height * factor)
        else:  # CONTAIN
            scale = min(self.surface.width / src.width, self.surface.height / src.height)
            width = src.width * scale
            height = src.height * scale

        # Snap to whole logical pixels: bar edges become crisp and, crucially,
        # left/right and top/bottom bars stay equal for odd remainders.
        int_width = max(1, math.floor(width))
        int_height = max(1, math.floor(height))
        left = (self.surface.width - int_width) // 2
        top = (self.surface.height - int_height) // 2
        return Rect(float(left), float(top), float(int_width), float(int_height))

    def bars(self) -> dict[str, int]:
        """Thickness of the black bars on each side, in logical pixels."""
        rect = self.content_rect()
        return {
            "left": int(rect.x),
            "top": int(rect.y),
            "right": int(self.surface.width - rect.right),
            "bottom": int(self.surface.height - rect.bottom),
        }

    def has_bars(self) -> bool:
        return any(value > 0 for value in self.bars().values())

    def scale(self) -> float:
        """Logical pixels per Android pixel along x."""
        src = self.rotated_buffer
        rect = self.content_rect()
        if src.width == 0:
            return 0.0
        return rect.width / src.width

    # -- transforms ------------------------------------------------------
    def widget_to_android(self, px: float, py: float) -> tuple[float, float] | None:
        """Map a window point to Android pixels.

        Returns ``None`` when the point lies on a black bar or outside the
        window: those coordinates must not become touches (stage 6).
        """
        rect = self.content_rect()
        if rect.width <= 0 or rect.height <= 0:
            return None
        if not rect.contains(px, py):
            return None
        u = (px - rect.x) / rect.width  # 0..1 across the pictured area
        v = (py - rect.y) / rect.height
        pu, pv = self._unrotate_to_android(u, v)
        return self.profile_to_android(pu, pv)

    def widget_to_normalized(self, px: float, py: float) -> tuple[float, float] | None:
        """Window point -> (u, v) of the rotated picture, both in [0, 1)."""
        rect = self.content_rect()
        if rect.width <= 0 or rect.height <= 0 or not rect.contains(px, py):
            return None
        return ((px - rect.x) / rect.width, (py - rect.y) / rect.height)

    def normalized_to_android(self, u: float, v: float) -> tuple[float, float]:
        """(u, v) of the rotated picture -> Android pixels, clamped inside."""
        q, r = self._unrotate_to_android(min(max(u, 0.0), 1.0), min(max(v, 0.0), 1.0))
        return self.profile_to_android(q, r)

    def profile_to_android(self, u: float, v: float) -> tuple[float, float]:
        """Profile coordinates (buffer normalised) -> Android pixels.

        Profiles are authored in the Android buffer's own space, so this is the
        transform the input engine must use for key positions. It never applies
        rotation: rotation is a property of the window, not of the game.
        """
        width = self.buffer.width
        height = self.buffer.height
        x = min(max(u * width, 0.0), float(max(width - 1, 0)))
        y = min(max(v * height, 0.0), float(max(height - 1, 0)))
        return (x, y)

    def android_to_profile(self, ax: float, ay: float) -> tuple[float, float]:
        """Android pixels -> profile coordinates (for pasting a key position)."""
        width = max(self.buffer.width, 1)
        height = max(self.buffer.height, 1)
        return (
            min(max(ax / width, 0.0), 1.0),
            min(max(ay / height, 0.0), 1.0),
        )

    def android_to_widget(self, ax: float, ay: float) -> tuple[float, float]:
        """Android pixels -> window point (used to draw the key overlay)."""
        pu, pv = self.android_to_profile(ax, ay)
        return self.normalized_to_widget(pu, pv)

    def normalized_to_widget(self, u: float, v: float) -> tuple[float, float]:
        """Profile coordinates (Android normalised) -> window point.

        The result is clamped just inside the content rectangle, so an
        element authored exactly on the buffer edge is still drawn and hit
        inside the window (a coordinate on the boundary is not "contained").
        """
        rect = self.content_rect()
        if rect.width <= 0 or rect.height <= 0:
            return (rect.x, rect.y)
        ru, rv = self._rotate(min(max(u, 0.0), 1.0), min(max(v, 0.0), 1.0))
        x = rect.x + ru * rect.width
        y = rect.y + rv * rect.height
        return (
            min(max(x, rect.x), rect.right - _EDGE_EPSILON),
            min(max(y, rect.y), rect.bottom - _EDGE_EPSILON),
        )

    def widget_to_normalized_profile(self, px: float, py: float) -> tuple[float, float] | None:
        """Window point -> profile coordinates, i.e. *undo* the rotation.

        Profiles are stored in Android buffer space so that a key placed while
        the device was in landscape lands on the same physical part of the
        screen after the orientation changes.
        """
        rotated = self.widget_to_normalized(px, py)
        if rotated is None:
            return None
        return self._unrotate_normalized(*rotated)

    # -- internals -------------------------------------------------------
    def _rotate(self, u: float, v: float) -> tuple[float, float]:
        """Buffer-normalised -> rotated-picture-normalised (clockwise)."""
        if self.rotation is Rotation.CW90:
            return (1.0 - v, u)
        if self.rotation is Rotation.CW180:
            return (1.0 - u, 1.0 - v)
        if self.rotation is Rotation.CW270:
            return (v, 1.0 - u)
        return (u, v)

    def _unrotate_normalized(self, ru: float, rv: float) -> tuple[float, float]:
        """Inverse of ``_rotate``: rotated-picture normalised -> buffer normalised."""
        if self.rotation is Rotation.CW90:
            return (rv, 1.0 - ru)
        if self.rotation is Rotation.CW180:
            return (1.0 - ru, 1.0 - rv)
        if self.rotation is Rotation.CW270:
            return (1.0 - rv, ru)
        return (ru, rv)

    def _unrotate_to_android(self, ru: float, rv: float) -> tuple[float, float]:
        """Rotated-picture normalised -> buffer normalised (inverse rotation)."""
        u, v = self._unrotate_normalized(ru, rv)
        return (min(max(u, 0.0), 1.0), min(max(v, 0.0), 1.0))

    # -- reporting -------------------------------------------------------
    def describe(self) -> str:
        rect = self.content_rect()
        bars = self.bars()
        return (
            f"buffer {self.buffer.width}x{self.buffer.height} -> surface "
            f"{self.surface.width}x{self.surface.height} fit={self.fit.value} "
            f"rot={self.rotation.value} dpr={self.dpr:.2f} "
            f"content={rect.as_tuple()} bars=L{bars['left']}/T{bars['top']}/"
            f"R{bars['right']}/B{bars['bottom']}"
        )

    def as_dict(self) -> dict[str, object]:
        rect = self.content_rect()
        return {
            "buffer": list(self.buffer.as_tuple()),
            "surface": list(self.surface.as_tuple()),
            "fit": self.fit.value,
            "rotation": self.rotation.value,
            "dpr": self.dpr,
            "content": list(rect.as_tuple()),
            "bars": self.bars(),
            "scale": self.scale(),
        }
