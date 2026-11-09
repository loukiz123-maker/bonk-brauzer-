"""Keyboard -> multitouch translation engine.

This is the piece the whole product leans on, so its contract is small and
explicit:

Input
    physical key transitions (down/up, with OS auto-repeat flagged),
    plus an optional analog pointer inside a d-pad widget.

State
    one ``_Contact`` per live touch pointer, one ``_Binding`` per element.

Output
    an ordered list of ``TouchEvent`` in Android pixels.

Guarantees (each one is a test in ``tests/input``):

1. Every touch that goes DOWN eventually goes UP or CANCEL - including on
   focus loss, profile switch, geometry change and backend failure.
2. A pointer id is never reused while its contact is still open.
3. Diagonal d-pad movement is normalised, never longer than the radius.
4. Releasing one of two held direction keys keeps the other direction.
5. A tap emits exactly one down/up pair at one point.
6. Updates are atomic: ``set_profile``/``set_geometry`` release everything
   first, so no event can be computed against a half-changed mapping.
"""

from __future__ import annotations

import math
import time
from collections.abc import Callable, Iterable, Sequence
from dataclasses import dataclass, field

from lynxdroid.display.geometry import ViewportGeometry
from lynxdroid.input.events import (
    Action,
    EventQueue,
    InputError,
    TouchEvent,
    TouchSlotExhausted,
)
from lynxdroid.profiles.model import Element, ElementType, Profile

#: Clock returns milliseconds. Injectable so tests are deterministic.
Clock = Callable[[], int]

MAX_TOUCH_SLOTS = 10


def monotonic_ms() -> int:
    return int(time.monotonic() * 1000)


@dataclass(slots=True)
class _Contact:
    """A live touch pointer."""

    pointer: int
    element_id: str
    x: float
    y: float
    started_ms: int
    seq: int = 0


@dataclass(slots=True)
class _DirState:
    """Held directions for a d-pad, in press order (later press wins ties)."""

    order: list[str] = field(default_factory=list)

    def press(self, direction: str) -> None:
        if direction in self.order:
            self.order.remove(direction)
        self.order.append(direction)

    def release(self, direction: str) -> None:
        if direction in self.order:
            self.order.remove(direction)

    @property
    def empty(self) -> bool:
        return not self.order

    def vector(self) -> tuple[float, float]:
        """Sum of held directions, y pointing down (Android screen axes)."""
        dx = dy = 0.0
        for direction in self.order:
            if direction == "up":
                dy -= 1.0
            elif direction == "down":
                dy += 1.0
            elif direction == "left":
                dx -= 1.0
            elif direction == "right":
                dx += 1.0
        return (dx, dy)


@dataclass(slots=True)
class _Binding:
    """Runtime state for one profile element."""

    element: Element
    pointer: int = 0
    # tap/hold/mouse
    held_keys: set[str] = field(default_factory=set)
    # dpad
    directions: _DirState = field(default_factory=_DirState)
    # swipe
    swipe_started_ms: int = 0
    swipe_progress: float = 0.0
    # mouse
    mouse_active: bool = False
    mouse_x: float = 0.0
    mouse_y: float = 0.0


