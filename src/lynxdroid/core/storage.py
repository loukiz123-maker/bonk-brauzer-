"""Atomic, crash-safe JSON persistence.

Requirements that drive this module:

* profiles and settings must survive a crash mid-write (stage 9, 15);
* a failed save must never leave a half-written file behind;
* a corrupt file must be reported, never silently replaced;
* writes carry a schema version so migration is possible (stage 18).

Only the two primitives the app actually uses live here: ``read_json`` and
``write_json``. Two convenience wrappers (``patched_file``, ``atomic_update``)
were removed after an audit showed no caller in ``src/`` or ``tests/``; the
policy for unreferenced code is in ``docs/KNOWN_ISSUES.md`` #27 and #29.
"""

from __future__ import annotations

import contextlib
import json
import os
import tempfile
from pathlib import Path
from typing import Any

from lynxdroid.core.errors import DataCorrupted


def read_json(path: Path) -> dict[str, Any]:
    """Read a JSON object, raising ``DataCorrupted`` with a precise reason."""
    path = Path(path)
    try:
        raw = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        raise
    except OSError as exc:
        raise DataCorrupted(f"cannot read {path.name}: {exc.strerror}") from exc
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise DataCorrupted(
            f"{path.name} is not valid JSON (line {exc.lineno}, column {exc.colno})",
            hint="restore a backup or delete the file to start over",
        ) from exc
    if not isinstance(data, dict):
        raise DataCorrupted(f"{path.name} must contain a JSON object")
    return data


def write_json(
    path: Path,
    payload: dict[str, Any],
    *,
    mode: int = 0o600,
    fsync: bool = True,
) -> None:
    """Write ``payload`` atomically: tmp file -> fsync -> replace.

    ``os.replace`` is atomic on POSIX, so a reader either sees the old file or
    the complete new one, never a mixture.
    """
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    text = json.dumps(payload, indent=2, ensure_ascii=False, sort_keys=True) + "\n"
    fd, tmp_name = tempfile.mkstemp(dir=str(path.parent), prefix=f".{path.name}.", suffix=".tmp")
    tmp_path = Path(tmp_name)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            handle.write(text)
            handle.flush()
            if fsync:
                os.fsync(handle.fileno())
        tmp_path.chmod(mode)
        # Explicit os.replace, not Path.replace: this call is the atomicity
        # guarantee, and naming it makes it patchable in failure-injection
        # tests (Path.replace wraps it in a way that defeats monkeypatching on
        # some Python versions).
        os.replace(tmp_path, path)
        if fsync:
            _fsync_dir(path.parent)
    except BaseException:
        with contextlib.suppress(OSError):
            tmp_path.unlink()
        raise


def _fsync_dir(directory: Path) -> None:
    try:
        dir_fd = os.open(str(directory), os.O_RDONLY)
    except OSError:  # pragma: no cover - platform dependent
        return
    try:
        os.fsync(dir_fd)
    except OSError:  # pragma: no cover
        pass
    finally:
        os.close(dir_fd)
