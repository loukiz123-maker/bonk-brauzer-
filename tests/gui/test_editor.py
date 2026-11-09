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

    def test_selecting_by_id_matches_clicking(self) -> None:
        """Choosing a row in the element list selects the same key on the canvas."""
        editor = LayoutEditor(profile=profile(tap()))
        assert editor.select("tap-a") == "tap-a"
        assert editor.selected == "tap-a"

    def test_selecting_none_clears(self) -> None:
        editor = LayoutEditor(profile=profile(tap()), selected="tap-a")
        assert editor.select(None) is None
        assert editor.selected is None

    def test_selecting_an_unknown_id_selects_nothing(self) -> None:
        """A stale row must not leave a selection pointing at nothing."""
        editor = LayoutEditor(profile=profile(tap()), selected="tap-a")
        assert editor.select("gone") is None
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


class TestMainWindowEditorIntegration:
    """Execute actual Qt-free portions of MainWindow with a real input engine.

    PySide's shared libraries are absent in this sandbox. Extracting the real
    method AST tests behaviour rather than merely looking for source strings;
    it is still not a substitute for running the complete Qt window.
    """

    @staticmethod
    def make_window():
        import ast
        import logging
        from dataclasses import replace
        from pathlib import Path
        from types import SimpleNamespace

        from lynxdroid.backend.contract import BackendState
        from lynxdroid.input.engine import InputEngine

        source = (
            Path(__file__).resolve().parents[2] / "src/lynxdroid/gui/main_window.py"
        ).read_text()
        tree = ast.parse(source)
        main = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == "MainWindow")
        wanted = {
            "_on_element_moved",
            "_ensure_engine",
            "focusOutEvent",
            "_refresh_surface",
            "_stop_session",
            "closeEvent",
            "_confirm_profile_change",
            "_save_profile",
            "_refresh_edited_profile",
            "_on_profile_selected",
        }
        methods = [n for n in main.body if isinstance(n, ast.FunctionDef) and n.name in wanted]
        assert len(methods) == len(wanted)

        class Base:
            def focusOutEvent(self, event):
                pass

            def closeEvent(self, event):
                pass

        class MessageBox:
            StandardButton = SimpleNamespace(Save=1, Discard=2, Cancel=4)
            answer = 4

            @classmethod
            def question(cls, *_args):
                return cls.answer

        module = ast.Module(
            body=[
                ast.ImportFrom(module="__future__", names=[ast.alias(name="annotations")], level=0),
                ast.ClassDef(
                    name="Harness",
                    bases=[ast.Name(id="Base", ctx=ast.Load())],
                    keywords=[],
                    body=methods,
                    decorator_list=[],
                ),
            ],
            type_ignores=[],
        )
        namespace = {
            "Base": Base,
            "InputEngine": InputEngine,
            "BackendState": BackendState,
            "QMessageBox": MessageBox,
            "log": logging.getLogger(__name__),
        }
        exec(compile(ast.fix_missing_locations(module), "<gui-harness>", "exec"), namespace)  # noqa: S102
        window = namespace["Harness"]()
        original = profile(replace(tap(), type=ElementType.HOLD))
        window._profile = original
        window._editor = LayoutEditor(profile=original, saved=original)
        window._engine = InputEngine(original, GEOMETRY)
        window.events = []
        window._deliver = lambda events: window.events.extend(events)
        window._ensure_channel = lambda: None
        window._sync_aim_mode = lambda: None
        window._refresh_geometry_label = lambda: None
        window._render_elements = lambda: None
        window._sync_element_row = lambda _id: None
        window._set_status = lambda *args: None
        window._save_profile_button = SimpleNamespace(
            setEnabled=lambda enabled: setattr(window, "dirty", enabled)
        )
        window._surface = SimpleNamespace(
            geometry_info=lambda: GEOMETRY,
            set_active_elements=lambda ids: None,
            set_profile=lambda p: setattr(window, "surface_profile", p),
            set_selected=lambda element_id: None,
        )
        window._settings = SimpleNamespace(release_on_focus_loss=True)
        window._poller = SimpleNamespace(stop=lambda: None)
        window._tasks = SimpleNamespace(shutdown=lambda: None, submit=lambda *args, **kwargs: None)
        window._backend = SimpleNamespace(stop=lambda: None)
        window._snapshot = SimpleNamespace(state=BackendState.RUNNING)
        window._stop_button = SimpleNamespace(setEnabled=lambda enabled: None)
        window._on_snapshot = lambda value: None
        window._messagebox = MessageBox
        window._profile_store = SimpleNamespace(save=lambda value: None)
        window._remove_control_button = SimpleNamespace(setEnabled=lambda value: None)
        return window

    def test_drag_updates_editor_saved_profile_and_engine(self) -> None:
        window = self.make_window()
        window._on_element_moved("tap-a", 0.3, 0.7)
        assert (
            window._editor.profile.element("tap-a").x,
            window._editor.profile.element("tap-a").y,
        ) == (0.3, 0.7)
        assert window._profile == window._editor.profile == window.surface_profile
        assert window._engine.profile == window._profile
        assert window._editor.selected == "tap-a" and window.dirty

    def test_drag_emits_cancel_before_rebinding_pressed_key(self) -> None:
        from lynxdroid.input.events import Action

        window = self.make_window()
        window._engine.key_down("j")
        window._on_element_moved("tap-a", 0.3, 0.7)
        assert [e.action for e in window.events] == [Action.CANCEL]
        assert not window._engine.active_pointers

    def test_focus_loss_delivers_cancel(self) -> None:
        from lynxdroid.input.events import Action

        window = self.make_window()
        window._engine.key_down("j")
        window.focusOutEvent(None)
        assert [e.action for e in window.events] == [Action.CANCEL]
        assert not window._engine.active_pointers

    def test_profile_switch_delivers_release(self) -> None:
        from lynxdroid.input.events import Action

        window = self.make_window()
        window._engine.key_down("j")
        window._profile = profile(tap("tap-b", keys=("k",)))
        window._ensure_engine()
        assert [e.action for e in window.events] == [Action.CANCEL]
        assert window._engine.profile == window._profile

    def test_geometry_change_releases_but_unchanged_geometry_does_not(self) -> None:
        from lynxdroid.input.events import Action

        window = self.make_window()
        window._engine.key_down("j")
        window._refresh_surface()
        assert window.events == [] and window._engine.active_pointers
        window._surface.geometry_info = lambda: WIDE
        window._refresh_surface()
        assert [e.action for e in window.events] == [Action.CANCEL]
        assert window._engine.geometry == WIDE

    def test_surface_geometry_signal_is_connected_after_ui_construction(self) -> None:
        from pathlib import Path

        root = Path(__file__).resolve().parents[2] / "src/lynxdroid/gui"
        main = (root / "main_window.py").read_text()
        surface = (root / "overlay.py").read_text()
        assert "self._surface.geometry_changed.connect(self._refresh_surface)" in main
        assert "geometry_changed = Signal()" in surface
        for name in ("set_surface_size", "set_buffer", "set_rotation", "set_fit_mode"):
            section = surface.split(f"    def {name}(", 1)[1].split("\n    def ", 1)[0]
            assert "self.geometry_changed.emit()" in section

    def test_stopping_session_delivers_cancel(self) -> None:
        from lynxdroid.input.events import Action

        window = self.make_window()
        window._engine.key_down("j")
        window._stop_session()
        assert [event.action for event in window.events] == [Action.CANCEL]

    def test_closing_window_delivers_cancel(self) -> None:
        from lynxdroid.input.events import Action

        window = self.make_window()
        window._engine.key_down("j")
        window.closeEvent(None)
        assert [event.action for event in window.events] == [Action.CANCEL]

    def test_cancelled_switch_restores_previous_selection_and_edits(self) -> None:
        from types import SimpleNamespace

        window = self.make_window()
        window._editor.profile = window._editor.profile.move_element("tap-a", 0.2, 0.3)
        calls = []
        previous = object()
        window._profile_list = SimpleNamespace(
            blockSignals=lambda value: calls.append(("block", value)),
            setCurrentItem=lambda item: calls.append(("selected", item)),
        )
        window._on_profile_selected(object(), previous)
        assert ("selected", previous) in calls
        assert window._editor.dirty
        assert window._editor.profile.element("tap-a").x == 0.2

    def test_failed_save_blocks_profile_switch(self) -> None:
        from types import SimpleNamespace

        window = self.make_window()
        window._editor.profile = window._editor.profile.move_element("tap-a", 0.2, 0.3)
        window._messagebox.answer = window._messagebox.StandardButton.Save

        def fail_save(value):
            raise OSError("disk full")

        window._profile_store = SimpleNamespace(save=fail_save)
        assert not window._confirm_profile_change()
        assert window._editor.dirty

    def test_discard_restores_surface_and_engine(self) -> None:
        window = self.make_window()
        saved = window._editor.profile
        window._editor.profile = saved.move_element("tap-a", 0.2, 0.3)
        window._messagebox.answer = window._messagebox.StandardButton.Discard
        assert window._confirm_profile_change()
        assert window._editor.profile == window.surface_profile == saved
        assert window._engine.profile == saved
        assert not window._editor.dirty

    def test_close_can_be_cancelled_without_losing_edits(self) -> None:
        from types import SimpleNamespace

        window = self.make_window()
        window._editor.profile = window._editor.profile.move_element("tap-a", 0.2, 0.3)
        calls = []
        window.closeEvent(SimpleNamespace(ignore=lambda: calls.append("ignored")))
        assert calls == ["ignored"] and window._editor.dirty


