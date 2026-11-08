"""The engine-to-channel path.

Written because the GUI used to compute touches and then drop them: the
window called ``engine.key_down(...)``, drew the result, and never handed it
to a channel. Nothing caught it, because the code lived inside a Qt widget.

``InputPipeline`` is that path, outside Qt, so these tests can exist.
"""

from __future__ import annotations

import pytest

from lynxdroid.input.channels import NullChannel
from lynxdroid.input.events import Action, InputError, TouchEvent
from lynxdroid.input.pipeline import InputPipeline


def events(*specs: tuple[str, int, float, float]) -> list[TouchEvent]:
    return [
        TouchEvent(Action(spec[0]), spec[1], spec[2], spec[3], index, index)
        for index, spec in enumerate(specs)
    ]


class TestDelivery:
    def test_events_reach_the_channel(self) -> None:
        channel = NullChannel()
        pipeline = InputPipeline()
        pipeline.attach(channel)
        assert pipeline.deliver(events(("down", 1, 0.5, 0.5)))
        assert len(channel.sent) == 1

    def test_every_event_of_a_frame_is_sent(self) -> None:
        channel = NullChannel()
        pipeline = InputPipeline()
        pipeline.attach(channel)
        pipeline.deliver(events(("down", 1, 0.1, 0.1), ("up", 1, 0.1, 0.1)))
        assert len(channel.sent) == 2

    def test_an_empty_frame_is_not_sent(self) -> None:
        channel = NullChannel()
        pipeline = InputPipeline()
        pipeline.attach(channel)
        assert pipeline.deliver([]) is False
        assert channel.sent == []

    def test_no_channel_reports_an_error_instead_of_crashing(self) -> None:
        pipeline = InputPipeline()
        assert pipeline.deliver(events(("down", 1, 0.5, 0.5))) is False
        assert pipeline.last_error is not None
        assert "no touch channel" in pipeline.last_error

    def test_a_failing_channel_is_reported_and_recorded(self) -> None:
        channel = NullChannel(fail_with="connection reset")
        pipeline = InputPipeline()
        pipeline.attach(channel)
        assert pipeline.deliver(events(("down", 1, 0.5, 0.5))) is False
        assert pipeline.last_error == "connection reset"
        assert any("connection reset" in note for note in pipeline.notes)

    def test_a_failure_does_not_stop_later_delivery(self) -> None:
        """One dropped frame must not silence the channel for good."""
        channel = NullChannel(fail_with="temporary")
        pipeline = InputPipeline()
        pipeline.attach(channel)
        pipeline.deliver(events(("down", 1, 0.5, 0.5)))
        channel.fail_with = None
        assert pipeline.deliver(events(("up", 1, 0.5, 0.5))) is True
        assert pipeline.last_error is None

    def test_a_dropped_event_leaves_no_stale_error(self) -> None:
        channel = NullChannel(fail_with="boom")
        pipeline = InputPipeline()
        pipeline.attach(channel)
        pipeline.deliver(events(("down", 1, 0.5, 0.5)))
        pipeline.deliver([])  # nothing to send
        assert pipeline.last_error == "boom", "an empty frame must not clear the error"

    def test_input_error_type_is_used_for_failures(self) -> None:
        class Exploding(NullChannel):
            def send(self, events):  # type: ignore[override]
                raise InputError("nope", code="channel-failure")

        pipeline = InputPipeline()
        pipeline.attach(Exploding())
        assert pipeline.deliver(events(("down", 1, 0.5, 0.5))) is False
        assert pipeline.last_error == "nope"


class TestMeasurement:
    def test_a_successful_send_is_timed(self) -> None:
        pipeline = InputPipeline()
        pipeline.attach(NullChannel())
        pipeline.deliver(events(("down", 1, 0.5, 0.5)))
        assert pipeline.stats.produced == 1
        assert pipeline.stats.median_latency_ms() is not None

    def test_failed_sends_are_not_timed_as_successes(self) -> None:
        pipeline = InputPipeline()
        pipeline.attach(NullChannel(fail_with="no"))
        pipeline.deliver(events(("down", 1, 0.5, 0.5)))
        assert pipeline.stats.produced == 0

    def test_summary_says_it_is_not_end_to_end(self) -> None:
        pipeline = InputPipeline()
        pipeline.attach(NullChannel())
        pipeline.deliver(events(("down", 1, 0.5, 0.5)))
        summary = pipeline.summary()
        assert "not end-to-end" in summary
        assert "median" in summary

    def test_summary_before_any_event_is_honest(self) -> None:
        assert "no events" in InputPipeline().summary()

    def test_summary_reports_the_error_when_there_is_one(self) -> None:
        pipeline = InputPipeline()
        pipeline.deliver(events(("down", 1, 0.5, 0.5)))
        assert "no touch channel" in pipeline.summary()


