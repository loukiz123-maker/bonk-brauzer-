"""The LynxDroid main window.

Layout, and why:

    ┌──────────────────────────────────────────────────────────────┐
    │ top bar: state pill, session start/stop, doctor, settings     │
    ├──────────────────────────────────┬───────────────────────────┤
    │ game surface (letterbox + keys)  │ side panel tabs:          │
    │                                  │  Session / Display /      │
    │                                  │  Layout / Apps / Doctor   │
    ├──────────────────────────────────┴───────────────────────────┤
    │ status bar: geometry · channel · latency · paths             │
    └──────────────────────────────────────────────────────────────┘

The side panel is intentionally narrow and holds only controls that the
backend reports as *supported on this host*. Controls for unsupported
features are shown disabled with the reason, never hidden, so the user can
tell "not implemented" from "not working right now".
"""

from __future__ import annotations

import os
import time
from pathlib import Path

from PySide6.QtCore import QSize, Qt, QTimer, Signal
from PySide6.QtGui import QCloseEvent, QColor, QFocusEvent, QKeyEvent, QResizeEvent
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QFrame,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QScrollArea,
    QSpinBox,
    QSplitter,
    QStatusBar,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from lynxdroid.backend.contract import BackendCapabilities, BackendState, Snapshot
from lynxdroid.backend.waydroid import BackendConfig, FakeBackend, WaydroidBackend
from lynxdroid.core.diagnostics import Report
from lynxdroid.core.logging_setup import get
from lynxdroid.core.paths import AppPaths
from lynxdroid.display.geometry import Rotation, ViewportGeometry
from lynxdroid.display.resolution import PRESETS
from lynxdroid.gui import viewmodel as vm
from lynxdroid.gui.editor import LayoutEditor
from lynxdroid.gui.overlay import GameSurface
from lynxdroid.gui.theme import Palette, palette_for, stylesheet
from lynxdroid.gui.workers import Poller, TaskRunner
from lynxdroid.input.channels import Capabilities as ChannelCapabilities
from lynxdroid.input.engine import InputEngine
from lynxdroid.input.events import TouchEvent
from lynxdroid.input.pipeline import InputPipeline
from lynxdroid.profiles.model import Profile, ReferenceResolution
from lynxdroid.profiles.store import ProfileStore
from lynxdroid.settings import SettingsStore
from lynxdroid.version import version_string

log = get("gui.main")


