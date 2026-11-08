"""Coordinate transform tests - the highest-risk maths in the project.

Stage 15 asks specifically for: different aspect ratios, black bars, HiDPI,
rotations 0/90/180/270, extreme points and zero sizes.
"""

from __future__ import annotations

import math

import pytest

from lynxdroid.display.geometry import FitMode, Point, Rect, Rotation, Size, ViewportGeometry


def geo(buffer=(1920, 1080), surface=(960, 540), **kwargs) -> ViewportGeometry:
    return ViewportGeometry(buffer=Size(*buffer), surface=Size(*surface), **kwargs)


class TestContentRect:
    def test_exact_match_has_no_bars(self) -> None:
        g = geo((1920, 1080), (960, 540))
        assert g.content_rect() == Rect(0, 0, 960, 540)
        assert not g.has_bars()

    def test_letterbox_when_window_is_wider(self) -> None:
        # 1920x1080 (16:9) inside a 1000x500 (2:1) window -> bars left/right
        g = geo((1920, 1080), (1000, 500))
        rect = g.content_rect()
        assert rect.height == 500
        assert rect.width == 888  # snapped to whole logical pixels
        bars = g.bars()
        assert bars["left"] == bars["right"]
        assert bars["top"] == 0
        assert bars["left"] > 0

    def test_pillarbox_when_window_is_taller(self) -> None:
        g = geo((1920, 1080), (800, 800))
        rect = g.content_rect()
        assert rect.width == 800
        assert rect.height == pytest.approx(450, abs=0.01)
        bars = g.bars()
        assert bars["top"] == bars["bottom"]
        assert bars["left"] == 0
        assert bars["top"] > 0

    def test_bars_are_symmetric_and_integer(self) -> None:
        for width, height in [(1000, 500), (801, 802), (1023, 769), (640, 1136)]:
            g = geo((1920, 1080), (width, height))
            bars = g.bars()
            assert abs(bars["left"] - bars["right"]) <= 1
            assert abs(bars["top"] - bars["bottom"]) <= 1
            rect = g.content_rect()
            assert rect.x + rect.width <= width
            assert rect.y + rect.height <= height

    def test_zero_surface_gives_empty_rect(self) -> None:
        g = geo((1920, 1080), (0, 0))
        assert g.content_rect() == Rect(0, 0, 0, 0)
        assert g.widget_to_android(0, 0) is None

    def test_zero_buffer_gives_empty_rect(self) -> None:
        g = geo((0, 0), (800, 600))
        assert g.content_rect() == Rect(0, 0, 0, 0)

    def test_fill_mode_stretches(self) -> None:
        g = geo((1920, 1080), (1000, 1000), fit=FitMode.FILL)
        assert g.content_rect() == Rect(0, 0, 1000, 1000)
        assert not g.has_bars()

    def test_integer_mode_uses_whole_scale(self) -> None:
        g = geo((640, 480), (1000, 700), fit=FitMode.INTEGER)
        rect = g.content_rect()
        assert rect.width % 640 == 0
        assert rect.height % 480 == 0
        assert rect.width in (640, 1920)


class TestWidgetToAndroid:
    def test_centre_maps_to_centre(self) -> None:
        g = geo()
        assert g.widget_to_android(480, 270) == pytest.approx((960.0, 540.0))

    def test_corners_map_inside_buffer(self) -> None:
        g = geo()
        x0, y0 = g.widget_to_android(0, 0)
        assert (x0, y0) == pytest.approx((0.0, 0.0))
        # The inclusive corner maps to the last pixel, never past it.
        x1, y1 = g.widget_to_android(959.9, 539.9)
        assert 0 <= x1 <= 1920
        assert 0 <= y1 <= 1080

    def test_pointer_on_bar_returns_none(self) -> None:
        g = geo((1920, 1080), (1000, 500))
        bars = g.bars()
        assert g.widget_to_android(bars["left"] / 2, 250) is None
        assert g.widget_to_android(999 - bars["right"] / 2, 250) is None

    def test_pointer_outside_window_returns_none(self) -> None:
        g = geo()
        assert g.widget_to_android(-1, 10) is None
        assert g.widget_to_android(10, -1) is None
        assert g.widget_to_android(960, 10) is None
        assert g.widget_to_android(10, 540) is None

    def test_hidpi_logical_pixels_are_used(self) -> None:
        # dpr 2.0: Qt reports logical pixels, so a 960x540 window is 2x scaled.
        g = geo((1920, 1080), (960, 540), dpr=2.0)
        assert g.widget_to_android(480, 270) == pytest.approx((960.0, 540.0))
        assert g.scale() == pytest.approx(0.5)

    def test_fractional_scaling_keeps_proportionality(self) -> None:
        g = geo((1920, 1080), (1333, 777))
        rect = g.content_rect()
        ax, ay = g.widget_to_android(rect.x + rect.width / 2, rect.y + rect.height / 4)
        assert ax / 1920 == pytest.approx(0.5, abs=1e-3)
        assert ay / 1080 == pytest.approx(0.25, abs=1e-3)

    @pytest.mark.parametrize("width,height", [(1, 1), (2, 1), (1, 2), (3840, 100)])
    def test_extreme_aspect_ratios_stay_in_bounds(self, width: int, height: int) -> None:
        g = geo((1920, 1080), (width, height))
        rect = g.content_rect()
        assert rect.width <= width
        assert rect.height <= height
        for px, py in [(0, 0), (width - 1, 0), (0, height - 1), (width - 1, height - 1)]:
            result = g.widget_to_android(px, py)
            if result is not None:
                ax, ay = result
                assert -0.001 <= ax <= 1920.001
                assert -0.001 <= ay <= 1080.001


