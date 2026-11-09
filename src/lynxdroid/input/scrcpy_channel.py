"""Experimental scrcpy touch transport for an already connected control stream.

No discovery, server startup, ADB, or socket connection is performed here.
The caller owns protocol version verification and connection setup. This is not
wired into the launcher, and has not been tested against a real device.
"""

from __future__ import annotations

import socket
from dataclasses import replace
from threading import Lock

from lynxdroid.input.channels import Capabilities, Channel, ChannelKind
from lynxdroid.input.events import Action, InputError, TouchEvent
from lynxdroid.input.scrcpy_touch import encode_touch

MAX_POINTERS = 10


class ScrcpyChannel(Channel):
    """Write ordered touch packets to a caller-supplied scrcpy control socket.

    Socket writes are not acknowledgements of Android injection. After a
    failed write the stream is closed and remote pointer state is unknown.
    Calls are serialized; a bounded socket timeout prevents indefinite waits.
    """

    def __init__(self, stream: socket.socket, width: int, height: int) -> None:
        if not 1 <= width <= 65535 or not 1 <= height <= 65535:
            raise ValueError("invalid scrcpy display size")
        self._stream = stream
        self._stream.settimeout(1.0)
        self._width = width
        self._height = height
        self._active: dict[int, TouchEvent] = {}
        self._closed = False
        self._lock = Lock()

    def capabilities(self) -> Capabilities:
        return Capabilities(
            ChannelKind.SCRCPY,
            False,
            0,
            False,
            False,
            reason="experimental stream; protocol and Android injection are not verified",
        )

    def send(self, events: list[TouchEvent]) -> None:
        with self._lock:
            if self._closed:
                raise InputError("scrcpy stream is closed", code="channel-closed")
            active = dict(self._active)
            packets: list[bytes] = []
            try:
                for event in events:
                    if event.action is Action.DOWN:
                        if event.pointer in active or len(active) >= MAX_POINTERS:
                            raise ValueError("duplicate pointer or too many active pointers")
                        active[event.pointer] = event
                    elif event.action in (Action.MOVE, Action.UP, Action.CANCEL):
                        if event.pointer not in active:
                            raise ValueError("pointer is not active")
                        if event.action is Action.MOVE:
                            active[event.pointer] = event
                        else:
                            del active[event.pointer]
                    else:
                        raise ValueError("unsupported touch action")
                    # scrcpy's server removes finger state only on ACTION_UP,
                    # so translate each per-pointer cancel into an UP packet.
                    wire_event = (
                        replace(event, action=Action.UP) if event.action is Action.CANCEL else event
                    )
                    packets.append(encode_touch(wire_event, self._width, self._height))
            except (ValueError, OverflowError) as exc:
                raise InputError(
                    f"invalid scrcpy touch batch: {exc}", code="invalid-touch"
                ) from exc
            if not packets:
                return
            try:
                self._stream.sendall(b"".join(packets))
            except OSError as exc:
                self._closed = True
                self._active.clear()
                self._stream.close()
                raise InputError(
                    "scrcpy write failed; remote touch state is unknown", code="channel-failure"
                ) from exc
            self._active = active

    def close(self) -> None:
        with self._lock:
            if self._closed:
                return
            try:
                # Best effort only: sendall does not prove the device received it.
                packets = [
                    encode_touch(replace(event, action=Action.UP), self._width, self._height)
                    for event in self._active.values()
                ]
                if packets:
                    self._stream.sendall(b"".join(packets))
            except OSError:
                pass
            finally:
                self._active.clear()
                self._closed = True
                self._stream.close()
