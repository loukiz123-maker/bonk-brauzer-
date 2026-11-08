"""Architecture tests: the dependency rules from docs/ARCHITECTURE.md §1.

These are cheap static checks, but they catch the most common way a layered
design rots: someone imports "just one" module across a boundary.
"""

from __future__ import annotations

import ast
from pathlib import Path

import pytest

SRC = Path(__file__).resolve().parents[1] / "src" / "lynxdroid"

#: layer -> layers it may import (itself is implicit)
ALLOWED: dict[str, set[str]] = {
    "core": set(),
    "doctor": {"core", "doctor"},
    "display": {"core", "display"},
    "input": {"core", "display", "input", "profiles"},
    "profiles": {"core", "display", "profiles"},
    "apps": {"core", "apps"},
    "settings": {"core", "display", "settings"},
    "backend": {"core", "backend", "doctor", "display", "profiles"},
    # The CLI is the launcher: it may import ``gui`` lazily for the `gui`
    # subcommand (and only there, inside the handler).
    "cli": {
        "core",
        "doctor",
        "display",
        "input",
        "profiles",
        "apps",
        "settings",
        "backend",
        "cli",
        "version",
        "gui",
    },
    "gui": {
        "core",
        "doctor",
        "display",
        "input",
        "profiles",
        "apps",
        "settings",
        "backend",
        "gui",
        "version",
    },
}


