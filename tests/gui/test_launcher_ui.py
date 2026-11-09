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
