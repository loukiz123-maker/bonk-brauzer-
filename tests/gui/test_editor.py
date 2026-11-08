"""Layout editor behaviour (stage 9), tested without Qt.

These tests exist because the editor code used to be unreachable: ``hit_test``
lived in a module that imports PySide6 at import time, so nothing could check
it. The behaviour now lives in Qt-free modules and is exercised here.
"""

from __future__ import annotations

import pytest

from lynxdroid.display.geometry import FitMode, Rotation, Size, ViewportGeometry
from lynxdroid.gui.editor import LayoutEditor, orientation_swapped
from lynxdroid.gui.viewport import content_rect_contains, hit_test
from lynxdroid.profiles.model import (
    Element,
    ElementType,
    Profile,
    ProfileError,
    Target,
)

TARGET = Target(package="org.example.game")
GEOMETRY = ViewportGeometry(
    buffer=Size(1000, 1000), surface=Size(500, 500), fit=FitMode.CONTAIN, rotation=Rotation.NONE
)
#: 16:9 buffer in a square window -> letterbox bars top and bottom.
WIDE = ViewportGeometry(
    buffer=Size(1920, 1080), surface=Size(800, 800), fit=FitMode.CONTAIN, rotation=Rotation.NONE
)


def tap(element_id: str = "tap-a", x: float = 0.5, y: float = 0.5, keys=("j",)) -> Element:
    return Element(id=element_id, type=ElementType.TAP, label="A", x=x, y=y, keys=keys)


def stick(element_id: str = "stick", x: float = 0.2, y: float = 0.8) -> Element:
    return Element(
        id=element_id,
        type=ElementType.DPAD,
        label="Move",
        x=x,
        y=y,
        dpad_keys=("w", "s", "a", "d"),
        dpad_radius=0.15,
    )


def profile(*elements: Element) -> Profile:
    return Profile.new("Game", TARGET, elements=elements)


class TestMoveElement:
    def test_moves_a_tap_to_the_requested_point(self) -> None:
        moved = profile(tap()).move_element("tap-a", 0.25, 0.75)
        element = moved.element("tap-a")
        assert element is not None
        assert (element.x, element.y) == (0.25, 0.75)

    def test_clamps_instead_of_failing_mid_drag(self) -> None:
        moved = profile(tap()).move_element("tap-a", 1.4, -0.3)
        element = moved.element("tap-a")
        assert element is not None
        assert (element.x, element.y) == (1.0, 0.0)

    def test_without_clamp_an_illegal_position_raises(self) -> None:
        with pytest.raises(ProfileError, match=r"outside 0\.\.1"):
            profile(tap()).move_element("tap-a", 1.4, 0.5, clamp=False)

    def test_unknown_element_raises(self) -> None:
        with pytest.raises(ProfileError, match="no element"):
            profile(tap()).move_element("nope", 0.5, 0.5)

    def test_dpad_centre_stays_far_enough_from_the_edge(self) -> None:
        """A stick dragged to the corner must still fit on screen."""
        moved = profile(stick()).move_element("stick", 0.99, 0.01)
        element = moved.element("stick")
        assert element is not None
        assert element.x == pytest.approx(1.0 - element.dpad_radius)
        assert element.y == pytest.approx(element.dpad_radius)
        element.validate()  # the whole point: the result is still legal

    def test_move_keeps_other_elements_untouched(self) -> None:
        moved = profile(tap("tap-a", 0.1, 0.1), tap("tap-b", 0.9, 0.9, keys=("k",))).move_element(
            "tap-a", 0.5, 0.5
        )
        other = moved.element("tap-b")
        assert other is not None
        assert (other.x, other.y) == (0.9, 0.9)

    def test_move_is_immutable(self) -> None:
        original = profile(tap())
        original.move_element("tap-a", 0.1, 0.1)
        untouched = original.element("tap-a")
        assert untouched is not None
        assert (untouched.x, untouched.y) == (0.5, 0.5)


