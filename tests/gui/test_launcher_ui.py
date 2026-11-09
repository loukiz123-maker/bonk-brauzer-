"""Real Qt widget smoke tests; skipped when Qt runtime cannot be loaded."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

_QT_APP = None


@pytest.fixture(scope="session")
def qt_app():
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    try:
        from PySide6.QtWidgets import QApplication
    except ImportError as exc:
        pytest.skip(f"Qt runtime not available: {exc}")
    global _QT_APP
    _QT_APP = QApplication.instance() or QApplication([])
    yield _QT_APP
    _QT_APP.closeAllWindows()
    _QT_APP.processEvents()


@pytest.fixture
def window(qt_app, tmp_path: Path):
    from lynxdroid.core.paths import AppPaths
    from lynxdroid.gui.launcher import LauncherWindow

    paths = AppPaths.resolve(tmp_path)
    paths.ensure()
    widget = LauncherWindow(paths)
    widget.show()
    qt_app.processEvents()
    yield widget
    widget.close()
    qt_app.processEvents()
    import shiboken6

    shiboken6.delete(widget)


def test_navigation_and_disabled_launch(window, qt_app, monkeypatch) -> None:
    from PySide6.QtCore import QTimer
    from PySide6.QtWidgets import QApplication, QMessageBox

    opened = []

    def close_settings() -> None:
        dialog = QApplication.activeModalWidget()
        assert dialog is not None
        opened.append(dialog.windowTitle())
        dialog.close()

    assert window.findChild(type(window.centralWidget()), "sidebar") is None
    assert window.home_button.parentWidget() is window.controls_tool.parentWidget()
    assert window.library_button.parentWidget() is window.settings_tool.parentWidget()
    QTimer.singleShot(0, close_settings)
    window.settings_tool.click()
    assert opened == ["Настройки LynxDroid"]
    assert window.pages.currentIndex() == 0
    window.library_button.click()
    assert window.pages.currentIndex() == 1
    assert not window.play_button.isEnabled()
    window.controls_tool.click()
    assert window.pages.currentIndex() == 3
    editor = window.pages.currentWidget()
    editor.kind.setCurrentText("Огонь")
    editor.place(0.5, 0.5)
    assert editor.controls[0]["kind"] == "Огонь"
    monkeypatch.setattr(QMessageBox, "question", lambda *args: QMessageBox.StandardButton.Yes)
    editor.close_button.click()
    assert window.pages.currentIndex() == 0
    window.controls_tool.click()
    assert window.pages.currentIndex() == 3 and window.pages.currentWidget() is not editor
    assert not window.pages.currentWidget().controls
    window.about_button.click()
    assert window.pages.currentIndex() == 2
    window.home_button.click()
    assert window.pages.currentIndex() == 0


def test_import_search_and_remove_without_deleting_file(
    window, qt_app, tmp_path, monkeypatch
) -> None:
    from PySide6.QtWidgets import QFileDialog, QMessageBox

    apk = tmp_path / "MyGame.apk"
    apk.write_bytes(b"PK\x03\x04test")
    other = tmp_path / "Other.apk"
    other.write_bytes(b"PK\x03\x04test")
    invalid = tmp_path / "Broken.apk"
    invalid.write_bytes(b"not an apk")
    warnings = []
    monkeypatch.setattr(
        QFileDialog, "getOpenFileNames", lambda *a: ([str(apk), str(other), str(invalid)], "")
    )
    monkeypatch.setattr(QMessageBox, "warning", lambda *args: warnings.append(args[2]))
    assert window.add_tool.parentWidget() is window.controls_tool.parentWidget()
    window.add_tool.click()
    assert window.pages.currentIndex() == 1
    assert window.list.count() == 2
    assert len(warnings) == 1 and "Broken.apk" in warnings[0]
    window.search.setText("missing")
    assert window.list.count() == 0
    window.search.setText("mygame")
    assert window.list.count() == 1
    window.list.setCurrentRow(0)
    assert window.remove_button.isEnabled()
    monkeypatch.setattr(QMessageBox, "question", lambda *a: QMessageBox.StandardButton.Yes)
    window.remove_button.click()
    assert window.list.count() == 0 and apk.exists()
    window.search.clear()
    assert window.list.count() == 1 and other.exists()


def test_fullscreen_and_escape(window, qt_app) -> None:
    from PySide6.QtWidgets import QPushButton

    header = window.fullscreen_button.parentWidget()
    assert header.objectName() == "header"
    buttons = header.findChildren(QPushButton, "windowButton")
    assert len(buttons) == 3
    assert all(button.size() == window.fullscreen_button.size() for button in buttons)
    assert window.fullscreen_button.text() == "⛶"
    window.fullscreen_button.click()
    qt_app.processEvents()
    assert window.isFullScreen() and window.escape_shortcut.isEnabled()
    assert window.fullscreen_button.text() == "❐"
    window.escape_shortcut.activated.emit()
    qt_app.processEvents()
    assert not window.isFullScreen() and not window.escape_shortcut.isEnabled()


def test_selected_file_actions(window, qt_app, tmp_path, monkeypatch) -> None:
    from PySide6.QtGui import QDesktopServices
    from PySide6.QtWidgets import QMessageBox

    apk = tmp_path / "Game.apk"
    apk.write_bytes(b"PK\x03\x04test")
    window.library.add(apk)
    window._reload()
    window.library_button.click()
    assert not window.folder_button.isEnabled()
    assert not window.copy_button.isEnabled()
    window.list.setCurrentRow(0)
    assert window.folder_button.isEnabled() and window.copy_button.isEnabled()
    window.copy_button.click()
    assert qt_app.clipboard().text() == str(apk)
    opened = []
    monkeypatch.setattr(
        QDesktopServices, "openUrl", lambda url: opened.append(url.toLocalFile()) or True
    )
    window.folder_button.click()
    assert opened == [str(tmp_path)]
    warnings = []
    monkeypatch.setattr(QDesktopServices, "openUrl", lambda url: False)
    monkeypatch.setattr(QMessageBox, "warning", lambda *args: warnings.append(args[1]))
    window.folder_button.click()
    assert warnings == ["Не удалось открыть папку"]
    window.search.setText("not found")
    assert not window.folder_button.isEnabled() and not window.copy_button.isEnabled()


def test_control_editor_saves_layout_and_refuses_corrupt_data(window, qt_app, tmp_path) -> None:
    import shiboken6

    from lynxdroid.gui.control_editor import ControlEditor, load_controls

    path = tmp_path / "controls.json"
    editor = ControlEditor(path, window)
    editor.kind.setCurrentText("Огонь")
    editor.key.setText("F")
    editor.place(0.25, 0.75)
    editor.test_key.setText("F")
    editor.test_button.click()
    assert "2 событий" in editor.status.text() and "480 × 810" in editor.status.text()
    editor.save_button.click()
    assert load_controls(path) == [{"kind": "Огонь", "key": "F", "x": 0.25, "y": 0.75}]
    editor.undo()
    assert not editor.controls
    editor.save_button.click()
    assert load_controls(path) == []
    shiboken6.delete(editor)
    path.write_text("broken")
    editor = ControlEditor(path, window)
    assert not editor.save_button.isEnabled()
    editor.place(0.5, 0.5)
    editor.save()
    assert path.read_text() == "broken"
    shiboken6.delete(editor)


def test_control_editor_select_drag_edit_and_delete(window, qt_app, tmp_path) -> None:
    import shiboken6
    from PySide6.QtCore import QPoint, Qt
    from PySide6.QtTest import QTest

    from lynxdroid.gui.control_editor import ControlEditor, load_controls

    path = tmp_path / "editor.json"
    editor = ControlEditor(path, window)
    editor.show()
    qt_app.processEvents()
    editor.place(0.25, 0.25)
    canvas = editor.canvas
    start = QPoint(round(canvas.width() * 0.25), round(canvas.height() * 0.25))
    end = QPoint(round(canvas.width() * 0.7), round(canvas.height() * 0.6))
    QTest.mousePress(canvas, Qt.MouseButton.LeftButton, pos=start)
    QTest.mouseMove(canvas, end)
    QTest.mouseRelease(canvas, Qt.MouseButton.LeftButton, pos=end)
    assert len(editor.controls) == 1
    assert editor.controls[0]["x"] == pytest.approx(0.7, abs=0.01)
    assert editor.controls[0]["y"] == pytest.approx(0.6, abs=0.01)
    editor.undo_button.click()
    assert editor.controls[0]["x"] == pytest.approx(0.25)
    editor.redo_button.click()
    assert editor.controls[0]["x"] == pytest.approx(0.7, abs=0.01)
    editor.select(0)
    editor.kind.setCurrentText("Огонь")
    editor.key.setText("G")
    editor.apply_button.click()
    editor.save_button.click()
    assert load_controls(path)[0]["kind"] == "Огонь"
    assert load_controls(path)[0]["key"] == "G"
    editor.delete_button.click()
    assert not editor.controls and not editor.delete_button.isEnabled()
    editor.undo_button.click()
    assert editor.controls[0]["key"] == "G"
    editor.redo_button.click()
    assert not editor.controls
    editor.save_button.click()
    assert load_controls(path) == []
    editor.close()
    shiboken6.delete(editor)


def test_separate_apk_profiles_and_unsaved_switch(window, qt_app, tmp_path, monkeypatch) -> None:
    from PySide6.QtWidgets import QMessageBox

    from lynxdroid.gui.control_editor import load_controls
    from lynxdroid.gui.control_profiles import control_profile_path

    first = tmp_path / "one" / "Game.apk"
    second = tmp_path / "two" / "Game.apk"
    for apk in (first, second):
        apk.parent.mkdir()
        apk.write_bytes(b"PK\x03\x04test")
        window.library.add(apk)
    window.controls_tool.click()
    editor = window.pages.currentWidget()
    assert editor.profile_choice.count() == 3
    editor.kind.setCurrentText("Огонь")
    editor.key.setText("F")
    editor.place(0.2, 0.3)
    monkeypatch.setattr(QMessageBox, "question", lambda *a: QMessageBox.StandardButton.No)
    editor.profile_choice.setCurrentIndex(2)
    assert editor.profile_choice.currentIndex() == 1 and len(editor.controls) == 1
    editor.save_button.click()
    editor.profile_choice.setCurrentIndex(2)
    assert not editor.controls
    editor.key.setText("G")
    editor.place(0.7, 0.8)
    editor.save_button.click()
    editor.profile_choice.setCurrentIndex(1)
    assert editor.controls[0]["key"] == "F"
    a = control_profile_path(window.control_path.parent, first)
    b = control_profile_path(window.control_path.parent, second)
    assert a != b and load_controls(a)[0]["key"] == "F"
    assert load_controls(b)[0]["key"] == "G"
    editor.close_button.click()


def test_live_control_test_holds_and_releases_without_a_channel(window, qt_app, tmp_path) -> None:
    import shiboken6
    from PySide6.QtCore import Qt
    from PySide6.QtTest import QTest

    from lynxdroid.gui.control_editor import ControlEditor

    editor = ControlEditor(tmp_path / "controls.json", window)
    editor.show()
    qt_app.processEvents()
    editor.kind.setCurrentText("Огонь")
    editor.key.setText("F")
    editor.place(0.3, 0.4)
    editor.live_button.click()
    assert editor.test_engine is not None
    QTest.keyPress(editor.canvas, Qt.Key.Key_F)
    assert len(editor.test_engine.active_pointers) == 1
    assert "удерживается: 1" in editor.status.text()
    QTest.keyRelease(editor.canvas, Qt.Key.Key_F)
    assert not editor.test_engine.active_pointers
    assert "удерживается: 0" in editor.status.text()
    QTest.keyPress(editor.canvas, Qt.Key.Key_F)
    engine = editor.test_engine
    editor.end_live_test()
    assert not engine.active_pointers and editor.test_engine is None
    assert "Ничего не отправлено" in editor.status.text()
    editor.close()
    shiboken6.delete(editor)


def test_live_joystick_default_and_direction_keys(window, qt_app, tmp_path) -> None:
    import shiboken6
    from PySide6.QtCore import Qt
    from PySide6.QtTest import QTest

    from lynxdroid.gui.control_editor import ControlEditor

    editor = ControlEditor(tmp_path / "controls.json", window)
    editor.show()
    qt_app.processEvents()
    editor.kind.setCurrentText("Джойстик")
    assert editor.key.text() == "W,D,S,A"
    editor.place(0.5, 0.5)
    editor.live_button.click()
    QTest.keyPress(editor.canvas, Qt.Key.Key_W)
    assert editor.test_engine is not None
    assert len(editor.test_engine.active_pointers) == 1
    assert "событий: 2" in editor.status.text()
    QTest.keyRelease(editor.canvas, Qt.Key.Key_W)
    assert not editor.test_engine.active_pointers
    editor.end_live_test()
    editor.close()
    shiboken6.delete(editor)


def test_history_is_bounded_and_isolated_between_profiles(window, qt_app, tmp_path) -> None:
    import shiboken6

    from lynxdroid.gui.control_editor import ControlEditor

    editor = ControlEditor(
        tmp_path / "general.json", window, games=[("First", tmp_path / "first.json")]
    )
    editor.place(0.5, 0.5)
    for i in range(60):
        editor.move_selected(0.1 + i / 100, 0.5)
    assert len(editor._undo_stack) == 50
    editor.undo_button.click()
    assert editor.redo_button.isEnabled()
    editor.select(0)
    editor.move_selected(0.9, 0.5)
    assert not editor.redo_button.isEnabled()
    editor.save_button.click()
    editor.profile_choice.setCurrentIndex(0)
    assert not editor.controls and not editor.undo_button.isEnabled()
    editor.profile_choice.setCurrentIndex(1)
    assert len(editor.controls) == 1 and not editor.undo_button.isEnabled()
    shiboken6.delete(editor)


def test_selected_apk_opens_its_own_controls(window, qt_app, tmp_path) -> None:
    from lynxdroid.gui.control_editor import load_controls
    from lynxdroid.gui.control_profiles import control_profile_path

    first = tmp_path / "Alpha.apk"
    second = tmp_path / "Bravo.apk"
    for apk in (first, second):
        apk.write_bytes(b"PK\x03\x04test")
        window.library.add(apk)
    window._reload()
    window.library_button.click()
    assert not window.edit_controls_button.isEnabled()
    window.list.setCurrentRow(1)
    assert window.edit_controls_button.isEnabled()
    window.edit_controls_button.click()
    editor = window.pages.currentWidget()
    assert editor.profile_choice.currentIndex() == 2
    assert editor.path == control_profile_path(window.control_path.parent, second)
    editor.place(0.5, 0.5)
    editor.save_button.click()
    assert load_controls(editor.path)[0]["key"] == "F"
    editor.close_button.click()
    window.library_button.click()
    assert window._selected_path() == second
    window.controls_tool.click()
    editor = window.pages.currentWidget()
    assert editor.profile_choice.currentIndex() == 2
    assert editor.controls[0]["key"] == "F"
    editor.close_button.click()


def test_import_export_layout_is_undoable_and_does_not_save_profile(
    window, qt_app, tmp_path, monkeypatch
) -> None:
    import shiboken6
    from PySide6.QtWidgets import QFileDialog, QMessageBox

    from lynxdroid.gui.control_editor import ControlEditor, load_controls

    profile, backup = tmp_path / "profile.json", tmp_path / "backup.json"
    editor = ControlEditor(profile, window)
    editor.place(0.2, 0.3)
    editor.save_button.click()
    monkeypatch.setattr(QFileDialog, "getSaveFileName", lambda *args: (str(backup), ""))
    editor.export_button.click()
    assert load_controls(backup) == load_controls(profile)
    editor.key.setText("G")
    editor.apply_button.click()
    monkeypatch.setattr(QFileDialog, "getOpenFileName", lambda *args: (str(backup), ""))
    monkeypatch.setattr(QMessageBox, "question", lambda *args: QMessageBox.StandardButton.Yes)
    editor.import_button.click()
    assert editor.controls[0]["key"] == "F"
    assert load_controls(profile)[0]["key"] == "F"
    editor.undo_button.click()
    assert editor.controls[0]["key"] == "G"
    assert load_controls(profile)[0]["key"] == "F"
    bad = tmp_path / "bad.json"
    bad.write_text("broken")
    warnings = []
    monkeypatch.setattr(QFileDialog, "getOpenFileName", lambda *args: (str(bad), ""))
    monkeypatch.setattr(QMessageBox, "warning", lambda *args: warnings.append(args[1]))
    editor.import_button.click()
    assert warnings == ["Ошибка импорта"] and editor.controls[0]["key"] == "G"
    shiboken6.delete(editor)


def test_live_mouse_fire_press_and_release(window, qt_app, tmp_path) -> None:
    import shiboken6
    from PySide6.QtCore import QPoint, Qt
    from PySide6.QtTest import QTest

    from lynxdroid.gui.control_editor import ControlEditor

    editor = ControlEditor(tmp_path / "controls.json", window)
    editor.show()
    qt_app.processEvents()
    editor.kind.setCurrentText("Огонь")
    editor.key.setText("Mouse1")
    editor.place(0.5, 0.5)
    editor.live_button.click()
    assert editor.test_engine is not None
    QTest.mousePress(editor.canvas, Qt.MouseButton.LeftButton, pos=QPoint(100, 100))
    assert len(editor.test_engine.active_pointers) == 1
    assert "MOUSE1" in editor.status.text()
    QTest.mouseRelease(editor.canvas, Qt.MouseButton.LeftButton, pos=QPoint(100, 100))
    assert not editor.test_engine.active_pointers
    editor.end_live_test()
    editor.close()
    shiboken6.delete(editor)


def test_binding_capture_is_explicit_and_cancellable(window, qt_app, tmp_path) -> None:
    import shiboken6
    from PySide6.QtCore import Qt
    from PySide6.QtTest import QTest

    from lynxdroid.gui.control_editor import ControlEditor

    editor = ControlEditor(tmp_path / "controls.json", window)
    editor.show()
    qt_app.processEvents()
    button = editor.capture_button
    button.click()
    QTest.keyClick(button, Qt.Key.Key_G)
    assert editor.key.text() == "G" and not button.armed
    button.click()
    QTest.keyClick(button, Qt.Key.Key_Escape)
    assert editor.key.text() == "G" and not button.armed
    button.click()
    QTest.mouseClick(button, Qt.MouseButton.RightButton)
    assert editor.key.text() == "Mouse2" and not button.armed
    button.click()
    editor.key.setFocus()
    qt_app.processEvents()
    assert not button.armed
    editor.kind.setCurrentText("Джойстик")
    assert not button.isEnabled()
    assert not editor.controls
    editor.close()
    shiboken6.delete(editor)


def test_invalid_utf8_profiles_and_library_are_protected(window, qt_app, tmp_path) -> None:
    import shiboken6

    from lynxdroid.gui.control_editor import ControlEditor

    window.library.path.write_bytes(b"\xff")
    window._reload()
    assert window._library_error
    assert window.library.path.read_bytes() == b"\xff"
    path = tmp_path / "bad.json"
    path.write_bytes(b"\xff")
    editor = ControlEditor(path, window)
    assert editor.read_only and not editor.save_button.isEnabled()
    assert path.read_bytes() == b"\xff"
    path.write_text('{"schema": 1, "controls": []}', encoding="utf-8")
    editor.reload_button.click()
    assert not editor.read_only and editor.save_button.isEnabled()
    shiboken6.delete(editor)


def test_live_focus_and_escape_do_not_restart_or_hide_editor(window, qt_app) -> None:
    from PySide6.QtCore import QEvent, Qt
    from PySide6.QtGui import QFocusEvent, QKeyEvent
    from PySide6.QtWidgets import QApplication

    window.open_controls()
    editor = window.pages.currentWidget()
    editor.kind.setCurrentText("Огонь")
    editor.key.setText("1")
    editor.place(0.5, 0.5)
    editor.toggle_live_test()
    engine = editor.test_engine
    # Same native scan code, even when Shift changes the release symbol.
    press = QKeyEvent(
        QEvent.Type.KeyPress, Qt.Key.Key_1, Qt.KeyboardModifier.NoModifier, 10, 0, 0, "1"
    )
    release = QKeyEvent(
        QEvent.Type.KeyRelease, Qt.Key.Key_Exclam, Qt.KeyboardModifier.ShiftModifier, 10, 0, 0, "!"
    )
    QApplication.sendEvent(editor.canvas, press)
    assert engine.active_pointers
    QApplication.sendEvent(editor.canvas, release)
    assert not engine.active_pointers
    QApplication.sendEvent(editor.canvas, press)
    QApplication.sendEvent(editor.canvas, QFocusEvent(QEvent.Type.FocusOut))
    assert editor.test_engine is engine and not engine.active_pointers
    editor.live_button.click()
    assert editor.test_engine is None
    editor.toggle_live_test()
    window.toggle_fullscreen()
    window._escape()
    assert editor.test_engine is None and window.isFullScreen() and editor.isVisible()
    window._escape()
    assert not window.isFullScreen()
    editor.reject()
    assert window.pages.currentWidget() is editor and editor.isVisible()
    editor.controls.clear()


def test_edit_stops_old_live_engine_and_releases_contacts(window, qt_app, tmp_path) -> None:
    import shiboken6

    from lynxdroid.gui.control_editor import ControlEditor

    editor = ControlEditor(tmp_path / "controls.json", window)
    editor.kind.setCurrentText("Огонь")
    editor.key.setText("F")
    editor.place(0.5, 0.5)
    editor.toggle_live_test()
    engine = editor.test_engine
    assert engine is not None
    editor.live_key("f", down=True)
    assert engine.active_pointers
    editor.key.setText("G")
    editor.apply_selected()
    assert editor.test_engine is None
    assert not engine.active_pointers
    assert editor.live_button.text() == "Тест клавиш"
    assert editor.controls[0]["key"] == "G"
    editor.toggle_live_test()
    assert editor.test_engine is not None
    editor.live_key("f", down=True)
    assert not editor.test_engine.active_pointers
    editor.live_key("g", down=True)
    assert editor.test_engine.active_pointers
    editor.reset()
    assert editor.test_engine is None
    shiboken6.delete(editor)


def test_two_editors_do_not_overwrite_each_others_profiles(
    window, qt_app, tmp_path, monkeypatch
) -> None:
    import shiboken6
    from PySide6.QtWidgets import QMessageBox

    from lynxdroid.gui.control_editor import ControlEditor, load_controls

    path = tmp_path / "game.json"
    first, second = ControlEditor(path, window), ControlEditor(path, window)
    first.place(0.2, 0.3)
    second.key.setText("G")
    second.place(0.7, 0.8)
    first.save()
    warnings = []
    monkeypatch.setattr(QMessageBox, "warning", lambda *args: warnings.append(args[2]))
    second.save()
    assert warnings and "изменилась" in warnings[0]
    assert load_controls(path)[0]["key"] == "F"
    assert second.controls[0]["key"] == "G"
    assert second._snapshot() != second._baseline
    first.key.setText("H")
    first.apply_selected()
    first.save()
    assert load_controls(path)[0]["key"] == "H"
    monkeypatch.setattr(QMessageBox, "question", lambda *args: QMessageBox.StandardButton.No)
    second.reload_button.click()
    assert second.controls[0]["key"] == "G" and second._snapshot() != second._baseline
    monkeypatch.setattr(QMessageBox, "question", lambda *args: QMessageBox.StandardButton.Yes)
    second.reload_button.click()
    assert second.controls[0]["key"] == "H" and second._snapshot() == second._baseline
    second.select(0)
    second.key.setText("J")
    second.apply_selected()
    second.save()
    assert load_controls(path)[0]["key"] == "J"
    shiboken6.delete(first)
    shiboken6.delete(second)


def test_copy_saved_profile_is_confirmed_undoable_and_never_auto_saves(
    window, qt_app, tmp_path, monkeypatch
) -> None:
    import shiboken6
    from PySide6.QtWidgets import QMessageBox

    from lynxdroid.gui.control_editor import ControlEditor, load_controls

    alpha, beta = tmp_path / "alpha.json", tmp_path / "beta.json"
    editor = ControlEditor(
        tmp_path / "general.json", window, games=[("Alpha", alpha), ("Beta", beta)]
    )
    editor.place(0.2, 0.3)
    editor.save()
    editor.profile_choice.setCurrentIndex(2)
    editor.key.setText("G")
    editor.place(0.7, 0.8)
    editor.copy_source.setCurrentIndex(editor.copy_source.findData(str(alpha)))
    monkeypatch.setattr(QMessageBox, "question", lambda *args: QMessageBox.StandardButton.No)
    editor.copy_button.click()
    assert editor.controls[0]["key"] == "G"
    monkeypatch.setattr(QMessageBox, "question", lambda *args: QMessageBox.StandardButton.Yes)
    editor.copy_button.click()
    assert editor.controls[0]["key"] == "F" and not beta.exists()
    assert load_controls(alpha)[0]["key"] == "F"
    editor.undo()
    assert editor.controls[0]["key"] == "G"
    alpha.write_bytes(b"\xff")
    warnings = []
    monkeypatch.setattr(QMessageBox, "warning", lambda *args: warnings.append(args[2]))
    editor.copy_button.click()
    assert warnings and editor.controls[0]["key"] == "G" and not beta.exists()
    assert alpha.read_bytes() == b"\xff"
    shiboken6.delete(editor)


def test_control_list_selects_overlapping_items_and_tracks_edits(window, qt_app, tmp_path) -> None:
    import shiboken6

    from lynxdroid.gui.control_editor import ControlEditor

    path = tmp_path / "controls.json"
    editor = ControlEditor(path, window)
    editor.key.setText("F")
    editor.place(0.5, 0.5)
    editor.key.setText("G")
    editor.place(0.5, 0.5)
    assert editor.control_list.count() == 2
    editor.control_list.setCurrentRow(0)
    assert editor.selected == 0 and editor.key.text() == "F"
    editor.key.setText("H")
    editor.apply_selected()
    assert "H" in editor.control_list.item(0).text()
    editor.move_selected(0.25, 0.75)
    assert "25%, 75%" in editor.control_list.item(0).text()
    editor.delete_selected()
    assert editor.control_list.count() == 1 and "G" in editor.control_list.item(0).text()
    editor.undo()
    assert editor.control_list.count() == 2 and editor.selected is None
    assert not path.exists()
    editor.save()
    editor.reload_profile()
    assert editor.control_list.count() == 2 and "H" in editor.control_list.item(0).text()
    shiboken6.delete(editor)


def test_keyboard_nudge_clamps_and_supports_undo(window, qt_app, tmp_path) -> None:
    import shiboken6
    from PySide6.QtCore import Qt
    from PySide6.QtTest import QTest

    from lynxdroid.gui.control_editor import ControlEditor

    path = tmp_path / "controls.json"
    editor = ControlEditor(path, window)
    editor.kind.setCurrentText("Огонь")
    editor.place(0.98, 0.5)
    editor.toggle_live_test()
    engine = editor.test_engine
    assert engine is not None
    editor.live_key("f", down=True)
    assert engine.active_pointers
    QTest.keyClick(editor.control_list, Qt.Key.Key_Right, Qt.KeyboardModifier.AltModifier)
    assert editor.controls[0]["x"] == 0.99
    assert editor.test_engine is None and not engine.active_pointers
    QTest.keyClick(
        editor.control_list,
        Qt.Key.Key_Right,
        Qt.KeyboardModifier.AltModifier | Qt.KeyboardModifier.ShiftModifier,
    )
    assert editor.controls[0]["x"] == 1.0
    QTest.keyClick(editor.control_list, Qt.Key.Key_Right, Qt.KeyboardModifier.AltModifier)
    assert editor.controls[0]["x"] == 1.0
    editor.undo()
    assert editor.controls[0]["x"] == 0.99
    assert "99%, 50%" in editor.control_list.item(0).text()
    assert not path.exists()
    shiboken6.delete(editor)


def test_double_tap_offline_timer_and_focus_loss(window, qt_app, tmp_path) -> None:
    import shiboken6
    from PySide6.QtCore import Qt
    from PySide6.QtTest import QTest

    from lynxdroid.gui.control_editor import ControlEditor

    editor = ControlEditor(tmp_path / "controls.json", window)
    editor.show()
    qt_app.processEvents()
    editor.kind.setCurrentText("Двойное нажатие")
    editor.key.setText("F")
    editor.place(0.5, 0.5)
    editor.test_key.setText("F")
    editor.test_button.click()
    assert "4 событий" in editor.status.text()
    editor.live_button.click()
    QTest.keyClick(editor.canvas, Qt.Key.Key_F)
    assert editor.test_timer.isActive()
    QTest.qWait(200)
    assert "второе нажатие" in editor.status.text()
    assert not editor.test_timer.isActive()
    QTest.keyClick(editor.canvas, Qt.Key.Key_F)
    assert editor.test_timer.isActive()
    editor.release_live_inputs()
    assert not editor.test_timer.isActive()
    status = editor.status.text()
    QTest.qWait(200)
    assert editor.status.text() == status
    editor.end_live_test()
    shiboken6.delete(editor)


def test_offline_camera_and_drag_mouse_motion(window, qt_app, tmp_path) -> None:
    import shiboken6
    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtGui import QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication

    from lynxdroid.gui.control_editor import ControlEditor

    editor = ControlEditor(tmp_path / "controls.json", window)
    editor.show()
    qt_app.processEvents()
    editor.kind.setCurrentText("Камера")
    editor.key.setText("C")
    editor.place(0.5, 0.5)
    editor.live_button.click()
    QTest.keyClick(editor.canvas, Qt.Key.Key_C)
    assert editor.test_engine is not None
    for position in (QPointF(100, 100), QPointF(120, 110)):
        event = QMouseEvent(
            QEvent.Type.MouseMove,
            position,
            position,
            position,
            Qt.MouseButton.NoButton,
            Qt.MouseButton.NoButton,
            Qt.KeyboardModifier.NoModifier,
        )
        QApplication.sendEvent(editor.canvas, event)
    assert next(iter(editor.test_engine.active_pointers.values())).x > 960
    QTest.keyClick(editor.canvas, Qt.Key.Key_C)
    assert not editor.test_engine.active_pointers
    editor.end_live_test()
    shiboken6.delete(editor)
