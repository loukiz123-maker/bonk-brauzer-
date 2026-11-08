"""The requirement registry must point at tests that exist.

A registry that cites a test nobody wrote is worse than no registry: it looks
like evidence. This module keeps `docs/REQUIREMENTS.md` honest by resolving
every test it names against the real test suite.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs"
TESTS = ROOT / "tests"


def _defined_names() -> tuple[set[str], set[str]]:
    """Every test function and test class name in the suite."""
    functions: set[str] = set()
    classes: set[str] = set()
    for path in TESTS.rglob("test_*.py"):
        text = path.read_text(encoding="utf-8")
        functions |= set(re.findall(r"^\s*def (test_[A-Za-z0-9_]+)", text, re.MULTILINE))
        classes |= set(re.findall(r"^\s*class (Test[A-Za-z0-9_]+)", text, re.MULTILINE))
    return functions, classes


def _cited_names() -> tuple[set[str], set[str]]:
    """Names the registry presents as tests (backticked, or after ``::``)."""
    text = (DOCS / "REQUIREMENTS.md").read_text(encoding="utf-8")
    # Backticked identifiers that look like tests; `docs/...`, paths and CLI
    # names are filtered out by the prefix check below.
    cited = set(re.findall(r"`([A-Za-z_][A-Za-z0-9_:.]*)`", text))
    # `test_images.py` is a file, not a test; only bare identifiers count.
    functions = {c for c in cited if c.startswith("test_") and not c.endswith(".py")}
    # Explicit ::qualified names are always claims about a specific test.
    functions |= set(re.findall(r"::(test_[A-Za-z0-9_]+)", text))
    classes = set(re.findall(r"::(Test[A-Za-z0-9_]+)", text))
    return functions, classes


def test_every_cited_test_exists() -> None:
    functions, classes = _defined_names()
    cited_functions, cited_classes = _cited_names()
    missing_functions = sorted(cited_functions - functions)
    missing_classes = sorted(cited_classes - classes)
    assert not missing_functions, (
        f"REQUIREMENTS.md cites tests that do not exist: {missing_functions}"
    )
    assert not missing_classes, (
        f"REQUIREMENTS.md cites test classes that do not exist: {missing_classes}"
    )


def test_every_cited_test_file_exists() -> None:
    """Paths like ``tests/backend/test_images.py`` must resolve."""
    text = (DOCS / "REQUIREMENTS.md").read_text(encoding="utf-8")
    paths = set(re.findall(r"`(tests/[A-Za-z0-9_/]+\.py)`", text))
    assert paths, "the registry should cite test files"
    missing = sorted(p for p in paths if not (ROOT / p).exists())
    assert not missing, f"REQUIREMENTS.md cites files that do not exist: {missing}"


def _collected_count(relative: str) -> int:
    """How many test items pytest really collects, parametrisation included.

    Counting ``def test_`` lines would undercount parametrised tests (one
    function, several cases), so the number comes from pytest itself rather
    than from a regex over the source.
    """
    import subprocess
    import sys

    result = subprocess.run(
        [sys.executable, "-m", "pytest", relative, "--collect-only", "-q"],
        capture_output=True,
        text=True,
        cwd=ROOT,
    )
    # Collected items are printed as "tests/path.py::TestX::test_y".
    return len([line for line in result.stdout.splitlines() if "::" in line])


def test_every_test_count_in_the_registry_matches_reality() -> None:
    """A row that says "(12 tests)" must match reality, or it is a stale claim."""
    text = (DOCS / "REQUIREMENTS.md").read_text(encoding="utf-8")
    claims = re.findall(r"`(tests/[A-Za-z0-9_/]+\.py)`\s*\((\d+) tests?\)", text)
    assert claims, "the registry should state how many tests prove each row"
    problems: list[str] = []
    for path_text, count_text in claims:
        found = _collected_count(path_text)
        if found != int(count_text):
            problems.append(f"{path_text}: registry says {count_text}, pytest collects {found}")
    assert not problems, "stale test counts in the registry:\n" + "\n".join(problems)


def test_registry_statuses_use_the_documented_vocabulary() -> None:
    """Only the four documented statuses may appear in the Status column."""
    allowed = {"НЕ НАЧАТ", "В РАБОТЕ", "ЗАБЛОКИРОВАН", "ПРОВЕРЕН"}
    text = (DOCS / "REQUIREMENTS.md").read_text(encoding="utf-8")
    rows = [
        line for line in text.splitlines() if line.startswith("| P0-") or line.startswith("| P1-")
    ]
    assert rows, "the registry has no requirement rows"
    problems: list[str] = []
    for row in rows:
        cells = [c.strip() for c in row.strip("|").split("|")]
        status_cell = cells[-1]
        if not any(word in status_cell for word in allowed):
            problems.append(row[:80])
    assert not problems, "rows without a documented status:\n" + "\n".join(problems)


def test_every_p0_requirement_has_evidence() -> None:
    text = (DOCS / "REQUIREMENTS.md").read_text(encoding="utf-8")
    rows = [line for line in text.splitlines() if line.startswith("| P0-")]
    assert rows
    for row in rows:
        cells = [c.strip() for c in row.strip("|").split("|")]
        assert len(cells) >= 4, f"malformed row: {row}"
        evidence = cells[2]
        assert len(evidence) > 3, f"P0 row without evidence: {cells[0]}"
