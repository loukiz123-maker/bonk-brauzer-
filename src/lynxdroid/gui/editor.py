"""Layout editor interaction model (stage 9), deliberately Qt-free.

The widgets in ``overlay.py`` / ``main_window.py`` cannot be instantiated on
this machine (no ``libGL``), so every piece of editor *behaviour* lives here
where it can be tested: drag state, snapping to the content rectangle, undo of
an in-progress drag, and the "modified" flag that drives the save button.

The rule the tests protect: a drag must never leave the profile in a state that
cannot be saved, and losing the mouse button or focus mid-drag must not strand
the element somewhere the user did not ask for.
"""

from __future__ import annotations

import uuid
from collections.abc import Sequence
from dataclasses import dataclass, field, replace

from lynxdroid.display.geometry import ViewportGeometry
from lynxdroid.gui.viewport import content_rect_contains, hit_test
from lynxdroid.profiles.model import Element, ElementType, Profile, ProfileError


@dataclass(frozen=True)
class DragResult:
    """Outcome of one pointer movement during a drag."""

    profile: Profile
    element_id: str | None = None
    changed: bool = False
    #: True when the requested position was clamped back onto the screen.
    clamped: bool = False


@dataclass
class LayoutEditor:
    """Tracks one profile being edited and an optional in-progress drag."""

    profile: Profile
    #: The profile as stored on disk, for the "reset" and "dirty" questions.
    saved: Profile | None = None
    selected: str | None = None
    dragging: str | None = None
    _drag_origin: tuple[float, float] | None = field(default=None, repr=False)

    # -- selection -------------------------------------------------------
    def select_at(self, geometry: ViewportGeometry, px: float, py: float) -> str | None:
        """Select the element under a window point; clicking empty space clears."""
        element = hit_test(geometry, self.profile, px, py)
        self.selected = element.id if element else None
        return self.selected

    def select(self, element_id: str | None) -> str | None:
        """Select from outside the canvas (the element list, a test).

        The canvas and the list must agree: clicking a key highlights its row,
        so choosing a row must highlight the key and mark it on the surface.
        An id that is not in this profile selects nothing rather than leaving a
        stale selection behind.
        """
        if element_id is not None and self.profile.element(element_id) is None:
            element_id = None
        self.selected = element_id
        return self.selected

    def begin_drag(self, geometry: ViewportGeometry, px: float, py: float) -> str | None:
        """Start dragging whatever is under the pointer, inside the content area.

        A press on the letterbox bars selects nothing and starts no drag: those
        pixels do not map to the Android screen, so moving a key there would be
        meaningless.
        """
        if not content_rect_contains(geometry, px, py):
            self.dragging = None
            self._drag_origin = None
            return None
        element = hit_test(geometry, self.profile, px, py)
        if element is None:
            self.dragging = None
            self._drag_origin = None
            return None
        self.selected = element.id
        self.dragging = element.id
        self._drag_origin = (element.x, element.y)
        return element.id

    def drag_to(self, geometry: ViewportGeometry, px: float, py: float) -> DragResult:
        """Move the dragged element to the normalised point under the pointer."""
        if self.dragging is None:
            return DragResult(profile=self.profile)
        element = self.profile.element(self.dragging)
        if element is None:  # pragma: no cover - dragging implies existence
            self.dragging = None
            return DragResult(profile=self.profile)

        point = geometry.widget_to_normalized_profile(px, py)
        if point is None:
            # Pointer wandered onto a letterbox bar: keep the element where it is.
            return DragResult(profile=self.profile, element_id=element.id)
        x, y = point
        moved = self.profile.move_element(element.id, x, y)
        new_element = moved.element(element.id)
        if new_element is None:  # pragma: no cover - move_element keeps elements
            return DragResult(profile=self.profile, element_id=element.id)
        clamped = abs(new_element.x - x) > 1e-9 or abs(new_element.y - y) > 1e-9
        changed = new_element.x != element.x or new_element.y != element.y
        self.profile = moved
        return DragResult(profile=moved, element_id=element.id, changed=changed, clamped=clamped)

    def end_drag(self, *, commit: bool = True) -> Profile:
        """Finish a drag. ``commit=False`` (Escape, focus loss) restores the start."""
        if self.dragging is not None and not commit and self._drag_origin is not None:
            element = self.profile.element(self.dragging)
            if element is not None:
                x, y = self._drag_origin
                self.profile = self.profile.replace_element(replace(element, x=x, y=y))
        self.dragging = None
        self._drag_origin = None
        return self.profile

    # -- profile state ---------------------------------------------------
    @property
    def dirty(self) -> bool:
        """True when the edited profile differs from what is stored."""
        if self.saved is None:
            return bool(self.profile.elements)
        return self.profile.checksum() != self.saved.checksum() or (
            self.profile.name != self.saved.name
        )

    def reset(self) -> bool:
        """Discard edits, back to the stored profile. Returns whether anything changed."""
        if self.saved is None:
            return False
        changed = self.dirty
        self.profile = self.saved
        self.selected = None
        self.dragging = None
        self._drag_origin = None
        return changed

    def mark_saved(self, stored: Profile) -> None:
        self.profile = stored
        self.saved = stored
        self.dragging = None
        self._drag_origin = None

    def add(self, element: Element) -> Profile:
        """Add then select, so the new key is visibly active."""
        self.profile = self.profile.add_element(element)
        self.selected = element.id
        return self.profile

    def remove_selected(self) -> Profile:
        if self.selected is None:
            return self.profile
        self.profile = self.profile.remove_element(self.selected)
        self.selected = None
        self.dragging = None
        return self.profile


