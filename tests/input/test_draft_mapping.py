"""Editor drafts, existing mapping engine and scrcpy wire format (offline)."""

from __future__ import annotations

import struct

import pytest

from lynxdroid.input.draft_mapping import draft_engine
from lynxdroid.input.events import Action, TouchEvent
from lynxdroid.input.scrcpy_touch import encode_touch


def draft(kind: str = "Огонь", key: str = "F") -> dict[str, object]:
    return {"kind": kind, "key": key, "x": 0.25, "y": 0.75}


def test_fire_is_held_until_release() -> None:
    engine = draft_engine([draft()])
    down = engine.key_down("f")
    assert [e.action for e in down] == [Action.DOWN]
    assert (down[0].x, down[0].y) == (480, 810)
    assert [e.action for e in engine.key_up("f")] == [Action.UP]
    assert not engine.active_pointers


def test_tap_is_a_down_up_pair() -> None:
    engine = draft_engine([draft("Нажатие")])
    assert [e.action for e in engine.key_down("f")] == [Action.DOWN, Action.UP]
    assert engine.key_up("f") == []


def test_unsupported_kinds_and_keys_are_not_silently_simulated() -> None:
    with pytest.raises(ValueError):
        draft_engine([draft("Камера")])
    with pytest.raises(ValueError):
        draft_engine([draft(key="Mouse1")])
    with pytest.raises(ValueError):
        draft_engine([draft(), draft("Нажатие", "f")])


def test_scrcpy_wire_message() -> None:
    event = TouchEvent(Action.DOWN, 7, 480, 810, 1)
    wire = encode_touch(event, 1920, 1080)
    assert len(wire) == 32
    assert struct.unpack(">BBQIIHHHII", wire) == (2, 0, 7, 480, 810, 1920, 1080, 65535, 0, 0)
    assert (
        struct.unpack(
            ">BBQIIHHHII", encode_touch(TouchEvent(Action.UP, 7, 480, 810, 2), 1920, 1080)
        )[7]
        == 0
    )


def test_scrcpy_rejects_out_of_bounds_and_invalid_size() -> None:
    event = TouchEvent(Action.DOWN, 1, 1920, 0, 1)
    with pytest.raises(ValueError):
        encode_touch(event, 1920, 1080)
    with pytest.raises(ValueError):
        encode_touch(event, 0, 1080)


def test_profile_path_is_stable_and_distinguishes_same_names(tmp_path) -> None:
    from pathlib import Path

    from lynxdroid.gui.control_profiles import control_profile_path

    a = tmp_path / "a" / "Game.apk"
    b = tmp_path / "b" / "Game.apk"
    path = control_profile_path(tmp_path, a)
    assert path == control_profile_path(tmp_path, a)
    assert path != control_profile_path(tmp_path, b)
    assert path.parent == tmp_path / "control-profiles"
    with pytest.raises(ValueError):
        control_profile_path(tmp_path, Path("Game.apk"))
