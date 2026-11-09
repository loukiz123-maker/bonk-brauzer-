"""Standalone library: no Android backend, execution, or destructive APK operations."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from lynxdroid.core.errors import DataCorrupted
from lynxdroid.gui.library import Library, filter_entries


@pytest.fixture
def library(tmp_path: Path) -> Library:
    return Library(tmp_path / "data" / "library.json")


def apk(tmp_path: Path, name: str = "Game.apk") -> Path:
    path = tmp_path / name
    path.write_bytes(b"PK\x03\x04test")
    return path


def test_empty_library(library: Library) -> None:
    assert library.load() == []


def test_add_round_trip_and_duplicate(library: Library, tmp_path: Path) -> None:
    file = apk(tmp_path)
    assert len(library.add(file)) == 1
    assert len(library.add(file)) == 1
    assert library.load()[0].title == "Game"
    assert file.read_bytes() == b"PK\x03\x04test"


def test_remove_only_deletes_reference(library: Library, tmp_path: Path) -> None:
    file = apk(tmp_path)
    library.add(file)
    assert library.remove(file) == []
    assert file.exists()
    assert library.load() == []


def test_missing_file_is_not_erased(library: Library, tmp_path: Path) -> None:
    file = apk(tmp_path)
    library.add(file)
    file.unlink()
    assert len(library.load()) == 1 and not library.load()[0].available


def test_search_is_case_insensitive(library: Library, tmp_path: Path) -> None:
    library.add(apk(tmp_path, "Standoff.apk"))
    library.add(apk(tmp_path, "Other.apk"))
    assert [entry.title for entry in filter_entries(library.load(), " STAND ")] == ["Standoff"]


@pytest.mark.parametrize("name,data", [("game.apk", b"invalid"), ("game.txt", b"PK\x03\x04")])
def test_invalid_inputs_are_not_saved(
    library: Library, tmp_path: Path, name: str, data: bytes
) -> None:
    path = tmp_path / name
    path.write_bytes(data)
    with pytest.raises(ValueError):
        library.add(path)
    assert not library.path.exists()


def test_symlink_is_rejected(library: Library, tmp_path: Path) -> None:
    link = tmp_path / "link.apk"
    link.symlink_to(apk(tmp_path))
    with pytest.raises(ValueError):
        library.add(link)
    assert not library.path.exists()


def test_corrupt_library_is_never_overwritten(library: Library, tmp_path: Path) -> None:
    library.path.parent.mkdir(parents=True)
    library.path.write_text("broken json")
    with pytest.raises(DataCorrupted):
        library.add(apk(tmp_path))
    assert library.path.read_text() == "broken json"


def test_bad_schema_and_relative_paths_are_rejected(library: Library) -> None:
    library.path.parent.mkdir(parents=True)
    library.path.write_text(json.dumps({"schema": 1, "files": ["relative.apk"]}))
    with pytest.raises(DataCorrupted):
        library.load()


def test_saved_file_is_private(library: Library, tmp_path: Path) -> None:
    library.add(apk(tmp_path))
    assert library.path.stat().st_mode & 0o777 == 0o600


def test_launcher_art_is_packaged() -> None:
    from importlib.resources import files

    image = files("lynxdroid").joinpath("data/launcher-bg.jpg")
    assert image.is_file()
    assert image.read_bytes().startswith(b"\xff\xd8\xff")
    preview_image = Path(__file__).resolve().parents[2] / "web-preview" / "launcher-bg.jpg"
    assert preview_image.read_bytes() == image.read_bytes(), "Update browser preview artwork too"
    desktop = files("lynxdroid").joinpath("data/desktop-bg.jpg")
    preview_desktop = preview_image.with_name("desktop-bg.jpg")
    assert desktop.is_file() and desktop.read_bytes() == preview_desktop.read_bytes()


def test_invalid_utf8_library_does_not_overwrite(library: Library, tmp_path: Path) -> None:
    from lynxdroid.core.errors import DataCorrupted

    library.path.parent.mkdir(parents=True, exist_ok=True)
    library.path.write_bytes(b"\xff")
    apk = tmp_path / "game.apk"
    apk.write_bytes(b"PK\x03\x04")
    with pytest.raises(DataCorrupted):
        library.load()
    with pytest.raises(DataCorrupted):
        library.add(apk)
    assert library.path.read_bytes() == b"\xff"
