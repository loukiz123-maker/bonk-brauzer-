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
