"""The small predicates a doctor report answers about itself.

``ok_for`` / ``blockers_for`` decide whether the container path may start,
and two copies of that rule had already drifted apart once (the CLI used one,
the GUI the other). These tests pin the single definition.
"""

from __future__ import annotations

import pytest

from lynxdroid.core.diagnostics import Check, Report, Status


def check(check_id: str, status: Status) -> Check:
    return Check(id=check_id, title=check_id.upper(), status=status, detail=f"{check_id} detail")


def report_with(*checks: Check) -> Report:
    report = Report(area="test")
    for item in checks:
        report.add(item)
    return report


class TestBlockersFor:
    def test_ok_checks_block_nothing(self) -> None:
        report = report_with(check("a", Status.OK), check("b", Status.WARNING))
        assert report.blockers_for(["a", "b"]) == []
        assert report.ok_for(["a", "b"]) is True

    @pytest.mark.parametrize("status", [Status.ERROR, Status.UNKNOWN])
    def test_error_and_unknown_both_block(self, status: Status) -> None:
        """ "Cannot tell" must not be treated as "fine"."""
        report = report_with(check("a", status))
        assert [c.id for c in report.blockers_for(["a"])] == ["a"]
        assert report.ok_for(["a"]) is False

    def test_only_the_required_ids_are_considered(self) -> None:
        report = report_with(check("a", Status.OK), check("b", Status.ERROR))
        assert report.ok_for(["a"]) is True
        assert report.ok_for(["b"]) is False

    def test_a_required_check_that_was_never_reported_does_not_block(self) -> None:
        """A missing check is a different failure from a failing one; the CLI
        reports missing ids separately rather than silently passing them."""
        report = report_with(check("a", Status.OK))
        assert report.ok_for(["a", "never-ran"]) is True

    def test_blockers_keep_the_report_order(self) -> None:
        report = report_with(
            check("z", Status.ERROR), check("a", Status.OK), check("m", Status.UNKNOWN)
        )
        assert [c.id for c in report.blockers_for(["z", "a", "m"])] == ["z", "m"]

    def test_an_empty_requirement_is_trivially_satisfied(self) -> None:
        assert report_with(check("a", Status.ERROR)).ok_for([]) is True


class TestReportAggregates:
    def test_blockers_is_every_error(self) -> None:
        report = report_with(
            check("a", Status.ERROR), check("b", Status.WARNING), check("c", Status.ERROR)
        )
        assert [c.id for c in report.blockers] == ["a", "c"]

    def test_worst_status_is_the_most_severe(self) -> None:
        report = report_with(check("a", Status.OK), check("b", Status.ERROR))
        assert report.worst is Status.ERROR