class TestAddKeyControl:
    @pytest.mark.parametrize(
        "raw,expected",
        [
            ("A", "a"),
            ("z", "z"),
            ("5", "5"),
            ("  space  ", "Space"),
            ("LEFT", "Left"),
            ("shift", "Shift"),
            ("return", "Return"),
        ],
    )
    def test_normalises_supported_keys(self, raw, expected) -> None:
        from lynxdroid.gui.editor import normalize_control_key

        assert normalize_control_key(raw) == expected

    @pytest.mark.parametrize("raw", ["", " ", "F11", "Escape", "ё", "Ctrl+K", "not-a-key"])
    def test_unreachable_keys_are_rejected(self, raw) -> None:
        from lynxdroid.gui.editor import normalize_control_key

        with pytest.raises(ProfileError):
            normalize_control_key(raw)

    @pytest.mark.parametrize("hold,kind", [(False, ElementType.TAP), (True, ElementType.HOLD)])
    def test_adds_selects_and_marks_profile_dirty(self, hold, kind) -> None:
        from lynxdroid.gui.editor import add_key_control

        old = profile()
        editor = LayoutEditor(profile=old, saved=old)
        result = add_key_control(editor, "J", hold=hold)
        assert len(result.elements) == 1
        control = result.elements[0]
        assert control.type == kind and control.keys == ("j",)
        assert (control.x, control.y) == (0.5, 0.5)
        assert editor.selected == control.id and editor.dirty
        assert old.elements == ()
        result.validate()

    def test_key_conflicts_do_not_mutate_editor(self) -> None:
        from lynxdroid.gui.editor import add_key_control

        original = profile(tap(keys=("j",)))
        editor = LayoutEditor(profile=original, saved=original)
        with pytest.raises(ProfileError, match="already assigned"):
            add_key_control(editor, "J")
        assert editor.profile == original and not editor.dirty

    def test_dpad_direction_cannot_be_reused(self) -> None:
        from lynxdroid.gui.editor import add_key_control

        editor = LayoutEditor(profile=profile(stick()))
        with pytest.raises(ProfileError, match="already assigned"):
            add_key_control(editor, "W")

    def test_added_key_can_be_moved_saved_and_reloaded(self, tmp_path) -> None:
        from lynxdroid.core.paths import AppPaths
        from lynxdroid.gui.editor import add_key_control
        from lynxdroid.profiles.store import ProfileStore

        paths = AppPaths.resolve(tmp_path)
        paths.ensure()
        old = profile()
        editor = LayoutEditor(profile=old, saved=old)
        add_key_control(editor, "a")
        assert editor.selected is not None
        editor.profile = editor.profile.move_element(editor.selected, 0.2, 0.7)
        store = ProfileStore(paths)
        store.save(editor.profile)
        loaded = store.load(old.id)
        assert loaded.elements[0].keys == ("a",)
        assert (loaded.elements[0].x, loaded.elements[0].y) == (0.2, 0.7)

    def test_remove_does_not_change_saved_profile(self) -> None:
        from lynxdroid.gui.editor import add_key_control

        old = profile()
        editor = LayoutEditor(profile=old, saved=old)
        add_key_control(editor, "a")
        editor.remove_selected()
        assert not editor.profile.elements and not editor.dirty
        assert editor.selected is None
