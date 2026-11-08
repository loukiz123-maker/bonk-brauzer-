"""Application bootstrap for the Qt shell.

Fails loudly and usefully when Qt is missing: on a headless CI machine the
tests still exercise ``gui.viewmodel`` and ``gui.overlay`` helpers instead of
importing live widgets.
"""

from __future__ import annotations

import importlib.util
import sys

from lynxdroid.core.logging_setup import configure as configure_logging
from lynxdroid.core.paths import AppPaths
from lynxdroid.settings import SettingsStore
from lynxdroid.version import version_string


def _qt_module_present() -> bool:
    """True when the PySide6 distribution itself is importable."""
    return importlib.util.find_spec("PySide6") is not None


def _load_qt_modules() -> None:
    """Import the three Qt modules the shell needs (import is the load test)."""
    from PySide6 import QtCore, QtGui, QtWidgets  # noqa: F401


def qt_probe() -> tuple[bool, str]:
    """Report whether Qt widgets can actually be *loaded*, not just located.

    ``import PySide6`` succeeds on a machine that has the wheel installed but
    is missing Qt's own shared libraries (libGL, libEGL, libxkbcommon, ...).
    Claiming the GUI is available there would be a lie, so the probe imports
    the widget module, which is what triggers the loader.
    """
    if not _qt_module_present():
        return False, "PySide6 is not installed"
    try:
        _load_qt_modules()
    except ImportError as exc:
        # The wheel is installed but Qt's own shared objects are not on the
        # loader path. This is the common "installed pip package, broken
        # system" case, and it must not be reported as working.
        return False, f"PySide6 is installed but its Qt libraries are not loadable: {exc}"
    except OSError as exc:  # pragma: no cover - depends on the machine
        return False, f"PySide6 is installed but its Qt libraries are not loadable: {exc}"
    return True, "PySide6 and the Qt widget libraries are available"


def _import_qt() -> bool:
    ok, reason = qt_probe()
    if not ok:
        raise ImportError(f"{reason}") from None
    return True


def run_gui(*, paths: AppPaths | None = None, backend_kind: str | None = None) -> int:
    """Start the desktop application; returns a process exit code."""
    _import_qt()

    from PySide6.QtCore import Qt
    from PySide6.QtWidgets import QApplication

    paths = paths or AppPaths.resolve()
    paths.ensure()
    settings_store = SettingsStore(paths)
    configure_logging(paths.log_file, console_level=None)

    QApplication.setAttribute(Qt.ApplicationAttribute.AA_DontUseNativeDialogs, False)
    app = QApplication.instance() or QApplication(sys.argv[:1])
    app.setApplicationName("LynxDroid")
    app.setApplicationVersion(version_string())
    app.setOrganizationName("LynxDroid")
    if hasattr(app, "setDesktopFileName"):
        app.setDesktopFileName("lynxdroid")

    from lynxdroid.gui.main_window import MainWindow

    window = MainWindow(paths=paths, settings_store=settings_store, backend_kind=backend_kind)
    window.show()
    return app.exec()


def main() -> int:  # pragma: no cover - process entry point
    return run_gui()


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
