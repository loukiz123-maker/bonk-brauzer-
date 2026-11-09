"""Visual control-layout drafts. They do not send input to Android."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor, QFocusEvent, QKeyEvent, QMouseEvent, QPainter, QPaintEvent
from PySide6.QtWidgets import (
    QComboBox,
    QDialog,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QScrollArea,
    QVBoxLayout,
    QWidget,
)

from lynxdroid.core.errors import DataCorrupted, LynxError
from lynxdroid.core.storage import read_json, write_json
from lynxdroid.input.draft_mapping import draft_engine
from lynxdroid.input.engine import InputEngine
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
        self.dragging = False
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setMinimumSize(420, 260)
        self.setStyleSheet("background:#0c101c; border:1px solid #334258; border-radius:6px")
        self.setToolTip("Нажмите на поле для добавления; перетащите элемент для перемещения")

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        if self.editor.test_engine is not None:
            self.setFocus()
            return
        x, y = event.position().x(), event.position().y()
        for index in range(len(self.editor.controls) - 1, -1, -1):
            item = self.editor.controls[index]
            dx = x - item["x"] * self.width()
            dy = y - item["y"] * self.height()
            if dx * dx + dy * dy <= 22 * 22:
                self.editor.select(index)
                self.dragging = True
                return
        self.editor.place(x / self.width(), y / self.height())

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self.dragging:
            self.editor.move_selected(
                event.position().x() / self.width(), event.position().y() / self.height()
            )

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self.dragging = False

    def keyPressEvent(self, event: QKeyEvent) -> None:
        if self.editor.test_engine is None:
            super().keyPressEvent(event)
        elif event.key() == Qt.Key.Key_Escape:
            self.editor.end_live_test()
            event.accept()
        else:
            self.editor.live_key(event.text().lower(), down=True, repeat=event.isAutoRepeat())
            event.accept()

    def keyReleaseEvent(self, event: QKeyEvent) -> None:
        if self.editor.test_engine is None:
            super().keyReleaseEvent(event)
        elif not event.isAutoRepeat():
            self.editor.live_key(event.text().lower(), down=False)
            event.accept()

    def focusOutEvent(self, event: QFocusEvent) -> None:
        self.editor.end_live_test()
        super().focusOutEvent(event)

    def paintEvent(self, event: QPaintEvent) -> None:
        super().paintEvent(event)
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        for index, item in enumerate(self.editor.controls):
            active = self.editor.test_engine is not None and any(
                c.element_id == f"draft-{index}"
                for c in self.editor.test_engine.active_pointers.values()
            )
            painter.setPen(
                QColor("#ffffff" if index == self.editor.selected or active else "#47d2f0")
            )
            painter.setBrush(
                QColor(
                    "#49a8a2"
                    if active
                    else "#367382"
                    if index == self.editor.selected
                    else "#243e55"
                )
            )
            x, y = int(item["x"] * self.width()), int(item["y"] * self.height())
            painter.drawEllipse(x - 22, y - 22, 44, 44)
            caption = "✥" if item["kind"] == "Джойстик" else item["key"]
            painter.drawText(x - 19, y - 9, 38, 18, int(Qt.AlignmentFlag.AlignCenter), caption)
        if self.editor.test_engine is not None:
            painter.setPen(QColor("#ffffff"))
            painter.setBrush(QColor("#8ff7e3"))
            for contact in self.editor.test_engine.active_pointers.values():
                x = round(contact.x / 1920 * self.width())
                y = round(contact.y / 1080 * self.height())
                painter.drawEllipse(x - 6, y - 6, 12, 12)


class ControlEditor(QDialog):
    def __init__(
        self,
        path: Path,
        parent: QWidget | None = None,
        *,
        games: list[tuple[str, Path]] | None = None,
    ) -> None:
        super().__init__(parent)
        self.path = path
        self._baseline = "[]"
        self._profile_index = 0
        self.setWindowTitle("Редактор управления — шаблон")
        self.resize(880, 520)
        self.controls: list[dict[str, Any]] = []
        self.selected: int | None = None
        self.test_engine: InputEngine | None = None
        self.read_only = False
        layout = QHBoxLayout(self)
        self.canvas = Canvas(self)
        layout.addWidget(self.canvas, 1)
        side_widget = QWidget()
        side = QVBoxLayout(side_widget)
        side_scroll = QScrollArea()
        side_scroll.setWidgetResizable(True)
        side_scroll.setMinimumWidth(250)
        side_scroll.setMaximumWidth(300)
        side_scroll.setWidget(side_widget)
        self.close_button = QPushButton("✕  Закрыть редактор")
        side.addWidget(self.close_button)
        side.addWidget(QLabel("РЕДАКТОР УПРАВЛЕНИЯ"))
        side.addWidget(QLabel("Раскладка для APK"))
        self.profile_choice = QComboBox()
        self.profile_choice.addItem("Общий шаблон (старый)", str(path))
        for title, profile_path in games or []:
            self.profile_choice.addItem(title, str(profile_path))
        side.addWidget(self.profile_choice)
        note = QLabel(
            "Нажмите на поле для добавления, перетащите элемент для перемещения. "
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
        key_hint = QLabel("Джойстик: вверх, вправо, вниз, влево — W,D,S,A")
        key_hint.setWordWrap(True)
        side.addWidget(key_hint)
        self.kind.currentTextChanged.connect(self._kind_changed)
        self.apply_button = QPushButton("Изменить выбранный")
        self.apply_button.setEnabled(False)
        self.apply_button.clicked.connect(self.apply_selected)
        side.addWidget(self.apply_button)
        self.delete_button = QPushButton("Удалить выбранный")
        self.delete_button.setEnabled(False)
        self.delete_button.clicked.connect(self.delete_selected)
        side.addWidget(self.delete_button)
        self.test_key = QLineEdit()
        self.test_key.setMaxLength(1)
        self.test_key.setPlaceholderText("Клавиша для проверки")
        side.addWidget(self.test_key)
        self.test_button = QPushButton("Проверить без Android")
        self.test_button.clicked.connect(self.test_mapping)
        side.addWidget(self.test_button)
        self.live_button = QPushButton("Тест клавиш")
        self.live_button.clicked.connect(self.toggle_live_test)
        side.addWidget(self.live_button)
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
        layout.addWidget(side_scroll)
        if games:
            self._profile_index = 1
            self.profile_choice.setCurrentIndex(1)
        self._load_profile(Path(str(self.profile_choice.currentData())))
        self.profile_choice.currentIndexChanged.connect(self.change_profile)

    def _kind_changed(self, kind: str) -> None:
        if kind == "Джойстик" and self.key.text().strip() == "F":
            self.key.setText("W,D,S,A")

    def _snapshot(self) -> str:
        return json.dumps(self.controls, ensure_ascii=False, sort_keys=True)

    def can_discard(self) -> bool:
        if self._snapshot() == self._baseline:
            return True
        return (
            QMessageBox.question(
                self,
                "Несохранённая раскладка",
                "Отменить несохранённые изменения?",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No,
            )
            == QMessageBox.StandardButton.Yes
        )

    def _load_profile(self, path: Path) -> None:
        self.end_live_test()
        self.path = path
        self.controls = []
        self.read_only = False
        self.save_button.setEnabled(True)
        self.select(None)
        try:
            self.controls = load_controls(path)
            self.status.setText(f"Элементов: {len(self.controls)}")
        except (OSError, LynxError) as exc:
            self.read_only = True
            self.save_button.setEnabled(False)
            self.status.setText(f"Ошибка чтения: {exc}. Сохранение отключено.")
        self._baseline = self._snapshot()
        self.canvas.update()

    def change_profile(self, index: int) -> None:
        if index == self._profile_index or index < 0:
            return
        if not self.can_discard():
            self.profile_choice.blockSignals(True)
            self.profile_choice.setCurrentIndex(self._profile_index)
            self.profile_choice.blockSignals(False)
            return
        self._profile_index = index
        self._load_profile(Path(str(self.profile_choice.itemData(index))))

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
        self.select(len(self.controls) - 1)
        self.status.setText(f"Элементов: {len(self.controls)} • не сохранено")

    def select(self, index: int | None) -> None:
        self.selected = index
        self.apply_button.setEnabled(index is not None)
        self.delete_button.setEnabled(index is not None)
        if index is not None:
            item = self.controls[index]
            self.kind.setCurrentText(item["kind"])
            self.key.setText(item["key"])
        self.canvas.update()

    def move_selected(self, x: float, y: float) -> None:
        if self.selected is None:
            return
        item = self.controls[self.selected]
        item["x"], item["y"] = min(1.0, max(0.0, x)), min(1.0, max(0.0, y))
        self.status.setText("Элемент перемещён • не сохранено")
        self.canvas.update()

    def apply_selected(self) -> None:
        if self.selected is None:
            return
        key = self.key.text().strip()
        if not key:
            self.status.setText("Укажите клавишу")
            return
        self.controls[self.selected].update(kind=self.kind.currentText(), key=key)
        self.status.setText("Элемент изменён • не сохранено")
        self.canvas.update()

    def delete_selected(self) -> None:
        if self.selected is None:
            return
        self.controls.pop(self.selected)
        self.select(None)
        self.status.setText(f"Элементов: {len(self.controls)} • не сохранено")

    def toggle_live_test(self) -> None:
        if self.test_engine is not None:
            self.end_live_test()
            return
        try:
            self.test_engine = draft_engine(self.controls)
        except ValueError as exc:
            self.status.setText(f"Проверка невозможна: {exc}")
            return
        self.live_button.setText("Остановить тест")
        self.canvas.setFocus(Qt.FocusReason.OtherFocusReason)
        self.status.setText("Тест без Android: нажмите клавишу на поле. Esc — выход.")

    def end_live_test(self) -> None:
        if self.test_engine is None:
            return
        self.test_engine.release_all("test ended")
        self.test_engine = None
        self.live_button.setText("Тест клавиш")
        self.status.setText("Тест остановлен. Ничего не отправлено в Android.")
        self.canvas.update()

    def live_key(self, key: str, *, down: bool, repeat: bool = False) -> None:
        if self.test_engine is None or len(key) != 1 or not key.isascii() or not key.isalnum():
            return
        try:
            events = (
                self.test_engine.key_down(key, repeat=repeat)
                if down
                else self.test_engine.key_up(key)
            )
        except LynxError as exc:
            self.end_live_test()
            self.status.setText(f"Тест остановлен: {exc}")
            return
        if events:
            active = len(self.test_engine.active_pointers)
            self.status.setText(
                f"Тест без Android: {key.upper()} • событий: {len(events)} • удерживается: {active}"
            )
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
            self.select(None)
            self.status.setText(f"Элементов: {len(self.controls)} • не сохранено")
            self.canvas.update()

    def reset(self) -> None:
        self.controls.clear()
        self.select(None)
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
        self._baseline = self._snapshot()
        self.status.setText(f"Сохранено элементов: {len(self.controls)}")
