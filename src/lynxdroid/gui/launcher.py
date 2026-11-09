"""Standalone desktop launcher. It does not use Waydroid or launch Android yet."""

from __future__ import annotations

from pathlib import Path
from typing import cast

from PySide6.QtCore import Qt, QUrl
from PySide6.QtGui import QCloseEvent, QDesktopServices, QKeySequence, QShortcut
from PySide6.QtWidgets import (
    QApplication,
    QDialog,
    QFileDialog,
    QFrame,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QStackedWidget,
    QVBoxLayout,
    QWidget,
)

from lynxdroid.core.errors import LynxError
from lynxdroid.core.paths import AppPaths
from lynxdroid.gui.control_editor import ControlEditor
from lynxdroid.gui.control_profiles import control_profile_path
from lynxdroid.gui.library import Library, LibraryEntry, filter_entries

STYLE = """
QWidget { background: #0b1019; color: #e9f0f8;
    font-family: 'Inter', 'DejaVu Sans'; font-size: 13px; }
QFrame#header { background: #101823; border-bottom: 1px solid #253344; }
QFrame#sidebar { background: #0d1520; border-right: 1px solid #253344; }
QFrame#tools { background: #10151e; border-left: 1px solid #253344; }
QPushButton#tool { background: transparent; border: 0; border-radius: 6px;
    padding: 10px 5px; font-size: 19px; color: #ffffff; }
QPushButton#tool:hover, QPushButton#tool:checked { background: #19313c; color: #ffffff; }
QFrame#card { background: #141f2d; border: 1px solid #293b4c; border-radius: 14px; }
QLabel#brand { color: #f4f9ff; font-size: 19px; font-weight: 800; }
QLabel#title { font-size: 30px; font-weight: 800; color: #f7fbff; }
QLabel#section { font-size: 20px; font-weight: 700; }
QLabel#muted { color: #a5b5c7; }
QLabel#badge { color: #81e4cf; background: #173638; border-radius: 9px; padding: 6px 10px; }
QPushButton { color: #ffffff; background: #1b2a3a; border: 1px solid #30475b;
    border-radius: 9px; padding: 10px 16px; font-weight: 600; }
QPushButton:hover:enabled { background: #263d50; border-color: #65d5d0; }
QPushButton#windowButton { padding: 0; }
QPushButton:disabled { color: #718092; background: #18212d; border-color: #26313f; }
QPushButton#primary { background: #146d70; color: #ffffff; border: 0; font-weight: 800; }
QPushButton#primary:hover { background: #20848a; }
QPushButton#nav { text-align: left; background: transparent; border: 0; padding: 13px 15px; }
QPushButton#nav:checked { background: #19313c; color: #79e8da; }
QLineEdit, QListWidget { background: #111b28; border: 1px solid #293b4c;
    border-radius: 9px; padding: 10px; selection-background-color: #275267; }
QListWidget::item { padding: 12px; border-bottom: 1px solid #253344; }
QLabel#heroText { background: transparent; color: #f7fbff; }
"""


def label(text: str, name: str = "") -> QLabel:
    widget = QLabel(text)
    if name:
        widget.setObjectName(name)
    widget.setWordWrap(True)
    return widget


