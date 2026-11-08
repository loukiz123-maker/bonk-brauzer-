"""Touch event vocabulary and the ring buffer used to hand events to a channel.

Everything is expressed in **Android buffer pixels** with a monotonically
increasing per-pointer sequence number, so a consumer can detect dropped or
reordered events instead of silently applying stale touches.
"""

from __future__ import annotations

from collections import deque
from collections.abc import Iterable
from dataclasses import dataclass, field
from enum import Enum

from lynxdroid.core.errors import ExitCode, LynxError


class Action(str, Enum):
    DOWN = "down"
    MOVE = "move"
    UP = "up"
    CANCEL = "cancel"

    @property
    def ends_contact(self) -> bool:
        return self in (Action.UP, Action.CANCEL)


@dataclass(frozen=True, slots=True)
class TouchEvent:
    """A single pointer state change."""

    action: Action
    pointer: int
    x: float
    y: float
    timestamp_ms: int
    #: Global monotonic sequence, used by tests and the agent protocol.
    seq: int = 0
    #: Which element produced it, for debugging and the overlay.
    source: str = ""

    def as_dict(self) -> dict[str, object]:
        return {
            "action": self.action.value,
            "pointer": self.pointer,
            "x": round(self.x, 3),
            "y": round(self.y, 3),
            "t": self.timestamp_ms,
            "seq": self.seq,
        }


@dataclass(frozen=True, slots=True)
class KeyEventIn:
    """A physical key transition arriving from the window system."""

    key: str
    down: bool
    timestamp_ms: int
    repeat: bool = False

    def __post_init__(self) -> None:
        if not self.key:
            raise ValueError("key must not be empty")


class EventQueue:
    """Bounded FIFO: the newest events win, stale ones are dropped.

    Stage 6 requires a bounded frame/event queue that never blocks input.
    """

    def __init__(self, capacity: int = 256) -> None:
        if capacity < 8:
            raise ValueError("capacity must be >= 8")
        self._capacity = capacity
        self._items: deque[TouchEvent] = deque()
        self.dropped = 0

    def __len__(self) -> int:
        return len(self._items)

    def push(self, event: TouchEvent) -> None:
        if len(self._items) >= self._capacity:
            self._items.popleft()
            self.dropped += 1
        self._items.append(event)

    def extend(self, events: Iterable[TouchEvent]) -> None:
        for event in events:
            self.push(event)

    def drain(self, limit: int | None = None) -> list[TouchEvent]:
        count = len(self._items) if limit is None else min(limit, len(self._items))
        return [self._items.popleft() for _ in range(count)]

    def clear(self) -> None:
        self._items.clear()


class InputError(LynxError):
    """A documented, user-visible input failure (e.g. no free touch slot).

    It used to derive from ``RuntimeError``, which meant a failure to deliver
    a touch escaped the CLI's error handling and printed a traceback instead of
    a sentence. Every user-visible failure in this project goes through
    ``LynxError`` so the exit code and the message stay consistent.
    """

    exit_code = ExitCode.FAIL

    def __init__(self, message: str, *, code: str = "input-error", hint: str | None = None) -> None:
        super().__init__(message, hint=hint)
        self.code = code


class TouchSlotExhausted(InputError):
    def __init__(self, capacity: int) -> None:
        super().__init__(
            f"all {capacity} touch slots are in use; the action was not sent",
            code="touch-slots-exhausted",
        )


@dataclass
class FrameStats:
    """Latency instrumentation (stage 11/13), no invented numbers."""

    produced: int = 0
    dropped: int = 0
    last_latency_ms: float = 0.0
    latencies_ms: list[float] = field(default_factory=list)

    def record(self, latency_ms: float) -> None:
        self.produced += 1
        self.last_latency_ms = latency_ms
        if len(self.latencies_ms) < 512:
            self.latencies_ms.append(latency_ms)

    def median_latency_ms(self) -> float | None:
        if not self.latencies_ms:
            return None
        ordered = sorted(self.latencies_ms)
        mid = len(ordered) // 2
        if len(ordered) % 2:
            return ordered[mid]
        return (ordered[mid - 1] + ordered[mid]) / 2

    def p95_latency_ms(self) -> float | None:
        if not self.latencies_ms:
            return None
        ordered = sorted(self.latencies_ms)
        index = min(len(ordered) - 1, round(0.95 * (len(ordered) - 1)))
        return ordered[index]
