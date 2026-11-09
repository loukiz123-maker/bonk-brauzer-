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


def test_navigation_and_disabled_launch(window, qt_app) -> None:
    assert window.pages.currentIndex() == 0
    window.library_button.click()
    assert window.pages.currentIndex() == 1
    assert not window.play_button.isEnabled()
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
    monkeypatch.setattr(QFileDialog, "getOpenFileName", lambda *a: (str(apk), ""))
    window.add_apk()
    assert window.pages.currentIndex() == 1
    assert window.list.count() == 1
    window.search.setText("missing")
    assert window.list.count() == 0
    window.search.setText("mygame")
    assert window.list.count() == 1
    window.list.setCurrentRow(0)
    assert window.remove_button.isEnabled()
    monkeypatch.setattr(QMessageBox, "question", lambda *a: QMessageBox.StandardButton.Yes)
    window.remove_button.click()
    assert window.list.count() == 0 and apk.exists()


def test_fullscreen_and_escape(window, qt_app) -> None:
    window.toggle_fullscreen()
    qt_app.processEvents()
    assert window.isFullScreen() and window.escape_shortcut.isEnabled()
    window.escape_shortcut.activated.emit()
    qt_app.processEvents()
    assert not window.isFullScreen() and not window.escape_shortcut.isEnabled()
