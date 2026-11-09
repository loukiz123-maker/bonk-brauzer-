"""Visual design tokens and the Qt stylesheet built from them.

The palette is a single dict so the whole app can be re-themed without
hunting for hex codes in widget code. Contrast is checked in tests against
WCAG AA for normal text (>= 4.5:1) on the surfaces it is used on.
"""

from __future__ import annotations

from dataclasses import dataclass

from lynxdroid.gui.viewmodel import Tone


@dataclass(frozen=True)
class Palette:
    name: str
    window: str
    surface: str
    surface_alt: str
    border: str
    text: str
    text_muted: str
    accent: str
    accent_text: str
    positive: str
    warning: str
    danger: str
    info: str
    letterbox: str

    def tone_colour(self, tone: Tone) -> str:
        return {
            Tone.NEUTRAL: self.text_muted,
            Tone.POSITIVE: self.positive,
            Tone.WARNING: self.warning,
            Tone.DANGER: self.danger,
            Tone.INFO: self.info,
        }[tone]


DARK = Palette(
    name="dark",
    window="#11151b",
    surface="#171d26",
    surface_alt="#1e2632",
    border="#2b3648",
    text="#e8edf4",
    text_muted="#9aa7b8",
    accent="#4f9dff",
    accent_text="#08101c",
    positive="#3ddc84",
    warning="#f2b134",
    danger="#ff6b6b",
    info="#7cc4ff",
    letterbox="#000000",
)

LIGHT = Palette(
    name="light",
    window="#f4f6f9",
    surface="#ffffff",
    surface_alt="#eef1f6",
    border="#d3dae4",
    text="#151b23",
    text_muted="#5b6675",
    accent="#1f6feb",
    accent_text="#ffffff",
    positive="#12874a",
    warning="#9a6300",
    danger="#c62828",
    info="#0b62a4",
    letterbox="#000000",
)

PALETTES = {"dark": DARK, "light": LIGHT}


def palette_for(name: str, *, system_is_dark: bool = True) -> Palette:
    """Resolve a theme name to a palette.

    ``"system"`` is a real choice in ``settings.Settings``, so it has to mean
    something: it follows the desktop's colour scheme. The caller passes what
    Qt reported (``system_is_dark``); without that information the dark
    palette is used, which is this project's default rather than a guess about
    the user's desktop.
    """
    if name == "system":
        return DARK if system_is_dark else LIGHT
    return PALETTES.get(name, DARK)


STYLESHEET = """
QWidget {{
    background: {window};
    color: {text};
    font-size: 13px;
}}
QMainWindow, QDialog {{ background: {window}; }}

QFrame#Card {{
    background: {surface};
    border: 1px solid {border};
    border-radius: 10px;
}}
QFrame#Banner {{
    border-radius: 8px;
    padding: 8px;
}}
QLabel#Headline {{ font-size: 19px; font-weight: 600; }}
QLabel#SectionTitle {{ font-size: 14px; font-weight: 600; color: {text}; }}
QLabel#Muted {{ color: {text_muted}; }}
QLabel#Mono {{ font-family: "JetBrains Mono", "DejaVu Sans Mono", monospace; }}

QPushButton {{
    background: {surface_alt};
    border: 1px solid {border};
    border-radius: 8px;
    padding: 7px 14px;
    color: {text};
}}
QPushButton:hover:enabled {{ border-color: {accent}; }}
QPushButton:pressed:enabled {{ background: {border}; }}
QPushButton:disabled {{ color: {text_muted}; border-color: {border}; background: {surface}; }}
QPushButton#Primary {{
    background: {accent};
    color: {accent_text};
    border: 1px solid {accent};
    font-weight: 600;
}}
QPushButton#Primary:hover:enabled {{ background: {info}; }}
QPushButton#Danger {{ border-color: {danger}; color: {danger}; }}
QPushButton:checked {{ border-color: {accent}; background: {surface_alt}; }}

QLineEdit, QSpinBox, QComboBox {{
    background: {surface_alt};
    border: 1px solid {border};
    border-radius: 6px;
    padding: 5px 8px;
    selection-background-color: {accent};
}}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus {{ border-color: {accent}; }}
QLineEdit[invalid="true"] {{ border-color: {danger}; }}

QListWidget, QTreeWidget, QTableWidget {{
    background: {surface};
    border: 1px solid {border};
    border-radius: 8px;
    alternate-background-color: {surface_alt};
}}
QListWidget::item, QTreeWidget::item {{ padding: 6px; }}
QListWidget::item:selected, QTreeWidget::item:selected {{
    background: {surface_alt};
    color: {text};
    border-left: 2px solid {accent};
}}

QTabWidget::pane {{ border: 1px solid {border}; border-radius: 8px; top: -1px; }}
QTabBar::tab {{
    background: transparent;
    padding: 8px 14px;
    color: {text_muted};
    border-bottom: 2px solid transparent;
}}
QTabBar::tab:selected {{ color: {text}; border-bottom: 2px solid {accent}; }}

QScrollBar:vertical {{ background: transparent; width: 10px; margin: 2px; }}
QScrollBar::handle:vertical {{ background: {border}; border-radius: 5px; min-height: 24px; }}
QScrollBar::add-line, QScrollBar::sub-line {{ height: 0; }}

QSplitter::handle {{ background: {border}; }}
QToolTip {{
    background: {surface_alt};
    color: {text};
    border: 1px solid {border};
    padding: 4px;
}}
QStatusBar {{ color: {text_muted}; }}
QCheckBox {{ spacing: 8px; }}
QGroupBox {{
    border: 1px solid {border};
    border-radius: 8px;
    margin-top: 12px;
    padding-top: 8px;
}}
QGroupBox::title {{
    subcontrol-origin: margin;
    left: 10px;
    color: {text_muted};
}}
"""


def stylesheet(palette: Palette) -> str:
    return STYLESHEET.format(**palette.__dict__)


# --------------------------------------------------------------------------
# Contrast (used by tests)
# --------------------------------------------------------------------------


def _srgb_to_linear(channel: float) -> float:
    return channel / 12.92 if channel <= 0.04045 else ((channel + 0.055) / 1.055) ** 2.4


def relative_luminance(hex_colour: str) -> float:
    value = hex_colour.lstrip("#")
    if len(value) != 6:
        raise ValueError(f"expected #rrggbb, got {hex_colour!r}")
    r, g, b = (int(value[i : i + 2], 16) / 255 for i in (0, 2, 4))
    return 0.2126 * _srgb_to_linear(r) + 0.7152 * _srgb_to_linear(g) + 0.0722 * _srgb_to_linear(b)


def contrast_ratio(foreground: str, background: str) -> float:
    """WCAG 2.1 contrast ratio between two colours (1.0 .. 21.0)."""
    first = relative_luminance(foreground)
    second = relative_luminance(background)
    lighter, darker = max(first, second), min(first, second)
    return (lighter + 0.05) / (darker + 0.05)
