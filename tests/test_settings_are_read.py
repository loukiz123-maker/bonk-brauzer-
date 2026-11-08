"""Every setting the program saves must be read by something.

`Settings` offers 17 fields. Seven of them - fit_mode, fullscreen,
input_mode, capture_pointer, allow_service_control, allow_prop_writes,
show_bar_thickness - were written, clamped, persisted, and never read by any
code path. A user could change `input_mode` to "adb" and the program would
still try the agent first; nothing failed, because a setting nobody reads
fails silently by definition.

This module makes that impossible to reintroduce. It does not check that the
*value* is used correctly - a test cannot - but it refuses the state where a
setting is advertised and abandoned.
"""

from __future__ import annotations

import ast
import re
from pathlib import Path

from lynxdroid.settings import Settings

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "lynxdroid"
SETTINGS_SRC = SRC / "settings.py"

#: Fields that exist for the persistence machinery itself, not for behaviour.
STRUCTURAL = {"schema"}

#: Settings that are read, but only by tests or by the CLI's own output -
#: recording why each one is here is the point of the list.
DELIBERATE: dict[str, str] = {}


def _settings_fields() -> set[str]:
    return set(Settings.__dataclass_fields__) - STRUCTURAL


def _readers() -> dict[str, list[str]]:
    """Every place a setting is read as an attribute, outside settings.py."""
    found: dict[str, list[str]] = {}
    names = _settings_fields()
    for path in sorted(SRC.rglob("*.py")):
        if path == SETTINGS_SRC:
            continue
        source = path.read_text(encoding="utf-8")
        for node in ast.walk(ast.parse(source)):
            if not isinstance(node, ast.Attribute) or node.attr not in names:
                continue
            # `self._settings.theme`, `settings.doctor_on_start`, `snap.detail`
            if (
                isinstance(node.value, ast.Name)
                and "settings" not in node.value.id.lower()
                and not node.value.id.startswith("_")
            ):
                continue
            found.setdefault(node.attr, []).append(f"{path.relative_to(ROOT)}:{node.lineno}")
    return found


class TestEverySettingIsRead:
    def test_no_setting_is_offered_and_ignored(self) -> None:
        readers = _readers()
        unread = sorted(_settings_fields() - set(readers) - set(DELIBERATE))
        assert unread == [], (
            "these settings are saved but never read, so changing them does "
            "nothing: " + ", ".join(unread)
        )

    def test_the_scan_finds_readers(self) -> None:
        """Otherwise the test above passes because the scanner sees nothing."""
        readers = _readers()
        assert len(readers) >= 8, f"only {len(readers)} settings appear to be read"

    def test_the_field_list_is_not_empty(self) -> None:
        assert len(_settings_fields()) >= 12


class TestDeliberateEntries:
    def test_every_exemption_names_a_real_setting(self) -> None:
        """A stale exemption would hide a genuinely abandoned setting."""
        for name in DELIBERATE:
            assert name in _settings_fields(), f"{name} is not a setting any more"

    def test_every_exemption_says_why(self) -> None:
        for name, reason in DELIBERATE.items():
            assert reason.strip(), f"{name} is exempt for no stated reason"


class TestSettingsSurfaceIsDocumented:
    """A setting the user cannot find in the docs does not exist for them."""

    def test_every_setting_is_documented(self) -> None:
        docs = "\n".join(
            p.read_text(encoding="utf-8") for p in sorted((ROOT / "docs").glob("*.md"))
        )
        undocumented = []
        for name in sorted(_settings_fields()):
            if not re.search(rf"\b{re.escape(name)}\b", docs):
                undocumented.append(name)
        assert undocumented == [], "settings that appear in no document: " + ", ".join(undocumented)


class TestSettingsTableMatchesTheCode:
    """The table must name the module that reads each setting.

    A document that says "read by gui/main_window.py" when the reader is in
    gui/viewmodel.py is the same kind of error as I fixed by hand twice: true
    in spirit, wrong in the file a reader would open.
    """

    def _table(self) -> str:
        return (ROOT / "docs" / "SETTINGS.md").read_text(encoding="utf-8")

    def test_every_setting_has_a_row(self) -> None:
        table = self._table()
        missing = [
            name
            for name in sorted(_settings_fields())
            if not re.search(rf"^\| `{re.escape(name)}` \|", table, re.MULTILINE)
        ]
        assert missing == [], "settings with no row in docs/SETTINGS.md: " + ", ".join(missing)

    def test_every_module_the_table_names_exists(self) -> None:
        # Only the "read by" column counts; a module named in a sentence is
        # prose, not a claim about where the value is used.
        named = set()
        for row in self._table().splitlines():
            if row.startswith("| `"):
                named |= set(re.findall(r"`([a-z_]+/[a-z_]+\.py)", row.rsplit("|", 2)[-2]))
        missing = [name for name in sorted(named) if not (SRC / name).exists()]
        assert missing == [], "docs/SETTINGS.md names modules that do not exist: " + ", ".join(
            missing
        )

    def test_the_table_names_the_module_that_actually_reads_it(self) -> None:
        readers = _readers()
        table = self._table()
        wrong: list[str] = []
        for name in sorted(_settings_fields()):
            row = re.search(rf"^\| `{re.escape(name)}` \|.*$", table, re.MULTILINE)
            if row is None:
                continue
            cited = {
                m.replace("lynxdroid/", "")
                for m in re.findall(r"`([a-z_]+/[a-z_]+\.py)", row.group(0))
            }
            if not cited:
                continue
            real = {
                loc.split(":")[0].replace("src/lynxdroid/", "") for loc in readers.get(name, [])
            }
            # The reading site may hand the value to another module that does
            # the work (`input_mode` is read in main_window and passed to the
            # selector), so an entry counts when the code path reaches it.
            if any(_module_is_reached(module, name) for module in cited):
                continue
            if not (cited & real):
                wrong.append(f"{name}: table says {sorted(cited)}, code reads it in {sorted(real)}")
        assert wrong == [], "the settings table is out of date: " + "; ".join(wrong)


def _module_is_reached(module: str, setting: str) -> bool:
    """True when ``module`` mentions the setting, or a helper named after it.

    ``gui/main_window.py`` reads ``input_mode`` and calls
    ``channel_for_mode`` from ``input/pipeline.py``, which is where the
    setting turns into a channel. Following the helper by name is enough to
    accept that, without pretending to be a call graph.
    """
    path = SRC / module
    if not path.exists():
        return False
    source = path.read_text(encoding="utf-8")
    if re.search(rf"\b{re.escape(setting)}\b", source):
        return True
    helpers = {
        "input_mode": ["channel_for_mode", "INPUT_MODES"],
        "theme": ["palette_for", "PALETTES"],
        "show_bar_thickness": ["set_bar_labels", "bar_labels"],
        "log_level": ["setup_logging", "log_level"],
    }.get(setting, [])
    return any(re.search(rf"\b{re.escape(h)}\b", source) for h in helpers)
