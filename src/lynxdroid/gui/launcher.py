"""Standalone desktop launcher. It does not use Waydroid or launch Android yet."""

from __future__ import annotations

from pathlib import Path
from typing import cast

from PySide6.QtCore import Qt, QUrl
from PySide6.QtGui import QDesktopServices, QKeySequence, QShortcut
from PySide6.QtWidgets import (
    QApplication,
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
from lynxdroid.gui.library import Library, LibraryEntry, filter_entries

STYLE = """
QWidget { background: #0b1019; color: #e9f0f8;
    font-family: 'Inter', 'DejaVu Sans'; font-size: 13px; }
QFrame#header { background: #101823; border-bottom: 1px solid #253344; }
QFrame#sidebar { background: #0d1520; border-right: 1px solid #253344; }
QFrame#card { background: #141f2d; border: 1px solid #293b4c; border-radius: 14px; }
QLabel#brand { color: #f4f9ff; font-size: 19px; font-weight: 800; }
QLabel#title { font-size: 30px; font-weight: 800; color: #f7fbff; }
QLabel#section { font-size: 20px; font-weight: 700; }
QLabel#muted { color: #a5b5c7; }
QLabel#badge { color: #81e4cf; background: #173638; border-radius: 9px; padding: 6px 10px; }
QPushButton { background: #1b2a3a; border: 1px solid #30475b;
    border-radius: 9px; padding: 10px 16px; font-weight: 600; }
QPushButton:hover:enabled { background: #263d50; border-color: #65d5d0; }
QPushButton:disabled { color: #718092; background: #18212d; border-color: #26313f; }
QPushButton#primary { background: #58d7cb; color: #071b1e; border: 0; font-weight: 800; }
QPushButton#primary:hover { background: #83eee2; }
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
        self.escape_shortcut.activated.connect(self.toggle_fullscreen)
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
        top.addSpacing(14)
        top.addWidget(label("LAUNCHER  /  EARLY ACCESS", "muted"))
        top.addStretch()
        top.addWidget(label("ДВИЖОК В РАЗРАБОТКЕ", "badge"))
        self.fullscreen_button = QPushButton("⛶  Полный экран")
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
        layout.addWidget(header)

        body = QHBoxLayout()
        body.setSpacing(0)
        sidebar = QFrame()
        sidebar.setObjectName("sidebar")
        sidebar.setFixedWidth(205)
        nav = QVBoxLayout(sidebar)
        nav.setContentsMargins(14, 28, 14, 20)
        nav.addWidget(label("МЕНЮ", "muted"))
        self.home_button = QPushButton("⌂   Главная")
        self.library_button = QPushButton("▦   Библиотека")
        self.about_button = QPushButton("◉   О проекте")
        for button in (self.home_button, self.library_button, self.about_button):
            button.setObjectName("nav")
            button.setCheckable(True)
            nav.addWidget(button)
        self.home_button.clicked.connect(lambda: self._show_page(0))
        self.library_button.clicked.connect(lambda: self._show_page(1))
        self.about_button.clicked.connect(lambda: self._show_page(2))
        nav.addStretch()
        nav.addWidget(label("НЕЗАВИСИМЫЙ ПРОЕКТ\nБез Waydroid", "muted"))
        body.addWidget(sidebar)
        self.pages = QStackedWidget()
        self.pages.addWidget(self._home_page())
        self.pages.addWidget(self._library_page())
        self.pages.addWidget(self._about_page())
        body.addWidget(self.pages, 1)
        layout.addLayout(body, 1)
        self._show_page(0)

    def _home_page(self) -> QWidget:
        page = QWidget()
        box = QVBoxLayout(page)
        box.setContentsMargins(34, 30, 34, 24)
        box.setSpacing(18)
        box.addWidget(label("Добро пожаловать", "section"))
        hero = QFrame()
        hero.setMinimumHeight(245)
        image = (Path(__file__).resolve().parents[1] / "data" / "launcher-bg.jpg").as_posix()
        hero.setStyleSheet(f"QFrame#hero {{ border-radius: 16px; border-image: url('{image}'); }}")
        hero.setObjectName("hero")
        content = QVBoxLayout(hero)
        content.setContentsMargins(30, 26, 30, 26)
        content.addWidget(label("ВАШЕ ИГРОВОЕ ПРОСТРАНСТВО", "heroText"))
        title = label("Игра начинается здесь", "heroText")
        title.setStyleSheet("font-size: 30px; font-weight: 800; background: transparent;")
        content.addWidget(title)
        subtitle = label(
            "Соберите APK в одном месте. Собственный Android-движок ещё создаётся.", "heroText"
        )
        subtitle.setMaximumWidth(440)
        content.addWidget(subtitle)
        content.addStretch()
        actions = QHBoxLayout()
        add = QPushButton("＋  Добавить APK")
        add.setObjectName("primary")
        add.clicked.connect(self.add_apk)
        actions.addWidget(add)
        browse = QPushButton("Моя библиотека  →")
        browse.clicked.connect(lambda: self._show_page(1))
        actions.addWidget(browse)
        actions.addStretch()
        content.addLayout(actions)
        box.addWidget(hero)
        box.addWidget(label("Ваша библиотека", "section"))
        card = QFrame()
        card.setObjectName("card")
        row = QHBoxLayout(card)
        row.setContentsMargins(22, 18, 22, 18)
        self.summary = label("", "muted")
        row.addWidget(self.summary, 1)
        open_library = QPushButton("Открыть →")
        open_library.clicked.connect(lambda: self._show_page(1))
        row.addWidget(open_library)
        box.addWidget(card)
        box.addStretch()
        return page

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
        self.remove_button = QPushButton("Удалить из библиотеки")
        self.remove_button.setEnabled(False)
        self.remove_button.clicked.connect(self.remove_selected)
        actions.addWidget(self.remove_button)
        self.folder_button = QPushButton("Открыть папку")
        self.folder_button.setToolTip("Открыть папку с выбранным APK")
        self.folder_button.clicked.connect(self.open_selected_folder)
        actions.addWidget(self.folder_button)
        self.copy_button = QPushButton("Копировать путь")
        self.copy_button.clicked.connect(self.copy_selected_path)
        actions.addWidget(self.copy_button)
        actions.addStretch()
        box.addLayout(actions)
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
        self.pages.setCurrentIndex(index)
        for i, button in enumerate((self.home_button, self.library_button, self.about_button)):
            button.setChecked(i == index)

    def _reload(self) -> None:
        try:
            self.entries = self.library.load()
            self._library_error = ""
        except (OSError, LynxError) as exc:
            self.entries = []
            self._library_error = str(exc)
        self._render_library()

    def _render_library(self) -> None:
        self.list.clear()
        for entry in filter_entries(self.entries, self.search.text()):
            status = "APK-файл" if entry.available else "Файл недоступен"
            item = QListWidgetItem(f"{entry.title}\n{status}  ·  {entry.path}")
            item.setData(Qt.ItemDataRole.UserRole, str(entry.path))
            self.list.addItem(item)
        self.summary.setText(
            f"APK в библиотеке: {len(self.entries)}\nЗапуск станет доступен после создания движка."
        )
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
        path, _ = QFileDialog.getOpenFileName(self, "Выберите APK", "", "APK (*.apk *.APK)")
        if not path:
            return
        try:
            self.entries = self.library.add(Path(path))
            self._library_error = ""
        except (OSError, ValueError, LynxError) as exc:
            QMessageBox.warning(self, "Не удалось добавить APK", str(exc))
            return
        self._render_library()
        self._show_page(1)

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

    def toggle_fullscreen(self) -> None:
        if self.isFullScreen():
            self.showNormal()
        else:
            self.showFullScreen()
        self.fullscreen_button.setText(
            "❐  Оконный режим" if self.isFullScreen() else "⛶  Полный экран"
        )
        self.escape_shortcut.setEnabled(self.isFullScreen())
