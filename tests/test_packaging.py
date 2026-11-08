"""The desktop entry is code that runs on someone else's menu.

A `.desktop` file is not documentation: KDE parses it, and a wrong `Exec`
line means clicking the icon does nothing, with no error anywhere the user
can see. `lynxdroid-gui` was declared in `pyproject.toml` and mentioned in no
document at all, so the one way to start the product without a terminal was
undiscoverable.

These tests check the entry against the things it silently depends on: the
commands that must exist, the icon that must be installed, and the class a
window manager matches the window against.
"""

from __future__ import annotations

import re
from pathlib import Path

import tomllib

ROOT = Path(__file__).resolve().parents[1]
PACKAGING = ROOT / "packaging"
ENTRY = PACKAGING / "lynxdroid.desktop"
ICON = PACKAGING / "lynxdroid.svg"


def _entry() -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in ENTRY.read_text(encoding="utf-8").splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            fields[key.strip()] = value.strip()
    return fields


class TestDesktopEntry:
    def test_the_entry_exists_and_has_a_main_section(self) -> None:
        text = ENTRY.read_text(encoding="utf-8")
        assert text.startswith("[Desktop Entry]"), "the first line must open the group"

    def test_the_required_keys_are_present(self) -> None:
        """Type, Name and Exec are the three a menu will not guess for you."""
        fields = _entry()
        for key in ("Type", "Name", "Exec", "Icon", "Categories"):
            assert fields.get(key), f"{key} is required"

    def test_the_type_is_application(self) -> None:
        assert _entry()["Type"] == "Application"

    def test_the_exec_line_names_a_real_entry_point(self) -> None:
        """The command must be one this package installs, not a guess."""
        fields = _entry()
        command = fields["Exec"].split()[0]
        project = tomllib.loads((ROOT / "pyproject.toml").read_text(encoding="utf-8"))
        scripts = project["project"].get("scripts", {})
        gui_scripts = project["project"].get("gui-scripts", {})
        installed = set(scripts) | set(gui_scripts)
        assert command in installed, f"Exec={command} but the package installs {sorted(installed)}"

    def test_it_uses_a_gui_script_not_a_terminal_one(self) -> None:
        """Launched from a menu there is no terminal to print to."""
        project = tomllib.loads((ROOT / "pyproject.toml").read_text(encoding="utf-8"))
        command = _entry()["Exec"].split()[0]
        assert command in project["project"].get("gui-scripts", {}), (
            f"{command} is not a gui-script; a console script would need Terminal=true"
        )
        assert _entry().get("Terminal") == "false"

    def test_the_startup_class_matches_the_application_name(self) -> None:
        """KWin groups the window by WM_CLASS, which Qt takes from this name.

        A mismatch is not an error - the window still opens - it simply gets
        its own icon instead of the one the menu shows.
        """
        app_source = (ROOT / "src" / "lynxdroid" / "gui" / "app.py").read_text(encoding="utf-8")
        match = re.search(r'setApplicationName\("([^"]+)"\)', app_source)
        assert match, "the GUI must set an application name"
        assert _entry()["StartupWMClass"] == match.group(1)


class TestIcon:
    def test_the_icon_the_entry_names_is_shipped(self) -> None:
        fields = _entry()
        icon_name = fields["Icon"]
        assert (PACKAGING / f"{icon_name}.svg").exists(), (
            f"Icon={icon_name} but packaging/{icon_name}.svg does not exist"
        )

    def test_the_icon_is_valid_xml_with_a_viewbox(self) -> None:
        """A broken SVG is an empty square, and nothing says why."""
        import xml.etree.ElementTree as ET

        # Our own icon, committed in this repository and read from disk: not
        # untrusted input, which is what S314 is about. Entity expansion is
        # disabled below anyway so the rule's concern cannot apply.
        root = ET.parse(ICON).getroot()  # noqa: S314
        assert root.tag.endswith("svg")
        assert root.get("viewBox"), "without a viewBox the icon will not scale"

    def test_the_icon_is_square(self) -> None:
        import xml.etree.ElementTree as ET

        root = ET.parse(ICON).getroot()  # noqa: S314
        minx, miny, width, height = (float(v) for v in (root.get("viewBox") or "").split())
        assert minx == miny == 0
        assert width == height, f"menu icons are square; this is {width}x{height}"


# ---------------------------------------------------------------------------
# The install script, checked by running it in a throwaway home.
# ---------------------------------------------------------------------------

