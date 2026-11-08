"""Latency measurement for the part of the path LynxDroid owns (stage 13).

Honesty rule for this module: it measures **our own** work and says so. The
full input-to-pixel latency on the target machine includes the compositor, the
container, Android's input dispatcher and the app's own frame - none of which
exist here, so none of which are reported.

What is measured, per sample:

1. ``engine``  - turning a key event into touch events (state machine + geometry);
2. ``encode``  - serialising those events into the agent wire format;
3. ``socket``  - writing the frame to a real AF_UNIX socket and reading it back
   on the other end (loopback, so it is the syscall cost, not a device round trip).

The three numbers are reported separately and never summed into a
"latency" figure that would imply more than it proves.
"""

from __future__ import annotations

import json
import socket
import tempfile
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

from lynxdroid.core.errors import LynxError
from lynxdroid.display.geometry import ViewportGeometry
from lynxdroid.input.channels import MAX_AGENT_MESSAGE
from lynxdroid.input.engine import InputEngine
from lynxdroid.input.events import TouchEvent
from lynxdroid.profiles.model import Profile


def _percentile(ordered: list[float], fraction: float) -> float:
    if not ordered:
        return 0.0
    if len(ordered) == 1:
        return ordered[0]
    index = min(len(ordered) - 1, round(fraction * (len(ordered) - 1)))
    return ordered[index]


@dataclass
class Sample:
    """One timing series, in milliseconds."""

    name: str
    values: list[float] = field(default_factory=list)

    def add(self, value_ms: float) -> None:
        self.values.append(value_ms)

    @property
    def count(self) -> int:
        return len(self.values)

    def summary(self) -> dict[str, float | int]:
        if not self.values:
            return {"count": 0}
        ordered = sorted(self.values)
        return {
            "count": len(ordered),
            "min": round(ordered[0], 4),
            "median": round(_percentile(ordered, 0.5), 4),
            "p95": round(_percentile(ordered, 0.95), 4),
            "max": round(ordered[-1], 4),
        }


@dataclass
class PerfResult:
    """Structured result; ``as_dict`` is what ``--json`` prints."""

    samples: dict[str, Sample]
    iterations: int
    events_per_sample: int
    notes: list[str] = field(default_factory=list)

    def as_dict(self) -> dict[str, object]:
        return {
            "schema": 1,
            "iterations": self.iterations,
            "events_per_sample": self.events_per_sample,
            "measurements": {name: s.summary() for name, s in self.samples.items()},
            "scope": self.notes,
        }


#: Sentences that must accompany every number this module produces.
SCOPE_NOTES = [
    "measured here: key event -> touch events, wire encoding, AF_UNIX write/read",
    "not measured: compositor, container, Android input dispatch, application frame",
    "no end-to-end latency figure is claimed; it cannot be obtained on this machine",
]


def _encode_frame(events: list[TouchEvent]) -> bytes:
    """Same framing as the agent protocol: 4-byte big-endian length + JSON."""
    body = json.dumps(
        {"op": "touch", "events": [e.as_dict() for e in events]},
        separators=(",", ":"),
    ).encode("utf-8")
    if len(body) > MAX_AGENT_MESSAGE:
        raise LynxError("sample frame exceeds the agent message limit")
    return len(body).to_bytes(4, "big") + body


def _loopback_server(path: Path, stop: threading.Event) -> None:
    """Read frames until told to stop; a real socket, not a mock."""
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(str(path))
    server.listen(1)
    server.settimeout(0.5)
    try:
        while not stop.is_set():
            try:
                conn, _ = server.accept()
            except TimeoutError:
                continue
            with conn:
                conn.settimeout(2.0)
                while not stop.is_set():
                    header = conn.recv(4)
                    if len(header) < 4:
                        break
                    remaining = int.from_bytes(header, "big")
                    while remaining:
                        chunk = conn.recv(min(remaining, 65536))
                        if not chunk:
                            break
                        remaining -= len(chunk)
    finally:
        server.close()


def profile_keys(profile: Profile) -> tuple[str, ...]:
    """Every key the profile actually binds, in a stable order.

    Measuring a key the profile does not bind would time an empty list and
    report a flattering, meaningless number - so the keys are taken from the
    profile rather than assumed.
    """
    seen: list[str] = []
    for element in profile.elements:
        for key in element.keys:
            if key and key not in seen:
                seen.append(key)
    return tuple(seen)


def measure(
    *,
    profile: Profile,
    geometry: ViewportGeometry,
    iterations: int = 200,
    keys: tuple[str, ...] | None = None,
) -> PerfResult:
    """Run the pipeline ``iterations`` times and collect three series."""
    if iterations < 1:
        raise LynxError("iterations must be at least 1")
    keys = keys if keys else profile_keys(profile)
    if not keys:
        raise LynxError(
            "the profile binds no keys, so there is no input path to measure",
            hint="add a tap element with a key, or pass keys explicitly",
        )

    engine = InputEngine(profile, geometry)
    engine_samples = Sample("engine")
    encode_samples = Sample("encode")
    socket_samples = Sample("socket")

    with tempfile.TemporaryDirectory() as tmp:
        sock_path = Path(tmp) / "agent.sock"
        stop = threading.Event()
        server = threading.Thread(target=_loopback_server, args=(sock_path, stop), daemon=True)
        server.start()
        deadline = time.monotonic() + 2.0
        while not sock_path.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        if not sock_path.exists():
            stop.set()
            raise LynxError("could not start the loopback timing server")

        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            client.connect(str(sock_path))
            events_per_sample = 0
            for index in range(iterations):
                key = keys[index % len(keys)]

                start = time.perf_counter()
                events = engine.key_down(key)
                if not events:
                    raise LynxError(
                        f"key '{key}' produced no touch events; the measurement "
                        "would be meaningless",
                        hint="the key is not bound by this profile",
                    )
                down_ns = time.perf_counter()
                engine_samples.add((down_ns - start) * 1000.0)

                frame = _encode_frame(events)
                encoded_ns = time.perf_counter()
                encode_samples.add((encoded_ns - down_ns) * 1000.0)

                client.sendall(frame)
                sent_ns = time.perf_counter()
                socket_samples.add((sent_ns - encoded_ns) * 1000.0)
                events_per_sample = max(events_per_sample, len(events))

                # Release so the next iteration starts from a clean state.
                engine.key_up(key)
        finally:
            client.close()
            stop.set()
            server.join(timeout=2.0)

    return PerfResult(
        samples={"engine": engine_samples, "encode": encode_samples, "socket": socket_samples},
        iterations=iterations,
        events_per_sample=events_per_sample,
        notes=list(SCOPE_NOTES),
    )


def render(result: PerfResult, *, emit: Callable[[str], None] = print) -> None:
    """Human-readable table. Deliberately quiet about aggregate "latency"."""
    emit("LynxDroid pipeline timing (milliseconds)")
    emit("")
    emit(
        f"  iterations: {result.iterations}    events per sample (max): {result.events_per_sample}"
    )
    emit("")
    emit(f"  {'stage':<10} {'count':>6} {'min':>9} {'median':>9} {'p95':>9} {'max':>9}")
    for name, sample in result.samples.items():
        summary = sample.summary()
        if not summary["count"]:
            emit(f"  {name:<10} {'0':>6}       (no samples)")
            continue
        emit(
            f"  {name:<10} {summary['count']:>6} {summary['min']:>9.4f} "
            f"{summary['median']:>9.4f} {summary['p95']:>9.4f} {summary['max']:>9.4f}"
        )
    emit("")
    for note in result.notes:
        emit(f"  ! {note}")
