"""Storage guarantees: atomic writes, backups, safe deletes, corrupt files."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from lynxdroid.core.errors import DataCorrupted, PermissionDenied
from lynxdroid.profiles.model import Element, ElementType, Profile, ProfileError, Target
from lynxdroid.profiles.store import ProfileStore

TARGET = Target(package="com.example.game")


def make(name: str = "Layout") -> Profile:
    return Profile.new(
        name,
        TARGET,
        elements=(Element(id="tap-a", type=ElementType.TAP, label="A", x=0.5, y=0.5, keys=("j",)),),
    )


@pytest.fixture()
def store(app_paths) -> ProfileStore:
    return ProfileStore(app_paths)


class TestSaveLoad:
    def test_save_then_load(self, store: ProfileStore) -> None:
        profile = make()
        path = store.save(profile)
        assert path.exists()
        assert store.load(profile.id).to_dict() == profile.to_dict()

    def test_saved_file_is_user_only(self, store: ProfileStore) -> None:
        path = store.save(make())
        mode = path.stat().st_mode & 0o777
        assert mode == 0o600

    def test_list_and_load_all(self, store: ProfileStore) -> None:
        first, second = make("B layout"), make("A layout")
        store.save(first)
        store.save(second)
        names = [p.name for p in store.load_all()]
        assert names == sorted(names, key=str.lower)
        assert len(store.load_all()) == 2

    def test_find_for_package(self, store: ProfileStore) -> None:
        store.save(make())
        assert len(store.find_for_package("com.example.game")) == 1
        assert store.find_for_package("com.other.app") == []

    def test_invalid_id_is_rejected(self, store: ProfileStore) -> None:
        for bad in ("", "../escape", "/etc/passwd", ".hidden"):
            with pytest.raises(ProfileError):
                store.path_for(bad)

    def test_missing_profile_raises_filenotfound(self, store: ProfileStore) -> None:
        with pytest.raises(FileNotFoundError):
            store.load("does-not-exist")


class TestAtomicity:
    def test_no_temp_files_left_behind(self, store: ProfileStore) -> None:
        store.save(make())
        leftovers = [p.name for p in store.directory.iterdir() if p.name.startswith(".")]
        assert leftovers == []

    def test_second_save_creates_a_backup(self, store: ProfileStore) -> None:
        profile = store.save(make()) and store.load_all()[0]
        store.save(profile.with_name("Renamed"))
        backups = store.backups_for(profile.id)
        assert len(backups) == 1
        assert json.loads(backups[0].read_text())["name"] == "Layout"

    def test_backup_rotation_keeps_a_bounded_number(self, store: ProfileStore) -> None:
        store.save(make())
        profile = store.load_all()[0]
        for index in range(14):
            store.save(profile.with_name(f"v{index}"))
        assert len(store.backups_for(profile.id)) <= 10

    def test_backup_can_be_restored(self, store: ProfileStore) -> None:
        original = make()
        store.save(original)
        stored = store.load_all()[0]
        store.save(stored.with_name("Changed"))
        restored = store.restore_backup(original.id, 0)
        assert restored.name == "Layout"

    def test_restore_without_backups_raises(self, store: ProfileStore) -> None:
        profile = make()
        store.save(profile)
        with pytest.raises(FileNotFoundError):
            store.restore_backup(profile.id)

    def test_write_failure_does_not_destroy_the_old_file(
        self, store: ProfileStore, monkeypatch
    ) -> None:
        profile = make()
        path = store.save(profile)

        def explode(*args, **kwargs):
            raise OSError("disk full")

        monkeypatch.setattr("lynxdroid.core.storage.os.replace", explode)
        with pytest.raises(OSError):
            store.save(profile.with_name("New name"))
        assert json.loads(path.read_text())["name"] == "Layout"


class TestCorruption:
    def test_corrupt_json_is_reported_not_replaced(self, store: ProfileStore) -> None:
        path = store.save(make())
        path.write_text("{ this is not json", encoding="utf-8")
        with pytest.raises(DataCorrupted, match="not valid JSON"):
            store.load(path.stem)
        assert "this is not json" in path.read_text()

    def test_non_object_json_rejected(self, store: ProfileStore) -> None:
        path = store.directory / "abcdef.json"
        path.write_text("[1, 2, 3]", encoding="utf-8")
        with pytest.raises(DataCorrupted, match="JSON object"):
            store.load("abcdef")

    def test_oversized_file_rejected(self, store: ProfileStore) -> None:
        path = store.directory / "big.json"
        path.write_bytes(b"{" + b" " * 1_000_001)
        with pytest.raises(DataCorrupted, match="larger than"):
            store.load("big")

    def test_iter_profiles_reports_errors_without_raising(self, store: ProfileStore) -> None:
        store.save(make())
        (store.directory / "broken.json").write_text("oops", encoding="utf-8")
        entries = list(store.iter_profiles())
        assert len(entries) == 2
        assert sum(1 for stored, _, error in entries if error) == 1


class TestDelete:
    def test_delete_keeps_a_backup_by_default(self, store: ProfileStore) -> None:
        profile = make()
        store.save(profile)
        backup = store.delete(profile.id)
        assert backup is not None and backup.exists()
        assert store.load_ids() == []

    def test_hard_delete_leaves_nothing(self, store: ProfileStore) -> None:
        profile = make()
        store.save(profile)
        assert store.delete(profile.id, hard=True) is None
        assert store.backups_for(profile.id) == []

    def test_delete_missing_raises(self, store: ProfileStore) -> None:
        with pytest.raises(FileNotFoundError):
            store.delete("missing-id")

    def test_delete_refuses_to_escape_the_directory(
        self, store: ProfileStore, tmp_path: Path
    ) -> None:
        victim = tmp_path / "important.txt"
        victim.write_text("keep me")
        link = store.directory / "linked.json"
        link.symlink_to(victim)
        with pytest.raises(PermissionDenied):
            store.delete("linked")
        assert victim.exists()


class TestImportExport:
    def test_export_then_import(self, store: ProfileStore, tmp_path: Path) -> None:
        profile = make("Portable")
        store.save(profile)
        exported = store.export(profile, tmp_path / "out")
        assert exported.suffix == ".json"
        assert exported.stat().st_mode & 0o777 == 0o600
        store.delete(profile.id, hard=True)
        imported = store.import_file(exported)
        assert imported.name == "Portable"
        assert imported.elements == profile.elements

    def test_import_through_symlink_is_refused(self, store: ProfileStore, tmp_path: Path) -> None:
        source = tmp_path / "real.json"
        store.export(make(), source)
        link = tmp_path / "link.json"
        link.symlink_to(source)
        with pytest.raises(ProfileError, match="symlink"):
            store.import_file(link)

    def test_import_rejects_a_directory(self, store: ProfileStore, tmp_path: Path) -> None:
        with pytest.raises(ProfileError):
            store.import_file(tmp_path)

    def test_import_of_same_id_without_overwrite_creates_a_copy(
        self, store: ProfileStore, tmp_path: Path
    ) -> None:
        profile = make()
        store.save(profile)
        source = tmp_path / "copy.json"
        store.export(profile, source)
        second = store.import_file(source)
        assert second.id != profile.id
        assert "imported" in second.name

    def test_import_rejects_invalid_content(self, store: ProfileStore, tmp_path: Path) -> None:
        bad = tmp_path / "bad.json"
        bad.write_text('{"schema": 1}', encoding="utf-8")
        with pytest.raises(ProfileError):
            store.import_file(bad)


class TestStorageReport:
    def test_report_counts_files_and_bytes(self, store: ProfileStore) -> None:
        store.save(make())
        report = store.storage_report()
        assert report["files"] == 1
        assert int(report["bytes"]) > 0  # type: ignore[arg-type]


def test_no_world_readable_files(app_paths) -> None:
    store = ProfileStore(app_paths)
    store.save(make())
    for entry in store.directory.iterdir():
        if entry.is_file():
            assert not (entry.stat().st_mode & 0o077), f"{entry} is group/world accessible"


def test_store_never_writes_outside_its_root(app_paths, tmp_path: Path) -> None:
    store = ProfileStore(app_paths)
    store.save(make())
    outside = [p for p in tmp_path.rglob("*.json") if app_paths.data not in p.parents]
    assert outside == []
    assert app_paths.profiles_dir.exists()