class TestRotation:
    @pytest.mark.parametrize("rotation", list(Rotation))
    def test_round_trip_through_android_space(self, rotation: Rotation) -> None:
        g = geo((1920, 1080), (960, 540), rotation=rotation)
        for u, v in [(0.1, 0.2), (0.5, 0.5), (0.9, 0.8)]:
            px, py = g.normalized_to_widget(u, v)
            back = g.widget_to_normalized_profile(px, py)
            assert back is not None
            assert back[0] == pytest.approx(u, abs=3e-3)
            assert back[1] == pytest.approx(v, abs=3e-3)

    @pytest.mark.parametrize("rotation", list(Rotation))
    def test_corner_elements_are_hit_inside_the_window(self, rotation: Rotation) -> None:
        """A key authored exactly on the buffer edge must still be reachable."""
        g = geo((1920, 1080), (960, 540), rotation=rotation)
        for u, v in [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0), (1.0, 1.0)]:
            px, py = g.normalized_to_widget(u, v)
            assert g.widget_to_android(px, py) is not None

    def test_cw90_swaps_axes(self) -> None:
        g = geo((1920, 1080), (540, 960), rotation=Rotation.CW90)
        assert g.rotated_buffer == Size(1080, 1920)
        # Centre pixel is rotation invariant.
        assert g.widget_to_android(270, 480) == pytest.approx((960.0, 540.0))

    def test_rotation_does_not_change_content_rect(self) -> None:
        base = geo((1920, 1080), (960, 540))
        rotated = geo((1920, 1080), (960, 540), rotation=Rotation.CW180)
        assert base.content_rect() == rotated.content_rect()

    @pytest.mark.parametrize("rotation", list(Rotation))
    def test_every_mapped_point_is_inside_buffer(self, rotation: Rotation) -> None:
        g = geo((1920, 1080), (800, 1000), rotation=rotation)
        rect = g.content_rect()
        steps = 7
        for i in range(steps + 1):
            for j in range(steps + 1):
                px = rect.x + rect.width * i / steps
                py = rect.y + rect.height * j / steps
                result = g.widget_to_android(px, py)
                if result is None:
                    continue
                ax, ay = result
                assert 0 <= ax <= 1920
                assert 0 <= ay <= 1080

    def test_cw90_maps_top_left_of_window_to_bottom_left_of_buffer(self) -> None:
        g = geo((1000, 500), (500, 1000), rotation=Rotation.CW90)
        ax, ay = g.widget_to_android(1, 1)
        assert ax < 20  # near the left edge of the buffer
        assert ay > 480  # near the bottom edge (buffer is 1000x500)


class TestAndroidToWidget:
    def test_round_trip_both_directions(self) -> None:
        g = geo((1920, 1080), (777, 1234), rotation=Rotation.CW270)
        rect = g.content_rect()
        px = rect.x + rect.width * 0.3
        py = rect.y + rect.height * 0.6
        ax, ay = g.widget_to_android(px, py)  # type: ignore[misc]
        bx, by = g.android_to_widget(ax, ay)
        assert bx == pytest.approx(px, abs=0.5)
        assert by == pytest.approx(py, abs=0.5)

    def test_overlay_positions_match_injection_positions(self) -> None:
        g = geo((1280, 720), (901, 555), rotation=Rotation.CW180)
        for element in [(0.2, 0.8), (0.5, 0.5), (0.75, 0.25)]:
            px, py = g.normalized_to_widget(*element)
            back = g.widget_to_normalized_profile(px + 0.0001, py + 0.0001)
            assert back is not None
            assert back[0] == pytest.approx(element[0], abs=2e-3)
            assert back[1] == pytest.approx(element[1], abs=2e-3)


class TestNormalisation:
    def test_normalized_to_android_clamps(self) -> None:
        g = geo()
        assert g.normalized_to_android(-1.0, -1.0) == (0.0, 0.0)
        assert g.normalized_to_android(2.0, 2.0) == (1919.0, 1079.0)

    def test_point_on_bar_has_no_normalised_coordinate(self) -> None:
        g = geo((1920, 1080), (1000, 500))
        assert g.widget_to_normalized(5, 250) is None

    def test_scale_is_zero_for_degenerate_buffer(self) -> None:
        g = geo((0, 0), (800, 600))
        assert g.scale() == 0.0


class TestDescribe:
    def test_describe_is_informative(self) -> None:
        text = geo((1920, 1080), (1000, 500)).describe()
        assert "1920x1080" in text
        assert "1000x500" in text
        assert "bars=" in text

    def test_as_dict_is_jsonable(self) -> None:
        payload = geo().as_dict()
        assert payload["buffer"] == [1920, 1080]
        assert payload["fit"] == "contain"


def test_size_rejects_negative() -> None:
    with pytest.raises(ValueError):
        Size(-1, 10)


def test_point_dataclass_is_frozen() -> None:
    point = Point(1.0, 2.0)
    with pytest.raises(AttributeError):
        point.x = 5.0  # type: ignore[misc]


def test_geometry_is_immutable_and_replaceable() -> None:
    g = geo()
    assert not isinstance(g, dict)
    with pytest.raises(AttributeError):
        g.surface = Size(1, 1)  # type: ignore[misc]
    new = ViewportGeometry(buffer=g.buffer, surface=Size(800, 600))
    assert new.surface == Size(800, 600)
    assert g.surface == Size(960, 540)


def test_no_nan_in_mapping() -> None:
    g = geo((1920, 1080), (0, 540))
    result = g.widget_to_android(0, 0)
    assert result is None
    for value in (g.scale(), g.content_rect().width):
        assert not math.isnan(value)
