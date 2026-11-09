"""A local list of user-selected APK files, not an Android installer."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from lynxdroid.core.errors import DataCorrupted
from lynxdroid.core.storage import read_json, write_json
from lynxdroid.gui.file_lock import exclusive_file_lock

MAX_ENTRIES = 500


@dataclass(frozen=True)
class LibraryEntry:
    path: Path

    @property
    def title(self) -> str:
        return self.path.stem

    @property
    def available(self) -> bool:
        return self.path.is_file() and not self.path.is_symlink()


class Library:
    """Stores file references only; it never installs or deletes the APK."""

    def __init__(self, path: Path) -> None:
        self.path = path

    def load(self) -> list[LibraryEntry]:
        try:
            data = read_json(self.path)
        except FileNotFoundError:
            return []
        raw = data.get("files")
        if data.get("schema") != 1 or not isinstance(raw, list) or len(raw) > MAX_ENTRIES:
            raise DataCorrupted("Invalid launcher library; original file was not changed")
        if any(not isinstance(item, str) or not Path(item).is_absolute() for item in raw):
            raise DataCorrupted("Invalid launcher library paths; original file was not changed")
        if len(set(raw)) != len(raw):
            raise DataCorrupted("Duplicate launcher library paths; original file was not changed")
        return [LibraryEntry(Path(item)) for item in raw]

    def add(self, source: Path) -> list[LibraryEntry]:
        path = source.expanduser().absolute()
        if path.is_symlink() or not path.is_file() or path.suffix.lower() != ".apk":
            raise ValueError("Выберите обычный файл APK (не ссылку).")
        with path.open("rb") as handle:
            if handle.read(4) != b"PK\x03\x04":
                raise ValueError("Файл не похож на APK: отсутствует заголовок ZIP.")
        try:
            with exclusive_file_lock(self.path):
                entries = self.load()
                if any(entry.path == path for entry in entries):
                    return entries
                if len(entries) >= MAX_ENTRIES:
                    raise ValueError("В библиотеке уже 500 файлов.")
                entries.append(LibraryEntry(path))
                self._save(entries)
                return entries
        except BlockingIOError as exc:
            raise BlockingIOError("Библиотека занята другим окном. Повторите добавление.") from exc

    def remove(self, source: Path) -> list[LibraryEntry]:
        try:
            with exclusive_file_lock(self.path):
                entries = self.load()
                remaining = [entry for entry in entries if entry.path != source]
                if len(remaining) != len(entries):
                    self._save(remaining)
                return remaining
        except BlockingIOError as exc:
            raise BlockingIOError("Библиотека занята другим окном. Повторите удаление.") from exc

    def _save(self, entries: list[LibraryEntry]) -> None:
        write_json(self.path, {"schema": 1, "files": [str(entry.path) for entry in entries]})


def filter_entries(entries: list[LibraryEntry], query: str) -> list[LibraryEntry]:
    needle = query.strip().casefold()
    return [entry for entry in entries if needle in entry.title.casefold()]
