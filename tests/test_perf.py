"""The measurement tool must measure real work, and must not overclaim."""

from __future__ import annotations

import json

import pytest

from lynxdroid import perf
from lynxdroid.cli.main import _demo_profile_and_geometry
from lynxdroid.core.errors import LynxError
from lynxdroid.display.geometry import FitMode, Rotation, Size, ViewportGeometry
from lynxdroid.profiles.model import Element, ElementType, Profile, ProfileError, Target

GEOMETRY = ViewportGeometry(
    buffer=Size(1920, 1080), surface=Size(1200, 675), fit=FitMode.CONTAIN, rotation=Rotation.NONE
)


def profile_with(keys: tuple[str, ...]) -> Profile:
    elements = tuple(
        Element(
            id=f"tap-{key}",
            type=ElementType.TAP,
            label=key.title(),
            x=0.2 * (i + 1),
            y=0.5,
            keys=(key,),
        )
        for i, key in enumerate(keys)
    )
    return Profile.new("Perf", Target(package="org.example.perf"), elements=elements)


class TestScopeHonesty:
    """The whole point of this module is that it does not lie about latency."""

    def test_notes_forbid_an_end_to_end_claim(self) -> None:
        text = " ".join(perf.SCOPE_NOTES).lower()
        assert "not measured" in text
        assert "no end-to-end latency figure is claimed" in text
        assert "compositor" in text and "container" in text

    def test_result_carries_the_scope_into_json(self) -> None:
        result = perf.measure(profile=profile_with(("j",)), geometry=GEOMETRY, iterations=3)
        payload = json.loads(json.dumps(result.as_dict()))
        assert payload["scope"] == list(perf.SCOPE_NOTES)
        assert payload["schema"] == 1

    def test_stages_are_reported_separately(self) -> None:
        result = perf.measure(profile=profile_with(("j",)), geometry=GEOMETRY, iterations=3)
        assert set(result.samples) == {"engine", "encode", "socket"}
        # No summed "total latency" field exists to be mistaken for end-to-end.
        assert "total" not in json.dumps(result.as_dict()).lower()
        assert "latency_ms" not in json.dumps(result.as_dict())

    def test_render_states_what_is_not_measured(self, capsys) -> None:
        result = perf.measure(profile=profile_with(("j",)), geometry=GEOMETRY, iterations=2)
        perf.render(result)
        out = capsys.readouterr().out
        assert "not measured" in out
        assert "compositor" in out


class TestMeasurementIsReal:
    def test_engine_receives_events_not_an_empty_list(self) -> None:
        """Regression: an unbound key timed an empty list and still "passed"."""
        result = perf.measure(profile=profile_with(("j",)), geometry=GEOMETRY, iterations=3)
        assert result.events_per_sample >= 2, "a tap produces DOWN and UP"
        assert all(sample.count == 3 for sample in result.samples.values())

    def test_unbound_key_is_refused_instead_of_measured(self) -> None:
        with pytest.raises(LynxError, match="no touch events"):
            perf.measure(
                profile=profile_with(("j",)), geometry=GEOMETRY, iterations=3, keys=("unbound",)
            )

    def test_profile_without_keys_is_refused(self) -> None:
        empty = Profile.new("Empty", Target(package="org.example.perf"))
        with pytest.raises(LynxError, match="binds no keys"):
            perf.measure(profile=empty, geometry=GEOMETRY, iterations=3)

    def test_zero_iterations_is_refused(self) -> None:
        with pytest.raises(LynxError, match="at least 1"):
            perf.measure(profile=profile_with(("j",)), geometry=GEOMETRY, iterations=0)

    def test_keys_come_from_the_profile(self) -> None:
        profile = profile_with(("j", "k"))
        assert perf.profile_keys(profile) == ("j", "k")

    def test_a_key_cannot_be_bound_twice_so_keys_are_unique(self) -> None:
        """The model already forbids duplicate bindings; profile_keys preserves order."""
        tap = Element(id="a", type=ElementType.TAP, label="A", x=0.2, y=0.2, keys=("j",))
        hold = Element(id="b", type=ElementType.HOLD, label="B", x=0.6, y=0.6, keys=("j",))
        with pytest.raises(ProfileError, match="more than one element"):
            Profile.new("Dup", Target(package="org.example.perf"), elements=(tap, hold))

    def test_every_iteration_is_recorded(self) -> None:
        result = perf.measure(profile=profile_with(("j",)), geometry=GEOMETRY, iterations=25)
        assert result.iterations == 25
        for sample in result.samples.values():
            assert sample.count == 25

    def test_socket_stage_really_writes_to_a_socket(self) -> None:
        """The socket series must be non-zero, i.e. a syscall actually happened."""
        result = perf.measure(profile=profile_with(("j",)), geometry=GEOMETRY, iterations=20)
        summary = result.samples["socket"].summary()
        assert summary["median"] > 0.0
        assert summary["max"] >= summary["median"]

    def test_summary_is_ordered_and_bounded(self) -> None:
        sample = perf.Sample("x")
        for value in (5.0, 1.0, 3.0, 9.0, 2.0):
            sample.add(value)
        summary = sample.summary()
        assert summary["count"] == 5
        assert summary["min"] == 1.0
        assert summary["max"] == 9.0
        assert summary["median"] == 3.0
        assert summary["min"] <= summary["median"] <= summary["p95"] <= summary["max"]

    def test_empty_sample_does_not_crash(self) -> None:
        assert perf.Sample("empty").summary() == {"count": 0}


class TestCliPerf:
    def test_perf_json_is_valid_and_scoped(self, capsys, tmp_path) -> None:
        from lynxdroid.cli.main import main

        code = main(["--root", str(tmp_path), "perf", "--iterations", "5", "--json"])
        out = capsys.readouterr().out
        assert code == 0
        payload = json.loads(out)
        assert payload["iterations"] == 5
        assert payload["measurements"]["engine"]["count"] == 5
        assert payload["scope"]

    def test_perf_text_prints_the_caveat(self, capsys, tmp_path) -> None:
        from lynxdroid.cli.main import main

        main(["--root", str(tmp_path), "perf", "--iterations", "3"])
        out = capsys.readouterr().out
        assert "no end-to-end latency figure is claimed" in out

    def test_demo_profile_has_bound_keys(self) -> None:
        profile, geometry = _demo_profile_and_geometry()
        assert perf.profile_keys(profile)
        assert geometry.buffer.width == 1920
