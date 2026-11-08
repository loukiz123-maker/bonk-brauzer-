"""Architecture tests: the dependency rules from docs/ARCHITECTURE.md §1.

These are cheap static checks, but they catch the most common way a layered
design rots: someone imports "just one" module across a boundary.
"""

from __future__ import annotations

import ast
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "lynxdroid"

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


# ---------------------------------------------------------------------------
# SECURITY.md makes claims that are checkable. A claim nobody checks is a
# comment, and the section on command execution is the one that matters most.
# ---------------------------------------------------------------------------

PROCESS_APIS = {"Popen", "run", "call", "check_call", "check_output", "SystemExit"}


def _calls_in_source() -> dict[str, list[tuple[str, int]]]:
    """Find real subprocess/os-level process creation, not type annotations."""
    found: dict[str, list[tuple[str, int]]] = {}
    for path in sorted(SRC.rglob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"))
        for node in ast.walk(tree):
            # subprocess.Popen(...) / subprocess.run(...)
            if not isinstance(node, ast.Call):
                continue
            func = node.func
            if not (isinstance(func, ast.Attribute) and isinstance(func.value, ast.Name)):
                continue
            module, attr = func.value.id, func.attr
            if module == "subprocess" and attr in PROCESS_APIS:
                found.setdefault(str(path.relative_to(SRC)), []).append((attr, node.lineno))
            # os.system / os.exec* / os.spawn*
            if module == "os" and (attr.startswith(("exec", "spawn")) or attr == "system"):
                found.setdefault(str(path.relative_to(SRC)), []).append((f"os.{attr}", node.lineno))
    return found


class TestProcessCreationHasOneHome:
    def test_only_core_proc_creates_processes(self) -> None:
        """SECURITY.md §3: ``core/proc.py`` is the only place a subprocess is
        created. Everything else goes through it, so the argv validation,
        the environment scrubbing and the timeout apply everywhere."""
        calls = _calls_in_source()
        offenders = {path: where for path, where in calls.items() if path != "core/proc.py"}
        assert offenders == {}, "process creation outside core/proc.py: " + ", ".join(
            f"{p}:{loc[0][1]}" for p, loc in offenders.items()
        )

    def test_core_proc_does_create_processes(self) -> None:
        """The previous test would pass if the scanner were broken."""
        assert "core/proc.py" in _calls_in_source()

    def test_no_module_uses_shell_true(self) -> None:
        """A shell would turn every validated argv back into a string to be
        re-parsed, which is how injection bugs come back."""
        for path in sorted(SRC.rglob("*.py")):
            text = path.read_text(encoding="utf-8")
            tree = ast.parse(text)
            for node in ast.walk(tree):
                if isinstance(node, ast.Call):
                    for keyword in node.keywords:
                        if keyword.arg == "shell" and not (
                            isinstance(keyword.value, ast.Constant) and keyword.value.value is False
                        ):
                            pytest.fail(
                                f"{path.name}:{node.lineno} passes shell={ast.dump(keyword.value)}"
                            )


# ---------------------------------------------------------------------------
# The process that runs as the user must not write outside the XDG directories
# it owns. `core/paths.py` states this rule; nothing enforced it, and the
# Waydroid session log violated it by writing into /var/lib/waydroid, which is
# root-owned - so the write failed for every normal user.
# ---------------------------------------------------------------------------

#: Directories that belong to the system, never to this process.
SYSTEM_DIRS = ("/var", "/usr", "/etc", "/opt", "/boot")


class TestNothingIsWrittenToSystemDirectories:
    def test_no_write_call_targets_a_system_path(self) -> None:
        """Writes are found by the call that creates them.

        The path usually arrives in a variable (``log_path = ...; mkdir()``),
        so the scanner resolves the literal assignments in the same scope
        first. A search for the string "/var/lib" would miss a path assembled
        at runtime, which is exactly how the session log went wrong.
        """
        offenders: list[str] = []
        for path in sorted((ROOT / "src" / "lynxdroid").rglob("*.py")):
            source = path.read_text(encoding="utf-8")
            where = path.relative_to(ROOT)
            for tree, scope in _scopes(ast.parse(source)):
                literals = _literal_assignments(scope)
                for node in ast.walk(tree):
                    if not isinstance(node, ast.Call):
                        continue
                    name = _call_name(node.func)
                    if name not in {"mkdir", "open", "spawn", "write_text", "write_bytes"}:
                        continue
                    for arg in list(node.args) + [kw.value for kw in node.keywords]:
                        for literal in _resolve(arg, literals):
                            if literal.startswith(SYSTEM_DIRS):
                                offenders.append(f"{where}:{node.lineno} {name}({literal})")
        assert offenders == [], "writes outside the XDG directories: " + "; ".join(offenders)

    def test_the_scan_can_see_writes(self) -> None:
        """An empty scan must mean "nothing wrong", not "nothing looked at"."""
        writes = 0
        for path in sorted((ROOT / "src" / "lynxdroid").rglob("*.py")):
            source = path.read_text(encoding="utf-8")
            for tree, _scope in _scopes(ast.parse(source)):
                for node in ast.walk(tree):
                    if isinstance(node, ast.Call) and _call_name(node.func) in {
                        "mkdir",
                        "write_text",
                        "write_bytes",
                        "spawn",
                    }:
                        writes += 1
        assert writes > 5, f"the scanner found only {writes} write calls"

    def test_the_scanner_resolves_a_variable_holding_a_path(self) -> None:
        """The mutation that motivated this test must be caught.

        The first version of the scanner only read literals at the call site.
        Writing the log path to a variable - which is what the real bug did -
        slipped straight past it, so the scan was green and useless.
        """
        literals = _literal_assignments(ast.parse('target = Path("/var/lib/x/y.log")').body)
        call = ast.parse('open(target, "ab")').body[0]
        assert isinstance(call, ast.Expr) and isinstance(call.value, ast.Call)
        resolved = _resolve(call.value.args[0], literals)
        assert any(text.startswith("/var") for text in resolved), resolved


def _scopes(tree: ast.Module) -> list[tuple[ast.AST, list[ast.stmt]]]:
    """The module body and every function body, each scanned on its own."""
    scopes: list[tuple[ast.AST, list[ast.stmt]]] = [(tree, tree.body)]
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            scopes.append((node, node.body))
    return scopes


def _literal_assignments(body: list[ast.stmt]) -> dict[str, str]:
    """``name = <string literal>`` for names assigned exactly one value."""
    seen: dict[str, str] = {}
    conflicting: set[str] = set()
    for node in body:
        for inner in ast.walk(node):
            if isinstance(inner, ast.Assign) and len(inner.targets) == 1:
                target = inner.targets[0]
                if not isinstance(target, ast.Name):
                    continue
                text = _string_of(inner.value)
                if not text:
                    continue
                if target.id in seen and seen[target.id] != text:
                    conflicting.add(target.id)
                seen[target.id] = text
    for name in conflicting:
        seen.pop(name, None)
    return seen


def _resolve(node: ast.AST, literals: dict[str, str]) -> list[str]:
    """Every system-path literal an expression could denote."""
    found = [text for text in [_string_of(node)] if text]
    for inner in ast.walk(node):
        if isinstance(inner, ast.Name) and inner.id in literals:
            found.append(literals[inner.id])
    return found


def _string_of(node: ast.AST) -> str:
    """The literal text of a string, or of ``Path("/x")``, else ``""``."""
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.Call) and node.args:
        return _string_of(node.args[0])
    if isinstance(node, ast.BinOp):
        left = _string_of(node.left)
        right = _string_of(node.right)
        return left or right
    return ""


def _call_name(func: ast.expr) -> str:
    """``x.mkdir`` -> "mkdir", ``mkdir`` -> "mkdir", anything else -> ""."""
    if isinstance(func, ast.Attribute):
        return func.attr
    if isinstance(func, ast.Name):
        return func.id
    return ""