class TestSelection:
    def test_choose_reports_the_downgrades(self, monkeypatch) -> None:
        from lynxdroid.input import pipeline as pipeline_mod
        from lynxdroid.input.channels import ChannelChoice

        def fake_select(*, agent_socket=None, **kwargs):
            channel = NullChannel()
            return ChannelChoice(
                channel=channel,
                capabilities=channel.capabilities(),
                warnings=("adb unavailable", "no real touch channel"),
            )

        monkeypatch.setattr(pipeline_mod, "select_channel", fake_select)
        pipeline = InputPipeline()
        caps = pipeline.choose()
        assert caps.kind.value == "null"
        assert "adb unavailable" in pipeline.notes
        assert pipeline.channel is not None

    def test_capabilities_come_from_the_attached_channel(self) -> None:
        pipeline = InputPipeline()
        assert pipeline.capabilities is None
        pipeline.attach(NullChannel())
        caps = pipeline.capabilities
        assert caps is not None and caps.available is False

    def test_close_releases_the_channel(self) -> None:
        pipeline = InputPipeline()
        pipeline.attach(NullChannel())
        pipeline.close()
        assert pipeline.channel is None
        pipeline.close()  # idempotent

    def test_attaching_replaces_the_old_channel(self) -> None:
        first, second = NullChannel(), NullChannel()
        pipeline = InputPipeline()
        pipeline.attach(first)
        pipeline.attach(second)
        pipeline.deliver(events(("down", 1, 0.5, 0.5)))
        assert first.sent == [] and len(second.sent) == 1


class TestRegressionDroppedTouches:
    """The defect this module was created to make testable."""

    def test_engine_output_actually_leaves_the_process_boundary(self) -> None:
        from lynxdroid.display.geometry import FitMode, Rotation, Size, ViewportGeometry
        from lynxdroid.input.engine import InputEngine
        from lynxdroid.profiles.model import Element, ElementType, Profile, Target

        profile = Profile.new(
            "P",
            Target(package="org.example.game"),
            elements=(
                Element(id="tap", type=ElementType.TAP, label="Fire", x=0.5, y=0.5, keys=("j",)),
            ),
        )
        geometry = ViewportGeometry(
            buffer=Size(1920, 1080),
            surface=Size(960, 540),
            fit=FitMode.CONTAIN,
            rotation=Rotation.NONE,
        )
        engine = InputEngine(profile, geometry)
        channel = NullChannel()
        pipeline = InputPipeline()
        pipeline.attach(channel)

        produced = engine.key_down("j")
        assert produced, "the engine must produce a tap"
        assert pipeline.deliver(produced) is True
        assert len(channel.sent) == len(produced), "the touches must not be dropped"
        assert [e.action for e in channel.sent] == [Action.DOWN, Action.UP]

    def test_release_also_leaves_the_process(self) -> None:
        from lynxdroid.display.geometry import FitMode, Rotation, Size, ViewportGeometry
        from lynxdroid.input.engine import InputEngine
        from lynxdroid.profiles.model import Element, ElementType, Profile, Target

        hold = Element(id="hold", type=ElementType.HOLD, label="Run", x=0.2, y=0.2, keys=("k",))
        profile = Profile.new("P", Target(package="org.example.game"), elements=(hold,))
        geometry = ViewportGeometry(
            buffer=Size(1920, 1080),
            surface=Size(960, 540),
            fit=FitMode.CONTAIN,
            rotation=Rotation.NONE,
        )
        engine = InputEngine(profile, geometry)
        channel = NullChannel()
        pipeline = InputPipeline()
        pipeline.attach(channel)

        pipeline.deliver(engine.key_down("k"))
        held = len(channel.sent)
        pipeline.deliver(engine.key_up("k"))
        assert len(channel.sent) > held
        assert channel.sent[-1].action is Action.UP


class TestPerformanceIsNotOverclaimed:
    def test_no_end_to_end_claim_in_any_string(self) -> None:
        pipeline = InputPipeline()
        pipeline.attach(NullChannel())
        pipeline.deliver(events(("down", 1, 0.5, 0.5)))
        text = (pipeline.summary() + " " + " ".join(pipeline.notes)).lower()
        assert "(not end-to-end)" in text
        # "0 ms" is the forbidden promise. Work faster than the display
        # precision is reported in microseconds, never as zero.
        assert "0.000 ms" not in text
        assert "0 ms" not in text
        for word in ("guarantee", "instant", "zero latency"):
            assert word not in text

    def test_sub_microsecond_work_is_reported_in_microseconds(self) -> None:
        """Regression: a null sink rounded to "0.000 ms", the exact claim
        stage 13 forbids."""
        pipeline = InputPipeline()
        pipeline.attach(NullChannel())
        pipeline.deliver(events(("down", 1, 0.5, 0.5)))
        assert "µs" in pipeline.summary() or "timer resolution" in pipeline.summary()

    @pytest.mark.parametrize("field", ["produced", "latencies_ms"])
    def test_stats_are_plain_counters(self, field: str) -> None:
        pipeline = InputPipeline()
        assert hasattr(pipeline.stats, field)
