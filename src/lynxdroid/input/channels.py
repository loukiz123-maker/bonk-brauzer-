"""Touch injection channels (stage 8, "path A").

The specification refuses to let us pretend sequential ``adb shell input tap``
calls are multitouch. So this module defines a small capability contract and
implements only what can actually be verified:

``Channel``            one way of getting TouchEvents into Android
``AdbShellChannel``    sequential taps only -> NOT multitouch, marked as such
``AgentChannel``       our own Android-side agent over a loopback socket
``NullChannel``        sink used in tests and on machines without a backend

``Capabilities`` is the honest answer to "what will actually work on this
host": it lists simultaneous touch support, relative pointer motion, and the
reason when something is unavailable. The GUI shows these values instead of
disable/enable guesses.
"""

from __future__ import annotations

import abc
import contextlib
import json
import socket
import time
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path

from lynxdroid.core import proc
from lynxdroid.input.events import Action, InputError, TouchEvent
from lynxdroid.version import SCHEMA_VERSIONS

AGENT_PROTOCOL = SCHEMA_VERSIONS["agent-protocol"]
MAX_AGENT_MESSAGE = 64 * 1024


class ChannelKind(str, Enum):
    NULL = "null"
    ADB_SHELL = "adb-shell"
    AGENT = "agent"


@dataclass(frozen=True, slots=True)
class Capabilities:
    """What a channel can honestly do on this host."""

    kind: ChannelKind
    available: bool
    simultaneous_touches: int
    relative_motion: bool
    latency_measured: bool
    reason: str = ""
    #: True when the channel needs an explicitly trusted injection privilege.
    needs_privilege: bool = False

    @property
    def real_multitouch(self) -> bool:
        return self.available and self.simultaneous_touches > 1

    def describe(self) -> str:
        if not self.available:
            return f"{self.kind.value}: unavailable - {self.reason}"
        return (
            f"{self.kind.value}: up to {self.simultaneous_touches} simultaneous touch"
            f"{'es' if self.simultaneous_touches != 1 else ''}"
            f"{', relative motion' if self.relative_motion else ', no relative motion'}"
        )

    def as_dict(self) -> dict[str, object]:
        return {
            "kind": self.kind.value,
            "available": self.available,
            "simultaneous_touches": self.simultaneous_touches,
            "relative_motion": self.relative_motion,
            "real_multitouch": self.real_multitouch,
            "reason": self.reason,
            "needs_privilege": self.needs_privilege,
        }


class Channel(abc.ABC):
    """A sink for touch events."""

    @abc.abstractmethod
    def capabilities(self) -> Capabilities: ...

    @abc.abstractmethod
    def send(self, events: list[TouchEvent]) -> None:
        """Deliver events in order. Must raise ``InputError`` on failure."""

    def close(self) -> None:  # noqa: B027 - optional hook, not abstract
        """Release sockets/processes. Idempotent; the default does nothing."""


# --------------------------------------------------------------------------
# Null sink
# --------------------------------------------------------------------------


@dataclass
class NullChannel(Channel):
    """Accepts everything and records it. Used by tests and dry runs.

    It reports ``available=False`` on purpose. The events it "sends" go into a
    list and reach no Android process, so claiming availability would enable
    the key overlay on a machine where nothing can ever be delivered. Tests
    that need to observe delivery read ``sent``; they do not need the
    capability to lie.
    """

    sent: list[TouchEvent] = field(default_factory=list)
    fail_with: str | None = None

    def capabilities(self) -> Capabilities:
        return Capabilities(
            kind=ChannelKind.NULL,
            available=False,
            simultaneous_touches=1,
            relative_motion=False,
            latency_measured=False,
            reason=(
                "no touch channel is attached: events are recorded locally and never reach Android"
            ),
        )

    def send(self, events: list[TouchEvent]) -> None:
        if self.fail_with:
            raise InputError(self.fail_with, code="channel-failure")
        self.sent.extend(events)


# --------------------------------------------------------------------------
# adb shell (sequential, honest about being useless for multitouch)
# --------------------------------------------------------------------------


