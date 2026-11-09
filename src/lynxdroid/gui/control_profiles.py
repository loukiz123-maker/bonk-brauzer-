"""Isolate launcher control drafts by the absolute APK reference, not title."""

from __future__ import annotations

import hashlib
from pathlib import Path


def control_profile_path(data_dir: Path, apk_path: Path) -> Path:
    """Use a stable digest so APK names cannot escape the data directory.

    Identity is the local path, not an Android package name. Moving the APK
    therefore creates a different profile; no Android runtime is involved.
    """
    if not apk_path.is_absolute():
        raise ValueError("APK reference must be absolute")
    digest = hashlib.sha256(str(apk_path).encode("utf-8")).hexdigest()
    return data_dir / "control-profiles" / f"{digest}.json"