class MainWindow(QMainWindow):
    """One window, one backend, one active profile."""

    settings_changed = Signal(object)

    def __init__(
        self,
        *,
        paths: AppPaths,
        settings_store: SettingsStore,
        backend_kind: str | None = None,
    ) -> None:
        super().__init__()
        self._paths = paths
        self._settings_store = settings_store
        self._settings, notes = settings_store.load()
        self._profile_store = ProfileStore(paths)
        self._tasks = TaskRunner()
        self._snapshot = Snapshot(state=BackendState.NOT_CONFIGURED, detail="not checked")
        self._caps: BackendCapabilities | None = None
        self._report: Report | None = None
        self._profile: Profile | None = None
        self._editor: LayoutEditor | None = None
        self._engine: InputEngine | None = None
        self._channel_caps: ChannelCapabilities | None = None
        self._pipeline = InputPipeline()
        self._activity: list[str] = []
        self._banner_widgets: list[QWidget] = []

        kind = backend_kind or self._settings.backend
        self._backend = FakeBackend() if kind == "fake" else WaydroidBackend(BackendConfig())

        self.setWindowTitle(f"LynxDroid — {version_string()}")
        self.setMinimumSize(QSize(1000, 640))
        self._build_ui()
        self._apply_theme()
        self._load_profiles()

        self._poller = Poller(interval_ms=2_000, parent=self)
        self._poller.tick.connect(self._poll_status)
        if self._settings.doctor_on_start:
            QTimer.singleShot(150, self._run_doctor)
        QTimer.singleShot(50, self._poll_status)

        for note in notes:
            self._set_status(note)
        log.info("window ready (backend=%s)", self._backend.name)

    # ==================================================================
    # construction
    # ==================================================================
    def _build_ui(self) -> None:
        central = QWidget(self)
        outer = QVBoxLayout(central)
        outer.setContentsMargins(14, 12, 14, 10)
        outer.setSpacing(10)

        outer.addWidget(self._build_topbar())
        self._banner_area = QVBoxLayout()
        self._banner_area.setSpacing(6)
        outer.addLayout(self._banner_area)

        splitter = QSplitter(Qt.Orientation.Horizontal)
        splitter.setChildrenCollapsible(False)
        splitter.setHandleWidth(8)

        self._surface = GameSurface(palette_for(self._settings.theme))
        self._surface.element_selected.connect(self._on_element_selected)
        self._surface.element_moved.connect(self._on_element_moved)
        self._surface.touch_pressed.connect(self._on_touch_pressed)
        self._surface.touch_moved.connect(self._on_touch_moved)
        self._surface.touch_released.connect(self._on_touch_released)
        surface_frame = QFrame()
        surface_frame.setObjectName("Card")
        surface_layout = QVBoxLayout(surface_frame)
        surface_layout.setContentsMargins(6, 6, 6, 6)
        surface_layout.addWidget(self._surface)
        splitter.addWidget(surface_frame)

        self._panel = self._build_panel()
        splitter.addWidget(self._panel)
        splitter.setStretchFactor(0, 5)
        splitter.setStretchFactor(1, 2)
        splitter.setSizes([760, 340])
        outer.addWidget(splitter, 1)

        self.setCentralWidget(central)
        self._statusbar = QStatusBar(self)
        self.setStatusBar(self._statusbar)

        self._status_geometry = QLabel("geometry: —")
        self._status_channel = QLabel("input: not established")
        self._latency_label = QLabel("send: no samples yet")
        self._latency_label.setToolTip(
            "Time for the control layer to hand events to the channel. "
            "This is NOT end-to-end latency: compositor, container and the "
            "Android app are not measured here."
        )
        self._statusbar.addWidget(self._status_geometry)
        self._statusbar.addPermanentWidget(self._status_channel)
        self._statusbar.addPermanentWidget(self._latency_label)

    def _build_topbar(self) -> QWidget:
        bar = QFrame()
        bar.setObjectName("Card")
        layout = QHBoxLayout(bar)
        layout.setContentsMargins(12, 8, 12, 8)
        layout.setSpacing(10)

        self._state_pill = QLabel("not checked")
        self._state_pill.setObjectName("SectionTitle")
        layout.addWidget(self._state_pill)

        self._state_detail = QLabel("")
        self._state_detail.setObjectName("Muted")
        layout.addWidget(self._state_detail, 1)

        self._start_button = QPushButton("Start Android")
        self._start_button.setObjectName("Primary")
        self._start_button.clicked.connect(self._start_session)
        self._stop_button = QPushButton("Stop")
        self._stop_button.clicked.connect(self._stop_session)
        self._doctor_button = QPushButton("Run doctor")
        self._doctor_button.clicked.connect(self._run_doctor)

        layout.addWidget(self._start_button)
        layout.addWidget(self._stop_button)
        layout.addWidget(self._doctor_button)
        return bar

    def _build_panel(self) -> QWidget:
        tabs = QTabWidget()
        tabs.setDocumentMode(True)
        tabs.addTab(self._build_session_tab(), "Session")
        tabs.addTab(self._build_display_tab(), "Display")
        tabs.addTab(self._build_layout_tab(), "Layout")
        tabs.addTab(self._build_apps_tab(), "Apps")
        tabs.addTab(self._build_doctor_tab(), "Doctor")
        self._tabs = tabs
        return tabs

    # -- session tab -----------------------------------------------------
    def _build_session_tab(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(10, 10, 10, 10)
        layout.setSpacing(8)

        layout.addWidget(_section("Android session"))
        self._layer_list = QListWidget()
        self._layer_list.setSelectionMode(QListWidget.SelectionMode.NoSelection)
        layout.addWidget(self._layer_list)

        layout.addWidget(_section("Input channel"))
        self._channel_label = QLabel("no channel")
        self._channel_label.setWordWrap(True)
        self._channel_label.setObjectName("Muted")
        layout.addWidget(self._channel_label)

        self._overlay_checkbox = QCheckBox("Show key layout over the surface")
        self._overlay_checkbox.setChecked(True)
        self._overlay_checkbox.toggled.connect(self._refresh_surface)
        layout.addWidget(self._overlay_checkbox)

        layout.addWidget(_section("Full screen"))
        fullscreen_button = QPushButton("Toggle full screen (F11)")
        fullscreen_button.clicked.connect(self._toggle_fullscreen)
        layout.addWidget(fullscreen_button)

        layout.addStretch(1)
        self._session_notes = QLabel("")
        self._session_notes.setWordWrap(True)
        self._session_notes.setObjectName("Muted")
        layout.addWidget(self._session_notes)
        return _scrolled(page)

    # -- display tab -----------------------------------------------------
    def _build_display_tab(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(10, 10, 10, 10)
        layout.setSpacing(8)

        layout.addWidget(_section("Preset"))
        self._preset_combo = QComboBox()
        self._preset_combo.addItem("custom")
        for preset in PRESETS:
            self._preset_combo.addItem(f"{preset.name} · {preset.width}×{preset.height}")
        self._preset_combo.currentIndexChanged.connect(self._on_preset_chosen)
        layout.addWidget(self._preset_combo)

        form = QHBoxLayout()
        self._width_spin = QSpinBox()
        self._width_spin.setRange(320, 3840)
        self._width_spin.setSingleStep(16)
        self._height_spin = QSpinBox()
        self._height_spin.setRange(320, 3840)
        self._height_spin.setSingleStep(16)
        self._dpi_spin = QSpinBox()
        self._dpi_spin.setRange(72, 960)
        self._dpi_spin.setSingleStep(10)
        for label, widget in (
            ("W", self._width_spin),
            ("H", self._height_spin),
            ("DPI", self._dpi_spin),
        ):
            column = QVBoxLayout()
            caption = QLabel(label)
            caption.setObjectName("Muted")
            column.addWidget(caption)
            column.addWidget(widget)
            form.addLayout(column)
            widget.valueChanged.connect(self._on_display_form_changed)
        layout.addLayout(form)

        self._display_warning = QLabel("")
        self._display_warning.setWordWrap(True)
        self._display_warning.setObjectName("Muted")
        layout.addWidget(self._display_warning)

        self._apply_display_button = QPushButton("Apply resolution…")
        self._apply_display_button.clicked.connect(self._apply_display)
        layout.addWidget(self._apply_display_button)

        layout.addWidget(_section("Window"))
        fit_row = QHBoxLayout()
        self._fit_combo = QComboBox()
        self._fit_combo.addItems(["contain", "fill", "integer"])
        self._fit_combo.currentTextChanged.connect(self._on_fit_changed)
        fit_row.addWidget(QLabel("scale"))
        fit_row.addWidget(self._fit_combo, 1)
        layout.addLayout(fit_row)

        rotation_row = QHBoxLayout()
        self._rotation_combo = QComboBox()
        for label, value in (("0°", 0), ("90°", 90), ("180°", 180), ("270°", 270)):
            self._rotation_combo.addItem(label, value)
        self._rotation_combo.currentIndexChanged.connect(self._on_rotation_changed)
        rotation_row.addWidget(QLabel("rotate"))
        rotation_row.addWidget(self._rotation_combo, 1)
        layout.addLayout(rotation_row)

        layout.addWidget(_section("Current geometry"))
        self._geometry_label = QLabel("—")
        self._geometry_label.setWordWrap(True)
        self._geometry_label.setObjectName("Muted")
        layout.addWidget(self._geometry_label)

        layout.addStretch(1)
        return _scrolled(page)

    # -- layout tab ------------------------------------------------------
    def _build_layout_tab(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(10, 10, 10, 10)
        layout.setSpacing(8)

        layout.addWidget(_section("Profile"))
        self._profile_list = QListWidget()
        self._profile_list.currentItemChanged.connect(self._on_profile_selected)
        layout.addWidget(self._profile_list, 1)

        buttons = QHBoxLayout()
        self._new_profile_button = QPushButton("New…")
        self._new_profile_button.clicked.connect(self._create_profile)
        self._delete_profile_button = QPushButton("Delete")
        self._delete_profile_button.setObjectName("Danger")
        self._delete_profile_button.clicked.connect(self._delete_profile)
        self._save_profile_button = QPushButton("Save")
        self._save_profile_button.clicked.connect(self._save_profile)
        self._save_profile_button.setEnabled(False)
        buttons.addWidget(self._save_profile_button)
        buttons.addWidget(self._new_profile_button)
        buttons.addWidget(self._delete_profile_button)
        layout.addLayout(buttons)

        layout.addWidget(_section("Keys in this profile"))
        self._element_list = QListWidget()
        layout.addWidget(self._element_list, 1)

        self._overlay_hint = QLabel("")
        self._overlay_hint.setWordWrap(True)
        self._overlay_hint.setObjectName("Muted")
        layout.addWidget(self._overlay_hint)

        self._grid_checkbox = QCheckBox("Show alignment grid while editing")
        self._grid_checkbox.toggled.connect(self._surface.set_show_grid)
        layout.addWidget(self._grid_checkbox)
        return _scrolled(page)

    # -- apps tab --------------------------------------------------------
    def _build_apps_tab(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(10, 10, 10, 10)
        layout.setSpacing(8)

        layout.addWidget(_section("Installed packages"))
        self._apps_list = QListWidget()
        self._apps_list.itemDoubleClicked.connect(self._launch_selected_app)
        layout.addWidget(self._apps_list, 1)

        buttons = QHBoxLayout()
        refresh = QPushButton("Refresh")
        refresh.clicked.connect(self._refresh_apps)
        launch = QPushButton("Launch")
        launch.clicked.connect(self._launch_selected_app)
        buttons.addWidget(refresh)
        buttons.addWidget(launch)
        layout.addLayout(buttons)

        self._apps_note = QLabel("")
        self._apps_note.setWordWrap(True)
        self._apps_note.setObjectName("Muted")
        layout.addWidget(self._apps_note)
        return _scrolled(page)

    # -- doctor tab ------------------------------------------------------
    def _build_doctor_tab(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(10, 10, 10, 10)
        layout.setSpacing(8)

        self._doctor_summary = QLabel("not run yet")
        self._doctor_summary.setObjectName("SectionTitle")
        self._doctor_summary.setWordWrap(True)
        layout.addWidget(self._doctor_summary)

        row = QHBoxLayout()
        run_button = QPushButton("Run diagnostics")
        run_button.clicked.connect(self._run_doctor)
        self._hide_ok_checkbox = QCheckBox("Hide OK rows")
        self._hide_ok_checkbox.toggled.connect(self._render_doctor)
        row.addWidget(run_button)
        row.addWidget(self._hide_ok_checkbox)
        layout.addLayout(row)

        self._doctor_list = QListWidget()
        self._doctor_list.setWordWrap(True)
        layout.addWidget(self._doctor_list, 1)

        paths = QLabel(
            f"config: {self._paths.config}\n"
            f"profiles: {self._paths.profiles_dir}\n"
            f"reports: {self._paths.reports_dir}"
        )
        paths.setObjectName("Mono")
        paths.setWordWrap(True)
        layout.addWidget(paths)
        return _scrolled(page)

    # ==================================================================
    # theme / settings
    # ==================================================================
    def _apply_theme(self) -> None:
        palette = palette_for(self._settings.theme)
        self.setStyleSheet(stylesheet(palette))
        self._surface.set_palette(palette)

    def _set_status(self, text: str, timeout_ms: int = 8000) -> None:
        self._statusbar.showMessage(text, timeout_ms)

    def _save_setting(self, **changes: object) -> None:
        settings, notes = self._settings_store.update(**changes)
        self._settings = settings
        for note in notes:
            log.warning("settings: %s", note)
        self.settings_changed.emit(settings)

    # ==================================================================
    # banners
    # ==================================================================
    def _refresh_banners(self) -> None:
        for widget in self._banner_widgets:
            widget.setParent(None)
            widget.deleteLater()
        self._banner_widgets.clear()

        entries = vm.banners(
            snapshot=self._snapshot,
            caps=self._caps,
            report=self._report,
            settings=self._settings,
            channel=self._channel_caps,
        )
        palette = palette_for(self._settings.theme)
        for banner in entries[:4]:
            widget = _BannerWidget(banner, palette)
            self._banner_area.addWidget(widget)
            self._banner_widgets.append(widget)

    # ==================================================================
    # backend
    # ==================================================================
    def _poll_status(self) -> None:
        if self._tasks.active > 0:
            return
        self._tasks.submit(
            self._backend.snapshot,
            on_done=self._on_snapshot,
            on_error=lambda message, detail: log.warning("status: %s", message),
        )

    def _on_snapshot(self, snapshot: Snapshot) -> None:
        changed = snapshot.state is not self._snapshot.state
        self._snapshot = snapshot
        if self._caps is None:
            self._tasks.submit(self._backend.capabilities, on_done=self._on_capabilities)
        panel = vm.status_panel(snapshot, self._caps) if self._caps else None
        self._state_pill.setText(panel.headline if panel else snapshot.state.value)
        self._state_detail.setText(panel.detail if panel else snapshot.detail)
        if panel is not None:
            self._render_layers(panel)

        start_state = vm.start_control(snapshot, self._caps) if self._caps else None
        stop_state = vm.stop_control(snapshot, self._caps) if self._caps else None
        if start_state:
            self._start_button.setEnabled(start_state.enabled)
            self._start_button.setToolTip(start_state.reason)
        if stop_state:
            self._stop_button.setEnabled(stop_state.enabled)
            self._stop_button.setToolTip(stop_state.reason)

        if changed:
            self._refresh_banners()
        if snapshot.state.is_active and not self._poller.is_active:
            self._poller.start()
        elif snapshot.state in (BackendState.STOPPED, BackendState.ERROR):
            pass

    def _on_capabilities(self, caps: BackendCapabilities) -> None:
        self._caps = caps
        self._session_notes.setText("\n".join(caps.notes))
        self._refresh_banners()
        self._refresh_controls()

    def _render_layers(self, panel: vm.StatusPanel) -> None:
        self._layer_list.clear()
        palette = palette_for(self._settings.theme)
        for label, _readiness, text in panel.layers:
            item = QListWidgetItem(f"{label:<18} {text}")
            colour = {
                "ready": palette.positive,
                "failed": palette.danger,
                "not ready": palette.warning,
            }.get(text, palette.text_muted)
            item.setForeground(_colour(colour))
            self._layer_list.addItem(item)

    def _refresh_controls(self) -> None:
        if self._caps is None:
            return
        display = vm.display_control(self._caps)
        self._apply_display_button.setEnabled(display.enabled)
        self._apply_display_button.setToolTip(display.reason)
        apps = self._caps.supports("app-list")
        self._apps_note.setText("" if apps else "this backend cannot list installed applications")

    def _start_session(self) -> None:
        self._set_status("starting Android…")
        self._start_button.setEnabled(False)
        self._tasks.submit(
            self._backend.start,
            timeout_s=90.0,
            on_done=self._on_started,
            on_error=self._on_start_failed,
        )

    def _on_started(self, snapshot: Snapshot) -> None:
        self._on_snapshot(snapshot)
        self._set_status("Android is running")
        self._poller.start()

    def _on_start_failed(self, message: str, detail: str) -> None:
        log.warning("start failed: %s", message)
        self._set_status(f"start failed: {message}", 15000)
        self._on_snapshot(self._snapshot)
        self._refresh_controls()

    def _stop_session(self) -> None:
        if self._engine is not None:
            self._engine.focus_lost("session stopping")
            self._surface.set_active_elements(set())
        if self._snapshot.state is BackendState.STARTING:
            self._set_status("stop requested; will stop after the start attempt")
        else:
            self._set_status("stopping Android…")
        self._stop_button.setEnabled(False)
        self._tasks.submit(
            self._backend.stop,
            timeout_s=45.0,
            on_done=self._on_snapshot,
            on_error=lambda message, detail: self._set_status(f"stop failed: {message}", 15000),
        )

    # ==================================================================
    # input
    # ==================================================================
    def _ensure_engine(self) -> InputEngine | None:
        if self._profile is None:
            return None
        geometry = self._surface.geometry_info()
        if self._engine is None:
            self._engine = InputEngine(self._profile, geometry)
        else:
            self._engine.set_geometry(geometry)
            self._engine.set_profile(self._profile)
        self._ensure_channel()
        return self._engine

    def _append_log(self, message: str) -> None:
        """One line of user-visible activity, never a traceback."""
        log_line = f"{time.strftime('%H:%M:%S')}  {message}"
        self._activity.append(log_line)
        del self._activity[:-200]
        get("gui").info("%s", message)

    def _ensure_channel(self) -> None:
        """Open the touch transport once, and report honestly what we got."""
        if self._pipeline.channel is not None:
            return
        self._channel_caps = self._pipeline.choose(agent_socket=self._agent_socket_path())
        for warning in self._pipeline.notes:
            self._append_log(warning)
        self._status_channel.setText(f"input: {self._channel_caps.describe()}")
        self._refresh_overlay_hint()

    def _agent_socket_path(self) -> str | None:
        """The agent socket lives in the runtime dir, or it does not exist."""
        runtime = os.environ.get("XDG_RUNTIME_DIR")
        if not runtime:
            return None
        candidate = Path(runtime) / "lynxdroid-agent.sock"
        return str(candidate) if candidate.exists() else None

    def _deliver(self, events: list[TouchEvent]) -> None:
        """Send engine output to Android through the tested pipeline."""
        if self._pipeline.channel is None:
            self._ensure_channel()
        if not events:
            return
        delivered = self._pipeline.deliver(events)
        self._latency_label.setText(self._pipeline.summary())
        if not delivered and self._pipeline.last_error:
            self._set_status(f"input not delivered: {self._pipeline.last_error}", 8000)
            self._append_log(f"touch delivery failed: {self._pipeline.last_error}")

    def _on_touch_pressed(self, x: float, y: float) -> None:
        if self._engine is not None:
            self._set_status(f"touch down at Android ({x:.0f}, {y:.0f})")

    def _on_touch_moved(self, x: float, y: float) -> None:
        # Reserved for cursor feedback; the engine emits MOVE on key changes.
        _ = (x, y)

    def _on_touch_released(self, x: float, y: float) -> None:
        self._set_status(f"touch up at Android ({x:.0f}, {y:.0f})")

    def keyPressEvent(self, event: QKeyEvent) -> None:
        if event.key() == Qt.Key.Key_F11:
            self._toggle_fullscreen()
            return
        if event.key() == Qt.Key.Key_Escape and self.isFullScreen():
            self.showNormal()
            return
        if self._engine is not None and not event.isAutoRepeat():
            events = self._engine.key_down(_key_name(event))
            if events:
                self._deliver(events)
                self._surface.set_active_elements(self._engine.active_element_ids())
                event.accept()
                return
        super().keyPressEvent(event)

    def keyReleaseEvent(self, event: QKeyEvent) -> None:
        if self._engine is not None and not event.isAutoRepeat():
            events = self._engine.key_up(_key_name(event))
            if events:
                self._deliver(events)
                self._surface.set_active_elements(self._engine.active_element_ids())
                event.accept()
                return
        super().keyReleaseEvent(event)

    def focusOutEvent(self, event: QFocusEvent) -> None:
        # Stage 8: never leave a contact or a held key behind.
        if self._settings.release_on_focus_loss and self._engine is not None:
            released = self._engine.focus_lost()
            self._surface.set_active_elements(set())
            if released:
                log.info("released %d contact(s) on focus loss", len(released))
        super().focusOutEvent(event)

    def _toggle_fullscreen(self) -> None:
        if self.isFullScreen():
            self.showNormal()
        else:
            self.showFullScreen()

    # ==================================================================
    # display
    # ==================================================================
    def _on_preset_chosen(self, index: int) -> None:
        if index <= 0:
            return
        preset = PRESETS[index - 1]
        self._width_spin.setValue(preset.width)
        self._height_spin.setValue(preset.height)
        self._dpi_spin.setValue(preset.dpi)

    def _on_display_form_changed(self, _value: int) -> None:
        form = self._display_form().validated()
        if form.warnings or not form.is_valid:
            self._display_warning.setText(form.message())
        else:
            self._display_warning.setText(
                "buffer size applies on the next session restart; DPI applies live"
            )
        self._apply_display_button.setEnabled(
            form.is_valid and (self._caps is None or vm.display_control(self._caps).enabled)
        )

    def _display_form(self) -> vm.DisplayForm:
        return vm.DisplayForm(
            width=str(self._width_spin.value()),
            height=str(self._height_spin.value()),
            dpi=str(self._dpi_spin.value()),
        )

    def _apply_display(self) -> None:
        form = self._display_form().validated()
        if not form.is_valid:
            QMessageBox.warning(self, "Invalid resolution", form.message())
            return
        request, _ = __import__("lynxdroid.display.resolution", fromlist=["x"]).validate_display(
            form.width, form.height, form.dpi
        )
        if request is None:
            return
        accepted = QMessageBox.question(
            self,
            "Change the Android viewport?",
            vm.consent_text(
                f"Set Android to {request.width}×{request.height} @ {request.dpi} dpi",
                "LynxDroid stores the previous values, then writes "
                "persist.waydroid.width/height and runs 'wm density'.",
            ),
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if accepted != QMessageBox.StandardButton.Yes:
            return
        self._surface.set_buffer(request.width, request.height)
        self._save_setting(
            buffer_width=request.width, buffer_height=request.height, dpi=request.dpi
        )
        self._tasks.submit(
            self._backend.apply_display,
            request.width,
            request.height,
            request.dpi,
            on_done=self._on_display_applied,
            on_error=lambda message, detail: self._set_status(
                f"display change failed: {message}", 15000
            ),
        )

    def _on_display_applied(self, steps: list[str]) -> None:
        message, _tone = vm.apply_changes_message(steps)
        self._set_status(message, 20000)
        self._refresh_geometry_label()

    def _on_fit_changed(self, mode: str) -> None:
        self._surface.set_fit_mode(mode)
        self._save_setting(fit_mode=mode)
        self._refresh_geometry_label()

    def _on_rotation_changed(self, index: int) -> None:
        rotation = Rotation(self._rotation_combo.itemData(index))
        self._surface.set_rotation(rotation)
        if self._engine is not None:
            self._engine.set_geometry(self._surface.geometry_info())
        self._refresh_geometry_label()

    def _refresh_geometry_label(self) -> None:
        geometry = self._surface.geometry_info()
        self._geometry_label.setText(vm.geometry_caption(geometry))
        self._status_geometry.setText(
            f"geometry: {geometry.buffer.width}×"
            f"{geometry.buffer.height} / "
            f"{geometry.bars()['left']}+{geometry.bars()['top']} bars"
        )

    # ==================================================================
    # profiles
    # ==================================================================
    def _load_profiles(self) -> None:
        profiles = self._profile_store.load_all()
        self._profile_list.clear()
        rows = vm.profile_rows(
            profiles,
            live_buffer=(
                self._surface.geometry_info().buffer.width,
                self._surface.geometry_info().buffer.height,
            ),
        )
        for row in rows:
            text = f"{row.name}\n{row.package} · {row.elements}"
            if row.warning:
                text += f"\n⚠ {row.warning}"
            item = QListWidgetItem(text)
            item.setData(Qt.ItemDataRole.UserRole, row.id)
            self._profile_list.addItem(item)
        if rows:
            self._profile_list.setCurrentRow(0)

    def _on_profile_selected(
        self, current: QListWidgetItem | None, _previous: QListWidgetItem | None
    ) -> None:
        if current is None:
            self._discard_unsaved()
            self._profile = None
            self._editor = None
            self._surface.set_editor_mode(False)
            self._surface.set_profile(None)
            self._element_list.clear()
            return
        profile_id = current.data(Qt.ItemDataRole.UserRole)
        try:
            self._profile = self._profile_store.load(profile_id)
        except Exception as exc:
            self._set_status(f"cannot open profile: {exc}", 15000)
            return
        self._editor = LayoutEditor(profile=self._profile, saved=self._profile)
        self._surface.set_editor_mode(True)
        self._surface.set_profile(self._profile)
        self._render_elements()
        self._ensure_engine()
        self._refresh_overlay_hint()

    def _on_element_selected(self, element_id: str) -> None:
        if self._editor is not None:
            self._editor.selected = element_id or None
        for row in range(self._element_list.count()):
            item = self._element_list.item(row)
            if item is not None:
                item.setSelected(item.data(Qt.ItemDataRole.UserRole) == element_id)

    def _on_element_moved(self, element_id: str, x: float, y: float) -> None:
        """A drag finished a step: mirror it into the list and the dirty state."""
        if self._editor is None:
            return
        self._profile = self._editor.profile
        self._render_elements()
        self._save_profile_button.setEnabled(self._editor.dirty)
        self._set_status(f"{element_id}: {x:.3f}, {y:.3f} (unsaved)", 4000)

    def _discard_unsaved(self) -> None:
        """Ask before silently dropping a moved key when the user switches."""
        if self._editor is None or not self._editor.dirty:
            return
        answer = QMessageBox.question(
            self,
            "Unsaved layout changes",
            f"The layout '{self._profile.name if self._profile else ''}' has unsaved "
            "changes. Save them before switching?",
            QMessageBox.StandardButton.Save | QMessageBox.StandardButton.Discard,
            QMessageBox.StandardButton.Save,
        )
        if answer == QMessageBox.StandardButton.Save:
            self._save_profile()

    def _save_profile(self) -> None:
        if self._editor is None or self._profile is None:
            return
        try:
            self._profile_store.save(self._editor.profile)
        except Exception as exc:
            self._set_status(f"cannot save profile: {exc}", 15000)
            return
        self._profile = self._editor.profile
        self._editor.mark_saved(self._profile)
        self._save_profile_button.setEnabled(False)
        self._set_status("layout saved", 4000)

    def _render_elements(self) -> None:
        self._element_list.clear()
        if self._profile is None:
            return
        for element in self._profile.elements:
            item = QListWidgetItem(f"{element.label[:14]:<14} {vm.element_summary(element)}")
            item.setData(Qt.ItemDataRole.UserRole, element.id)
            self._element_list.addItem(item)

    def _refresh_overlay_hint(self) -> None:
        if self._caps is None:
            self._overlay_hint.setText("overlay inactive: backend capabilities not read yet")
            return
        state = vm.overlay_control(self._caps, self._channel_caps, self._profile)
        self._overlay_hint.setText(
            "overlay active — presses are converted to Android touches"
            if state.enabled
            else f"overlay inactive: {state.reason}"
        )
        self._overlay_hint.setStyleSheet("" if state.enabled else "color: #f2b134;")

    def _create_profile(self) -> None:
        from PySide6.QtWidgets import QInputDialog

        name, ok = QInputDialog.getText(self, "New layout profile", "Profile name:")
        if not ok or not name.strip():
            return
        package, ok = QInputDialog.getText(
            self,
            "Android application",
            "Package name (for example com.company.game):",
        )
        if not ok or not package.strip():
            return
        try:
            from lynxdroid.profiles.model import Profile, Target

            profile = Profile.new(
                name.strip(),
                Target(package=package.strip()),
                reference=_reference_for(self._surface.geometry_info()),
            )
            self._profile_store.save(profile)
        except Exception as exc:
            QMessageBox.warning(self, "Cannot create profile", str(exc))
            return
        self._load_profiles()

    def _delete_profile(self) -> None:
        if self._profile is None:
            return
        if self._settings.confirm_destructive:
            accepted = QMessageBox.question(
                self,
                "Delete profile?",
                f"Delete '{self._profile.name}'? A backup copy is kept in\n"
                f"{self._paths.backups_dir}",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No,
            )
            if accepted != QMessageBox.StandardButton.Yes:
                return
        try:
            self._profile_store.delete(self._profile.id)
        except Exception as exc:
            QMessageBox.warning(self, "Cannot delete", str(exc))
            return
        self._profile = None
        self._surface.set_profile(None)
        self._load_profiles()

    # ==================================================================
    # apps
    # ==================================================================
    def _refresh_apps(self) -> None:
        self._apps_list.clear()
        self._apps_note.setText("loading…")
        self._tasks.submit(
            self._backend.list_apps,
            on_done=self._on_apps,
            on_error=lambda message, detail: self._apps_note.setText(
                f"cannot list applications: {message}"
            ),
        )

    def _on_apps(self, raw: list[dict[str, str]]) -> None:
        from lynxdroid.apps.catalog import merge_apps

        profiled = {p.target.package for p in self._profile_store.load_all()}
        entries = merge_apps(raw, profiled_packages=profiled)
        self._apps_list.clear()
        for entry in entries:
            marker = "•" if entry.has_profile else " "
            item = QListWidgetItem(f"{marker} {entry.label}\n   {entry.package}")
            item.setData(Qt.ItemDataRole.UserRole, entry.package)
            self._apps_list.addItem(item)
        self._apps_note.setText(
            f"{len(entries)} package(s); • marks packages with a layout profile"
            if entries
            else "no installed packages reported by the backend"
        )

    def _launch_selected_app(self, item: QListWidgetItem) -> None:
        package = item.data(Qt.ItemDataRole.UserRole)
        if not package:
            return
        self._set_status(f"launching {package}…")
        self._tasks.submit(
            self._backend.launch_app,
            package,
            on_done=lambda ok: self._set_status(f"launched {package}"),
            on_error=lambda message, detail: self._set_status(f"launch failed: {message}", 15000),
        )

    # ==================================================================
    # doctor
    # ==================================================================
    def _run_doctor(self) -> None:
        from lynxdroid.doctor import checks as doctor_checks

        self._doctor_summary.setText("running diagnostics…")
        self._tasks.submit(
            doctor_checks.full_report,
            on_done=self._on_report,
            on_error=lambda message, detail: self._doctor_summary.setText(
                f"diagnostics failed: {message}"
            ),
        )

    def _on_report(self, report: Report) -> None:
        self._report = report
        summary, tone = vm.report_summary(report)
        palette = palette_for(self._settings.theme)
        self._doctor_summary.setText(summary)
        self._doctor_summary.setStyleSheet(f"color: {palette.tone_colour(tone)};")
        self._render_doctor()
        self._refresh_banners()
        report_path = self._paths.reports_dir / "doctor-latest.json"
        try:
            report_path.write_text(report.to_json(), encoding="utf-8")
        except OSError as exc:
            log.warning("cannot write report: %s", exc)

    def _render_doctor(self) -> None:
        if self._report is None:
            return
        self._doctor_list.clear()
        palette = palette_for(self._settings.theme)
        rows = vm.check_rows(self._report, include_ok=not self._hide_ok_checkbox.isChecked())
        for row in rows:
            text = f"{row.symbol} {row.title}\n    {row.detail}"
            if row.next_step:
                text += f"\n    → {row.next_step}"
            item = QListWidgetItem(text)
            item.setForeground(_colour(palette.tone_colour(row.tone)))
            self._doctor_list.addItem(item)

    # ==================================================================
    # surface
    # ==================================================================
    def _refresh_surface(self) -> None:
        geometry = self._surface.geometry_info()
        if self._engine is not None:
            self._engine.set_geometry(geometry)
        self._refresh_geometry_label()

    def resizeEvent(self, event: QResizeEvent) -> None:
        super().resizeEvent(event)
        self._refresh_geometry_label()

    def closeEvent(self, event: QCloseEvent) -> None:
        if self._engine is not None:
            self._engine.focus_lost("window closed")
        self._poller.stop()
        self._tasks.shutdown()
        super().closeEvent(event)


# --------------------------------------------------------------------------
# small widget helpers
# --------------------------------------------------------------------------


def _section(text: str) -> QLabel:
    label = QLabel(text)
    label.setObjectName("SectionTitle")
    return label


def _scrolled(page: QWidget) -> QWidget:
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setFrameShape(QFrame.Shape.NoFrame)
    scroll.setWidget(page)
    return scroll


def _colour(hex_colour: str) -> QColor:
    return QColor(hex_colour)


SPECIAL_KEYS: dict[int, str] = {}


def _key_name(event: QKeyEvent) -> str:
    """Map a Qt key event to the profile's key vocabulary.

    Lower case letters and digits map to themselves; everything else uses
    Qt's key name so a profile can bind e.g. "Space" or "Shift" explicitly.
    """
    text = event.text()
    if text and text.isprintable() and not text.isspace():
        return text.lower()
    from PySide6.QtCore import Qt as _Qt

    special: dict[int, str] = {
        int(_Qt.Key.Key_Space): "Space",
        int(_Qt.Key.Key_Return): "Return",
        int(_Qt.Key.Key_Enter): "Enter",
        int(_Qt.Key.Key_Escape): "Escape",
        int(_Qt.Key.Key_Tab): "Tab",
        int(_Qt.Key.Key_Backspace): "Backspace",
        int(_Qt.Key.Key_Shift): "Shift",
        int(_Qt.Key.Key_Control): "Control",
        int(_Qt.Key.Key_Alt): "Alt",
        int(_Qt.Key.Key_Up): "Up",
        int(_Qt.Key.Key_Down): "Down",
        int(_Qt.Key.Key_Left): "Left",
        int(_Qt.Key.Key_Right): "Right",
    }
    code = int(event.key())
    return special.get(code, f"Key{code}")


def _reference_for(geometry: ViewportGeometry) -> ReferenceResolution:
    """Reference resolution for a new profile: the live buffer, sanitised."""
    return ReferenceResolution(
        width=max(320, geometry.buffer.width),
        height=max(320, geometry.buffer.height),
        dpi=240,
    )


class _BannerWidget(QFrame):
    """One banner row; the tone drives its colour."""

    def __init__(self, banner: vm.Banner, palette: Palette, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("Banner")
        colour = palette.tone_colour(banner.tone)
        self.setStyleSheet(
            f"QFrame#Banner {{ background: {palette.surface_alt}; border: 1px solid {colour}; }}"
        )
        layout = QHBoxLayout(self)
        layout.setContentsMargins(10, 6, 10, 6)
        title = QLabel(banner.text)
        title.setStyleSheet(f"color: {colour}; font-weight: 600;")
        layout.addWidget(title)
        if banner.detail:
            detail = QLabel(banner.detail)
            detail.setObjectName("Muted")
            detail.setWordWrap(True)
            layout.addWidget(detail, 1)
        if banner.dismissible:
            close = QPushButton("✕")
            close.setFixedWidth(26)
            close.clicked.connect(self._dismiss)
            layout.addWidget(close)

    def _dismiss(self) -> None:
        self.setParent(None)
        self.deleteLater()
