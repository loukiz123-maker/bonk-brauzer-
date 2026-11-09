"""Serialize scrcpy control touch messages; no socket or runtime is started.

Adapted from scrcpy app/src/control_msg.c and app/src/util/binary.h,
revision a60891aea193d92e7e5c3942700eca63f9d19a5f.
Copyright (C) 2018 Genymobile; Copyright (C) 2018-2026 Romain Vimont.
Licensed under Apache-2.0; see licenses/scrcpy-APACHE-2.0.txt.
This is a Python adaptation, not the original C implementation.
"""

from __future__ import annotations

import math
import struct

from lynxdroid.input.events import Action, TouchEvent

_ACTIONS = {Action.DOWN: 0, Action.UP: 1, Action.MOVE: 2, Action.CANCEL: 3}


def encode_touch(event: TouchEvent, width: int, height: int) -> bytes:
    """Return a v5 scrcpy 32-byte touch message, or reject unsafe coordinates.

    This is a codec only: callers must separately establish and verify a
    compatible scrcpy control connection. Do not send it to arbitrary sockets.
    """
    if not (1 <= width <= 65535 and 1 <= height <= 65535):
        raise ValueError("screen size must fit nonzero unsigned 16-bit integers")
    if not (0 <= event.pointer < 2**64):
        raise ValueError("pointer id out of range")
    if not (math.isfinite(event.x) and math.isfinite(event.y)):
        raise ValueError("touch coordinates must be finite")
    if not (0 <= event.x < width and 0 <= event.y < height):
        raise ValueError("touch lies outside the screen")
    pressure = 0 if event.action in (Action.UP, Action.CANCEL) else 0xFFFF
    # type, action, pointer ID, x/y, display size, pressure, action button, buttons
    return struct.pack(
        ">BBQIIHHHII",
        2,
        _ACTIONS[event.action],
        event.pointer,
        int(event.x),
        int(event.y),
        width,
        height,
        pressure,
        0,
        0,
    )
