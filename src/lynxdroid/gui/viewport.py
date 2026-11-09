"""Qt-free viewport helpers shared by the drawing code and the editor.

``hit_test`` and ``content_rect_contains`` used to live in ``overlay.py``,
which imports PySide6 at module level. That made them untestable on any
machine without Qt (including CI), so the whole "the drawn position is the
clickable position" guarantee was asserted by nothing.

They live here now: no Qt import, real tests, and ``overlay.py`` imports them
back so there is still exactly one implementation.
"""

from __future__ import annotations

from lynxdroid.display.geometry import Rect, ViewportGeometry
from lynxdroid.profiles.model import Element, ElementType, Profile

#: Smallest clickable radius in logical pixels, so a tiny key is still reachable.
MIN_HIT_RADIUS_PX = 12.0


def _element_radius_px(geometry: ViewportGeometry, element: Element) -> float:
    content = geometry.content_rect()
    short_side = min(content.width, content.height)
    radius = max(MIN_HIT_RADIUS_PX, element.radius * short_side)
    if element.type is ElementType.DPAD:
        radius = max(radius, element.dpad_radius * short_side)
    return radius


def hit_test(
    geometry: ViewportGeometry, profile: Profile | None, px: float, py: float
) -> Element | None:
    """Topmost element under a window point, or ``None``.

    Used by the editor to select a key, and by tests to prove the drawn
    position is the clickable position. Points on the letterbox bars never
    hit anything, because they do not map onto the Android screen.
    """
    if profile is None:
        return None
    if not content_rect_contains(geometry, px, py):
        return None
    best: Element | None = None
    best_distance = float("inf")
    for element in profile.elements:
        ex, ey = geometry.normalized_to_widget(element.x, element.y)
        radius = _element_radius_px(geometry, element)
        distance = ((px - ex) ** 2 + (py - ey) ** 2) ** 0.5
        if distance <= radius and distance < best_distance:
            best = element
            best_distance = distance
    return best


def content_rect_contains(geometry: ViewportGeometry, px: float, py: float) -> bool:
    rect: Rect = geometry.content_rect()
    return rect.contains(px, py)