class TestHitTest:
    def test_centre_of_the_pointer_hits_the_element(self) -> None:
        element = hit_test(GEOMETRY, profile(tap()), 250, 250)
        assert element is not None and element.id == "tap-a"

    def test_empty_space_hits_nothing(self) -> None:
        assert hit_test(GEOMETRY, profile(tap("tap-a", 0.1, 0.1)), 250, 250) is None

    def test_letterbox_bar_never_hits(self) -> None:
        """The drawn image is 800x450 inside an 800x800 window: bars above/below."""
        assert not content_rect_contains(WIDE, 400, 10)
        assert hit_test(WIDE, profile(tap(x=0.5, y=0.5)), 400, 10) is None

    def test_none_profile_hits_nothing(self) -> None:
        assert hit_test(GEOMETRY, None, 250, 250) is None

    def test_nearest_element_wins_when_two_overlap(self) -> None:
        near = tap("tap-a", 0.50, 0.50)
        far = tap("tap-b", 0.54, 0.50, keys=("k",))
        found = hit_test(GEOMETRY, profile(near, far), 251, 250)
        assert found is not None and found.id == "tap-a"

    def test_a_dpad_is_hit_across_its_radius(self) -> None:
        element = hit_test(GEOMETRY, profile(stick(x=0.5, y=0.5)), 250 + 60, 250)
        assert element is not None and element.id == "stick"


class TestEditorSelection:
    def test_click_selects_the_element_under_the_pointer(self) -> None:
        editor = LayoutEditor(profile=profile(tap()))
        assert editor.select_at(GEOMETRY, 250, 250) == "tap-a"
        assert editor.selected == "tap-a"

    def test_click_on_empty_space_clears_the_selection(self) -> None:
        editor = LayoutEditor(profile=profile(tap("tap-a", 0.1, 0.1)), selected="tap-a")
        assert editor.select_at(GEOMETRY, 250, 250) is None
        assert editor.selected is None


class TestEditorDrag:
    def test_drag_updates_the_position(self) -> None:
        editor = LayoutEditor(profile=profile(tap()))
        assert editor.begin_drag(GEOMETRY, 250, 250) == "tap-a"
        result = editor.drag_to(GEOMETRY, 400, 100)
        assert result.changed and not result.clamped
        element = editor.profile.element("tap-a")
        assert element is not None
        assert element.x == pytest.approx(0.8) and element.y == pytest.approx(0.2)

    def test_drag_onto_a_bar_leaves_the_element_alone(self) -> None:
        editor = LayoutEditor(profile=profile(tap()))
        editor.begin_drag(WIDE, 400, 400)
        before = editor.profile.element("tap-a")
        editor.drag_to(WIDE, 400, 5)  # a letterbox bar
        after = editor.profile.element("tap-a")
        assert before is not None and after is not None
        assert (after.x, after.y) == (before.x, before.y)

    def test_cancelled_drag_restores_the_starting_point(self) -> None:
        editor = LayoutEditor(profile=profile(tap()))
        editor.begin_drag(GEOMETRY, 250, 250)
        editor.drag_to(GEOMETRY, 400, 100)
        editor.end_drag(commit=False)
        element = editor.profile.element("tap-a")
        assert element is not None
        assert (element.x, element.y) == (0.5, 0.5)

    def test_committed_drag_keeps_the_new_point(self) -> None:
        editor = LayoutEditor(profile=profile(tap()))
        editor.begin_drag(GEOMETRY, 250, 250)
        editor.drag_to(GEOMETRY, 400, 100)
        editor.end_drag(commit=True)
        element = editor.profile.element("tap-a")
        assert element is not None
        assert element.x == pytest.approx(0.8)

    def test_drag_on_a_bar_does_not_start(self) -> None:
        editor = LayoutEditor(profile=profile(tap()))
        assert editor.begin_drag(WIDE, 400, 5) is None
        assert editor.dragging is None

    def test_geometry_alone_cannot_push_a_tap_off_the_edge(self) -> None:
        """`widget_to_normalized_profile` already clamps, so a tap never needs it."""
        editor = LayoutEditor(profile=profile(tap(x=0.95)))
        editor.begin_drag(GEOMETRY, 475, 250)
        result = editor.drag_to(GEOMETRY, 499, 250)
        element = editor.profile.element("tap-a")
        assert element is not None
        assert 0.0 <= element.x <= 1.0
        assert not result.clamped, "the geometry clamps before the editor has to"

    def test_clamping_is_reported_for_a_stick_near_the_corner(self) -> None:
        """A d-pad centre must stay a radius away from the edge, so its move *is*
        clamped - this is the case the clamp in `move_element` exists for."""
        wide = ViewportGeometry(
            buffer=Size(1000, 1000),
            surface=Size(1000, 1000),
            fit=FitMode.CONTAIN,
            rotation=Rotation.NONE,
        )
        editor = LayoutEditor(profile=profile(stick(x=0.5, y=0.5)))
        editor.begin_drag(wide, 500, 500)
        result = editor.drag_to(wide, 999, 1)
        element = editor.profile.element("stick")
        assert element is not None
        assert result.clamped
        assert element.x == pytest.approx(1.0 - element.dpad_radius)
        assert element.y == pytest.approx(element.dpad_radius)
        element.validate()

    def test_drag_without_a_start_is_a_no_op(self) -> None:
        editor = LayoutEditor(profile=profile(tap()))
        result = editor.drag_to(GEOMETRY, 400, 100)
        assert not result.changed
        assert result.element_id is None


