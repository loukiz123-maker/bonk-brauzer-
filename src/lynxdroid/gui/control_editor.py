"""Visual control-layout drafts. They do not send input to Android."""

from __future__ import annotations

import copy
import json
from pathlib import Path
from typing import Any

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor, QFocusEvent, QKeyEvent, QMouseEvent, QPainter, QPaintEvent
from PySide6.QtWidgets import (
    QComboBox,
    QDialog,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QMessageBox,
    QPushButton,
    QScrollArea,
    QVBoxLayout,
    QWidget,
)

from lynxdroid.core.errors import LynxError
from lynxdroid.core.storage import write_json
from lynxdroid.gui.control_profiles import (
    KINDS,
    ProfileConflict,
    read_control_file,
    read_profile,
    save_profile,
)
from lynxdroid.input.draft_mapping import DOUBLE_TAP_INTERVAL_MS, DraftInputEngine, draft_engine
from lynxdroid.input.scrcpy_touch import encode_touch


def load_controls(path: Path) -> list[dict[str, Any]]:
    try:
        return read_control_file(path)
    except FileNotFoundError:
        return []


class BindingCapture(QPushButton):
    """Capture one supported binding without globally intercepting input."""

    def __init__(self, field: QLineEdit) -> None:
        super().__init__("Назначить нажатием")
        self.field = field
        self.armed = False
        self.clicked.connect(self.arm)

    def arm(self) -> None:
        self.armed = True
        self.setText("Нажмите клавишу или кнопку мыши здесь • Esc — отмена")
        self.setFocus(Qt.FocusReason.OtherFocusReason)

    def cancel(self) -> None:
        self.armed = False
        self.setText("Назначить нажатием")

    def keyPressEvent(self, event: QKeyEvent) -> None:
        if not self.armed:
            super().keyPressEvent(event)
            return
        if event.key() == Qt.Key.Key_Escape:
            self.cancel()
        elif (
            not event.modifiers()
            & (
                Qt.KeyboardModifier.ControlModifier
                | Qt.KeyboardModifier.AltModifier
                | Qt.KeyboardModifier.MetaModifier
            )
            and len(event.text()) == 1
            and event.text().isascii()
            and event.text().isalnum()
        ):
            self.field.setText(event.text().upper())
            self.cancel()
        event.accept()

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if not self.armed:
            super().mousePressEvent(event)
            return
        key = Canvas._mouse_key(event.button())
        if key:
            self.field.setText("Mouse1" if key == "mouse1" else "Mouse2")
            self.cancel()
        event.accept()

    def focusOutEvent(self, event: QFocusEvent) -> None:
        self.cancel()
        super().focusOutEvent(event)


class ControlList(QListWidget):
    """List navigation remains ordinary; Alt+arrows nudge the selected marker."""

    def __init__(self, editor: ControlEditor) -> None:
        super().__init__()
        self.editor = editor

    def keyPressEvent(self, event: QKeyEvent) -> None:
        directions: dict[int, tuple[int, int]] = {
            Qt.Key.Key_Left: (-1, 0),
            Qt.Key.Key_Right: (1, 0),
            Qt.Key.Key_Up: (0, -1),
            Qt.Key.Key_Down: (0, 1),
        }
        mods = event.modifiers()
        if (
            self.editor.selected is not None
            and event.key() in directions
            and mods & Qt.KeyboardModifier.AltModifier
            and not mods & (Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.MetaModifier)
        ):
            step = 0.05 if mods & Qt.KeyboardModifier.ShiftModifier else 0.01
            dx, dy = directions[event.key()]
            self.editor.nudge_selected(dx * step, dy * step)
            event.accept()
            return
        super().keyPressEvent(event)


