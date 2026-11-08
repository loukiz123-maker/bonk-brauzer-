"""Documented test counts must match reality.

Three documents quote numbers - the per-file table in docs/TESTING.md, the
total in README/TESTING/ROADMAP, and individual suites mentioned in prose.
They have drifted repeatedly: updating a suite meant remembering to edit four
files, and nothing failed when a step was missed. A stale count is small, but
it is exactly the kind of wrong that makes a reader distrust the rest of the
document.

This test reads the documents and compares them with a real collection pass.
One collection run answers every question, so the whole module costs about a
second rather than one subprocess per row.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
TESTING = ROOT / "docs" / "TESTING.md"
README = ROOT / "README.md"
ROADMAP = ROOT / "docs" / "ROADMAP.md"

#: "| `tests/foo/test_bar.py` | 12 | what it pins down |"
ROW = re.compile(r"^\| `(tests/[^`]+\.py)` \| (\d+) \|", re.MULTILINE)


def _interpreter() -> str:
    """The interpreter running the suite, so pytest is guaranteed importable.

    Preferring a project .venv would be wrong here: CI has none, and the tests
    must work in whatever environment ran them.
    """
    import sys

    return sys.executable


def _collect(*paths: str) -> dict[str, int]:
    """Run pytest once and return {node path: case count} plus the total."""
    result = subprocess.run(
        [_interpreter(), "-m", "pytest", "--collect-only", "-q", *paths],
        capture_output=True,
        text=True,
        cwd=ROOT,
        check=False,
    )
    counts: dict[str, int] = {}
    for line in result.stdout.splitlines():
        # "tests/foo/test_bar.py::TestX::test_y[param]"
        if "::" not in line:
            continue
        file_part = line.split("::", 1)[0]
        counts[file_part] = counts.get(file_part, 0) + 1
    total = sum(counts.values())
    assert total > 0, f"collected nothing for {paths}: {result.stdout[-400:]}"
    counts["(total)"] = total
    return counts


@pytest.fixture(scope="module")
def counts() -> dict[str, int]:
    return _collect("tests")


def declared_rows() -> list[tuple[str, int]]:
    return [(path, int(count)) for path, count in ROW.findall(TESTING.read_text("utf-8"))]


ROWS = declared_rows()


class TestPerFileTable:
    def test_the_table_is_not_empty(self) -> None:
        assert len(ROWS) >= 10, f"only found {len(ROWS)} rows; did the format change?"

    @pytest.mark.parametrize(("path", "declared"), ROWS)
    def test_the_declared_count_matches_the_suite(
        self, counts: dict[str, int], path: str, declared: int
    ) -> None:
        actual = counts.get(path, 0)
        assert actual == declared, (
            f"docs/TESTING.md says {path} has {declared} tests, it has {actual}"
        )


class TestTotals:
    def test_readme_total(self, counts: dict[str, int]) -> None:
        declared = int(re.search(r"Unit tests \((\d+)\)", README.read_text("utf-8")).group(1))
        assert declared == counts["(total)"]

    def test_testing_total(self, counts: dict[str, int]) -> None:
        declared = int(re.search(r"no display \| (\d+) \|", TESTING.read_text("utf-8")).group(1))
        assert declared == counts["(total)"]

    def test_roadmap_total(self, counts: dict[str, int]) -> None:
        declared = int(re.search(r"(\d+) automated tests", ROADMAP.read_text("utf-8")).group(1))
        assert declared == counts["(total)"]