class TestEditorState:
    def test_freshly_loaded_profile_is_not_dirty(self) -> None:
        stored = profile(tap())
        assert not LayoutEditor(profile=stored, saved=stored).dirty

    def test_a_drag_makes_it_dirty(self) -> None:
        stored = profile(tap())
        editor = LayoutEditor(profile=stored, saved=stored)
        editor.begin_drag(GEOMETRY, 250, 250)
        editor.drag_to(GEOMETRY, 400, 100)
        editor.end_drag()
        assert editor.dirty

    def test_reset_restores_the_stored_profile(self) -> None:
        stored = profile(tap())
        editor = LayoutEditor(profile=stored, saved=stored)
        editor.begin_drag(GEOMETRY, 250, 250)
        editor.drag_to(GEOMETRY, 400, 100)
        editor.end_drag()
        assert editor.reset() is True
        assert not editor.dirty
        element = editor.profile.element("tap-a")
        assert element is not None and element.x == 0.5

    def test_reset_with_nothing_to_discard_reports_no_change(self) -> None:
        stored = profile(tap())
        assert LayoutEditor(profile=stored, saved=stored).reset() is False

    def test_reset_without_a_stored_profile_changes_nothing(self) -> None:
        assert LayoutEditor(profile=profile(tap())).reset() is False

    def test_mark_saved_clears_dirty(self) -> None:
        stored = profile(tap())
        editor = LayoutEditor(profile=stored, saved=stored)
        editor.begin_drag(GEOMETRY, 250, 250)
        editor.drag_to(GEOMETRY, 400, 100)
        editor.end_drag()
        editor.mark_saved(editor.profile)
        assert not editor.dirty

    def test_rename_makes_it_dirty(self) -> None:
        stored = profile(tap())
        editor = LayoutEditor(profile=stored, saved=stored)
        editor.profile = editor.profile.with_name("Renamed")
        assert editor.dirty

    def test_add_selects_the_new_element(self) -> None:
        editor = LayoutEditor(profile=profile())
        editor.add(tap("tap-new"))
        assert editor.selected == "tap-new"
        assert editor.profile.element("tap-new") is not None

    def test_remove_selected_clears_the_selection(self) -> None:
        editor = LayoutEditor(profile=profile(tap()), selected="tap-a")
        editor.remove_selected()
        assert editor.selected is None
        assert editor.profile.element("tap-a") is None

    def test_remove_with_no_selection_is_a_no_op(self) -> None:
        editor = LayoutEditor(profile=profile(tap()))
        assert len(editor.remove_selected().elements) == 1


class TestOrientationSwap:
    def test_coordinates_are_transposed(self) -> None:
        swapped = orientation_swapped(profile(tap(x=0.2, y=0.8)))
        element = swapped.element("tap-a")
        assert element is not None
        assert (element.x, element.y) == (0.8, 0.2)

    def test_swipe_end_points_swap_too(self) -> None:
        swipe = Element(
            id="swipe-a",
            type=ElementType.SWIPE,
            label="Swipe",
            x=0.1,
            y=0.9,
            keys=("l",),
            swipe_to=(0.4, 0.6),
        )
        swapped = orientation_swapped(profile(swipe))
        element = swapped.element("swipe-a")
        assert element is not None
        assert element.swipe_to == (0.6, 0.4)

    def test_swapping_twice_returns_the_original_positions(self) -> None:
        original = profile(tap(x=0.3, y=0.7))
        twice = orientation_swapped(orientation_swapped(original))
        element = twice.element("tap-a")
        assert element is not None
        assert (element.x, element.y) == pytest.approx((0.3, 0.7))

    def test_the_result_is_still_valid(self) -> None:
        swapped = orientation_swapped(profile(stick(x=0.5, y=0.5)))
        swapped.validate()
