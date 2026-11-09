"""Offline stream tests, with no Android device or external service."""

from __future__ import annotations

import socket
import struct

import pytest

from lynxdroid.input.draft_mapping import draft_engine
from lynxdroid.input.events import Action, InputError, TouchEvent
from lynxdroid.input.pipeline import InputPipeline
from lynxdroid.input.scrcpy_channel import ScrcpyChannel


def touch(action: Action, pointer: int = 1, x: float = 200, y: float = 100) -> TouchEvent:
    return TouchEvent(action, pointer, x, y, 0)


def decode(data: bytes) -> tuple[int, ...]:
    return struct.unpack(">BBQIIHHHII", data)


def test_engine_to_connected_stream_and_release() -> None:
    left, right = socket.socketpair()
    try:
        channel = ScrcpyChannel(left, 1920, 1080)
        pipeline = InputPipeline()
        assert not pipeline.attach(channel).available
        engine = draft_engine([{"kind": "Огонь", "key": "F", "x": 0.25, "y": 0.75}])
        assert pipeline.deliver(engine.key_down("f"))
        assert pipeline.deliver(engine.key_up("f"))
        raw = right.recv(64, socket.MSG_WAITALL)
        assert len(raw) == 64
        assert decode(raw[:32])[1:5] == (0, 1, 480, 810)
        assert decode(raw[32:])[1:5] == (1, 1, 480, 810)
        pipeline.close()
    finally:
        left.close()
        right.close()


def test_cancel_is_sent_as_up_and_close_releases_remaining_pointer() -> None:
    left, right = socket.socketpair()
    try:
        channel = ScrcpyChannel(left, 800, 600)
        channel.send([touch(Action.DOWN), touch(Action.DOWN, 2), touch(Action.CANCEL)])
        channel.close()
        data = right.recv(128, socket.MSG_WAITALL)
        assert [decode(data[i : i + 32])[1:3] for i in range(0, 128, 32)] == [
            (0, 1),
            (0, 2),
            (1, 1),
            (1, 2),
        ]
        channel.close()
        with pytest.raises(InputError):
            channel.send([touch(Action.UP, 2)])
    finally:
        left.close()
        right.close()


def test_invalid_batch_is_not_partially_written() -> None:
    left, right = socket.socketpair()
    right.settimeout(0.05)
    try:
        channel = ScrcpyChannel(left, 800, 600)
        with pytest.raises(InputError):
            channel.send([touch(Action.DOWN), touch(Action.MOVE, 2)])
        with pytest.raises(TimeoutError):
            right.recv(1)
        channel.send([touch(Action.DOWN), touch(Action.UP)])
        assert len(right.recv(64, socket.MSG_WAITALL)) == 64
        channel.close()
    finally:
        left.close()
        right.close()


def test_disconnected_stream_fails_closed() -> None:
    left, right = socket.socketpair()
    right.close()
    channel = ScrcpyChannel(left, 800, 600)
    with pytest.raises(InputError, match="remote touch state is unknown"):
        channel.send([touch(Action.DOWN)])
    with pytest.raises(InputError, match="closed"):
        channel.send([touch(Action.DOWN)])
    channel.close()


def test_rejects_invalid_size() -> None:
    left, right = socket.socketpair()
    try:
        with pytest.raises(ValueError):
            ScrcpyChannel(left, 0, 600)
    finally:
        left.close()
        right.close()


def test_focus_loss_releases_at_last_position() -> None:
    engine = draft_engine([{"kind": "Огонь", "key": "F", "x": 0.25, "y": 0.75}])
    engine.key_down("f")
    cancelled = engine.release_all("focus lost")
    assert len(cancelled) == 1
    assert cancelled[0].action is Action.CANCEL
    assert (cancelled[0].x, cancelled[0].y) == (480, 810)