class LauncherWindow(QMainWindow):
    """Launcher UI with a local library; the future engine is explicitly unavailable."""

    def __init__(self, paths: AppPaths) -> None:
        super().__init__()
        self.library = Library(paths.data / "launcher-library.json")
        self.control_path = paths.data / "launcher-controls.json"
        self.entries: list[LibraryEntry] = []
        self._library_error = ""
        self.setWindowTitle("LynxDroid")
        self.setMinimumSize(860, 560)
        self.setStyleSheet(STYLE)
        self._build()
        self.fullscreen_shortcut = QShortcut(QKeySequence("F11"), self)
        self.fullscreen_shortcut.activated.connect(self.toggle_fullscreen)
        self.escape_shortcut = QShortcut(QKeySequence("Escape"), self)
        self.escape_shortcut.setEnabled(False)
        self.escape_shortcut.activated.connect(self._escape)
        self._reload()

    def _build(self) -> None:
        root = QWidget()
        self.setCentralWidget(root)
        layout = QVBoxLayout(root)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        header = QFrame()
        header.setObjectName("header")
        top = QHBoxLayout(header)
        top.setContentsMargins(24, 12, 20, 12)
        top.addWidget(label("◆  LYNXDROID", "brand"))
        top.addStretch()
        self.fullscreen_button = QPushButton("⛶")
        self.fullscreen_button.setToolTip("Полный экран (F11)")
        self.fullscreen_button.setAccessibleName("Полный экран")
        self.fullscreen_button.clicked.connect(self.toggle_fullscreen)
        top.addWidget(self.fullscreen_button)
        minimize = QPushButton("—")
        minimize.setToolTip("Свернуть окно")
        minimize.clicked.connect(self.showMinimized)
        top.addWidget(minimize)
        close = QPushButton("✕")
        close.setToolTip("Закрыть лаунчер")
        close.clicked.connect(self.close)
        top.addWidget(close)
        for button in (self.fullscreen_button, minimize, close):
            button.setObjectName("windowButton")
            button.setFixedSize(38, 36)
        layout.addWidget(header)

        body = QHBoxLayout()
        body.setSpacing(0)
        self.pages = QStackedWidget()
        self.pages.addWidget(self._home_page())
        self.pages.addWidget(self._library_page())
        self.pages.addWidget(self._about_page())
        body.addWidget(self.pages, 1)
        tools = QFrame()
        tools.setObjectName("tools")
        tools.setFixedWidth(54)
        tools_layout = QVBoxLayout(tools)
        tools_layout.setContentsMargins(5, 12, 5, 12)
        self.home_button = QPushButton("⌂")
        self.library_button = QPushButton("▦")
        self.about_button = QPushButton("ⓘ")
        for button, title in (
            (self.home_button, "Главная"),
            (self.library_button, "Библиотека"),
            (self.about_button, "О проекте"),
        ):
            button.setObjectName("tool")
            button.setToolTip(title)
            button.setAccessibleName(title)
            button.setCheckable(True)
            tools_layout.addWidget(button)
        self.home_button.clicked.connect(lambda: self._show_page(0))
        self.library_button.clicked.connect(lambda: self._show_page(1))
        self.about_button.clicked.connect(lambda: self._show_page(2))
        self.controls_tool = QPushButton("⌨")
        self.controls_tool.setObjectName("tool")
        self.controls_tool.setToolTip("Редактор управления")
        self.controls_tool.setAccessibleName("Редактор управления")
        self.controls_tool.clicked.connect(self.open_controls)
        tools_layout.addWidget(self.controls_tool)
        self.add_tool = QPushButton("＋")
        self.add_tool.setObjectName("tool")
        self.add_tool.setToolTip("Добавить APK")
        self.add_tool.setAccessibleName("Добавить APK")
        self.add_tool.clicked.connect(self.add_apk)
        tools_layout.addWidget(self.add_tool)
        tools_layout.addStretch()
        self.settings_tool = QPushButton("⚙")
        self.settings_tool.setObjectName("tool")
        self.settings_tool.setToolTip("Настройки")
        self.settings_tool.setAccessibleName("Настройки")
        self.settings_tool.clicked.connect(self.open_settings)
        tools_layout.addWidget(self.settings_tool)
        body.addWidget(tools)
        layout.addLayout(body, 1)
        self._show_page(0)

    def _home_page(self) -> QWidget:
        page = QWidget()
        image = (Path(__file__).resolve().parents[1] / "data" / "desktop-bg.jpg").as_posix()
        page.setObjectName("desktop")
        page.setStyleSheet(f"QWidget#desktop {{ border-image: url('{image}'); }}")
        return page

    def open_settings(self) -> None:
        dialog = QDialog(self)
        dialog.setWindowTitle("Настройки LynxDroid")
        dialog.resize(600, 320)
        layout = QVBoxLayout(dialog)
        layout.addWidget(label("Настройки", "section"))
        layout.addWidget(
            label(
                "Движок Android ещё не создан. Разрешение, FPS, профиль телефона "
                "и настройки игр пока недоступны.",
                "muted",
            )
        )
        layout.addWidget(label("Полный экран: F11 • Выход: Esc", "muted"))
        layout.addStretch()
        close = QPushButton("Закрыть")
        close.clicked.connect(dialog.accept)
        layout.addWidget(close)
        dialog.exec()

    def open_controls(self) -> None:
        if self.pages.currentIndex() == 3:
            return
        if self.pages.count() > 3:
            previous = self.pages.widget(3)
            if previous is not None:
                self.pages.removeWidget(previous)
                previous.deleteLater()
        chosen = self._selected_path()
        self._reload()
        games = [
            (
                f"{entry.title} — {entry.path}",
                control_profile_path(self.control_path.parent, entry.path),
            )
            for entry in self.entries
        ]
        initial = next((i + 1 for i, entry in enumerate(self.entries) if entry.path == chosen), 1)
        editor = ControlEditor(self.control_path, self.pages, games=games, initial_profile=initial)
        editor.setWindowFlags(Qt.WindowType.Widget)
        editor.close_button.clicked.connect(lambda: self._show_page(0))
        self.pages.addWidget(editor)
        self._show_page(3)

    def _library_page(self) -> QWidget:
        page = QWidget()
        box = QVBoxLayout(page)
        box.setContentsMargins(34, 30, 34, 24)
        box.setSpacing(16)
        box.addWidget(label("Библиотека", "title"))
        box.addWidget(
            label("Здесь хранятся ссылки на выбранные APK. Это не установка игр.", "muted")
        )
        controls = QHBoxLayout()
        self.search = QLineEdit()
        self.search.setPlaceholderText("Поиск по имени файла…")
        self.search.textChanged.connect(self._render_library)
        controls.addWidget(self.search, 1)
        add = QPushButton("＋  Добавить APK")
        add.setObjectName("primary")
        add.clicked.connect(self.add_apk)
        controls.addWidget(add)
        refresh = QPushButton("Обновить")
        refresh.clicked.connect(self._reload)
        controls.addWidget(refresh)
        box.addLayout(controls)
        self.list = QListWidget()
        self.list.currentItemChanged.connect(self._selection_changed)
        box.addWidget(self.list, 1)
        self.library_note = label("", "muted")
        box.addWidget(self.library_note)
        actions = QHBoxLayout()
        self.play_button = QPushButton("▶  Запустить")
        self.play_button.setEnabled(False)
        self.play_button.setToolTip("Собственный Android-движок ещё не реализован")
        actions.addWidget(self.play_button)
        self.edit_controls_button = QPushButton("Настроить управление")
        self.edit_controls_button.setEnabled(False)
        self.edit_controls_button.clicked.connect(self.open_controls)
        actions.addWidget(self.edit_controls_button)
        self.remove_button = QPushButton("Удалить из библиотеки")
        self.remove_button.setEnabled(False)
        self.remove_button.clicked.connect(self.remove_selected)
        actions.addWidget(self.remove_button)
        actions.addStretch()
        box.addLayout(actions)
        file_actions = QHBoxLayout()
        self.folder_button = QPushButton("Открыть папку")
        self.folder_button.setToolTip("Открыть папку с выбранным APK")
        self.folder_button.clicked.connect(self.open_selected_folder)
        file_actions.addWidget(self.folder_button)
        self.copy_button = QPushButton("Копировать путь")
        self.copy_button.clicked.connect(self.copy_selected_path)
        file_actions.addWidget(self.copy_button)
        file_actions.addStretch()
        box.addLayout(file_actions)
        return page

    def _about_page(self) -> QWidget:
        page = QWidget()
        box = QVBoxLayout(page)
        box.setContentsMargins(34, 30, 34, 24)
        box.setSpacing(16)
        box.addWidget(label("О проекте", "title"))
        card = QFrame()
        card.setObjectName("card")
        content = QVBoxLayout(card)
        content.setContentsMargins(24, 24, 24, 24)
        content.addWidget(label("LynxDroid · самостоятельный лаунчер", "section"))
        content.addWidget(
            label(
                "Лаунчер не использует Waydroid. Сейчас доступны библиотека APK, "
                "поиск и управление окном. Запуск Android-приложений ещё не реализован.",
                "muted",
            )
        )
        content.addWidget(label("Полный экран: F11  •  Выход из полного экрана: Esc", "muted"))
        box.addWidget(card)
        box.addStretch()
        return page

    def _show_page(self, index: int) -> None:
        if self.pages.currentIndex() == 3 and index != 3:
            current = self.pages.currentWidget()
            if isinstance(current, ControlEditor):
                if not current.can_discard():
                    return
                current.end_live_test()
        self.pages.setCurrentIndex(index)
        for i, button in enumerate((self.home_button, self.library_button, self.about_button)):
            button.setChecked(i == index)

    def closeEvent(self, event: QCloseEvent) -> None:
        if self.pages.currentIndex() == 3:
            current = self.pages.currentWidget()
            if isinstance(current, ControlEditor) and not current.can_discard():
                event.ignore()
                return
        super().closeEvent(event)

    def _reload(self) -> None:
        chosen = self._selected_path()
        try:
            self.entries = self.library.load()
            self._library_error = ""
        except (OSError, LynxError) as exc:
            self.entries = []
            self._library_error = str(exc)
        self._render_library()
        if chosen is not None:
            for row in range(self.list.count()):
                if self.list.item(row).data(Qt.ItemDataRole.UserRole) == str(chosen):
                    self.list.setCurrentRow(row)
                    break

    def _render_library(self) -> None:
        self.list.clear()
        for entry in filter_entries(self.entries, self.search.text()):
            status = "APK-файл" if entry.available else "Файл недоступен"
            item = QListWidgetItem(f"{entry.title}\n{status}  ·  {entry.path}")
            item.setData(Qt.ItemDataRole.UserRole, str(entry.path))
            self.list.addItem(item)
        self.library_note.setText(
            self._library_error
            or (
                "Выберите APK для удаления из списка."
                if self.entries
                else "Библиотека пуста — добавьте APK, чтобы сохранить ссылку на файл."
            )
        )
        self._selection_changed()

    def _selection_changed(self, *args: object) -> None:
        selected = self.list.currentItem() is not None
        self.remove_button.setEnabled(selected and not self._library_error)
        self.edit_controls_button.setEnabled(selected and not self._library_error)
        self.folder_button.setEnabled(selected)
        self.copy_button.setEnabled(selected)

    def _selected_path(self) -> Path | None:
        item = cast("QListWidgetItem | None", self.list.currentItem())
        if item is None:
            return None
        return Path(str(item.data(Qt.ItemDataRole.UserRole)))

    def copy_selected_path(self) -> None:
        path = self._selected_path()
        if path is not None:
            QApplication.clipboard().setText(str(path))

    def open_selected_folder(self) -> None:
        path = self._selected_path()
        if path is None:
            return
        if not path.parent.is_dir() or not QDesktopServices.openUrl(
            QUrl.fromLocalFile(str(path.parent))
        ):
            QMessageBox.warning(
                self,
                "Не удалось открыть папку",
                "Папка недоступна или файловый менеджер не запущен.",
            )

    def add_apk(self) -> None:
        paths, _ = QFileDialog.getOpenFileNames(self, "Выберите APK", "", "APK (*.apk *.APK)")
        if not paths:
            return
        failures: list[str] = []
        for path in paths:
            try:
                self.entries = self.library.add(Path(path))
            except (OSError, ValueError, LynxError) as exc:
                failures.append(f"{Path(path).name}: {exc}")
        # Reload even after partial failure so the view reflects what was saved.
        self._reload()
        self._show_page(1)
        if failures:
            details = "\n".join(failures[:10])
            if len(failures) > 10:
                details += f"\nИ ещё ошибок: {len(failures) - 10}"
            QMessageBox.warning(
                self, "Не все APK добавлены", f"Некоторые файлы не добавлены:\n{details}"
            )

    def remove_selected(self) -> None:
        item = cast("QListWidgetItem | None", self.list.currentItem())
        if item is None:
            return
        answer = QMessageBox.question(
            self,
            "Удалить из библиотеки?",
            "Будет удалена только ссылка. Сам APK останется на диске.",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if answer != QMessageBox.StandardButton.Yes:
            return
        try:
            self.entries = self.library.remove(Path(item.data(Qt.ItemDataRole.UserRole)))
        except (OSError, LynxError) as exc:
            QMessageBox.warning(self, "Не удалось удалить", str(exc))
            return
        self._render_library()

    def _escape(self) -> None:
        editor = self.pages.currentWidget()
        if isinstance(editor, ControlEditor) and editor.capture_button.armed:
            editor.capture_button.cancel()
        elif isinstance(editor, ControlEditor) and editor.test_engine is not None:
            editor.end_live_test()
        elif self.isFullScreen():
            self.toggle_fullscreen()

    def toggle_fullscreen(self) -> None:
        if self.isFullScreen():
            self.showNormal()
        else:
            self.showFullScreen()
        self.fullscreen_button.setText("❐" if self.isFullScreen() else "⛶")
        title = "Выйти из полного экрана (Esc)" if self.isFullScreen() else "Полный экран (F11)"
        self.fullscreen_button.setToolTip(title)
        self.fullscreen_button.setAccessibleName(title)
        self.escape_shortcut.setEnabled(self.isFullScreen())
