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


# Portable draft format is deliberately data-only; never evaluate imported files.
MAX_DRAFT_BYTES = 128 * 1024
KINDS = ("Нажатие", "Двойное нажатие", "Мультитач", "Джойстик", "Камера", "Перетаскивание", "Огонь")


def validate_controls(data: object) -> list[dict[str, object]]:
    """Validate a portable or local draft without changing any stored profile."""
    from lynxdroid.core.errors import DataCorrupted

    if (
        not isinstance(data, dict)
        or set(data) != {"schema", "controls"}
        or type(data.get("schema")) is not int
        or data.get("schema") != 1
    ):
        raise DataCorrupted("Неверный формат или версия раскладки.")
    items = data.get("controls")
    if not isinstance(items, list) or len(items) > 100:
        raise DataCorrupted("Неверный список элементов раскладки.")
    for item in items:
        if (
            not isinstance(item, dict)
            or set(item) != {"kind", "key", "x", "y"}
            or item.get("kind") not in KINDS
            or not isinstance(item.get("key"), str)
            or not 1 <= len(item["key"]) <= 16
            or any(
                type(item.get(axis)) not in (float, int) or not 0 <= item[axis] <= 1
                for axis in ("x", "y")
            )
        ):
            raise DataCorrupted("Некорректный элемент раскладки.")
    return items


def read_control_file(path: Path) -> list[dict[str, object]]:
    """Read bounded UTF-8 JSON from a regular file without following symlinks."""
    import json
    import os
    import stat

    from lynxdroid.core.errors import DataCorrupted

    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    with os.fdopen(fd, "rb") as handle:
        if not stat.S_ISREG(os.fstat(handle.fileno()).st_mode):
            raise DataCorrupted("Выберите обычный файл раскладки.")
        raw = handle.read(MAX_DRAFT_BYTES + 1)
    if len(raw) > MAX_DRAFT_BYTES:
        raise DataCorrupted("Файл раскладки больше 128 КБ.")
    try:
        data = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError) as exc:
        raise DataCorrupted("Файл раскладки не является корректным JSON UTF-8.") from exc
    return validate_controls(data)


class ProfileConflict(Exception):
    """The on-disk profile no longer matches the editor's loaded version."""


def read_profile(path: Path) -> list[dict[str, object]] | None:
    """Return None for an absent profile, distinct from a saved empty one."""
    try:
        return read_control_file(path)
    except FileNotFoundError:
        return None


def save_profile(
    path: Path,
    controls: list[dict[str, object]],
    expected: list[dict[str, object]] | None,
) -> None:
    """Save iff the version read by the editor is current.

    Cooperating launcher processes serialize the comparison and atomic write
    with a persistent advisory lock. Uncooperative external writers do not
    take this lock and cannot be made transactional by this protocol.
    """
    from lynxdroid.core.storage import write_json
    from lynxdroid.gui.file_lock import exclusive_file_lock

    validate_controls({"schema": 1, "controls": controls})
    try:
        with exclusive_file_lock(path):
            if read_profile(path) != expected:
                raise ProfileConflict(
                    "Раскладка изменилась в другом окне. "
                    "Экспортируйте черновик и нажмите «Обновить раскладку»."
                )
            write_json(path, {"schema": 1, "controls": controls})
    except BlockingIOError as exc:
        raise ProfileConflict("Профиль занят другим окном. Повторите сохранение.") from exc