def orientation_swapped(profile: Profile) -> Profile:
    """Return a copy with every coordinate transposed (portrait <-> landscape).

    Stage 9 asks for separate portrait and landscape layouts. Supporting them
    by transposing the existing one is offered as a starting point, and the
    documentation says plainly that it is a starting point: a real game often
    moves its controls for portrait, so the transposed layout is something the
    user then edits, not a claim that it is correct.
    """
    swapped = tuple(
        replace(
            element,
            x=element.y,
            y=element.x,
            swipe_to=(element.swipe_to[1], element.swipe_to[0]),
        )
        for element in profile.elements
    )
    candidate = profile.with_elements(swapped)
    candidate.validate()
    return candidate


_SPECIAL_KEYS = {
    "space": "Space",
    "return": "Return",
    "enter": "Enter",
    "tab": "Tab",
    "backspace": "Backspace",
    "shift": "Shift",
    "control": "Control",
    "alt": "Alt",
    "up": "Up",
    "down": "Down",
    "left": "Left",
    "right": "Right",
}


def normalize_control_key(text: str) -> str:
    """Accept only keys the desktop event mapper can actually produce."""
    key = text.strip()
    if len(key) == 1 and key.isascii() and key.isalnum():
        return key.lower()
    special = _SPECIAL_KEYS.get(key.lower())
    if special:
        return special
    raise ProfileError(
        "Use one Latin letter or digit, or Space, Return, Tab, Shift, Control, Alt, "
        "Backspace, Enter or an arrow name."
    )


def _ensure_keys_free(profile: Profile, keys: Sequence[str]) -> None:
    for key in keys:
        for element in profile.elements:
            occupied = [*element.keys, *element.dpad_keys]
            if element.type is ElementType.MOUSE:
                occupied.append(element.exit_key)
            if any(existing.casefold() == key.casefold() for existing in occupied if existing):
                raise ProfileError(f"key '{key}' is already assigned to '{element.label}'")


def add_key_control(editor: LayoutEditor, key_text: str, *, hold: bool = False) -> Profile:
    """Add a validated key binding at the centre; never mutate on failure."""
    key = normalize_control_key(key_text)
    _ensure_keys_free(editor.profile, [key])
    control = Element(
        id="control-" + uuid.uuid4().hex[:16],
        type=ElementType.HOLD if hold else ElementType.TAP,
        label=key,
        x=0.5,
        y=0.5,
        keys=(key,),
    )
    return editor.add(control)


def add_dpad_control(editor: LayoutEditor, keys: Sequence[str]) -> Profile:
    """Add a movement control with keys ordered up, right, down, left."""
    if len(keys) != 4:
        raise ProfileError("Enter four keys in order: up, right, down, left")
    up, right, down, left = (normalize_control_key(key) for key in keys)
    ordered = (up, right, down, left)
    if len(set(ordered)) != 4:
        raise ProfileError("Movement directions need four distinct keys")
    _ensure_keys_free(editor.profile, ordered)
    control = Element(
        id="control-" + uuid.uuid4().hex[:16],
        type=ElementType.DPAD,
        label="Movement",
        x=0.5,
        y=0.5,
        dpad_keys=ordered,
    )
    return editor.add(control)