def imports_of_full(path: Path) -> set[str]:
    """Every module name imported, project or not (for "no subprocess" checks)."""
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    found: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            found.update(alias.name for alias in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module:
            found.add(node.module)
    return found


def called_names(path: Path) -> set[str]:
    """Names of functions/methods actually called in the module body."""
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    names: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            func = node.func
            if isinstance(func, ast.Attribute):
                names.add(func.attr)
            elif isinstance(func, ast.Name):
                names.add(func.id)
    return names


def python_files() -> list[Path]:
    return sorted(SRC.rglob("*.py"))


def imports_of(path: Path) -> set[str]:
    """Project-relative package names imported by ``path``."""
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    found: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                if alias.name.startswith("lynxdroid"):
                    found.add(alias.name.split(".")[1])
        elif isinstance(node, ast.ImportFrom):
            if node.level:  # relative import: same layer
                continue
            if node.module and node.module.startswith("lynxdroid"):
                parts = node.module.split(".")
                found.add(parts[1] if len(parts) > 1 else "lynxdroid")
    return found


def layer_of(path: Path) -> str:
    relative = path.relative_to(SRC)
    return relative.parts[0] if len(relative.parts) > 1 else relative.stem


class TestDependencyRules:
    def test_no_layer_imports_upwards(self) -> None:
        violations: list[str] = []
        for path in python_files():
            layer = layer_of(path)
            if layer not in ALLOWED:
                continue
            permitted = ALLOWED[layer] | {layer}
            for imported in imports_of(path):
                if imported == "version":
                    continue
                if imported not in permitted and imported != "lynxdroid":
                    violations.append(f"{path.relative_to(SRC)}: {layer} imports {imported}")
        assert not violations, "layer violations:\n" + "\n".join(violations)

    def test_core_is_dependency_free(self) -> None:
        for path in (SRC / "core").rglob("*.py"):
            for imported in imports_of(path):
                assert imported in {"core", "lynxdroid", "version"}, (
                    f"{path.name} imports {imported}"
                )

    def test_doctor_never_writes(self) -> None:
        """doctor is read-only: no filesystem mutation calls in real code."""
        forbidden = {
            "write_text",
            "write_bytes",
            "mkdir",
            "unlink",
            "rmdir",
            "remove",
            "rename",
            "replace",
            "chmod",
            "symlink_to",
        }
        offenders: list[str] = []
        for path in (SRC / "doctor").rglob("*.py"):
            for call in called_names(path):
                if call in forbidden:
                    offenders.append(f"{path.name} calls {call}()")
        assert not offenders, "doctor must not mutate the host: " + ", ".join(offenders)

    def test_gui_never_executes_processes_directly(self) -> None:
        """Only core.proc may spawn; the GUI goes through the backend/workers."""
        for path in (SRC / "gui").rglob("*.py"):
            modules = imports_of_full(path)
            assert "subprocess" not in modules, f"{path.name} imports subprocess"
            assert not {"os.system", "os.popen"} & called_names(path), (
                f"{path.name} spawns processes"
            )

    def test_only_core_spawns_processes(self) -> None:
        allowed = {
            SRC / "core" / "proc.py",
            SRC / "backend" / "waydroid.py",  # through core.proc, not subprocess
            SRC / "doctor" / "platform.py",  # through core.proc
        }
        offenders: list[str] = []
        for path in python_files():
            if path in allowed:
                continue
            modules = imports_of_full(path)
            if "subprocess" in modules:
                offenders.append(str(path.relative_to(SRC)))
        assert not offenders, f"direct subprocess import in: {offenders}"

    def test_shell_true_is_never_used(self) -> None:
        """Check real keyword arguments, not prose in docstrings."""
        offenders: list[str] = []
        for path in python_files():
            tree = ast.parse(path.read_text(encoding="utf-8"))
            for node in ast.walk(tree):
                if isinstance(node, ast.Call):
                    for keyword in node.keywords:
                        if keyword.arg == "shell":
                            offenders.append(f"{path.relative_to(SRC)}:{node.lineno}")
        assert not offenders, f"shell= passed to a call at: {offenders}"


class TestStyleRules:
    def test_no_bare_prints_in_library_code(self) -> None:
        """Only the CLI and one GUI debug path may print."""
        allowed = {"cli", "gui"}
        offenders: list[str] = []
        for path in python_files():
            if layer_of(path) in allowed:
                continue
            source = path.read_text(encoding="utf-8")
            for line in source.splitlines():
                stripped = line.strip()
                if stripped.startswith("print(") and "#" not in stripped.split("print")[0]:
                    offenders.append(f"{path.relative_to(SRC)}: {stripped[:60]}")
        assert not offenders, "prints outside cli/gui:\n" + "\n".join(offenders)

    def test_every_module_has_a_docstring(self) -> None:
        missing: list[str] = []
        for path in python_files():
            tree = ast.parse(path.read_text(encoding="utf-8"))
            if not ast.get_docstring(tree):
                missing.append(str(path.relative_to(SRC)))
        assert not missing, f"modules without a docstring: {missing}"

    def test_ui_logic_modules_are_importable_without_qt(self) -> None:
        """Behaviour must live outside the widget modules.

        Regression: ``hit_test`` sat in ``overlay.py``, which imports PySide6 at
        module level, so it could not be tested anywhere and was in fact never
        called. A module listed here must not import Qt.
        """
        qt_free_ui = ["gui/viewmodel.py", "gui/theme.py", "gui/viewport.py", "gui/editor.py"]
        offenders: list[str] = []
        for relative in qt_free_ui:
            path = SRC / relative
            assert path.exists(), f"{relative} is missing"
            modules = imports_of_full(path)
            if any(m.startswith("PySide6") for m in modules):
                offenders.append(relative)
        assert not offenders, f"UI logic must be Qt-free: {offenders}"

    def test_no_todo_markers_without_context(self) -> None:
        """A bare TODO hides work; require a reason after it."""
        bad: list[str] = []
        for path in python_files():
            for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                if "TODO" in line or "FIXME" in line or "XXX" in line:
                    tail = line.split("TODO", 1)[-1].split("FIXME", 1)[-1]
                    if len(tail.strip(" :#-")) < 10:
                        bad.append(f"{path.relative_to(SRC)}:{number}")
        assert not bad, f"unexplained TODO/FIXME: {bad}"


@pytest.mark.parametrize(
    "module",
    [
        "lynxdroid",
        "lynxdroid.core.errors",
        "lynxdroid.core.paths",
        "lynxdroid.core.proc",
        "lynxdroid.core.storage",
        "lynxdroid.doctor.platform",
        "lynxdroid.display.geometry",
        "lynxdroid.input.engine",
        "lynxdroid.profiles.model",
        "lynxdroid.settings",
    ],
)
def test_core_modules_import_without_qt(module: str) -> None:
    """The library must be usable on a machine with no Qt and no display."""
    import importlib

    imported = importlib.import_module(module)
    assert imported.__name__ == module