@dataclass
class AdbShellChannel(Channel):
    """``adb shell input`` - one event at a time, therefore *not* multitouch.

    It is kept because it is the only path that works on an unmodified image
    without extra privileges, and it is genuinely useful for automation tests
    of a single tap. The capabilities object says exactly what it cannot do.
    """

    adb: str | None = None
    serial: str | None = None
    dry_run: bool = False
    _sends: int = 0

    def _binary(self) -> str | None:
        return self.adb or proc.which("adb")

    def _base(self) -> list[str]:
        binary = self._binary()
        if binary is None:
            raise InputError("adb is not installed", code="adb-missing")
        argv = [binary]
        if self.serial:
            argv += ["-s", self.serial]
        return argv

    def capabilities(self) -> Capabilities:
        binary = self._binary()
        if binary is None:
            return Capabilities(
                ChannelKind.ADB_SHELL, False, 0, False, False, reason="adb binary not found"
            )
        return Capabilities(
            kind=ChannelKind.ADB_SHELL,
            available=True,
            simultaneous_touches=1,
            relative_motion=False,
            latency_measured=False,
            reason="sequential input taps: a single contact at a time, not simultaneous multitouch",
        )

    def send(self, events: list[TouchEvent]) -> None:
        # Only complete taps are meaningful here: down+up on the same pointer.
        pending_down: dict[int, TouchEvent] = {}
        for event in events:
            if event.action is Action.DOWN:
                pending_down[event.pointer] = event
            elif event.action is Action.UP and event.pointer in pending_down:
                down = pending_down.pop(event.pointer)
                self._tap(down.x, down.y)
            elif event.action is Action.CANCEL:
                pending_down.pop(event.pointer, None)
        if pending_down:
            raise InputError(
                "adb shell channel cannot hold a touch: only complete taps are supported",
                code="not-multitouch",
            )

    def _tap(self, x: float, y: float) -> None:
        argv = [*self._base(), "shell", "input", "tap", str(int(x)), str(int(y))]
        self._sends += 1
        if self.dry_run:
            return
        result = proc.run(argv, timeout=8.0)
        if not result.ok:
            raise InputError(f"adb input tap failed: {result.output.strip()}", code="adb-failed")


# --------------------------------------------------------------------------
# Agent channel: our protocol, over loopback only
# --------------------------------------------------------------------------


class AgentChannel(Channel):
    """Length-prefixed JSON over a **unix socket**, never a TCP port.

    Why unix socket: stage 14 forbids debug ports reachable from other
    machines, and requires honest disclosure that other local processes can
    reach a loopback socket anyway. A unix socket in ``$XDG_RUNTIME_DIR`` with
    0600 permissions at least keeps other *users* out and makes the boundary
    visible.

    The Android-side agent (``android_agent/``) is what turns these messages
    into real `MotionEvent`s; without it the channel reports ``available=False``
    rather than pretending touches work.
    """

    def __init__(
        self,
        socket_path: str,
        *,
        connect_timeout: float = 1.5,
        dry_run: bool = False,
    ) -> None:
        self._socket_path = socket_path
        self._connect_timeout = connect_timeout
        self._dry_run = dry_run
        self._sock: socket.socket | None = None
        self._hello: dict[str, object] | None = None
        self._closed = False

    # -- connection ------------------------------------------------------
    def connect(self) -> dict[str, object]:
        """Handshake. Returns the agent's declared capabilities.

        Refuses to talk to an agent that does not speak the same protocol
        version - a mismatched agent silently dropping touches is worse than
        a clear error.
        """
        if self._dry_run:
            self._hello = {
                "protocol": AGENT_PROTOCOL,
                "max_touches": 10,
                "relative_motion": True,
                "dry_run": True,
            }
            return self._hello
        if self._sock is not None:
            return self._hello or {}
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(self._connect_timeout)
        try:
            sock.connect(self._socket_path)
            sock.sendall(
                _encode({"op": "hello", "protocol": AGENT_PROTOCOL, "client": "lynxdroid"})
            )
            reply = _decode(_read_frame(sock))
        except OSError as exc:
            sock.close()
            raise InputError(
                f"cannot reach touch agent: {exc.strerror or exc}", code="agent-unreachable"
            ) from exc
        except ValueError as exc:
            sock.close()
            raise InputError(f"touch agent handshake failed: {exc}", code="agent-protocol") from exc
        # The agent answers a refusal two ways: a bare ok=false (what the
        # shipped Java does) or a protocol field naming its version. Both mean
        # the same thing, and reporting "speaks protocol None" for the first
        # would tell the user nothing.
        if reply.get("ok") is False or reply.get("protocol") != AGENT_PROTOCOL:
            sock.close()
            # Three shapes exist in the wild and they must not be conflated:
            #   {protocol: N}            the agent's own version is N
            #   {ok: false, supported:N} the agent wants N and refused ours
            #   {ok: false}              the agent declined without detail
            # Printing "speaks protocol 1, this build speaks 1" (both sides
            # naming themselves) is what happened when these were merged.
            theirs = reply.get("protocol")
            supported = reply.get("supported")
            reason = reply.get("error")
            if theirs is not None and theirs != AGENT_PROTOCOL:
                detail = f"speaks protocol {theirs}"
            elif supported is not None:
                detail = f"wants protocol {supported}, this build sends {AGENT_PROTOCOL}"
            elif reason:
                detail = f"refused the handshake ({reason})"
            else:
                detail = "refused the handshake"
            message = f"touch agent {detail}"
            if "this build" not in detail:
                message += f"; this build speaks {AGENT_PROTOCOL}"
            raise InputError(message, code="agent-protocol-mismatch")
        if not reply.get("max_touches"):
            sock.close()
            raise InputError("touch agent reported no touch slots", code="agent-no-slots")
        self._sock = sock
        self._hello = reply
        return reply

    def capabilities(self) -> Capabilities:
        if self._closed:
            return Capabilities(ChannelKind.AGENT, False, 0, False, False, reason="channel closed")
        if self._hello is None:
            return Capabilities(
                ChannelKind.AGENT,
                False,
                0,
                False,
                False,
                reason="agent not connected yet",
                needs_privilege=True,
            )
        return Capabilities(
            kind=ChannelKind.AGENT,
            available=True,
            simultaneous_touches=_as_int(self._hello.get("max_touches")),
            relative_motion=bool(self._hello.get("relative_motion", False)),
            latency_measured=bool(self._hello.get("latency_supported", False)),
            needs_privilege=True,
        )

    def send(self, events: list[TouchEvent]) -> None:
        if self._closed:
            raise InputError("touch channel is closed", code="channel-closed")
        if not events:
            return
        if self._sock is None:
            self.connect()
        if self._sock is None:  # pragma: no cover - connect() raises instead
            raise InputError("touch channel is not connected", code="channel-closed")
        payload = {
            "op": "touch",
            "protocol": AGENT_PROTOCOL,
            "events": [event.as_dict() for event in events],
        }
        try:
            self._sock.sendall(_encode(payload))
            ack = _decode(_read_frame(self._sock))
        except OSError as exc:
            self.close()
            raise InputError(
                f"touch agent connection lost: {exc.strerror or exc}", code="agent-lost"
            ) from exc
        if ack.get("ok") is not True:
            raise InputError(
                f"touch agent rejected events: {ack.get('error', 'unknown')}", code="agent-rejected"
            )

    def close(self) -> None:
        self._closed = True
        if self._sock is not None:
            with contextlib.suppress(OSError):
                self._sock.sendall(_encode({"op": "bye"}))
            self._sock.close()
            self._sock = None
        self._hello = None

    @property
    def socket_path(self) -> str:
        return self._socket_path


