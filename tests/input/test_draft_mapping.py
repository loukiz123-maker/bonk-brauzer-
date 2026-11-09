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


def test_draft_joystick_uses_four_direction_keys_and_releases() -> None:
    engine = draft_engine([draft("Джойстик", "W,D,S,A")])
    events = engine.key_down("w")
    assert [e.action for e in events] == [Action.DOWN, Action.MOVE]
    diagonal = engine.key_down("d")
    assert [e.action for e in diagonal] == [Action.MOVE]
    assert diagonal[0].x > 480 and diagonal[0].y < 810
    assert [e.action for e in engine.key_up("w")] == [Action.MOVE]
    assert [e.action for e in engine.key_up("d")] == [Action.UP]
    assert not engine.active_pointers


def test_draft_joystick_rejects_invalid_and_overlapping_keys() -> None:
    with pytest.raises(ValueError):
        draft_engine([draft("Джойстик", "W,W,S,A")])
    with pytest.raises(ValueError):
        draft_engine([draft("Джойстик", "W,A,S")])
    with pytest.raises(ValueError):
        draft_engine([draft("Джойстик", "W,D,S,A"), draft("Огонь", "W")])


def test_draft_joystick_cannot_extend_beyond_edge() -> None:
    data = draft("Джойстик", "W,D,S,A")
    data["x"] = 0.01
    with pytest.raises(ValueError):
        draft_engine([data])


def test_draft_joystick_opposite_keys_return_to_center() -> None:
    engine = draft_engine([draft("Джойстик", "W,D,S,A")])
    engine.key_down("w")
    event = engine.key_down("s")[0]
    assert (event.x, event.y) == (480, 810)
    assert len(engine.release_all("focus lost")) == 1
    assert not engine.active_pointers
