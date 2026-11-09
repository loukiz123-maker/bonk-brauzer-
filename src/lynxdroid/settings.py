"""User settings: typed, versioned, atomically stored.

Settings are separate from profiles because their failure modes differ: a
broken profile only affects one game, a broken settings file affects startup.
So the reader is *lenient* - unknown keys are preserved, out-of-range values
fall back to the default with a recorded warning, and nothing raises into the
UI. The one thing it will not do is guess at a corrupt file: that is reported.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass, field, replace
from pathlib import Path
from typing import Any

from lynxdroid.core.paths import AppPaths
from lynxdroid.core.storage import read_json, write_json
from lynxdroid.display.resolution import MAX_DPI, MAX_EDGE, MIN_DPI, MIN_EDGE
from lynxdroid.version import SCHEMA_VERSIONS

SETTINGS_SCHEMA = SCHEMA_VERSIONS["settings"]


@dataclass(frozen=True)
class Settings:
    """Everything the user can configure. Immutable; use ``with_changes``."""

    schema: int = SETTINGS_SCHEMA
    # --- display -------------------------------------------------------
    buffer_width: int = 1920
    buffer_height: int = 1080
    dpi: int = 320
    fit_mode: str = "contain"
    fullscreen: bool = False
    # --- input ---------------------------------------------------------
    input_mode: str = "auto"  # auto | agent | adb | none
    capture_pointer: bool = False  # relative mouse mode needs consent
    release_on_focus_loss: bool = True
    # --- backend -------------------------------------------------------
    backend: str = "waydroid"
    allow_service_control: bool = False  # root actions require opt-in
    allow_prop_writes: bool = False  # persistent props require opt-in
    arm_translation_warning: bool = True
    # --- ui ------------------------------------------------------------
    theme: str = "dark"
    show_bar_thickness: bool = True
    confirm_destructive: bool = True
    # --- diagnostics ---------------------------------------------------
    doctor_on_start: bool = True
    log_level: str = "INFO"

    def with_changes(self, **changes: Any) -> Settings:
        return replace(self, **changes)

    def clamped(self) -> tuple[Settings, list[str]]:
        """Return a settings object with every value forced into range."""
        warnings: list[str] = []
        width = _clamp_int(self.buffer_width, MIN_EDGE, MAX_EDGE, 1920, "buffer_width", warnings)
        height = _clamp_int(self.buffer_height, MIN_EDGE, MAX_EDGE, 1080, "buffer_height", warnings)
        dpi = _clamp_int(self.dpi, MIN_DPI, MAX_DPI, 320, "dpi", warnings)
        fit = self.fit_mode if self.fit_mode in {"contain", "fill", "integer"} else "contain"
        if fit != self.fit_mode:
            warnings.append(f"fit_mode '{self.fit_mode}' unknown, using contain")
        mode = self.input_mode
        if mode not in {"auto", "agent", "adb", "none"}:
            warnings.append(f"input_mode '{mode}' unknown, using auto")
            mode = "auto"
        backend = self.backend if self.backend in {"waydroid", "fake"} else "waydroid"
        if backend != self.backend:
            warnings.append(f"backend '{self.backend}' unknown, using waydroid")
        level = self.log_level.upper()
        if level not in {"DEBUG", "INFO", "WARNING", "ERROR"}:
            warnings.append(f"log_level '{self.log_level}' unknown, using INFO")
            level = "INFO"
        theme = self.theme if self.theme in {"dark", "light", "system"} else "dark"
        return (
            replace(
                self,
                schema=SETTINGS_SCHEMA,
                buffer_width=width,
                buffer_height=height,
                dpi=dpi,
                fit_mode=fit,
                input_mode=mode,
                backend=backend,
                log_level=level,
                theme=theme,
            ),
            warnings,
        )

    def to_dict(self) -> dict[str, Any]:
        data = asdict(self)
        data["schema"] = SETTINGS_SCHEMA
        return data

    @classmethod
    def from_dict(cls, raw: dict[str, Any]) -> tuple[Settings, list[str]]:
        notes: list[str] = []
        known = set(cls.__dataclass_fields__)
        values: dict[str, Any] = {}
        for key, value in raw.items():
            if key in known and key != "schema":
                values[key] = value
            elif key not in known:
                notes.append(f"ignored unknown setting '{key}' (kept in file)")
        settings = cls(**{k: v for k, v in values.items() if _type_ok(k, v)})
        for key, value in values.items():
            if not _type_ok(key, value):
                notes.append(f"setting '{key}' has wrong type, using default")
        clamped, clamp_notes = settings.clamped()
        return clamped, notes + clamp_notes


def _type_ok(key: str, value: Any) -> bool:
    bool_fields = {
        "fullscreen",
        "capture_pointer",
        "release_on_focus_loss",
        "allow_service_control",
        "allow_prop_writes",
        "arm_translation_warning",
        "show_bar_thickness",
        "confirm_destructive",
        "doctor_on_start",
    }
    int_fields = {"buffer_width", "buffer_height", "dpi"}
    if key in bool_fields:
        return isinstance(value, bool)
    if key in int_fields:
        return isinstance(value, int) and not isinstance(value, bool)
    return isinstance(value, str)


def _clamp_int(
    value: Any, low: int, high: int, default: int, name: str, warnings: list[str]
) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        warnings.append(f"{name} is not an integer, using {default}")
        return default
    bounded = min(max(value, low), high)
    if bounded != value:
        warnings.append(f"{name}={value} clamped to {bounded}")
    return bounded


@dataclass
class SettingsStore:
    """Load/save settings next to the profiles, never inside the repo."""

    paths: AppPaths
    _cache: Settings | None = field(default=None, repr=False)

    def load(self) -> tuple[Settings, list[str]]:
        path = self.paths.settings_file
        try:
            raw = read_json(path)
        except FileNotFoundError:
            settings, notes = Settings().clamped()
            self._cache = settings
            return settings, notes
        settings, notes = Settings.from_dict(raw)
        if raw.get("schema", SETTINGS_SCHEMA) > SETTINGS_SCHEMA:
            notes.append(
                f"settings file schema {raw.get('schema')} is newer than this build; "
                "unknown values were ignored"
            )
        self._cache = settings
        return settings, notes

    def save(self, settings: Settings) -> Path:
        settings, _ = settings.clamped()
        self.paths.ensure()
        write_json(self.paths.settings_file, settings.to_dict())
        self._cache = settings
        return self.paths.settings_file

    def update(self, **changes: Any) -> tuple[Settings, list[str]]:
        current, notes = self.load()
        merged = current.with_changes(**changes)
        clamped, more = merged.clamped()
        self.save(clamped)
        return clamped, notes + more
