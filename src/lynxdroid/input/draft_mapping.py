"""Translate supported editor drafts into the existing input engine, offline."""

from __future__ import annotations

import re
from typing import Any

from lynxdroid.display.geometry import Size, ViewportGeometry
from lynxdroid.input.engine import InputEngine
from lynxdroid.profiles.model import Element, ElementType, Profile, Target


def draft_engine(
    controls: list[dict[str, Any]], width: int = 1920, height: int = 1080
) -> InputEngine:
    """Build a local test engine for taps, fire and d-pad, never an input channel."""
    if not 1 <= width <= 65535 or not 1 <= height <= 65535:
        raise ValueError("invalid test display size")
    elements = []
    for i, control in enumerate(controls):
        kind, key = control.get("kind"), control.get("key")
        if kind not in ("Нажатие", "Огонь", "Джойстик"):
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
            if not re.fullmatch(r"[A-Za-z0-9]", key):
                raise ValueError("Для проверки используйте одну латинскую букву или цифру")
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
                type=ElementType.TAP if kind == "Нажатие" else ElementType.HOLD,
                label=kind,
                x=x,
                y=y,
                keys=(key.lower(),),
            )
        elements.append(element)
    profile = Profile.new("Черновик управления", Target(package="local.draft"), elements=elements)
    return InputEngine(
        profile, ViewportGeometry(buffer=Size(width, height), surface=Size(width, height))
    )
