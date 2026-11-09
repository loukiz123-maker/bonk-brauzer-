"""Translate supported editor drafts into the existing input engine, offline."""

from __future__ import annotations

import re
from typing import Any

from lynxdroid.display.geometry import Size, ViewportGeometry
from lynxdroid.input.engine import InputEngine
from lynxdroid.input.events import TouchEvent
from lynxdroid.profiles.model import Element, ElementType, Profile, Target

DOUBLE_TAP_INTERVAL_MS = 120


class DraftInputEngine(InputEngine):
    """Offline draft mapper with a timed second tap, never an input channel.

    ``tick`` must be called by the UI. Repeats while a pair is pending are
    coalesced; focus loss or profile changes cancel the pending second tap.
    """

    def __init__(self, profile: Profile, geometry: ViewportGeometry) -> None:
        super().__init__(profile, geometry)
        self._double_keys = self._profile_double_keys(profile)
        self._double_held: set[str] = set()
        self._double_pending: dict[str, int] = {}

    @property
    def has_pending_double_taps(self) -> bool:
        return bool(self._double_pending)

    @staticmethod
    def _profile_double_keys(profile: Profile) -> set[str]:
        return {
            key
            for element in profile.elements
            if element.type is ElementType.TAP and element.label == "Двойное нажатие"
            for key in element.keys
        }

    def key_down(
        self, key: str, *, now_ms: int | None = None, repeat: bool = False
    ) -> list[TouchEvent]:
        if key not in self._double_keys:
            return super().key_down(key, now_ms=now_ms, repeat=repeat)
        if repeat or key in self._double_held:
            return []
        if key in self._double_pending:
            self._double_held.add(key)
            return []
        timestamp = self._now(now_ms)
        events = super().key_down(key, now_ms=timestamp)
        self._double_held.add(key)
        if events:
            self._double_pending[key] = timestamp + DOUBLE_TAP_INTERVAL_MS
        return events

    def key_up(self, key: str, *, now_ms: int | None = None) -> list[TouchEvent]:
        self._double_held.discard(key)
        return super().key_up(key, now_ms=now_ms)

    def tick(self, *, now_ms: int | None = None) -> list[TouchEvent]:
        timestamp = self._now(now_ms)
        events = super().tick(now_ms=timestamp)
        for key, due in list(self._double_pending.items()):
            if timestamp >= due:
                del self._double_pending[key]
                events.extend(super().key_down(key, now_ms=timestamp))
        return events

    def release_all(self, reason: str = "released", *, cancel: bool = True) -> list[TouchEvent]:
        self._double_held.clear()
        self._double_pending.clear()
        return super().release_all(reason, cancel=cancel)

    def set_profile(self, profile: Profile) -> list[TouchEvent]:
        events = super().set_profile(profile)
        self._double_keys = self._profile_double_keys(profile)
        return events


def draft_engine(
    controls: list[dict[str, Any]], width: int = 1920, height: int = 1080
) -> DraftInputEngine:
    """Build a local test engine for taps, double taps, fire and d-pad."""
    if not 1 <= width <= 65535 or not 1 <= height <= 65535:
        raise ValueError("invalid test display size")
    elements = []
    for i, control in enumerate(controls):
        kind, key = control.get("kind"), control.get("key")
        if kind not in ("Нажатие", "Двойное нажатие", "Огонь", "Джойстик"):
            raise ValueError(f"Тип «{kind}» пока не поддерживает проверку ввода")
        if not isinstance(key, str):
            raise ValueError("Укажите клавиши")
        dpad_keys = ("", "", "", "")
        if kind == "Джойстик":
            parts = tuple(part.strip().lower() for part in key.split(","))
            if len(parts) != 4 or any(not re.fullmatch(r"[a-z0-9]", part) for part in parts):
                raise ValueError("Джойстик: 4 клавиши через запятую, вверх, вправо, вниз, влево")
            if len(set(parts)) != 4:
                raise ValueError("Клавиши джойстика должны отличаться")
            dpad_keys = (parts[0], parts[1], parts[2], parts[3])
        else:
            if not re.fullmatch(r"[A-Za-z0-9]", key) and key.lower() not in ("mouse1", "mouse2"):
                raise ValueError("Используйте латинскую букву, цифру, Mouse1 или Mouse2")
        x, y = control.get("x"), control.get("y")
        if (
            not isinstance(x, (int, float))
            or isinstance(x, bool)
            or not isinstance(y, (int, float))
            or isinstance(y, bool)
        ):
            raise ValueError("Некорректные координаты")
        if kind == "Джойстик":
            element = Element(
                id=f"draft-{i}",
                type=ElementType.DPAD,
                label=kind,
                x=x,
                y=y,
                dpad_keys=dpad_keys,
            )
        else:
            element = Element(
                id=f"draft-{i}",
                type=ElementType.HOLD if kind == "Огонь" else ElementType.TAP,
                label=kind,
                x=x,
                y=y,
                keys=(key.lower(),),
            )
        elements.append(element)
    profile = Profile.new("Черновик управления", Target(package="local.draft"), elements=elements)
    return DraftInputEngine(
        profile, ViewportGeometry(buffer=Size(width, height), surface=Size(width, height))
    )