def _as_int(value: object) -> int:
    """Best-effort int from agent-declared capabilities (0 when unusable)."""
    if isinstance(value, bool):
        return 0
    if isinstance(value, int):
        return max(0, value)
    if isinstance(value, float):
        return max(0, int(value))
    if isinstance(value, str):
        try:
            return max(0, int(value, 10))
        except ValueError:
            return 0
    return 0


def _encode(payload: dict[str, object]) -> bytes:
    body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    if len(body) > MAX_AGENT_MESSAGE:
        raise InputError("agent message too large", code="agent-message-too-large")
    return len(body).to_bytes(4, "big") + body


def _read_frame(sock: socket.socket) -> bytes:
    header = _recv_exact(sock, 4)
    length = int.from_bytes(header, "big")
    if length <= 0 or length > MAX_AGENT_MESSAGE:
        raise ValueError(f"invalid frame length {length}")
    return _recv_exact(sock, length)


def _recv_exact(sock: socket.socket, count: int) -> bytes:
    chunks: list[bytes] = []
    remaining = count
    while remaining:
        chunk = sock.recv(remaining)
        if not chunk:
            raise ValueError("connection closed by peer")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def _decode(body: bytes) -> dict[str, object]:
    try:
        payload = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"malformed agent frame: {exc}") from exc
    if not isinstance(payload, dict):
        raise ValueError("agent frame must be a JSON object")
    return payload


# --------------------------------------------------------------------------
# Selection
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class ChannelChoice:
    channel: Channel
    capabilities: Capabilities
    warnings: tuple[str, ...] = ()


def select_channel(
    *,
    prefer: ChannelKind | None = None,
    agent_socket: str | None = None,
    adb_serial: str | None = None,
    dry_run: bool = False,
) -> ChannelChoice:
    """Pick the best channel that actually works, with warnings attached.

    Order: agent (real multitouch) -> adb (single taps) -> null sink. The
    warnings list explains every downgrade in user terms.
    """
    warnings: list[str] = []

    if prefer in (None, ChannelKind.AGENT) and agent_socket:
        agent = AgentChannel(agent_socket, dry_run=dry_run)
        try:
            if not dry_run:
                agent.connect()
            else:
                agent.connect()
            caps = agent.capabilities()
            return ChannelChoice(agent, caps, tuple(warnings))
        except InputError as exc:
            warnings.append(f"touch agent unavailable: {exc}")
            agent.close()

    if prefer in (None, ChannelKind.ADB_SHELL, ChannelKind.AGENT):
        adb = AdbShellChannel(serial=adb_serial, dry_run=dry_run)
        caps = adb.capabilities()
        if caps.available:
            warnings.append(
                "using adb taps: one touch at a time, no held buttons, no "
                "simultaneous movement+action"
            )
            return ChannelChoice(adb, caps, tuple(warnings))
        warnings.append(f"adb unavailable: {caps.reason}")

    null = NullChannel()
    warnings.append("no real touch channel available; input is recorded but not delivered")
    return ChannelChoice(null, null.capabilities(), tuple(warnings))


def wait_for_agent_socket(path: str, *, timeout_s: float = 5.0) -> bool:
    """Poll for the agent socket to appear (started by the Android side)."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if Path(path).exists():
            return True
        time.sleep(0.1)
    return False
