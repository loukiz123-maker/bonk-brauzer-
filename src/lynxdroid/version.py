"""Single source of truth for the project version and wire/format schemas."""

from __future__ import annotations

__version__ = "0.1.0"

#: Human readable build stage. Updated only when the requirement registry
#: (docs/REQUIREMENTS.md) records a stage as verified.
BUILD_STAGE = "stage 4 - architecture and backend contract (Python vertical slice)"

#: Versions of every persisted or inter-process format. Bumping one of these
#: requires a migration step in ``profiles.codec.migrate`` plus a test; a
#: reader that meets a newer file must say so rather than guess
#: (``profiles.codec``, ``settings``).
SCHEMA_VERSIONS: dict[str, int] = {
    "profile": 1,
    "settings": 1,
    "agent-protocol": 1,
    "doctor-report": 1,
}


def version_string() -> str:
    """Return a one-line version banner used by ``--version`` and the GUI."""
    return f"LynxDroid {__version__} ({BUILD_STAGE})"
