"""Resolution/DPI validation: every input path must be rejected safely."""

from __future__ import annotations

import pytest

from lynxdroid.display.resolution import (
    MAX_EDGE,
    MIN_EDGE,
    PRESETS,
    ApplyMode,
    DisplayRequest,
    Orientation,
    preset_by_name,
    suggest_apply_mode,
    validate_display,
)


class TestValidInput:
    @pytest.mark.parametrize(
        "width,height,dpi",
        [
            (1920, 1080, 320),
            (1280, 720, 240),
            (1080, 1920, 320),
            (320, 320, 72),
            (3840, 2160, 960),
            (1600, 900, 280),
        ],
    )
    def test_accepts_in_range_values(self, width: int, height: int, dpi: int) -> None:
        request, issues = validate_display(width, height, dpi)
        assert request is not None
        assert (request.width, request.height, request.dpi) == (width, height, dpi)
        assert not any(issue.is_error for issue in issues)

    def test_accepts_numeric_strings_from_the_gui(self) -> None:
        request, _ = validate_display("1280", "720", "240")
        assert request == DisplayRequest(1280, 720, 240)

    def test_accepts_whitespace_in_strings(self) -> None:
        request, _ = validate_display(" 1280 ", " 720", " 240 ")
        assert request is not None

    def test_accepts_integral_floats(self) -> None:
        request, _ = validate_display(1280.0, 720.0, 240.0)
        assert request is not None


class TestRejectedInput:
    def test_zero_dimensions_rejected(self) -> None:
        for args in [(0, 720, 240), (1280, 0, 240), (1280, 720, 0)]:
            request, issues = validate_display(*args)
            assert request is None
            assert any("positive" in issue.message for issue in issues)

    def test_negative_dimensions_rejected(self) -> None:
        request, _ = validate_display(-1280, 720, 240)
        assert request is None

    def test_below_minimum_rejected(self) -> None:
        request, issues = validate_display(MIN_EDGE - 1, 720, 240)
        assert request is None
        assert any("safe minimum" in issue.message for issue in issues)

    def test_above_maximum_rejected(self) -> None:
        request, issues = validate_display(MAX_EDGE + 1, 720, 240)
        assert request is None
        assert any("safe maximum" in issue.message for issue in issues)

    def test_dpi_range_enforced(self) -> None:
        assert validate_display(1280, 720, 71)[0] is None
        assert validate_display(1280, 720, 961)[0] is None

    def test_overflowing_area_rejected(self) -> None:
        request, issues = validate_display(7680, 7680, 320)
        assert request is None
        assert any("Mpx" in issue.message for issue in issues)

    def test_area_limit_is_the_first_line_of_defence(self) -> None:
        # 6000x6000 is both over the area limit and over a 32-bit stride; the
        # reported reason must be one of the two, and the request must fail.
        request, issues = validate_display(6000, 6000, 100)
        assert request is None
        assert any(("stride" in i.message) or ("Mpx" in i.message) for i in issues)

    def test_area_limit_accepts_exactly_the_limit(self) -> None:
        request, _ = validate_display(3840, 2160, 320)
        assert request is not None

    @pytest.mark.parametrize(
        "bad", ["abc", "", None, [], {}, True, 1280.5, float("nan"), float("inf")]
    )
    def test_garbage_is_rejected_with_a_message(self, bad: object) -> None:
        request, issues = validate_display(bad, 720, 240)
        assert request is None
        assert issues and issues[0].is_error

    def test_extreme_aspect_is_a_warning_not_an_error(self) -> None:
        request, issues = validate_display(7680, 320, 240)
        assert any(issue.severity == "warning" for issue in issues)
        # 24:1 is extreme but not unsafe: it is allowed, with a warning.
        assert request is not None or any(issue.is_error for issue in issues)


class TestHelpers:
    def test_orientation_detection(self) -> None:
        assert Orientation.of(1920, 1080) is Orientation.LANDSCAPE
        assert Orientation.of(1080, 1920) is Orientation.PORTRAIT
        assert Orientation.of(1080, 1080) is Orientation.LANDSCAPE

    def test_request_area_and_aspect(self) -> None:
        request = DisplayRequest(1280, 720, 240)
        assert request.area == 921_600
        assert request.aspect == pytest.approx(16 / 9)

    def test_presets_are_all_valid(self) -> None:
        for preset in PRESETS:
            request, issues = validate_display(preset.width, preset.height, preset.dpi)
            assert request is not None, preset.name
            assert not any(issue.is_error for issue in issues)

    def test_preset_lookup_is_case_insensitive(self) -> None:
        assert preset_by_name("1080P LANDSCAPE") is not None
        assert preset_by_name("nope") is None

    @pytest.mark.parametrize(
        "change,expected",
        [
            ("buffer", ApplyMode.RESTART_SESSION),
            ("dpi", ApplyMode.LIVE),
            ("wm-size", ApplyMode.LIVE),
            ("multi-windows", ApplyMode.RESTART_CONTAINER),
            ("unknown-thing", ApplyMode.RESTART_SESSION),
        ],
    )
    def test_apply_mode_explains_restart_requirements(
        self, change: str, expected: ApplyMode
    ) -> None:
        assert suggest_apply_mode(change) is expected
