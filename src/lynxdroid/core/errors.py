"""Typed errors and process exit codes.

Every user-visible failure in LynxDroid is one of these classes, so the CLI,
the GUI and the backend all agree on *why* something failed instead of
collapsing everything into a single "error" string.
"""

from __future__ import annotations

from enum import IntEnum


class ExitCode(IntEnum):
    """Documented process exit codes (see ``docs/CLI.md``)."""

    OK = 0
    FAIL = 1
    USAGE = 2
    MISSING_DEPENDENCY = 3
    PERMISSION = 4
    UNSUPPORTED = 5
    BLOCKED = 6


class LynxError(Exception):
    """Base class for all expected, user-explainable failures."""

    exit_code: ExitCode = ExitCode.FAIL
    #: Short machine-readable token, used in JSON reports and tests.
    code: str = "error"

    def __init__(self, message: str, *, hint: str | None = None) -> None:
        super().__init__(message)
        self.message = message
        self.hint = hint

    def as_dict(self) -> dict[str, str]:
        data = {"code": self.code, "message": self.message}
        if self.hint:
            data["hint"] = self.hint
        return data

    def __str__(self) -> str:
        if self.hint:
            return f"{self.message} ({self.hint})"
        return self.message


class UsageError(LynxError):
    exit_code = ExitCode.USAGE
    code = "usage"


class DependencyMissing(LynxError):
    """A required external component (binary, module, device) is absent."""

    exit_code = ExitCode.MISSING_DEPENDENCY
    code = "missing-dependency"


class PermissionDenied(LynxError):
    """We found the component but may not use it with current privileges."""

    exit_code = ExitCode.PERMISSION
    code = "permission-denied"


class UnsupportedEnvironment(LynxError):
    """The host cannot support the requested feature at all."""

    exit_code = ExitCode.UNSUPPORTED
    code = "unsupported"


class Blocked(LynxError):
    """A hard precondition failed; the action must not be attempted."""

    exit_code = ExitCode.BLOCKED
    code = "blocked"


class DataCorrupted(LynxError):
    """Stored user data failed validation; we refuse to guess."""

    code = "data-corrupted"


class ProcessFailure(LynxError):
    """An external command ran but reported failure."""

    code = "process-failure"

    def __init__(self, argv: list[str], returncode: int, stderr: str = "") -> None:
        tail = _first_lines(stderr)
        message = f"command failed with exit {returncode}: {argv[0]}"
        if tail:
            message = f"{message}: {tail}"
        super().__init__(message)
        self.argv = list(argv)
        self.returncode = returncode
        self.stderr = stderr


def _first_lines(text: str, limit: int = 3) -> str:
    lines = [ln.strip() for ln in text.strip().splitlines() if ln.strip()]
    return " | ".join(lines[:limit])
