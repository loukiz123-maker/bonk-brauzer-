"""The path from an engine event to a channel, instrumented and Qt-free.

The window used to call ``engine.key_down(...)`` and then do nothing with the
result: the touches were computed, drawn and dropped. No test could see it
because the whole path lived inside a Qt widget.

This module owns that path so it can be tested:

    engine output ──▶ deliver() ──▶ Channel.send() ──▶ Android
                          │
                          └── FrameStats (how long *our* part took)

The timing reported here covers handing events to the channel. It is not
end-to-end latency, and the docstrings, the label text and
``docs/PERFORMANCE.md`` all say so.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field

from lynxdroid.core.errors import LynxError
from lynxdroid.input.channels import Capabilities as ChannelCapabilities
from lynxdroid.input.channels import Channel, select_channel
from lynxdroid.input.events import FrameStats, TouchEvent


@dataclass
class InputPipeline:
    """Delivers engine output to a channel and records what happened."""

    channel: Channel | None = None
    stats: FrameStats = field(default_factory=FrameStats)
    #: Human-readable notes (downgrades, failures) in the order they happened.
    notes: list[str] = field(default_factory=list)
    #: Last failure, for the status bar. Cleared on the next success.
    last_error: str | None = None
    _clock: object = field(default=None, repr=False)

    # -- setup -----------------------------------------------------------
    def attach(self, channel: Channel, *, notes: tuple[str, ...] = ()) -> ChannelCapabilities:
        self.channel = channel
        self.notes.extend(notes)
        return channel.capabilities()

    def choose(self, *, agent_socket: str | None = None) -> ChannelCapabilities:
        """Pick a channel with the real selector, keeping its warnings."""
        choice = select_channel(agent_socket=agent_socket)
        return self.attach(choice.channel, notes=tuple(choice.warnings))

    @property
    def capabilities(self) -> ChannelCapabilities | None:
        return self.channel.capabilities() if self.channel is not None else None

    # -- delivery --------------------------------------------------------
    def deliver(self, events: list[TouchEvent]) -> bool:
        """Send ``events``; return True when the channel accepted them.

        Delivery is attempted even when the channel reports itself
        unavailable: a test sink still records the events, and refusing early
        would hide the difference between "not delivered" and "no channel".
        """
        if not events:
            return False
        if self.channel is None:
            self.last_error = "no touch channel is attached"
            return False
        started = time.perf_counter()
        try:
            self.channel.send(events)
        except LynxError as exc:
            self.last_error = exc.message
            self.notes.append(f"touch delivery failed: {exc.message}")
            return False
        self.stats.record((time.perf_counter() - started) * 1000.0)
        self.last_error = None
        return True

    # -- reporting -------------------------------------------------------
    def summary(self) -> str:
        """One status line. Never claims end-to-end latency, never prints 0.

        The rounding matters: handing a frame to a local sink takes under a
        microsecond, and rounding that to ``0.000 ms`` is exactly the "0 ms"
        claim the specification forbids. Sub-microsecond work is reported as
        such instead.
        """
        if self.last_error:
            return f"input: {self.last_error}"
        median = self.stats.median_latency_ms()
        if median is None:
            return "input: no events sent yet"
        p95 = self.stats.p95_latency_ms() or 0.0

        def render(value_ms: float) -> str:
            if 0.0 < value_ms < 0.001:
                return f"{value_ms * 1000:.1f} µs"
            if value_ms <= 0.0:
                return "below timer resolution"
            return f"{value_ms:.3f} ms"

        return (
            f"send: {render(median)} median · p95 {render(p95)} · "
            f"{self.stats.produced} events (not end-to-end)"
        )

    def close(self) -> None:
        if self.channel is not None:
            self.channel.close()
            self.channel = None
