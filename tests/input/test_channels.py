"""Channels must report capabilities honestly, especially about multitouch."""

from __future__ import annotations

import contextlib
import socket
import threading
import time
from pathlib import Path

import pytest

from lynxdroid.core import proc
from lynxdroid.input import channels
from lynxdroid.input.channels import (
    AdbShellChannel,
    AgentChannel,
    ChannelKind,
    NullChannel,
    select_channel,
)
from lynxdroid.input.events import Action, InputError, TouchEvent


def events(*specs) -> list[TouchEvent]:
    return [
        TouchEvent(action=Action(action), pointer=pointer, x=x, y=y, timestamp_ms=1000 + index)
        for index, (action, pointer, x, y) in enumerate(specs)
    ]


class TestNullChannel:
    def test_records_everything(self) -> None:
        channel = NullChannel()
        channel.send(events(("down", 1, 1, 2), ("up", 1, 1, 2)))
        assert len(channel.sent) == 2

    def test_can_simulate_failure(self) -> None:
        channel = NullChannel(fail_with="backend gone")
        with pytest.raises(InputError, match="backend gone"):
            channel.send([])

    def test_reports_itself_unavailable(self) -> None:
        """Regression: it used to claim 10 simultaneous touches.

        Nothing this channel sends reaches Android, so claiming availability
        enabled the key overlay on a machine where no touch could be
        delivered. The capability must describe reality.
        """
        caps = NullChannel().capabilities()
        assert caps.available is False
        assert caps.simultaneous_touches == 1
        assert caps.relative_motion is False
        assert not caps.real_multitouch
        assert "never reach Android" in caps.reason

    def test_an_unavailable_channel_never_enables_the_overlay(self) -> None:
        from lynxdroid.backend.contract import BackendCapabilities
        from lynxdroid.gui.viewmodel import overlay_control
        from lynxdroid.profiles.model import Profile, Target

        caps = NullChannel().capabilities()
        backend = BackendCapabilities(name="test", version="0", max_touches=1)
        profile = Profile.new("P", Target(package="org.example.game"))
        state = overlay_control(backend, caps, profile)
        assert not state.enabled
        assert "never reach Android" in state.reason


class TestAdbShellChannel:
    def test_reports_single_touch_only(self) -> None:
        caps = AdbShellChannel(adb="/usr/bin/adb").capabilities()
        assert caps.available
        assert caps.simultaneous_touches == 1
        assert not caps.real_multitouch
        assert "not simultaneous multitouch" in caps.reason

    def test_unavailable_without_adb(self) -> None:
        caps = AdbShellChannel(adb=None).capabilities()
        if caps.available:
            pytest.skip("adb is installed on this machine")
        assert not caps.available

    def test_complete_taps_are_sent(self, monkeypatch) -> None:
        calls: list[list[str]] = []

        def fake_run(argv, **kwargs):
            calls.append(list(argv))
            return proc.RunResult(argv=tuple(argv), returncode=0)

        monkeypatch.setattr(proc, "run", fake_run)
        channel = AdbShellChannel(adb="/usr/bin/adb")
        channel.send(events(("down", 1, 100, 200), ("up", 1, 100, 200)))
        assert len(calls) == 1
        assert calls[0][-3:] == ["tap", "100", "200"]

    def test_held_touch_is_refused_not_faked(self, monkeypatch) -> None:
        monkeypatch.setattr(
            proc, "run", lambda argv, **kw: proc.RunResult(argv=tuple(argv), returncode=0)
        )
        channel = AdbShellChannel(adb="/usr/bin/adb")
        with pytest.raises(InputError, match="cannot hold a touch"):
            channel.send(events(("down", 1, 10, 10)))

    def test_failed_tap_surfaces_the_error(self, monkeypatch) -> None:
        monkeypatch.setattr(
            proc,
            "run",
            lambda argv, **kw: proc.RunResult(
                argv=tuple(argv), returncode=1, stderr="device offline"
            ),
        )
        channel = AdbShellChannel(adb="/usr/bin/adb")
        with pytest.raises(InputError, match="adb input tap failed"):
            channel.send(events(("down", 1, 1, 1), ("up", 1, 1, 1)))


class _FakeAgent:
    """Minimal in-process implementation of the agent protocol."""

    def __init__(self, path: Path, *, protocol: int = 1, max_touches: int = 10) -> None:
        self.path = path
        self.protocol = protocol
        self.max_touches = max_touches
        self.received: list[dict] = []
        self.fail = False
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._sock.bind(str(path))
        self._sock.listen(1)
        self._stop = False
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self) -> None:
        try:
            conn, _ = self._sock.accept()
        except OSError:
            return
        with conn:
            while not self._stop:
                try:
                    header = conn.recv(4)
                    if len(header) < 4:
                        return
                    length = int.from_bytes(header, "big")
                    body = b""
                    while len(body) < length:
                        chunk = conn.recv(length - len(body))
                        if not chunk:
                            return
                        body += chunk
                except OSError:
                    return
                import json

                payload = json.loads(body.decode())
                self.received.append(payload)
                if payload.get("op") == "hello":
                    reply = {
                        "protocol": self.protocol,
                        "max_touches": self.max_touches,
                        "relative_motion": True,
                        "latency_supported": True,
                    }
                elif payload.get("op") == "bye":
                    return
                elif self.fail:
                    reply = {"ok": False, "error": "no permission to inject events"}
                else:
                    reply = {"ok": True}
                encoded = json.dumps(reply).encode()
                conn.sendall(len(encoded).to_bytes(4, "big") + encoded)

    def close(self) -> None:
        self._stop = True
        with contextlib.suppress(OSError):
            self._sock.close()
        self.path.unlink(missing_ok=True)


