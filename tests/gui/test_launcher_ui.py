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


def test_navigation_and_disabled_launch(window, qt_app) -> None:
    from PySide6.QtCore import QTimer
    from PySide6.QtWidgets import QApplication

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
