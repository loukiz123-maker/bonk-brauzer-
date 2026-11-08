"""Stage 1's own promises, checked.

`docs/STAGE-CRITERIA.md` says a stage is finished when its criteria are
*checked*, not when the code exists. That is only worth writing down if
something reads it: this module resolves every criterion marked HERE to a test
that must exist, and every criterion marked TARGET to a command the user can
run on their machine.

The distinction matters. A criterion this environment cannot settle is not
removed from the document - it is left open and visible, so "DONE" here never
implies "works on the laptop".
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CRITERIA = ROOT / "docs" / "STAGE-CRITERIA.md"
TESTS = ROOT / "tests"


def _rows() -> list[dict[str, str]]:
    """One dict per criterion row: id, text, kind, evidence."""
    rows: list[dict[str, str]] = []
    for line in CRITERIA.read_text(encoding="utf-8").splitlines():
        if not re.match(r"^\| \d+\.\d+ \|", line):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) < 4:
            continue
        rows.append({"id": cells[0], "what": cells[1], "kind": cells[2], "evidence": cells[3]})
    return rows


def _cited_tests(row: dict[str, str]) -> list[str]:
    """Test node names cited as `tests/x.py` or `tests/x.py::Test`."""
    return re.findall(r"`(tests/[A-Za-z0-9_/]+\.py)(?:::[A-Za-z0-9_]+)?`", row["evidence"])


def _defined_names(path: Path) -> tuple[set[str], set[str]]:
    text = path.read_text(encoding="utf-8")
    functions = set(re.findall(r"^\s*def (test_[A-Za-z0-9_]+)", text, re.MULTILINE))
    classes = set(re.findall(r"^\s*class (Test[A-Za-z0-9_]+)", text, re.MULTILINE))
    return functions, classes


class TestCriteriaDocument:
    def test_the_document_lists_criteria(self) -> None:
        rows = _rows()
        assert len(rows) >= 13, f"only {len(rows)} criteria parsed"

    def test_every_criterion_declares_a_kind(self) -> None:
        for row in _rows():
            assert row["kind"] in {"HERE", "CI", "TARGET"}, f"{row['id']}: kind {row['kind']!r}"

    def test_every_criterion_cites_something(self) -> None:
        for row in _rows():
            assert row["evidence"].strip(), f"{row['id']} cites no evidence"

    def test_every_here_criterion_cites_an_existing_test_file(self) -> None:
        missing: list[str] = []
        for row in _rows():
            if row["kind"] != "HERE":
                continue
            cited = _cited_tests(row)
            if not cited:
                missing.append(f"{row['id']} cites no test file")
            for path in cited:
                if not (ROOT / path).is_file():
                    missing.append(f"{row['id']} cites {path}, which does not exist")
        assert missing == [], "; ".join(missing)

    def test_every_named_test_class_exists(self) -> None:
        """A criterion that points at `file.py::TestThing` must be testable."""
        broken: list[str] = []
        for row in _rows():
            for reference in re.findall(
                r"`(tests/[A-Za-z0-9_/]+\.py)::([A-Za-z0-9_]+)`", row["evidence"]
            ):
                path, name = ROOT / reference[0], reference[1]
                if not path.is_file():
                    broken.append(f"{row['id']}: {reference[0]} missing")
                    continue
                functions, classes = _defined_names(path)
                if name not in classes and name not in functions:
                    broken.append(f"{row['id']}: {reference[0]} has no {name}")
        assert broken == [], "; ".join(broken)

    def test_every_ci_criterion_names_a_job(self) -> None:
        """A CI criterion is only checkable if it says which job to look at."""
        workflow = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        for row in _rows():
            if row["kind"] != "CI":
                continue
            jobs = re.findall(r"job[s]? `([a-z0-9-]+)`", row["evidence"])
            assert jobs, f"{row['id']} is CI but names no job"
            # Job *keys* are cited, not display names: the display name of
            # the pytest job is a template and cannot be grepped for.
            keys = set(re.findall(r"^  ([a-z][a-z0-9-]*):$", workflow, re.MULTILINE))
            for job in jobs:
                assert job in keys, f"{row['id']}: job {job!r} is not in ci.yml"

    def test_every_target_criterion_names_a_command(self) -> None:
        """An open criterion is actionable or it is noise."""
        for row in _rows():
            if row["kind"] == "TARGET":
                assert "`" in row["evidence"], f"{row['id']} is open but names no command to run"

    def test_the_rule_that_blocked_stages_stay_blocked_is_written_down(self) -> None:
        text = CRITERIA.read_text(encoding="utf-8")
        assert "never `DONE`" in text or "never `DONE`" in text.replace("`", "`")
        assert "TARGET" in text and "HERE" in text


class TestStageOneIsActuallySatisfied:
    """The HERE criteria, asserted directly rather than by citation.

    Citing a test file is not the same as passing it, and a document that
    points at a green test can still be describing last week's code. Where a
    criterion is cheap to restate, it is restated here.
    """

    def test_no_placeholders_in_the_source(self) -> None:
        """1.1: TODO and FIXME are how a limitation escapes the record."""
        hits: list[str] = []
        for path in sorted((ROOT / "src").rglob("*.py")):
            for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                if re.search(r"\b(TODO|FIXME|XXX)\b", line):
                    hits.append(f"{path.relative_to(ROOT)}:{number}")
        assert hits == [], "placeholders left in src: " + ", ".join(hits)

    def test_every_defect_entry_says_how_it_was_found(self) -> None:
        """1.13: a defect row without a "how" teaches nothing."""
        text = (ROOT / "docs" / "KNOWN_ISSUES.md").read_text(encoding="utf-8")
        rows = [line for line in text.splitlines() if re.match(r"^\| \d+ \|", line)]
        assert rows, "no defect rows parsed"
        for row in rows:
            cells = [cell.strip() for cell in row.strip().strip("|").split("|")]
            assert len(cells) >= 4, f"malformed row: {row[:60]}"
            assert cells[2], f"row {cells[0]} does not say how the defect was caught"
            assert cells[3], f"row {cells[0]} has no status"

    def test_no_defect_is_left_without_a_status(self) -> None:
        text = (ROOT / "docs" / "KNOWN_ISSUES.md").read_text(encoding="utf-8")
        for row in text.splitlines():
            if re.match(r"^\| \d+ \|", row):
                cells = [cell.strip() for cell in row.strip().strip("|").split("|")]
                assert cells[-1], f"row {cells[0]} ends without a status"
