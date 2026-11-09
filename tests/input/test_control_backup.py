"""Portable control files are bounded data, not executable content."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from lynxdroid.core.errors import DataCorrupted
from lynxdroid.gui.control_profiles import MAX_DRAFT_BYTES, read_control_file, validate_controls


def test_layout_round_trip(tmp_path: Path) -> None:
    items = [{"kind": "Огонь", "key": "F", "x": 0.2, "y": 0.8}]
    path = tmp_path / "layout.json"
    path.write_text(json.dumps({"schema": 1, "controls": items}), encoding="utf-8")
    assert read_control_file(path) == items


def test_rejects_invalid_schema_and_elements() -> None:
    for data in (
        {"schema": True, "controls": []},
        {"schema": 1, "controls": [], "other": "not understood"},
        {"schema": 2, "controls": []},
        {"schema": 1, "controls": [{"kind": "Огонь", "key": "F", "x": 0, "y": 0, "extra": "bad"}]},
        {"schema": 1, "controls": [{"kind": "Огонь", "key": "F", "x": -1, "y": 0}]},
    ):
        with pytest.raises(DataCorrupted):
            validate_controls(data)


def test_refuses_oversized_file(tmp_path: Path) -> None:
    path = tmp_path / "large.json"
    path.write_bytes(b" " * (MAX_DRAFT_BYTES + 1))
    with pytest.raises(DataCorrupted):
        read_control_file(path)


def test_refuses_symlink(tmp_path: Path) -> None:
    original = tmp_path / "original.json"
    original.write_text('{"schema":1,"controls":[]}')
    link = tmp_path / "link.json"
    link.symlink_to(original)
    with pytest.raises(OSError):
        read_control_file(link)
    assert original.read_text() == '{"schema":1,"controls":[]}'


def test_refuses_invalid_json_and_utf8(tmp_path: Path) -> None:
    path = tmp_path / "invalid.json"
    for raw in (b"{", b"\xff"):
        path.write_bytes(raw)
        with pytest.raises(DataCorrupted):
            read_control_file(path)


def test_profile_save_rejects_stale_and_deleted_versions(tmp_path: Path) -> None:
    from lynxdroid.gui.control_profiles import ProfileConflict, read_profile, save_profile

    path = tmp_path / "game.json"
    a = [{"kind": "Огонь", "key": "F", "x": 0.5, "y": 0.5}]
    b = [{"kind": "Огонь", "key": "G", "x": 0.5, "y": 0.5}]
    assert read_profile(path) is None
    save_profile(path, a, None)
    with pytest.raises(ProfileConflict):
        save_profile(path, b, None)
    assert read_profile(path) == a
    save_profile(path, b, a)
    with pytest.raises(ProfileConflict):
        save_profile(path, a, a)
    assert read_profile(path) == b
    path.unlink()
    with pytest.raises(ProfileConflict):
        save_profile(path, a, b)
    assert not path.exists()
    save_profile(path, [], None)
    with pytest.raises(ProfileConflict):
        save_profile(path, a, None)
    assert read_profile(path) == []


def test_profile_save_refuses_corruption_and_preserves_original(tmp_path: Path) -> None:
    from lynxdroid.gui.control_profiles import save_profile

    path = tmp_path / "game.json"
    path.write_bytes(b"\xff")
    with pytest.raises(DataCorrupted):
        save_profile(path, [], None)
    assert path.read_bytes() == b"\xff"


def test_profile_lock_refuses_busy_and_symlink(tmp_path: Path) -> None:
    import fcntl

    from lynxdroid.gui.control_profiles import ProfileConflict, save_profile

    path = tmp_path / "game.json"
    lock = tmp_path / "game.json.lock"
    with lock.open("w") as handle:
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with pytest.raises(ProfileConflict):
            save_profile(path, [], None)
    assert not path.exists()
    lock.unlink()
    target = tmp_path / "target"
    target.write_text("keep")
    lock.symlink_to(target)
    with pytest.raises(OSError):
        save_profile(path, [], None)
    assert target.read_text() == "keep" and not path.exists()


def test_concurrent_profile_saves_have_one_winner(tmp_path: Path) -> None:
    from concurrent.futures import ThreadPoolExecutor
    from threading import Barrier

    from lynxdroid.gui.control_profiles import ProfileConflict, read_profile, save_profile

    path = tmp_path / "game.json"
    barrier = Barrier(2)

    def attempt(key: str) -> str:
        barrier.wait(timeout=5)
        try:
            save_profile(path, [{"kind": "Огонь", "key": key, "x": 0.5, "y": 0.5}], None)
            return key
        except ProfileConflict:
            return "conflict"

    with ThreadPoolExecutor(max_workers=2) as pool:
        results = list(pool.map(attempt, ("F", "G")))
    assert results.count("conflict") == 1
    assert read_profile(path)[0]["key"] in {"F", "G"}  # type: ignore[index]
