"""Crash-safe profile storage.

Layout (all under ``$XDG_DATA_HOME/lynxdroid``)::

    profiles/<profile-id>.json      one file per profile, atomic replace
    backups/<profile-id>/<ts>.json  previous versions kept before overwrite
    exports/                        user-initiated exports only

Why one file per profile instead of one big file: a corrupt single profile
cannot destroy the others, and a crash mid-save only risks that one profile,
which still has its atomic temp-file guarantee and a backup.
"""

from __future__ import annotations

import contextlib
import errno
import json
import logging
import shutil
import time
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

from lynxdroid.core.errors import DataCorrupted
from lynxdroid.core.paths import AppPaths, safe_remove
from lynxdroid.core.storage import read_json, write_json
from lynxdroid.profiles.codec import profile_from_dict, profile_to_dict
from lynxdroid.profiles.model import Profile, ProfileError

log = logging.getLogger("lynxdroid.profiles.store")

MAX_PROFILE_BYTES = 1_000_000
MAX_BACKUPS = 10


@dataclass(frozen=True)
class StoredProfile:
    """A profile plus where it came from, so the UI can show provenance."""

    profile: Profile
    path: Path
    loaded_at: float


class ProfileStore:
    """Filesystem-backed repository with atomic writes and soft deletes."""

    def __init__(self, paths: AppPaths) -> None:
        self._paths = paths
        self._dir = paths.profiles_dir

    # -- paths -----------------------------------------------------------
    @property
    def directory(self) -> Path:
        return self._dir

    def path_for(self, profile_id: str) -> Path:
        if not profile_id or "/" in profile_id or "\\" in profile_id or profile_id.startswith("."):
            raise ProfileError(f"invalid profile id '{profile_id}'", path="id")
        return self._dir / f"{profile_id}.json"

    # -- reads -----------------------------------------------------------
    def list_ids(self) -> list[str]:
        if not self._dir.is_dir():
            return []
        ids: list[str] = []
        for entry in sorted(self._dir.glob("*.json")):
            if entry.name.startswith("."):
                continue
            ids.append(entry.stem)
        return ids

    def load_ids(self) -> list[str]:
        """Ids of every stored profile file (cheap, no parsing)."""
        return self.list_ids()

    def iter_profiles(self) -> Iterator[tuple[StoredProfile | None, Path, str | None]]:
        """Yield ``(profile, path, error)`` for every stored file.

        A broken file yields ``profile=None`` with a message; listing never
        raises so the GUI can show "this profile is damaged" next to the rest.
        """
        for path in sorted(self._dir.glob("*.json")):
            if path.name.startswith("."):
                continue
            try:
                profile = self.load(path.stem)
            except DataCorrupted as exc:
                yield None, path, exc.message
                continue
            except ProfileError as exc:
                yield None, path, str(exc)
                continue
            except FileNotFoundError:
                continue
            yield StoredProfile(profile=profile, path=path, loaded_at=time.time()), path, None

    def load(self, profile_id: str) -> Profile:
        path = self.path_for(profile_id)
        size = path.stat().st_size if path.exists() else 0
        if size > MAX_PROFILE_BYTES:
            raise DataCorrupted(
                f"{path.name} is {size} bytes, larger than the {MAX_PROFILE_BYTES} byte limit"
            )
        data = read_json(path)
        return profile_from_dict(data)

    def load_all(self) -> list[Profile]:
        profiles = [
            stored.profile
            for stored, _, err in self.iter_profiles()
            if err is None and stored is not None
        ]
        return sorted(profiles, key=lambda p: (p.target.package, p.name.lower()))

    def find_for_package(self, package: str) -> list[Profile]:
        return [p for p in self.load_all() if p.target.package == package]

    # -- writes ----------------------------------------------------------
    def save(self, profile: Profile, *, keep_backup: bool = True) -> Path:
        """Atomically persist ``profile``; back up the previous version first."""
        profile.validate()
        self._dir.mkdir(parents=True, exist_ok=True, mode=0o700)
        path = self.path_for(profile.id)
        if keep_backup and path.exists():
            self._backup(path, profile.id)
        payload = profile_to_dict(profile)
        write_json(path, payload)
        log.info(
            "saved profile %s (%s) with %d elements",
            profile.id,
            profile.name,
            len(profile.elements),
        )
        return path

    def delete(self, profile_id: str, *, hard: bool = False) -> Path | None:
        """Delete a profile; by default keep a backup copy and return its path."""
        path = self.path_for(profile_id)
        if not path.exists():
            raise FileNotFoundError(path)
        backup: Path | None = None
        if not hard:
            backup = self._backup(path, profile_id)
        safe_remove(path, self._dir)
        log.info("deleted profile %s (backup=%s)", profile_id, backup)
        return backup

    def rename(self, profile_id: str, new_name: str) -> Profile:
        profile = self.load(profile_id)
        updated = profile.with_name(new_name)
        self.save(updated)
        return updated

    def duplicate(self, profile_id: str, new_name: str | None = None) -> Profile:
        source = self.load(profile_id)
        copy = source.duplicate(new_name)
        self.save(copy, keep_backup=False)
        return copy

    # -- import / export -------------------------------------------------
    def export(self, profile: Profile, destination: Path) -> Path:
        """Write a portable copy; contains no secrets by construction."""
        destination = Path(destination)
        if destination.suffix.lower() != ".json":
            destination = destination.with_suffix(".json")
        destination.parent.mkdir(parents=True, exist_ok=True)
        profile.validate()
        write_json(destination, profile_to_dict(profile), mode=0o600)
        return destination

    def import_file(self, source: Path, *, overwrite: bool = False) -> Profile:
        """Validate an external file and store it. Original is never modified."""
        source = Path(source)
        if not source.is_file():
            raise ProfileError(f"{source} is not a file")
        if source.is_symlink():
            # Any symlink is refused: a "profile" that is really a link to
            # /etc/shadow (or a FIFO, or /dev/zero) must never be read here.
            raise ProfileError(
                f"refusing to import {source.name}: it is a symlink, copy the real file first"
            )
        if source.stat().st_size > MAX_PROFILE_BYTES:
            raise ProfileError(f"{source.name} is too large to be a profile")
        raw = json.loads(source.read_text(encoding="utf-8"))
        if not isinstance(raw, dict):
            raise ProfileError("imported file must contain a JSON object")
        profile = profile_from_dict(raw)
        existing = self.path_for(profile.id)
        if existing.exists() and not overwrite:
            # Same id, different content: import as a copy to lose nothing.
            profile = profile.duplicate(f"{profile.name} (imported)")
        self.save(profile)
        return profile

    # -- backups ---------------------------------------------------------
    def backups_for(self, profile_id: str) -> list[Path]:
        directory = self._paths.backups_dir / profile_id
        if not directory.is_dir():
            return []
        return sorted(directory.glob("*.json"), reverse=True)

    def restore_backup(self, profile_id: str, index: int = 0) -> Profile:
        backups = self.backups_for(profile_id)
        if not backups:
            raise FileNotFoundError(f"no backups for profile {profile_id}")
        try:
            chosen = backups[index]
        except IndexError:
            raise FileNotFoundError(f"backup #{index} does not exist") from None
        profile = profile_from_dict(read_json(chosen))
        if profile.id != profile_id:
            raise DataCorrupted(f"backup {chosen.name} belongs to {profile.id}")
        self.save(profile, keep_backup=True)
        return profile

    def _backup(self, path: Path, profile_id: str) -> Path:
        directory = self._paths.backups_dir / profile_id
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        stamp = time.strftime("%Y%m%dT%H%M%S", time.gmtime())
        target = directory / f"{stamp}.json"
        counter = 1
        while target.exists():
            target = directory / f"{stamp}-{counter}.json"
            counter += 1
        try:
            shutil.copy2(path, target)
        except OSError as exc:
            if exc.errno == errno.ENOSPC:
                raise DataCorrupted("no space left for the profile backup") from exc
            raise
        self._prune_backups(directory)
        return target

    @staticmethod
    def _prune_backups(directory: Path) -> None:
        entries = sorted(directory.glob("*.json"), reverse=True)
        for stale in entries[MAX_BACKUPS:]:
            with contextlib.suppress(OSError):  # pragma: no cover - best effort
                stale.unlink()

    # -- maintenance -----------------------------------------------------
    def storage_report(self) -> dict[str, object]:
        files = list(self._dir.glob("*.json")) if self._dir.is_dir() else []
        total = sum(f.stat().st_size for f in files if f.is_file())
        return {
            "directory": str(self._dir),
            "files": len(files),
            "bytes": total,
            "backups": sum(len(self.backups_for(i)) for i in self.list_ids()),
        }
