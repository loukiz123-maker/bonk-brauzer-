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
        draft_engine([draft("Несуществующий тип")])
    with pytest.raises(ValueError):
        draft_engine([draft(key="Mouse3")])
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


def test_mouse_fire_and_tap_are_mapped_without_a_channel() -> None:
    engine = draft_engine([draft("Огонь", "Mouse1"), draft("Нажатие", "Mouse2")])
    assert [event.action for event in engine.key_down("mouse1")] == [Action.DOWN]
    assert [event.action for event in engine.key_down("mouse2")] == [Action.DOWN, Action.UP]
    assert len(engine.active_pointers) == 1
    assert [event.action for event in engine.key_up("mouse1")] == [Action.UP]
    assert not engine.active_pointers
    with pytest.raises(ValueError):
        draft_engine([draft("Огонь", "mouse1"), draft("Нажатие", "MOUSE1")])


def test_double_tap_is_timed_and_not_repeated_while_held() -> None:
    from lynxdroid.input.draft_mapping import DOUBLE_TAP_INTERVAL_MS

    engine = draft_engine([draft("Двойное нажатие", "F")])
    first = engine.key_down("f", now_ms=100)
    assert [event.action for event in first] == [Action.DOWN, Action.UP]
    assert engine.key_down("f", now_ms=105, repeat=True) == []
    assert engine.key_down("f", now_ms=106) == []
    assert engine.key_up("f", now_ms=107) == []
    assert engine.tick(now_ms=100 + DOUBLE_TAP_INTERVAL_MS - 1) == []
    second = engine.tick(now_ms=100 + DOUBLE_TAP_INTERVAL_MS)
    assert [event.action for event in second] == [Action.DOWN, Action.UP]
    assert all(event.timestamp_ms == 100 + DOUBLE_TAP_INTERVAL_MS for event in second)
    assert [event.seq for event in first + second] == sorted(event.seq for event in first + second)
    assert all(len(encode_touch(event, 1920, 1080)) == 32 for event in first + second)
    assert not engine.active_pointers and not engine.has_pending_double_taps


def test_double_tap_pending_is_cancelled_on_focus_loss() -> None:
    engine = draft_engine([draft("Двойное нажатие", "F"), draft("Огонь", "G")])
    engine.key_down("f", now_ms=100)
    engine.key_down("g", now_ms=101)
    assert engine.has_pending_double_taps
    released = engine.release_all("focus lost")
    assert [event.action for event in released] == [Action.CANCEL]
    assert not engine.active_pointers and not engine.has_pending_double_taps
    assert engine.tick(now_ms=500) == []
    assert [event.action for event in engine.key_down("f", now_ms=600)] == [Action.DOWN, Action.UP]


def test_double_tap_coalesces_another_press_while_pending() -> None:
    engine = draft_engine([draft("Двойное нажатие", "Mouse1")])
    engine.key_down("mouse1", now_ms=0)
    engine.key_up("mouse1", now_ms=1)
    assert engine.key_down("mouse1", now_ms=2) == []
    assert len(engine.tick(now_ms=120)) == 2
    assert engine.key_down("mouse1", now_ms=121) == []  # still physically held
    engine.key_up("mouse1", now_ms=122)
    assert len(engine.key_down("mouse1", now_ms=123)) == 2


def test_multitouch_independent_contacts_and_cleanup() -> None:
    first = draft("Мультитач", "F")
    second = {**draft("Мультитач", "G"), "x": 0.75}
    engine = draft_engine([first, second])
    one = engine.key_down("f")[0]
    two = engine.key_down("g")[0]
    assert one.action is Action.DOWN and two.action is Action.DOWN
    assert one.pointer != two.pointer
    assert len(engine.active_pointers) == 2
    up = engine.key_up("f")[0]
    assert up.action is Action.UP and up.pointer == one.pointer
    assert len(engine.active_pointers) == 1
    cancelled = engine.release_all("focus lost")
    assert len(cancelled) == 1 and cancelled[0].pointer == two.pointer
    assert not engine.active_pointers
    assert engine.key_up("g") == []


def test_camera_toggle_and_drag_follow_motion_independently() -> None:
    engine = draft_engine([draft("Камера", "C"), draft("Перетаскивание", "D")])
    assert [e.action for e in engine.key_down("c")] == [Action.DOWN, Action.MOVE]
    engine.key_up("c")
    assert [e.action for e in engine.key_down("d")] == [Action.DOWN, Action.MOVE]
    moved = engine.mouse_delta(100, -50)
    assert len(moved) == 2 and all(e.action is Action.MOVE for e in moved)
    assert all((e.x, e.y) == (580, 760) for e in moved)
    assert [e.action for e in engine.key_up("d")] == [Action.UP]
    assert len(engine.active_pointers) == 1
    assert [e.action for e in engine.key_down("c")] == [Action.UP]
    engine.key_up("c")
    assert not engine.active_pointers


def test_motion_clamping_repeats_and_focus_cancel() -> None:
    engine = draft_engine([draft("Камера", "C")])
    engine.key_down("c")
    assert engine.key_down("c") == []
    event = engine.mouse_delta(10000, -10000)[0]
    assert (event.x, event.y) == (1919, 0)
    assert len(engine.release_all("blur")) == 1
    assert not engine.active_pointers
    assert engine.mouse_delta(1, 1) == []
    assert engine.key_down("c")[0].action is Action.DOWN
