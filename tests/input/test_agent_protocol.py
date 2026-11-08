"""The two implementations of the wire protocol must agree.

android_agent/ holds a Java implementation of the same protocol that
input/channels.py speaks. Nothing compiled the Java, nothing compared the
constants, and the two had already drifted: the Java agent refuses a
handshake with ``ok=false`` and no ``protocol`` field, while the Python test
double replied with a ``protocol`` field and the client printed
"speaks protocol None" when given the real thing.

The Java cannot be compiled here (no Android SDK), so this file does the next
best thing: it reads the constants and the op names out of the source and holds
the Python side to them. When the agent is finally built on a device, the
handshake test below is the shape the real conversation will have.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from lynxdroid.input.channels import AGENT_PROTOCOL, MAX_AGENT_MESSAGE

ROOT = Path(__file__).resolve().parents[2]
JAVA = ROOT / "android_agent" / "app" / "src" / "main" / "java" / "id" / "lynxdroid" / "agent"
SERVICE = JAVA / "TouchAgentService.java"
AGENT_README = ROOT / "android_agent" / "README.md"


def java_constant(name: str) -> int:
    """Read ``private static final int NAME = 64 * 1024;`` from the service."""
    text = SERVICE.read_text(encoding="utf-8")
    match = re.search(rf"static final int {name}\s*=\s*([^;]+);", text)
    assert match is not None, f"{name} is not declared in TouchAgentService.java"
    expression = match.group(1).strip()
    # The declaration is a small integer expression ("64 * 1024").
    assert re.fullmatch(r"[\d\s*+]+", expression), f"cannot evaluate {expression!r}"
    return int(eval(expression, {"__builtins__": {}}, {}))  # noqa: S307 - digits only, asserted above


class TestConstantsMatch:
    def test_the_protocol_version_is_the_same_on_both_sides(self) -> None:
        assert java_constant("PROTOCOL") == AGENT_PROTOCOL

    def test_the_frame_limit_is_the_same_on_both_sides(self) -> None:
        """A frame the agent accepts and the client refuses is a hang, not an
        error message."""
        assert java_constant("MAX_FRAME") == MAX_AGENT_MESSAGE

    def test_the_agent_does_not_offer_more_touches_than_the_engine_allocates(self) -> None:
        """The engine caps contacts at 10; an agent promising more would be
        advertising slots that never arrive."""
        from lynxdroid.input.engine import MAX_TOUCH_SLOTS

        assert java_constant("MAX_TOUCHES") <= MAX_TOUCH_SLOTS


class TestOpsMatch:
    def test_every_op_the_client_sends_is_handled_by_the_agent(self) -> None:
        client = (ROOT / "src" / "lynxdroid" / "input" / "channels.py").read_text(encoding="utf-8")
        sent = set(re.findall(r'"op":\s*"([a-z]+)"', client))
        assert sent, "no ops found in the client; did the encoding change?"
        java = SERVICE.read_text(encoding="utf-8")
        handled = set(re.findall(r'"([a-z]+)"\.equals\(op\)', java))
        assert sent <= handled, f"the agent does not handle {sorted(sent - handled)}"

    def test_the_agent_handles_nothing_the_client_never_sends(self) -> None:
        """Not fatal, but it means one side changed and the other did not."""
        client = (ROOT / "src" / "lynxdroid" / "input" / "channels.py").read_text(encoding="utf-8")
        sent = set(re.findall(r'"op":\s*"([a-z]+)"', client))
        java = SERVICE.read_text(encoding="utf-8")
        handled = set(re.findall(r'"([a-z]+)"\.equals\(op\)', java))
        assert handled <= sent, f"the agent handles unknown ops {sorted(handled - sent)}"


class TestHandshakeShape:
    """What the agent actually sends back, taken from the Java source."""

    def test_a_successful_hello_names_the_protocol(self) -> None:
        java = SERVICE.read_text(encoding="utf-8")
        assert '.put("protocol", PROTOCOL)' in java

    def test_a_refusal_says_which_version_was_asked_for(self) -> None:
        """The client has to distinguish "old agent" from "new agent"; a bare
        "protocol mismatch" leaves the user with no idea which side updates."""
        java = SERVICE.read_text(encoding="utf-8")
        refusal = java.split('"protocol mismatch"', 1)[1][:500]
        # "protocol" must be the AGENT's own version. Sending back the
        # client's number made the desktop say "the agent speaks protocol 1,
        # this build speaks 1", which reads as if nothing were wrong.
        assert '.put("protocol", PROTOCOL)' in refusal, (
            "the agent must report its own version in the refusal"
        )
        assert '.put("protocol", theirs)' not in refusal, (
            "echoing the client's version makes the mismatch message nonsense"
        )


class TestDocumentationMatches:
    def test_the_documented_frame_format_is_the_implemented_one(self) -> None:
        readme = AGENT_README.read_text(encoding="utf-8")
        assert "uint32" in readme.lower() or "big-endian" in readme.lower()
        # The client writes a 4-byte big-endian length; the README must not
        # describe something else (e.g. a newline-delimited stream).
        assert "length" in readme.lower()

    @pytest.mark.parametrize("op", ["hello", "touch", "bye"])
    def test_every_op_appears_in_the_documented_protocol(self, op: str) -> None:
        assert f'"op": "{op}"' in AGENT_README.read_text(encoding="utf-8")
