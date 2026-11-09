"""Visual control-layout drafts. They do not send input to Android."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor, QMouseEvent, QPainter, QPaintEvent
from PySide6.QtWidgets import (
    QComboBox,
    QDialog,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from lynxdroid.core.errors import DataCorrupted, LynxError
from lynxdroid.core.storage import read_json, write_json
from lynxdroid.input.draft_mapping import draft_engine
from lynxdroid.input.scrcpy_touch import encode_touch

KINDS = ("Нажатие", "Двойное нажатие", "Мультитач", "Джойстик", "Камера", "Перетаскивание", "Огонь")


def load_controls(path: Path) -> list[dict[str, Any]]:
    try:
        data = read_json(path)
    except FileNotFoundError:
        return []
    items = data.get("controls")
    if data.get("schema") != 1 or not isinstance(items, list) or len(items) > 100:
        raise DataCorrupted("Повреждён файл раскладки. Исходный файл сохранён.")
    for item in items:
        if (
            not isinstance(item, dict)
            or item.get("kind") not in KINDS
            or not isinstance(item.get("key"), str)
            or not 1 <= len(item["key"]) <= 16
            or any(
                type(item.get(axis)) not in (float, int) or not 0 <= item[axis] <= 1
                for axis in ("x", "y")
            )
        ):
            raise DataCorrupted("Повреждена раскладка. Исходный файл сохранён.")
    return items


class Canvas(QWidget):
    def __init__(self, editor: ControlEditor) -> None:
        super().__init__()
        self.editor = editor
        self.setMinimumSize(420, 260)
        self.setStyleSheet("background:#0c101c; border:1px solid #334258; border-radius:6px")
        self.setToolTip("Нажмите на поле, чтобы разместить выбранный элемент")

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self.editor.place(
                event.position().x() / self.width(), event.position().y() / self.height()
            )

    def paintEvent(self, event: QPaintEvent) -> None:
        super().paintEvent(event)
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(QColor("#47d2f0"))
        painter.setBrush(QColor("#243e55"))
        for item in self.editor.controls:
            x, y = int(item["x"] * self.width()), int(item["y"] * self.height())
            painter.drawEllipse(x - 22, y - 22, 44, 44)
            painter.drawText(x - 19, y - 9, 38, 18, int(Qt.AlignmentFlag.AlignCenter), item["key"])


class ControlEditor(QDialog):
    def __init__(self, path: Path, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.path = path
        self.setWindowTitle("Редактор управления — шаблон")
        self.resize(880, 520)
        self.controls: list[dict[str, Any]] = []
        self.read_only = False
        layout = QHBoxLayout(self)
        self.canvas = Canvas(self)
        layout.addWidget(self.canvas, 1)
        side = QVBoxLayout()
        self.close_button = QPushButton("✕  Закрыть редактор")
        side.addWidget(self.close_button)
        side.addWidget(QLabel("РЕДАКТОР УПРАВЛЕНИЯ"))
        note = QLabel(
            "Выберите тип и клавишу, затем нажмите на поле. "
            "Это шаблон: ввод в игры пока не работает. "
            "Канал scrcpy подготовлен, но не подключён."
        )
        note.setWordWrap(True)
        side.addWidget(note)
        self.kind = QComboBox()
        self.kind.addItems(KINDS)
        side.addWidget(self.kind)
        self.key = QLineEdit("F")
        self.key.setMaxLength(16)
        self.key.setPlaceholderText("Клавиша, например F")
        side.addWidget(self.key)
        self.test_key = QLineEdit()
        self.test_key.setMaxLength(1)
        self.test_key.setPlaceholderText("Клавиша для проверки")
        side.addWidget(self.test_key)
        self.test_button = QPushButton("Проверить без Android")
        self.test_button.clicked.connect(self.test_mapping)
        side.addWidget(self.test_button)
        self.status = QLabel("")
        self.status.setWordWrap(True)
        side.addWidget(self.status)
        side.addStretch()
        undo = QPushButton("Убрать последний")
        undo.clicked.connect(self.undo)
        side.addWidget(undo)
        reset = QPushButton("Очистить")
        reset.clicked.connect(self.reset)
        side.addWidget(reset)
        self.save_button = QPushButton("Сохранить шаблон")
        self.save_button.clicked.connect(self.save)
        side.addWidget(self.save_button)
        layout.addLayout(side)
        try:
            self.controls = load_controls(path)
            self.status.setText(f"Элементов: {len(self.controls)}")
        except (OSError, LynxError) as exc:
            self.read_only = True
            self.save_button.setEnabled(False)
            self.status.setText(f"Ошибка чтения: {exc}. Сохранение отключено.")

    def place(self, x: float, y: float) -> None:
        key = self.key.text().strip()
        if not key or len(self.controls) >= 100:
            self.status.setText("Укажите клавишу; максимум 100 элементов.")
            return
        self.controls.append(
            {
                "kind": self.kind.currentText(),
                "key": key,
                "x": min(1.0, max(0.0, x)),
                "y": min(1.0, max(0.0, y)),
            }
        )
        self.status.setText(f"Элементов: {len(self.controls)} • не сохранено")
        self.canvas.update()

    def test_mapping(self) -> None:
        try:
            engine = draft_engine(self.controls)
            key = self.test_key.text().strip().lower()
            if len(key) != 1 or not key.isascii() or not key.isalnum():
                raise ValueError("Введите одну латинскую букву или цифру")
            events = engine.key_down(key) + engine.key_up(key)
            for event in events:
                encode_touch(event, 1920, 1080)
        except ValueError as exc:
            self.status.setText(f"Проверка невозможна: {exc}")
            return
        if events:
            self.status.setText(
                f"Тест без Android: {len(events)} событий касания, "
                f"позиция {int(events[0].x)} × {int(events[0].y)}. Ничего не отправлено."
            )
        else:
            self.status.setText("Тест без Android: клавиша не назначена. Ничего не отправлено.")

    def undo(self) -> None:
        if self.controls:
            self.controls.pop()
            self.status.setText(f"Элементов: {len(self.controls)} • не сохранено")
            self.canvas.update()

    def reset(self) -> None:
        self.controls.clear()
        self.status.setText("Шаблон очищен • не сохранено")
        self.canvas.update()

    def save(self) -> None:
        if self.read_only:
            return
        try:
            # Recheck the current file so corruption is never silently overwritten.
            load_controls(self.path)
            write_json(self.path, {"schema": 1, "controls": self.controls})
        except (OSError, LynxError) as exc:
            QMessageBox.warning(self, "Ошибка сохранения", str(exc))
            return
        self.status.setText(f"Сохранено элементов: {len(self.controls)}")