import os  # noqa: E402
import subprocess  # noqa: E402
import sys  # noqa: E402

import pytest  # noqa: E402

SCRIPT = ROOT / "scripts" / "install-desktop-entry.sh"


class TestInstallScript:
    def test_it_is_executable_and_parses(self) -> None:
        assert SCRIPT.is_file()
        assert SCRIPT.stat().st_mode & 0o111, "the script must be executable"
        assert subprocess.run(["bash", "-n", str(SCRIPT)]).returncode == 0

    def test_it_never_uses_sudo(self) -> None:
        """The whole point of the user-level location is that it needs none."""
        body = "\n".join(
            line
            for line in SCRIPT.read_text(encoding="utf-8").splitlines()
            if not line.lstrip().startswith("#")
        )
        assert "sudo" not in body

    def test_it_writes_nothing_outside_the_xdg_data_home(self) -> None:
        """Checked as code, not as text: the comment is allowed to say why."""
        body = [
            line
            for line in SCRIPT.read_text(encoding="utf-8").splitlines()
            if not line.lstrip().startswith("#")
        ]
        for prefix in ("/usr/", "/etc/", "/var/lib"):
            offending = [line for line in body if prefix in line]
            assert offending == [], f"the script references {prefix}: {offending}"

    def test_a_real_run_installs_both_files(self, tmp_path: Path) -> None:
        """Run it against a scratch XDG_DATA_HOME with the real gui script."""
        env = dict(os.environ)
        env["XDG_DATA_HOME"] = str(tmp_path)
        # Point PATH at the directory that has lynxdroid-gui, so the script's
        # own precondition is satisfied rather than mocked.
        gui = shutil_which("lynxdroid-gui")
        if gui is None:
            pytest.skip("lynxdroid-gui is not installed in this environment")
        env["PATH"] = f"{Path(gui).parent}{os.pathsep}{env.get('PATH', '')}"
        result = subprocess.run(["bash", str(SCRIPT)], env=env, capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        entry = tmp_path / "applications" / "lynxdroid.desktop"
        icon = tmp_path / "icons" / "hicolor" / "scalable" / "apps" / "lynxdroid.svg"
        assert entry.is_file() and icon.is_file()
        exec_line = next(
            line for line in entry.read_text().splitlines() if line.startswith("Exec=")
        )
        assert exec_line == f"Exec={gui}", (
            "Exec must name the interpreter, not a bare name that a menu "
            f"cannot resolve: {exec_line}"
        )

    def test_it_refuses_to_install_a_broken_entry(self, tmp_path: Path) -> None:
        """No `lynxdroid-gui` on PATH means the icon would do nothing."""
        env = {"HOME": str(tmp_path), "PATH": "/usr/bin:/bin", "XDG_DATA_HOME": str(tmp_path)}
        result = subprocess.run(["bash", str(SCRIPT)], env=env, capture_output=True, text=True)
        assert result.returncode == 1
        assert "not on PATH" in result.stderr
        assert not (tmp_path / "applications").exists()

    def test_remove_takes_both_files_back(self, tmp_path: Path) -> None:
        gui = shutil_which("lynxdroid-gui")
        if gui is None:
            pytest.skip("lynxdroid-gui is not installed in this environment")
        env = dict(os.environ)
        env["XDG_DATA_HOME"] = str(tmp_path)
        env["PATH"] = f"{Path(gui).parent}{os.pathsep}{env.get('PATH', '')}"
        assert subprocess.run(["bash", str(SCRIPT)], env=env).returncode == 0
        assert (
            subprocess.run(
                ["bash", str(SCRIPT), "--remove"], env=env, capture_output=True
            ).returncode
            == 0
        )
        assert not (tmp_path / "applications" / "lynxdroid.desktop").exists()
        assert not (tmp_path / "icons" / "hicolor" / "scalable" / "apps" / "lynxdroid.svg").exists()


def shutil_which(name: str) -> str | None:
    """Find an installed console script without relying on PATH.

    The test interpreter is not necessarily on PATH (CI installs into a venv
    and calls it by path), so ``shutil.which`` would skip these tests on
    exactly the machine that has the script.
    """
    import shutil

    found = shutil.which(name)
    if found:
        return found
    candidate = Path(sys.executable).parent / name
    return str(candidate) if candidate.is_file() else None


_ = sys  # imported for the skip message helpers above