class Canvas(QWidget):
    def __init__(self, editor: ControlEditor) -> None:
        super().__init__()
        self.editor = editor
        self.dragging = False
        self.drag_recorded = False
        self.pressed_keys: dict[int, str] = {}
        self.last_mouse_position: tuple[float, float] | None = None
        self.setMouseTracking(True)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setMinimumSize(420, 260)
        self.setStyleSheet("background:#0c101c; border:1px solid #334258; border-radius:6px")
        self.setToolTip("Нажмите на поле для добавления; перетащите элемент для перемещения")

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if self.editor.test_engine is not None:
            self.setFocus()
            self.last_mouse_position = (event.position().x(), event.position().y())
            name = self._mouse_key(event.button())
            if name:
                self.editor.live_key(name, down=True)
            event.accept()
            return
        if event.button() != Qt.MouseButton.LeftButton:
            return
        x, y = event.position().x(), event.position().y()
        for index in range(len(self.editor.controls) - 1, -1, -1):
            item = self.editor.controls[index]
            dx = x - item["x"] * self.width()
            dy = y - item["y"] * self.height()
            if dx * dx + dy * dy <= 22 * 22:
                self.editor.select(index)
                self.dragging = True
                self.drag_recorded = False
                return
        self.editor.place(x / self.width(), y / self.height())

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        position = (event.position().x(), event.position().y())
        previous = self.last_mouse_position
        self.last_mouse_position = position
        if self.editor.test_engine is not None:
            if previous is not None and self.width() and self.height():
                self.editor.live_motion(
                    (position[0] - previous[0]) * 1920 / self.width(),
                    (position[1] - previous[1]) * 1080 / self.height(),
                )
            return
        if self.dragging:
            moved = self.editor.move_selected(
                event.position().x() / self.width(),
                event.position().y() / self.height(),
                record=not self.drag_recorded,
            )
            self.drag_recorded = self.drag_recorded or moved

    def leaveEvent(self, event: Any) -> None:
        self.last_mouse_position = None
        super().leaveEvent(event)

    @staticmethod
    def _mouse_key(button: Qt.MouseButton) -> str:
        if button == Qt.MouseButton.LeftButton:
            return "mouse1"
        if button == Qt.MouseButton.RightButton:
            return "mouse2"
        return ""

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        if self.editor.test_engine is not None:
            name = self._mouse_key(event.button())
            if name:
                self.editor.live_key(name, down=False)
            event.accept()
            return
        if event.button() == Qt.MouseButton.LeftButton:
            self.dragging = False

    def keyPressEvent(self, event: QKeyEvent) -> None:
        if self.editor.test_engine is None:
            super().keyPressEvent(event)
        elif event.key() == Qt.Key.Key_Escape:
            self.editor.end_live_test()
            event.accept()
        else:
            physical = event.nativeScanCode() or event.key()
            key = event.text().lower()
            if not event.isAutoRepeat() and physical not in self.pressed_keys:
                self.pressed_keys[physical] = key
                self.editor.live_key(key, down=True)
            event.accept()

    def keyReleaseEvent(self, event: QKeyEvent) -> None:
        if self.editor.test_engine is None:
            super().keyReleaseEvent(event)
        elif not event.isAutoRepeat():
            physical = event.nativeScanCode() or event.key()
            key = self.pressed_keys.pop(physical, event.text().lower())
            self.editor.live_key(key, down=False)
            event.accept()

    def focusOutEvent(self, event: QFocusEvent) -> None:
        self.pressed_keys.clear()
        self.last_mouse_position = None
        self.editor.release_live_inputs()
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
            if caption.lower() in ("mouse1", "mouse2"):
                caption = "ЛКМ" if caption.lower() == "mouse1" else "ПКМ"
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
        initial_profile: int = 1,
    ) -> None:
        super().__init__(parent)
        self.path = path
        self._baseline = "[]"
        self._profile_index = 0
        self._stored_profile: list[dict[str, object]] | None = None
        self.setWindowTitle("Редактор управления — шаблон")
        self.resize(880, 520)
        self.controls: list[dict[str, Any]] = []
        self.selected: int | None = None
        self._undo_stack: list[list[dict[str, Any]]] = []
        self._redo_stack: list[list[dict[str, Any]]] = []
        self.test_engine: DraftInputEngine | None = None
        self.test_timer = QTimer(self)
        self.test_timer.setInterval(20)
        self.test_timer.timeout.connect(self._tick_live_test)
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
        self.profile_choice.setSizeAdjustPolicy(
            QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon
        )
        self.profile_choice.setMinimumContentsLength(12)
        self.profile_choice.setMaximumWidth(250)
        self.profile_choice.addItem("Общий шаблон (старый)", str(path))
        for title, profile_path in games or []:
            self.profile_choice.addItem(title, str(profile_path))
        side.addWidget(self.profile_choice)
        self.reload_button = QPushButton("Обновить раскладку")
        self.reload_button.setToolTip(
            "Загрузить профиль заново; несохранённые изменения требуют подтверждения"
        )
        self.reload_button.clicked.connect(self.reload_profile)
        side.addWidget(self.reload_button)
        side.addWidget(QLabel("Скопировать сохранённую раскладку из"))
        self.copy_source = QComboBox()
        self.copy_source.setSizeAdjustPolicy(
            QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon
        )
        self.copy_source.setMinimumContentsLength(12)
        self.copy_source.setMaximumWidth(250)
        side.addWidget(self.copy_source)
        self.copy_button = QPushButton("Скопировать в черновик")
        self.copy_button.setToolTip(
            "Копируются только сохранённые данные; профиль-источник не изменится"
        )
        self.copy_button.clicked.connect(self.copy_profile)
        side.addWidget(self.copy_button)
        note = QLabel(
            "Нажмите на поле для добавления, перетащите элемент для перемещения. "
            "Это шаблон: ввод в игры пока не работает. "
            "Канал scrcpy подготовлен, но не подключён. "
            "Изменение раскладки останавливает тест."
        )
        note.setWordWrap(True)
        side.addWidget(note)
        side.addWidget(QLabel("Элементы раскладки"))
        self.control_list = ControlList(self)
        self.control_list.setAccessibleName("Элементы раскладки")
        self.control_list.setMaximumHeight(128)
        self.control_list.setMaximumWidth(250)
        self.control_list.currentRowChanged.connect(
            lambda row: self.select(row if row >= 0 else None)
        )
        side.addWidget(self.control_list)
        move_hint = QLabel("В списке: Alt + стрелки — 1%; Alt + Shift + стрелки — 5%.")
        move_hint.setWordWrap(True)
        side.addWidget(move_hint)
        self.kind = QComboBox()
        self.kind.addItems(KINDS)
        side.addWidget(self.kind)
        self.key = QLineEdit("F")
        self.key.setMaxLength(16)
        self.key.setPlaceholderText("Клавиша, например F")
        side.addWidget(self.key)
        self.capture_button = BindingCapture(self.key)
        side.addWidget(self.capture_button)
        key_hint = QLabel("Джойстик: W,D,S,A. Мышь: Mouse1 — левая, Mouse2 — правая.")
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
        self.undo_button = QPushButton("Отменить")
        self.undo_button.clicked.connect(self.undo)
        self.undo_button.setEnabled(False)
        side.addWidget(self.undo_button)
        self.redo_button = QPushButton("Повторить")
        self.redo_button.clicked.connect(self.redo)
        self.redo_button.setEnabled(False)
        side.addWidget(self.redo_button)
        reset = QPushButton("Очистить")
        reset.clicked.connect(self.reset)
        side.addWidget(reset)
        self.import_button = QPushButton("Импорт раскладки…")
        self.import_button.clicked.connect(self.import_draft)
        side.addWidget(self.import_button)
        self.export_button = QPushButton("Экспорт раскладки…")
        self.export_button.clicked.connect(self.export_draft)
        side.addWidget(self.export_button)
        self.save_button = QPushButton("Сохранить шаблон")
        self.save_button.clicked.connect(self.save)
        side.addWidget(self.save_button)
        layout.addWidget(side_scroll)
        if games:
            self._profile_index = initial_profile if 1 <= initial_profile <= len(games) else 1
            self.profile_choice.setCurrentIndex(self._profile_index)
        self._load_profile(Path(str(self.profile_choice.currentData())))
        self.profile_choice.currentIndexChanged.connect(self.change_profile)

    def _kind_changed(self, kind: str) -> None:
        self.capture_button.cancel()
        self.capture_button.setEnabled(kind != "Джойстик")
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
        self._undo_stack.clear()
        self._redo_stack.clear()
        self._refresh_history()
        self.read_only = False
        self._stored_profile = None
        self.save_button.setEnabled(True)
        self.import_button.setEnabled(True)
        self.select(None)
        try:
            self._stored_profile = read_profile(path)
            self.controls = (
                copy.deepcopy(self._stored_profile) if self._stored_profile is not None else []
            )
            self.status.setText(f"Элементов: {len(self.controls)}")
        except (OSError, LynxError) as exc:
            self.read_only = True
            self.save_button.setEnabled(False)
            self.import_button.setEnabled(False)
            self.status.setText(f"Ошибка чтения: {exc}. Сохранение отключено.")
        self._baseline = self._snapshot()
        self._refresh_copy_sources()
        self._sync_control_list()
        self.canvas.update()

    def _refresh_copy_sources(self) -> None:
        self.copy_source.clear()
        for index in range(self.profile_choice.count()):
            path = str(self.profile_choice.itemData(index))
            if path != str(self.path):
                self.copy_source.addItem(self.profile_choice.itemText(index), path)
        enabled = not self.read_only and self.copy_source.count() > 0
        self.copy_source.setEnabled(enabled)
        self.copy_button.setEnabled(enabled)

    def copy_profile(self) -> None:
        if self.read_only or self.copy_source.currentIndex() < 0:
            return
        try:
            source = read_profile(Path(str(self.copy_source.currentData())))
            if source is None:
                raise ValueError("У этого профиля ещё нет сохранённой раскладки.")
        except (OSError, LynxError, ValueError) as exc:
            QMessageBox.warning(self, "Не удалось скопировать раскладку", str(exc))
            return
        if source == self.controls:
            self.status.setText("Раскладка уже совпадает с сохранённой копией.")
            return
        if (
            self.controls
            and QMessageBox.question(
                self,
                "Заменить черновик?",
                "Скопировать сохранённую раскладку в текущий черновик? Действие можно отменить.",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No,
            )
            != QMessageBox.StandardButton.Yes
        ):
            return
        self._record_change()
        self.controls = copy.deepcopy(source)
        self.select(None)
        self.status.setText("Раскладка скопирована в черновик • сохраните для выбранной игры")

    def reload_profile(self) -> None:
        if self.can_discard():
            self._load_profile(self.path)

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
        self._record_change()
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

    def _sync_control_list(self) -> None:
        labels = [
            f"{index + 1}. {item['kind']} · {item['key']} · "
            f"{int(item['x'] * 100 + 0.5)}%, {int(item['y'] * 100 + 0.5)}%"
            for index, item in enumerate(self.controls)
        ]
        old = [self.control_list.item(i).text() for i in range(self.control_list.count())]
        blocked = self.control_list.blockSignals(True)
        if old != labels:
            self.control_list.clear()
            self.control_list.addItems(labels)
        self.control_list.setCurrentRow(self.selected if self.selected is not None else -1)
        self.control_list.blockSignals(blocked)

    def select(self, index: int | None) -> None:
        self.selected = index
        self.apply_button.setEnabled(index is not None)
        self.delete_button.setEnabled(index is not None)
        if index is not None:
            item = self.controls[index]
            self.kind.setCurrentText(item["kind"])
            self.key.setText(item["key"])
        self._sync_control_list()
        self.canvas.update()

    def move_selected(self, x: float, y: float, *, record: bool = True) -> bool:
        if self.selected is None:
            return False
        item = self.controls[self.selected]
        position = (min(1.0, max(0.0, x)), min(1.0, max(0.0, y)))
        if (item["x"], item["y"]) == position:
            return False
        if record:
            self._record_change()
        item["x"], item["y"] = position
        self._sync_control_list()
        self.status.setText("Элемент перемещён • не сохранено")
        self.canvas.update()
        return True

    def nudge_selected(self, dx: float, dy: float) -> None:
        if self.selected is None:
            return
        item = self.controls[self.selected]
        x = round(item["x"] + dx, 6) if dx else item["x"]
        y = round(item["y"] + dy, 6) if dy else item["y"]
        self.move_selected(x, y)

    def apply_selected(self) -> None:
        if self.selected is None:
            return
        key = self.key.text().strip()
        if not key:
            self.status.setText("Укажите клавишу")
            return
        item = self.controls[self.selected]
        if item["kind"] == self.kind.currentText() and item["key"] == key:
            return
        self._record_change()
        item.update(kind=self.kind.currentText(), key=key)
        self._sync_control_list()
        self.status.setText("Элемент изменён • не сохранено")
        self.canvas.update()

    def delete_selected(self) -> None:
        if self.selected is None:
            return
        self._record_change()
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
        self.canvas.last_mouse_position = None
        self.canvas.setFocus(Qt.FocusReason.OtherFocusReason)
        self.status.setText(
            "Тест без Android: камера — переключатель; "
            "перетаскивание — удержание и движение мыши. Esc — выход."
        )

    def reject(self) -> None:
        # Embedded dialogs must not hide themselves and desynchronise the page stack.
        if self.test_engine is not None:
            self.end_live_test()
        elif self.isWindow():
            super().reject()

    def release_live_inputs(self) -> None:
        if self.test_engine is not None:
            self.test_timer.stop()
            self.test_engine.release_all("focus lost")
            self.canvas.update()

    def _tick_live_test(self) -> None:
        if self.test_engine is None:
            return
        try:
            events = self.test_engine.tick()
        except LynxError as exc:
            self.end_live_test()
            self.status.setText(f"Тест остановлен: {exc}")
            return
        if not self.test_engine.has_pending_double_taps:
            self.test_timer.stop()
        if events:
            self.status.setText(f"Тест без Android: второе нажатие • событий: {len(events)}")
            self.canvas.update()

    def end_live_test(self) -> None:
        self.test_timer.stop()
        if self.test_engine is None:
            return
        self.test_engine.release_all("test ended")
        self.canvas.pressed_keys.clear()
        self.test_engine = None
        self.live_button.setText("Тест клавиш")
        self.status.setText("Тест остановлен. Ничего не отправлено в Android.")
        self.canvas.update()

    def live_key(self, key: str, *, down: bool, repeat: bool = False) -> None:
        if self.test_engine is None:
            return
        if key not in ("mouse1", "mouse2") and (
            len(key) != 1 or not key.isascii() or not key.isalnum()
        ):
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
        if self.test_engine.has_pending_double_taps and not self.test_timer.isActive():
            self.test_timer.start()
        if events:
            active = len(self.test_engine.active_pointers)
            self.status.setText(
                f"Тест без Android: {key.upper()} • событий: {len(events)} • удерживается: {active}"
            )
            self.canvas.update()

    def live_motion(self, dx: float, dy: float) -> None:
        if self.test_engine is None:
            return
        try:
            events = self.test_engine.mouse_delta(dx, dy)
        except LynxError as exc:
            self.end_live_test()
            self.status.setText(f"Тест остановлен: {exc}")
            return
        if events:
            self.status.setText(f"Тест без Android: движение • событий: {len(events)}")
            self.canvas.update()

    def test_mapping(self) -> None:
        try:
            engine = draft_engine(self.controls)
            key = self.test_key.text().strip().lower()
            if len(key) != 1 or not key.isascii() or not key.isalnum():
                raise ValueError("Введите одну латинскую букву или цифру")
            events = (
                engine.key_down(key, now_ms=0)
                + engine.key_up(key, now_ms=0)
                + engine.tick(now_ms=DOUBLE_TAP_INTERVAL_MS)
            )
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

    def _refresh_history(self) -> None:
        self.undo_button.setEnabled(bool(self._undo_stack))
        self.redo_button.setEnabled(bool(self._redo_stack))

    def _record_change(self) -> None:
        # The test engine is a snapshot. Never keep using it after an edit.
        self.end_live_test()
        self._undo_stack.append(copy.deepcopy(self.controls))
        if len(self._undo_stack) > 50:
            self._undo_stack.pop(0)
        self._redo_stack.clear()
        self._refresh_history()

    def undo(self) -> None:
        if not self._undo_stack:
            return
        self.end_live_test()
        self._redo_stack.append(copy.deepcopy(self.controls))
        self.controls = self._undo_stack.pop()
        self.select(None)
        self._refresh_history()
        self.status.setText("Действие отменено • сохраните изменения")

    def redo(self) -> None:
        if not self._redo_stack:
            return
        self.end_live_test()
        self._undo_stack.append(copy.deepcopy(self.controls))
        self.controls = self._redo_stack.pop()
        self.select(None)
        self._refresh_history()
        self.status.setText("Действие повторено • сохраните изменения")

    def reset(self) -> None:
        if not self.controls:
            return
        self._record_change()
        self.controls.clear()
        self.select(None)
        self.status.setText("Шаблон очищен • не сохранено")
        self.canvas.update()

    def import_draft(self) -> None:
        if self.read_only:
            return
        path, _ = QFileDialog.getOpenFileName(self, "Импорт раскладки", "", "JSON (*.json)")
        if not path:
            return
        try:
            imported = read_control_file(Path(path))
        except (OSError, LynxError) as exc:
            QMessageBox.warning(self, "Ошибка импорта", str(exc))
            return
        if (
            self.controls
            and QMessageBox.question(
                self,
                "Заменить раскладку?",
                "Заменить текущий черновик? До сохранения можно отменить.",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No,
            )
            != QMessageBox.StandardButton.Yes
        ):
            return
        if imported != self.controls:
            self._record_change()
            self.controls = imported
            self.select(None)
        self.status.setText("Раскладка импортирована • сохраните для выбранной игры")

    def export_draft(self) -> None:
        path, _ = QFileDialog.getSaveFileName(
            self, "Экспорт раскладки", "layout.json", "JSON (*.json)"
        )
        if not path:
            return
        target = Path(path)
        if target.suffix.lower() != ".json":
            target = target.with_name(target.name + ".json")
        try:
            # Do not allow an export to bypass the editor's corruption guard.
            if target.resolve() == self.path.resolve():
                raise ValueError("Выберите другой файл, не профиль игры.")
            if target.exists() or target.is_symlink():
                answer = QMessageBox.question(
                    self,
                    "Заменить файл?",
                    f"Файл уже существует: {target.name}. Заменить?",
                    QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                    QMessageBox.StandardButton.No,
                )
                if answer != QMessageBox.StandardButton.Yes:
                    return
            write_json(target, {"schema": 1, "controls": self.controls})
        except (OSError, LynxError, ValueError) as exc:
            QMessageBox.warning(self, "Ошибка экспорта", str(exc))
            return
        self.status.setText("Раскладка экспортирована. Профиль игры не изменён.")

    def save(self) -> None:
        if self.read_only:
            return
        try:
            save_profile(self.path, self.controls, self._stored_profile)
        except (OSError, LynxError, ProfileConflict) as exc:
            QMessageBox.warning(self, "Ошибка сохранения", str(exc))
            return
        self._stored_profile = copy.deepcopy(self.controls)
        self._baseline = self._snapshot()
        self.status.setText(f"Сохранено элементов: {len(self.controls)}")