class InputEngine:
    """Deterministic mapper from keyboard state to Android touch events."""

    def __init__(
        self,
        profile: Profile,
        geometry: ViewportGeometry,
        *,
        clock: Clock = monotonic_ms,
        capacity: int = MAX_TOUCH_SLOTS,
    ) -> None:
        self._profile = profile
        self._geometry = geometry
        self._clock = clock
        self._capacity = max(1, min(capacity, MAX_TOUCH_SLOTS))
        self._queue = EventQueue(capacity=512)
        self._active_pointers: dict[int, _Contact] = {}
        self._bindings: dict[str, _Binding] = {}
        self._key_owner: dict[str, list[str]] = {}
        self._seq = 0
        self._errors: list[InputError] = []
        self._last_frame_ms = 0
        self._realtime_capture = False
        self._rebuild()

    # ------------------------------------------------------------------
    # Configuration
    # ------------------------------------------------------------------
    def _rebuild(self) -> None:
        self._bindings = {}
        self._key_owner = {}
        for element in self._profile.elements:
            binding = _Binding(element=element, pointer=element.pointer)
            self._bindings[element.id] = binding
            for key in self._element_keys(element):
                self._key_owner.setdefault(key, []).append(element.id)
            if element.type is ElementType.MOUSE:
                binding.mouse_x = element.x * self._geometry.buffer.width
                binding.mouse_y = element.y * self._geometry.buffer.height

    @staticmethod
    def _element_keys(element: Element) -> list[str]:
        if element.type in (ElementType.TAP, ElementType.HOLD, ElementType.SWIPE):
            return list(element.keys)
        if element.type is ElementType.DPAD:
            return [key for key in element.dpad_keys if key]
        if element.type is ElementType.MOUSE:
            # The grab key (``keys``) starts aim mode; the exit key ends it.
            # key_conflicts() already counts both, so the engine must too, or
            # a profile can warn about a conflict the engine cannot produce.
            return [*element.keys, element.exit_key] if element.exit_key else list(element.keys)
        raise AssertionError(f"unhandled element type {element.type!r}")

    @property
    def profile(self) -> Profile:
        return self._profile

    @property
    def geometry(self) -> ViewportGeometry:
        return self._geometry

    @property
    def capacity(self) -> int:
        return self._capacity

    @property
    def active_pointers(self) -> dict[int, _Contact]:
        return dict(self._active_pointers)

    def set_geometry(self, geometry: ViewportGeometry) -> list[TouchEvent]:
        """Atomically rebind to a new viewport; releases every open contact."""
        released = self.release_all("geometry changed")
        self._geometry = geometry
        for binding in self._bindings.values():
            if binding.element.type is ElementType.MOUSE:
                binding.mouse_x = binding.element.x * geometry.buffer.width
                binding.mouse_y = binding.element.y * geometry.buffer.height
        return released

    def set_profile(self, profile: Profile) -> list[TouchEvent]:
        """Switch profiles safely: old contacts are cancelled before the swap."""
        released = self.release_all("profile changed")
        self._profile = profile
        self._rebuild()
        return released

    # ------------------------------------------------------------------
    # Key input
    # ------------------------------------------------------------------
    def key_down(
        self, key: str, *, now_ms: int | None = None, repeat: bool = False
    ) -> list[TouchEvent]:
        timestamp = self._now(now_ms)
        owners = self._key_owner.get(key)
        if not owners:
            return []
        events: list[TouchEvent] = []
        for element_id in owners:
            binding = self._bindings[element_id]
            events.extend(self._on_element_key(binding, key, True, timestamp, repeat))
        self._flush(events, timestamp)
        return events

    def key_up(self, key: str, *, now_ms: int | None = None) -> list[TouchEvent]:
        timestamp = self._now(now_ms)
        owners = self._key_owner.get(key)
        if not owners:
            return []
        events: list[TouchEvent] = []
        for element_id in owners:
            binding = self._bindings[element_id]
            events.extend(self._on_element_key(binding, key, False, timestamp, False))
        self._flush(events, timestamp)
        return events

    def _on_element_key(
        self,
        binding: _Binding,
        key: str,
        down: bool,
        timestamp: int,
        repeat: bool,
    ) -> list[TouchEvent]:
        element = binding.element
        if element.type is ElementType.TAP:
            # A tap is a complete action: down and up are emitted together on
            # the key press, so a lost key-up can never strand a contact. The
            # key-up itself produces nothing.
            if down and not repeat:
                return self._tap(binding, timestamp)
            return []
        if element.type is ElementType.HOLD:
            if down:
                if repeat or key in binding.held_keys:
                    return []
                binding.held_keys.add(key)
                return self._press(binding, timestamp)
            if key not in binding.held_keys:
                return []
            binding.held_keys.discard(key)
            return self._release(binding, timestamp)
        if element.type is ElementType.DPAD:
            return self._dpad_key(binding, key, down, timestamp, repeat)
        if element.type is ElementType.SWIPE:
            if down and not repeat:
                return self._start_swipe(binding, timestamp)
            return []
        if element.type is ElementType.MOUSE:
            if down and not repeat:
                if key == element.exit_key:
                    return self._exit_mouse(binding, timestamp)
                if not binding.mouse_active:
                    binding.mouse_active = True
                    return self._move(binding, timestamp)
            elif not down and key == element.exit_key:
                return self._exit_mouse(binding, timestamp)
            return []
        raise AssertionError(f"unhandled element type {element.type!r}")

    # -- elementary actions ---------------------------------------------
    def _tap(self, binding: _Binding, timestamp: int) -> list[TouchEvent]:
        pointer = self._claim_pointer(binding)
        x, y = self._element_point(binding)
        down = self._emit(Action.DOWN, pointer, x, y, timestamp, binding.element.id)
        up = self._emit(Action.UP, pointer, x, y, timestamp, binding.element.id)
        self._free_pointer(pointer)
        return [down, up]

    def _press(self, binding: _Binding, timestamp: int) -> list[TouchEvent]:
        if binding.pointer and binding.pointer in self._active_pointers:
            return []
        pointer = self._claim_pointer(binding)
        x, y = self._element_point(binding)
        self._active_pointers[pointer].x = x
        self._active_pointers[pointer].y = y
        return [self._emit(Action.DOWN, pointer, x, y, timestamp, binding.element.id)]

    def _release(self, binding: _Binding, timestamp: int) -> list[TouchEvent]:
        contact = self._contact_for(binding)
        if contact is None:
            return []
        event = self._emit(
            Action.UP, contact.pointer, contact.x, contact.y, timestamp, binding.element.id
        )
        self._free_pointer(contact.pointer)
        return [event]

    def _move(self, binding: _Binding, timestamp: int) -> list[TouchEvent]:
        # Whether the contact already existed decides the first action: a new
        # one must open with DOWN. _contact_for() emits that DOWN, so it has to
        # be returned here as well - callers deliver the returned list, and a
        # DOWN left behind in the queue never reaches Android.
        contact = self._contact_for(binding)
        opening: TouchEvent | None = None
        if contact is None:
            contact = self._contact_for(binding, create=True)
            if contact is None:
                return []
            opening = self._emit(
                Action.DOWN,
                contact.pointer,
                contact.x,
                contact.y,
                timestamp,
                binding.element.id,
            )
        x, y = self._mouse_point(binding)
        contact.x, contact.y = x, y
        events = [self._emit(Action.MOVE, contact.pointer, x, y, timestamp, binding.element.id)]
        return [opening, *events] if opening is not None else events

    def _dpad_key(
        self,
        binding: _Binding,
        key: str,
        down: bool,
        timestamp: int,
        repeat: bool,
    ) -> list[TouchEvent]:
        direction = self._direction_for(binding.element, key)
        if direction is None:
            return []
        was_empty = binding.directions.empty
        if down:
            if repeat:
                return []
            binding.directions.press(direction)
        else:
            binding.directions.release(direction)

        if was_empty and not binding.directions.empty:
            pointer = self._claim_pointer(binding)
            # Contract from stage 10: the first direction key presses DOWN at
            # the centre point, then a MOVE follows to the actual direction.
            cx, cy = self._element_point(binding)
            events = [self._emit(Action.DOWN, pointer, cx, cy, timestamp, binding.element.id)]
            target = self._dpad_target(binding)
            if target is not None:
                contact = self._active_pointers[pointer]
                contact.x, contact.y = target
                events.append(
                    self._emit(Action.MOVE, pointer, *target, timestamp, binding.element.id)
                )
            return events

        open_contact: _Contact | None = self._contact_for(binding)
        if open_contact is None:
            return []

        if binding.directions.empty:
            event = self._emit(
                Action.UP,
                open_contact.pointer,
                open_contact.x,
                open_contact.y,
                timestamp,
                binding.element.id,
            )
            self._free_pointer(open_contact.pointer)
            return [event]

        target = self._dpad_target(binding)
        if target is None:
            return []
        open_contact.x, open_contact.y = target
        return [
            self._emit(Action.MOVE, open_contact.pointer, *target, timestamp, binding.element.id)
        ]

    @staticmethod
    def _direction_for(element: Element, key: str) -> str | None:
        for name, bound in zip(("up", "right", "down", "left"), element.dpad_keys, strict=True):
            if bound == key:
                return name
        return None

    def _dpad_target(self, binding: _Binding) -> tuple[float, float] | None:
        vector = binding.directions.vector()
        if vector == (0.0, 0.0):
            # Opposite held keys cancel out; move the existing touch back to
            # the neutral point rather than leaving it in the previous direction.
            return self._element_point(binding)
        return self._dpad_offset(binding.element, *vector)

    def _dpad_offset(self, element: Element, dx: float, dy: float) -> tuple[float, float] | None:
        """Diagonal-safe stick position: direction normalised, then scaled.

        ``(0, 0)`` is never normalised (zero-length vector), matching the
        explicit rule from stage 10.
        """
        length = math.hypot(dx, dy)
        if length == 0.0:
            return None
        ux, uy = dx / length, dy / length
        width = self._geometry.buffer.width
        height = self._geometry.buffer.height
        if width == 0 or height == 0:
            return None
        radius_px = element.dpad_radius * min(width, height)
        cx, cy = self._element_point_in_build_space(element)
        x = min(max(cx + ux * radius_px, 0.0), float(max(width - 1, 0)))
        y = min(max(cy + uy * radius_px, 0.0), float(max(height - 1, 0)))
        return (x, y)

    def analog_dpad(
        self, element_id: str, dx: float, dy: float, *, now_ms: int | None = None
    ) -> list[TouchEvent]:
        """Drive a d-pad from a pointer drag (analog stick path).

        ``dx``/``dy`` are normalised in -1..1. Values inside the dead zone move
        the contact back to the centre instead of producing jitter.
        """
        binding = self._bindings.get(element_id)
        if binding is None or binding.element.type is not ElementType.DPAD:
            return []
        timestamp = self._now(now_ms)
        magnitude = math.hypot(dx, dy)
        scale = 0.0 if magnitude < binding.element.dead_zone else 1.0
        if magnitude > 1.0:
            dx, dy = dx / magnitude, dy / magnitude
        contact = self._contact_for(binding, create=True, timestamp=timestamp)
        if contact is None:
            return []
        target = (
            self._dpad_offset(binding.element, dx * scale, dy * scale)
            if scale
            else self._element_point(binding)
        )
        if target is None:
            return []
        contact.x, contact.y = target
        event = self._emit(Action.MOVE, contact.pointer, *target, timestamp, element_id)
        self._flush([event], timestamp)
        return [event]

    def _start_swipe(self, binding: _Binding, timestamp: int) -> list[TouchEvent]:
        contact = self._contact_for(binding, create=True, timestamp=timestamp)
        if contact is None:
            return []
        binding.swipe_started_ms = timestamp
        binding.swipe_progress = 0.0
        x, y = self._element_point(binding)
        contact.x, contact.y = x, y
        return [self._emit(Action.DOWN, contact.pointer, x, y, timestamp, binding.element.id)]

    def _exit_mouse(self, binding: _Binding, timestamp: int) -> list[TouchEvent]:
        if not binding.mouse_active:
            return []
        binding.mouse_active = False
        contact = self._contact_for(binding)
        if contact is None:
            return []
        event = self._emit(
            Action.UP, contact.pointer, contact.x, contact.y, timestamp, binding.element.id
        )
        self._free_pointer(contact.pointer)
        return [event]

    # ------------------------------------------------------------------
    # Time-driven work
    # ------------------------------------------------------------------
    def tick(self, *, now_ms: int | None = None) -> list[TouchEvent]:
        """Advance timed behaviours (currently swipe interpolation).

        Called by the UI at a fixed rate; never blocks and never allocates a
        frame when nothing is in flight.
        """
        timestamp = self._now(now_ms)
        events: list[TouchEvent] = []
        for binding in self._bindings.values():
            element = binding.element
            if element.type is not ElementType.SWIPE:
                continue
            contact = self._contact_for(binding)
            if contact is None:
                continue
            elapsed = timestamp - binding.swipe_started_ms
            duration = max(element.swipe_duration_ms, 1)
            progress = min(1.0, elapsed / duration)
            if progress <= binding.swipe_progress:
                continue
            binding.swipe_progress = progress
            sx, sy = self._element_point(binding)
            ex, ey = self._point_from_profile(element.swipe_to[0], element.swipe_to[1])
            x = sx + (ex - sx) * progress
            y = sy + (ey - sy) * progress
            contact.x, contact.y = x, y
            events.append(self._emit(Action.MOVE, contact.pointer, x, y, timestamp, element.id))
            if progress >= 1.0:
                events.append(self._emit(Action.UP, contact.pointer, x, y, timestamp, element.id))
                self._free_pointer(contact.pointer)
        if events:
            self._flush(events, timestamp)
        return events

    def next_deadline_ms(self) -> int | None:
        """Earliest time ``tick`` has work to do, or ``None`` to idle."""
        deadlines: list[int] = []
        for binding in self._bindings.values():
            if binding.element.type is not ElementType.SWIPE:
                continue
            if self._contact_for(binding) is None:
                continue
            deadlines.append(binding.swipe_started_ms + max(binding.element.swipe_duration_ms, 1))
        return min(deadlines) if deadlines else None

    # ------------------------------------------------------------------
    # Focus / safety
    # ------------------------------------------------------------------
    def focus_lost(self, reason: str = "focus lost") -> list[TouchEvent]:
        """Cancel every contact. Called on window blur, minimise, suspend."""
        return self.release_all(reason, cancel=True)

    def release_all(self, reason: str = "released", *, cancel: bool = True) -> list[TouchEvent]:
        """Emit UP/CANCEL for every open contact and clear all key state."""
        timestamp = self._now(None)
        events: list[TouchEvent] = []
        action = Action.CANCEL if cancel else Action.UP
        for pointer in sorted(self._active_pointers):
            contact = self._active_pointers[pointer]
            events.append(
                self._emit(action, pointer, contact.x, contact.y, timestamp, contact.element_id)
            )
        self._active_pointers.clear()
        for binding in self._bindings.values():
            binding.held_keys.clear()
            binding.directions.order.clear()
            binding.swipe_progress = 0.0
            binding.swipe_started_ms = 0
            binding.mouse_active = False
        if events:
            self._flush(events, timestamp)
        return events

    def set_realtime_capture(self, enabled: bool) -> None:
        """Remember whether the platform granted pointer capture (stage 8)."""
        self._realtime_capture = enabled
        if not enabled:
            for binding in self._bindings.values():
                binding.mouse_active = False

    @property
    def realtime_capture(self) -> bool:
        return self._realtime_capture

    @property
    def aim_active(self) -> bool:
        """True while a mouse element is holding the pointer.

        The GUI asks this before capturing the pointer: capturing on a guess
        would hide the cursor and swallow motion for a key that only taps.
        """
        return any(b.mouse_active for b in self._bindings.values())

    # ------------------------------------------------------------------
    # Diagnostics
    # ------------------------------------------------------------------
    def drain_errors(self) -> list[InputError]:
        errors, self._errors = self._errors, []
        return errors

    def pending(self) -> list[TouchEvent]:
        return self._queue.drain()

    def stats(self) -> dict[str, object]:
        return {
            "active_pointers": len(self._active_pointers),
            "capacity": self._capacity,
            "queued": len(self._queue),
            "dropped": self._queue.dropped,
            "sequence": self._seq,
            "elements": len(self._bindings),
            "errors": len(self._errors),
        }

    # ------------------------------------------------------------------
    # Internals
    # ------------------------------------------------------------------
    def _now(self, override: int | None) -> int:
        return self._clock() if override is None else int(override)

    def _emit(
        self,
        action: Action,
        pointer: int,
        x: float,
        y: float,
        timestamp: int,
        source: str,
    ) -> TouchEvent:
        self._seq += 1
        event = TouchEvent(
            action=action,
            pointer=pointer,
            x=float(x),
            y=float(y),
            timestamp_ms=timestamp,
            seq=self._seq,
            source=source,
        )
        self._queue.push(event)
        return event

    def _flush(self, events: Sequence[TouchEvent], timestamp: int) -> None:
        if events:
            self._last_frame_ms = timestamp

    def _claim_pointer(self, binding: _Binding) -> int:
        existing = self._contact_for(binding)
        if existing is not None:
            return existing.pointer
        pointer: int | None = (
            binding.pointer
            if binding.pointer and binding.pointer not in self._active_pointers
            else self._allocate_pointer()
        )
        if pointer is None:
            # Every slot is in use: record it, then refuse. The caller decides
            # whether that is fatal (tap) or just skipped.
            error = TouchSlotExhausted(self._capacity)
            self._errors.append(error)
            raise error
        self._active_pointers[pointer] = _Contact(
            pointer=pointer,
            element_id=binding.element.id,
            x=0.0,
            y=0.0,
            started_ms=self._now(None),
        )
        return pointer

    def _allocate_pointer(self) -> int | None:
        for candidate in range(1, self._capacity + 1):
            if candidate not in self._active_pointers:
                return candidate
        return None

    def _free_pointer(self, pointer: int) -> None:
        self._active_pointers.pop(pointer, None)

    def _contact_for(
        self,
        binding: _Binding,
        *,
        create: bool = False,
        timestamp: int | None = None,
    ) -> _Contact | None:
        for contact in self._active_pointers.values():
            if contact.element_id == binding.element.id:
                return contact
        if not create:
            return None
        pointer = self._claim_pointer(binding)
        contact = self._active_pointers[pointer]
        x, y = self._element_point(binding)
        contact.x, contact.y = x, y
        if timestamp is not None:
            down = self._emit(Action.DOWN, pointer, x, y, timestamp, binding.element.id)
            self._flush([down], timestamp)
        return contact

    def _element_point(self, binding: _Binding) -> tuple[float, float]:
        element = binding.element
        return self._point_from_profile(element.x, element.y)

    def _element_point_in_build_space(self, element: Element) -> tuple[float, float]:
        width = self._geometry.buffer.width
        height = self._geometry.buffer.height
        return (element.x * width, element.y * height)

    def _point_from_profile(self, u: float, v: float) -> tuple[float, float]:
        width = self._geometry.buffer.width
        height = self._geometry.buffer.height
        x = min(max(u * width, 0.0), float(max(width - 1, 0)))
        y = min(max(v * height, 0.0), float(max(height - 1, 0)))
        return (x, y)

    def _mouse_point(self, binding: _Binding) -> tuple[float, float]:
        width = self._geometry.buffer.width
        height = self._geometry.buffer.height
        return (
            min(max(binding.mouse_x, 0.0), float(max(width - 1, 0))),
            min(max(binding.mouse_y, 0.0), float(max(height - 1, 0))),
        )

    def mouse_delta(self, dx: float, dy: float, *, now_ms: int | None = None) -> list[TouchEvent]:
        """Relative pointer motion for aim mode (only in a mouse element)."""
        mouse_binding = next(
            (b for b in self._bindings.values() if b.element.type is ElementType.MOUSE), None
        )
        if mouse_binding is None or not mouse_binding.mouse_active:
            return []
        timestamp = self._now(now_ms)
        sensitivity = mouse_binding.element.sensitivity
        mouse_binding.mouse_x += dx * sensitivity
        mouse_binding.mouse_y += dy * sensitivity
        event_list = self._move(mouse_binding, timestamp)
        self._flush(event_list, timestamp)
        return event_list

    def press_state(self) -> dict[str, set[str]]:
        """Currently held keys per element, for the GUI highlight overlay."""
        return {
            element_id: set(binding.held_keys)
            for element_id, binding in self._bindings.items()
            if binding.held_keys
        }

    def active_element_ids(self) -> set[str]:
        return {c.element_id for c in self._active_pointers.values()}


def keys_of(profile: Profile) -> set[str]:
    """Every key the profile binds (used to build the capture filter)."""
    keys: set[str] = set()
    for element in profile.elements:
        keys.update(InputEngine._element_keys(element))
    return keys


def element_iter(profile: Profile) -> Iterable[Element]:
    return iter(profile.elements)
