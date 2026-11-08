"""The game surface widget: black bars, the Android area, and the key overlay.

Two responsibilities, and they are deliberately adjacent so they can never
disagree:

1. **draw** the viewport (letterbox bars, orientation, key positions) using
   ``ViewportGeometry``;
2. **hit-test** pointer positions through the *same* geometry, converting a
   mouse press into Android coordinates.

Because both use one immutable geometry object, a key drawn at a pixel and a
touch injected at that pixel are guaranteed to agree - which is exactly what
stage 6 demands ("input must use the same transforms as the image").
"""

from __future__ import annotations

from PySide6.QtCore import QEvent, QPointF, QRectF, Qt, Signal
from PySide6.QtGui import (
    QColor,
    QFont,
    QFontMetricsF,
    QMouseEvent,
    QPainter,
    QPaintEvent,
    QPen,
    QResizeEvent,
    QWheelEvent,
)
from PySide6.QtWidgets import QWidget

from lynxdroid.display.geometry import Rotation, Size, ViewportGeometry
from lynxdroid.gui.theme import Palette
from lynxdroid.gui.viewport import hit_test
from lynxdroid.profiles.model import Element, ElementType, Profile


class GameSurface(QWidget):
    """Renders the Android viewport and the active key layout."""

    #: Emitted with (x, y) in Android pixels, already transformed.
    touch_pressed = Signal(float, float)
    touch_moved = Signal(float, float)
    touch_released = Signal(float, float)
    pointer_entered_content = Signal(bool)
    wheel_delta = Signal(int)
    #: Editor mode: an element was selected (or "" for a click on empty space).
    element_selected = Signal(str)
    #: Editor mode: an element was dragged to a new normalised position.
    element_moved = Signal(str, float, float)
    #: Aim mode: raw relative motion while the pointer is captured.
    pointer_moved_by = Signal(float, float)
    #: Aim mode started/stopped (locked to a mouse element or released).
    aim_active = Signal(bool)

    def __init__(self, palette: Palette, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._palette = palette
        self._geometry = ViewportGeometry(buffer=Size(1920, 1080), surface=Size(960, 540))
        self._profile: Profile | None = None
        self._active_ids: set[str] = set()
        self._selected_id: str | None = None
        #: Aim mode: the pointer is hidden, captured and reported relatively.
        self._aim = False
        self._aim_last = QPointF()
        self._show_grid = False
        self._editor_mode = False
        self._drawing = False
        self._last_android: tuple[float, float] | None = None
        self._dragging_element: str | None = None
        self.setMouseTracking(True)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setMinimumSize(320, 200)
        self.setAttribute(Qt.WidgetAttribute.WA_OpaquePaintEvent, True)

    # -- state -----------------------------------------------------------
    def set_palette(self, palette: Palette) -> None:
        self._palette = palette
        self.update()

    def geometry_info(self) -> ViewportGeometry:
        return self._geometry

    def set_surface_size(self, width: int, height: int) -> None:
        """Window resized: recompute the mapping atomically."""
        if width == self._geometry.surface.width and height == self._geometry.surface.height:
            return
        self._geometry = ViewportGeometry(
            buffer=self._geometry.buffer,
            surface=Size(max(0, width), max(0, height)),
            fit=self._geometry.fit,
            rotation=self._geometry.rotation,
            dpr=self._geometry.dpr,
            display_name=self._geometry.display_name,
        )
        self.update()

    def set_buffer(self, width: int, height: int) -> None:
        if width == self._geometry.buffer.width and height == self._geometry.buffer.height:
            return
        self._geometry = ViewportGeometry(
            buffer=Size(max(1, width), max(1, height)),
            surface=self._geometry.surface,
            fit=self._geometry.fit,
            rotation=self._geometry.rotation,
            dpr=self._geometry.dpr,
            display_name=self._geometry.display_name,
        )
        self.update()

    def set_rotation(self, rotation: Rotation) -> None:
        self._geometry = ViewportGeometry(
            buffer=self._geometry.buffer,
            surface=self._geometry.surface,
            fit=self._geometry.fit,
            rotation=rotation,
            dpr=self._geometry.dpr,
            display_name=self._geometry.display_name,
        )
        self.update()

    def set_fit_mode(self, fit: str) -> None:
        from lynxdroid.display.geometry import FitMode

        self._geometry = ViewportGeometry(
            buffer=self._geometry.buffer,
            surface=self._geometry.surface,
            fit=FitMode(fit),
            rotation=self._geometry.rotation,
            dpr=self.devicePixelRatioF(),
        )
        self.update()

    def set_profile(self, profile: Profile | None) -> None:
        self._profile = profile
        self._active_ids.clear()
        self.update()

    def set_active_elements(self, ids: set[str]) -> None:
        if ids != self._active_ids:
            self._active_ids = set(ids)
            self.update()

    def set_selected(self, element_id: str | None) -> None:
        self._selected_id = element_id
        self.update()

    def set_editor_mode(self, enabled: bool) -> None:
        self._editor_mode = enabled
        self.update()

    def set_aim_mode(self, enabled: bool) -> None:
        """Capture the pointer for aim mode, or hand it back.

        Capture is what makes relative motion usable: without it the pointer
        leaves the window and the aim stops. It is also a promise to the user,
        so the cursor is hidden only while captured and every exit path - the
        exit key, lost focus, a profile switch - must call this with False.
        """
        if enabled == self._aim:
            return
        self._aim = enabled
        if enabled:
            self._aim_last = QPointF(self.rect().center())
            self.setCursor(Qt.CursorShape.BlankCursor)
            self.grabMouse()
            # Park the pointer in the middle so the next motion has room in
            # every direction instead of hitting a window edge.
            self.cursor().setPos(self.mapToGlobal(self.rect().center()))
        else:
            self.releaseMouse()
            self.unsetCursor()
        self.aim_active.emit(enabled)

    @property
    def aim_mode(self) -> bool:
        return self._aim

    def set_show_grid(self, enabled: bool) -> None:
        self._show_grid = enabled
        self.update()

    # -- painting --------------------------------------------------------
    def paintEvent(self, event: QPaintEvent) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        painter.setRenderHint(QPainter.RenderHint.TextAntialiasing, True)
        paint_surface(
            painter,
            self._geometry,
            self._palette,
            self._profile,
            active_ids=self._active_ids,
            selected_id=self._selected_id,
            grid=self._show_grid,
            editor_mode=self._editor_mode,
        )
        painter.end()

    def resizeEvent(self, event: QResizeEvent) -> None:
        self.set_surface_size(self.width(), self.height())
        super().resizeEvent(event)

    # -- input -----------------------------------------------------------
    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        point = event.position()
        if self._editor_mode:
            # In the editor a click selects and starts a drag; it must never
            # inject a touch into the game.
            self._begin_element_drag(point.x(), point.y())
            return
        mapped = self._geometry.widget_to_android(point.x(), point.y())
        if mapped is None:
            # Press on a black bar: deliberately ignored, never clamped.
            self.pointer_entered_content.emit(False)
            return
        self.pointer_entered_content.emit(True)
        self._drawing = True
        self._last_android = mapped
        self.touch_pressed.emit(*mapped)

    def _begin_element_drag(self, px: float, py: float) -> None:
        element = hit_test(self._geometry, self._profile, px, py)
        self._selected_id = element.id if element else None
        self._dragging_element = element.id if element else None
        self.element_selected.emit(element.id if element else "")
        self.update()

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        point = event.position()
        if self._editor_mode:
            if self._dragging_element is not None and self._profile is not None:
                mapped_uv = self._geometry.widget_to_normalized_profile(point.x(), point.y())
                if mapped_uv is not None:
                    target = self._profile.element(self._dragging_element)
                    if target is not None:
                        moved = self._profile.move_element(target.id, *mapped_uv)
                        self._profile = moved
                        updated = moved.element(target.id)
                        if updated is not None:
                            self.element_moved.emit(updated.id, updated.x, updated.y)
                        self.update()
            return
        if self._aim:
            # Captured: the cursor is invisible and parked; only the motion
            # between two positions means anything to the game.
            dx = point.x() - self._aim_last.x()
            dy = point.y() - self._aim_last.y()
            self._aim_last = point
            if dx or dy:
                self.pointer_moved_by.emit(dx, dy)
            return
        mapped = self._geometry.widget_to_android(point.x(), point.y())
        self.setCursor(Qt.CursorShape.CrossCursor if mapped else Qt.CursorShape.ArrowCursor)
        if not self._drawing or mapped is None:
            return
        self._last_android = mapped
        self.touch_moved.emit(*mapped)

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        if self._editor_mode:
            self._dragging_element = None
            return
        if event.button() != Qt.MouseButton.LeftButton or not self._drawing:
            return
        self._drawing = False
        point = event.position()
        mapped = self._geometry.widget_to_android(point.x(), point.y())
        # Releasing on a bar still ends the touch where it last was valid.
        released_at = mapped or self._last_android
        if released_at is not None:
            self.touch_released.emit(*released_at)
        self._last_android = None

    def wheelEvent(self, event: QWheelEvent) -> None:
        delta = event.angleDelta().y()
        if delta:
            self.wheel_delta.emit(delta)
            event.accept()

    def leaveEvent(self, event: QEvent) -> None:
        self.setCursor(Qt.CursorShape.ArrowCursor)
        super().leaveEvent(event)


# --------------------------------------------------------------------------
# Pure painting helper (no QWidget state, so it is easy to reason about)
# --------------------------------------------------------------------------


def paint_surface(
    painter: QPainter,
    geometry: ViewportGeometry,
    palette: Palette,
    profile: Profile | None,
    *,
    active_ids: set[str] | None = None,
    selected_id: str | None = None,
    grid: bool = False,
    editor_mode: bool = False,
) -> None:
    active = active_ids or set()
    content = geometry.content_rect()

    painter.fillRect(painter.viewport(), QColor(palette.letterbox))

    area = QRectF(content.x, content.y, content.width, content.height)
    if content.width <= 0 or content.height <= 0:
        painter.setPen(QColor(palette.text_muted))
        painter.drawText(painter.viewport(), Qt.AlignmentFlag.AlignCenter, "no Android surface")
        return

    # Placeholder picture: we do not own Android's surface. The area is drawn
    # as a distinct plate so the user sees exactly where touches will land.
    painter.fillRect(area, QColor(palette.surface))
    painter.setPen(QPen(QColor(palette.border), 1))
    painter.drawRect(area)

    if grid:
        _paint_grid(painter, area, palette)

    if editor_mode:
        _paint_zone(painter, area, palette)

    if profile is not None:
        for element in profile.elements:
            _paint_element(
                painter,
                geometry,
                element,
                palette,
                active=element.id in active,
                selected=element.id == selected_id,
                editor_mode=editor_mode,
            )

    if not editor_mode:
        _paint_hint(painter, area, palette, profile)


def _paint_grid(painter: QPainter, area: QRectF, palette: Palette) -> None:
    pen = QPen(QColor(palette.border), 1, Qt.PenStyle.DotLine)
    painter.setPen(pen)
    for fraction in (0.25, 0.5, 0.75):
        x = area.x() + area.width() * fraction
        y = area.y() + area.height() * fraction
        painter.drawLine(QPointF(x, area.y()), QPointF(x, area.bottom()))
        painter.drawLine(QPointF(area.x(), y), QPointF(area.right(), y))


def _paint_zone(painter: QPainter, area: QRectF, palette: Palette) -> None:
    """Show the safe editing zone (10% margin) in editor mode."""
    pen = QPen(QColor(palette.warning), 1, Qt.PenStyle.DashLine)
    painter.setPen(pen)
    inset = QRectF(
        area.x() + area.width() * 0.1,
        area.y() + area.height() * 0.1,
        area.width() * 0.8,
        area.height() * 0.8,
    )
    painter.drawRect(inset)


def _element_pixel(geometry: ViewportGeometry, element: Element) -> QPointF:
    x, y = geometry.normalized_to_widget(element.x, element.y)
    return QPointF(x, y)


def _paint_element(
    painter: QPainter,
    geometry: ViewportGeometry,
    element: Element,
    palette: Palette,
    *,
    active: bool,
    selected: bool,
    editor_mode: bool,
) -> None:
    centre = _element_pixel(geometry, element)
    content = geometry.content_rect()
    short_side = min(content.width, content.height)
    radius = max(12.0, element.radius * short_side)

    fill = QColor(palette.accent)
    fill.setAlpha(200 if active else 70)
    stroke = QColor(palette.accent if not selected else palette.positive)
    stroke.setAlpha(255 if (active or selected) else 150)

    painter.setPen(QPen(stroke, 2 if selected else 1))
    painter.setBrush(fill)

    if element.type is ElementType.DPAD:
        outer = max(radius, element.dpad_radius * short_side)
        painter.drawEllipse(centre, outer, outer)
        painter.setBrush(QColor(palette.accent))
        cap = outer * max(0.15, element.dead_zone)
        painter.drawEllipse(centre, max(4.0, cap), max(4.0, cap))
    elif element.type is ElementType.SWIPE:
        end_x, end_y = geometry.normalized_to_widget(*element.swipe_to)
        painter.drawEllipse(centre, radius * 0.6, radius * 0.6)
        painter.drawLine(centre, QPointF(end_x, end_y))
        painter.drawEllipse(QPointF(end_x, end_y), radius * 0.4, radius * 0.4)
    elif element.type is ElementType.MOUSE:
        painter.drawEllipse(centre, radius, radius)
        painter.drawLine(
            QPointF(centre.x() - radius, centre.y()), QPointF(centre.x() + radius, centre.y())
        )
        painter.drawLine(
            QPointF(centre.x(), centre.y() - radius), QPointF(centre.x(), centre.y() + radius)
        )
    else:
        painter.drawEllipse(centre, radius, radius)

    if radius >= 9 or selected or editor_mode:
        font = QFont()
        font.setPointSizeF(max(7.0, min(11.0, radius * 0.55)))
        font.setBold(True)
        painter.setFont(font)
        painter.setPen(QColor(palette.text if not active else palette.accent_text))
        label = _element_label(element)
        metrics = QFontMetricsF(font)
        text_width = metrics.horizontalAdvance(label)
        painter.drawText(
            QRectF(
                centre.x() - text_width / 2 - 2, centre.y() - radius, text_width + 4, radius * 2
            ),
            Qt.AlignmentFlag.AlignCenter,
            label,
        )


def _element_label(element: Element) -> str:
    if element.type in (ElementType.TAP, ElementType.HOLD):
        return "/".join(k.upper() for k in element.keys)[:10]
    if element.type is ElementType.DPAD:
        return "STICK"
    if element.type is ElementType.SWIPE:
        return "SWIPE"
    if element.type is ElementType.MOUSE:
        return "AIM"
    raise AssertionError(f"unhandled element type {element.type!r}")


def _paint_hint(painter: QPainter, area: QRectF, palette: Palette, profile: Profile | None) -> None:
    painter.setPen(QColor(palette.text_muted))
    font = QFont()
    font.setPointSizeF(9.0)
    painter.setFont(font)
    first_line = (
        f"{profile.name} — {profile.target.package}"
        if profile
        else "Android area (no layout profile active)"
    )
    painter.drawText(
        QRectF(area.x() + 10, area.bottom() - 26, max(area.width() - 20, 0), 20),
        Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
        first_line,
    )