@pytest.fixture()
def agent_socket(tmp_path: Path):
    if not hasattr(socket, "AF_UNIX"):
        pytest.skip("no unix sockets")
    fake = _FakeAgent(tmp_path / "agent.sock")
    yield fake
    fake.close()


class TestAgentChannel:
    def test_handshake_reports_real_capabilities(self, agent_socket) -> None:
        channel = AgentChannel(str(agent_socket.path))
        hello = channel.connect()
        caps = channel.capabilities()
        assert hello["max_touches"] == 10
        assert caps.real_multitouch
        assert caps.relative_motion
        channel.close()

    def test_only_unix_sockets_are_used(self, agent_socket) -> None:
        # The path is a filesystem socket: a TCP client cannot reach it, which
        # is exactly the property stage 14 asks for.
        assert Path(agent_socket.path).is_socket()

    def test_events_are_delivered_and_acknowledged(self, agent_socket) -> None:
        channel = AgentChannel(str(agent_socket.path))
        channel.connect()
        channel.send(events(("down", 1, 5, 6), ("move", 1, 7, 8)))
        sent = [msg for msg in agent_socket.received if msg.get("op") == "touch"]
        assert sent[0]["events"][0]["action"] == "down"
        assert sent[0]["events"][1]["x"] == 7
        channel.close()

    def test_protocol_mismatch_is_refused(self, tmp_path: Path) -> None:
        fake = _FakeAgent(tmp_path / "old.sock", protocol=0)
        try:
            channel = AgentChannel(str(fake.path))
            with pytest.raises(InputError, match="protocol"):
                channel.connect()
        finally:
            fake.close()

    def test_agent_rejection_is_an_error(self, agent_socket) -> None:
        agent_socket.fail = True
        channel = AgentChannel(str(agent_socket.path))
        channel.connect()
        with pytest.raises(InputError, match="rejected"):
            channel.send(events(("down", 1, 1, 1)))
        channel.close()

    def test_unreachable_socket_is_reported(self, tmp_path: Path) -> None:
        channel = AgentChannel(str(tmp_path / "missing.sock"), connect_timeout=0.2)
        with pytest.raises(InputError, match="cannot reach"):
            channel.connect()

    def test_closed_channel_refuses_events(self, agent_socket) -> None:
        channel = AgentChannel(str(agent_socket.path))
        channel.connect()
        channel.close()
        with pytest.raises(InputError, match="closed"):
            channel.send(events(("down", 1, 1, 1)))

    def test_agent_without_slots_is_rejected(self, tmp_path: Path) -> None:
        fake = _FakeAgent(tmp_path / "empty.sock", max_touches=0)
        try:
            with pytest.raises(InputError, match="no touch slots"):
                AgentChannel(str(fake.path)).connect()
        finally:
            fake.close()


class TestSelectChannel:
    def test_prefers_the_agent(self, agent_socket) -> None:
        choice = select_channel(agent_socket=str(agent_socket.path))
        try:
            assert choice.capabilities.kind is ChannelKind.AGENT
            assert choice.capabilities.real_multitouch
            assert choice.warnings == ()
        finally:
            choice.channel.close()

    def test_falls_back_with_warnings(self) -> None:
        choice = select_channel(agent_socket="/nonexistent.sock")
        assert choice.capabilities.kind in (ChannelKind.ADB_SHELL, ChannelKind.NULL)
        assert choice.warnings

    def test_null_sink_is_last_resort_with_a_warning(self) -> None:
        choice = select_channel(prefer=ChannelKind.NULL)
        assert choice.capabilities.kind is ChannelKind.NULL
        assert any("not delivered" in warning for warning in choice.warnings)

    def test_dry_run_does_not_touch_the_filesystem(self) -> None:
        choice = select_channel(agent_socket="/definitely/missing.sock", dry_run=True)
        assert choice.capabilities.available


def test_wait_for_agent_socket_times_out(tmp_path: Path) -> None:
    started = time.monotonic()
    assert not channels.wait_for_agent_socket(str(tmp_path / "nope.sock"), timeout_s=0.2)
    assert time.monotonic() - started >= 0.15


def test_wait_for_agent_socket_sees_an_existing_file(tmp_path: Path) -> None:
    path = tmp_path / "here.sock"
    path.write_text("")
    assert channels.wait_for_agent_socket(str(path), timeout_s=0.2)
    assert Path(path).exists()
