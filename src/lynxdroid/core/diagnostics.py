"""Diagnostic model shared by ``doctor``, the GUI and the JSON reports.

The specification (stage 3) requires every finding to distinguish:

* **OK**      - checked and the value is as required
* **WARNING** - works, but with a documented limitation or risk
* **ERROR**   - a hard requirement is missing/refused
* **UNKNOWN** - we could not determine the value (never reported as "unsupported")

and to carry *reason*, *observed value* and *next step*, so the user can act
without reading our source code.
"""

from __future__ import annotations

import json
from collections.abc import Iterable
from dataclasses import dataclass, field
from enum import Enum


class Status(str, Enum):
    OK = "OK"
    WARNING = "WARNING"
    ERROR = "ERROR"
    UNKNOWN = "UNKNOWN"

    @property
    def severity(self) -> int:
        return {"OK": 0, "UNKNOWN": 1, "WARNING": 2, "ERROR": 3}[self.value]

    @property
    def symbol(self) -> str:
        return {"OK": "✔", "UNKNOWN": "?", "WARNING": "▲", "ERROR": "✘"}[self.value]


@dataclass(frozen=True)
class Check:
    """One inspected property of the host or the backend."""

    id: str
    title: str
    status: Status
    detail: str
    #: What we actually observed (command output, file contents). Empty when unknown.
    observed: str = ""
    #: Concrete, safe next step for the user. Empty when nothing to do.
    next_step: str = ""
    #: True when fixing this needs root/system changes -> requires consent.
    requires_privilege: bool = False
    #: Requirement id (P0/P1) from docs/REQUIREMENTS.md, for traceability.
    requirement: str = ""
    #: Extra structured data for the GUI and machine consumers.
    data: dict[str, object] = field(default_factory=dict)

    def as_dict(self) -> dict[str, object]:
        payload: dict[str, object] = {
            "id": self.id,
            "title": self.title,
            "status": self.status.value,
            "detail": self.detail,
            "observed": self.observed,
            "next_step": self.next_step,
            "requires_privilege": self.requires_privilege,
            "requirement": self.requirement,
        }
        if self.data:
            payload["data"] = self.data
        return payload


@dataclass
class Report:
    """Ordered collection of checks plus environment metadata."""

    area: str
    checks: list[Check] = field(default_factory=list)
    meta: dict[str, object] = field(default_factory=dict)

    def add(self, check: Check) -> Check:
        self.checks.append(check)
        return check

    def extend(self, checks: Iterable[Check]) -> None:
        self.checks.extend(checks)

    def by_status(self, status: Status) -> list[Check]:
        return [c for c in self.checks if c.status is status]

    @property
    def worst(self) -> Status:
        worst = Status.OK
        for check in self.checks:
            if check.status.severity > worst.severity:
                worst = check.status
        return worst

    @property
    def blockers(self) -> list[Check]:
        return self.by_status(Status.ERROR)

    def ok_for(self, required: Iterable[str]) -> bool:
        """True when none of the *required* check ids is ERROR/UNKNOWN."""
        wanted = set(required)
        return not any(
            c.id in wanted and c.status in (Status.ERROR, Status.UNKNOWN) for c in self.checks
        )

    def as_dict(self) -> dict[str, object]:
        payload: dict[str, object] = {
            "area": self.area,
            "status": self.worst.value,
        }
        # Promote schema/version to the top level: scripts should not have to
        # dig into meta to discover the format they just parsed.
        for key in ("schema", "version"):
            if key in self.meta:
                payload[key] = self.meta[key]
        payload["meta"] = self.meta
        payload["checks"] = [c.as_dict() for c in self.checks]
        return payload

    def to_json(self, *, indent: int | None = 2) -> str:
        return json.dumps(self.as_dict(), indent=indent, ensure_ascii=False, sort_keys=False)

    def to_text(self, *, show_ok: bool = True) -> str:
        """Human-readable table. Unknown/error always shown, OK optional."""
        width = max((len(c.title) for c in self.checks), default=10)
        width = min(max(width, 18), 46)
        rows: list[str] = []
        for check in self.checks:
            if check.status is Status.OK and not show_ok:
                continue
            head = f"{check.status.symbol} {check.status.value:<7} {check.title:<{width}}"
            rows.append(f"{head}  {check.detail}")
            if check.observed and check.status is not Status.OK:
                rows.append(f"{'':<10} {'observed:':<{width}}  {_short(check.observed)}")
            if check.next_step:
                rows.append(f"{'':<10} {'next:':<{width}}  {check.next_step}")
        summary = (
            f"{self.area}: "
            + ", ".join(
                f"{count} {status.value}"
                for status in (Status.ERROR, Status.WARNING, Status.UNKNOWN, Status.OK)
                if (count := len(self.by_status(status)))
            )
            or f"{self.area}: no checks"
        )
        return "\n".join([*rows, "", summary])


def _short(text: str, limit: int = 160) -> str:
    flat = " ".join(text.split())
    if len(flat) <= limit:
        return flat
    return flat[: limit - 1] + "…"
