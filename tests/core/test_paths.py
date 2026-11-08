"""Path safety and XDG behaviour (stage 14)."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

from lynxdroid.core.errors import PermissionDenied
from lynxdroid.core.paths import AppPaths, free_space_bytes, is_within, safe_remove


class TestXdg:
    def test_respects_absolute_xdg_variables(
        self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        monkeypatch.setenv("XDG_CONFIG_HOME", str(tmp_path / "cfg"))
        monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path / "dat"))
        paths = AppPaths.resolve()
        assert paths.config == tmp_path / "cfg" / "lynxdroid"
        assert paths.data == tmp_path / "dat" / "lynxdroid"

    def test_ignores_relative_xdg_variables(self, monkeypatch: pytest.MonkeyPatch) -> None:
        monkeypatch.setenv("XDG_CONFIG_HOME", "relative/path")
        paths = AppPaths.resolve()
        assert paths.config.is_absolute()
        assert "relative/path" not in str(paths.config)

    def test_empty_xdg_variable_falls_back_to_home(
        self, monkeypatch: pytest.MonkeyPatch, tmp_path: Path
    ) -> None:
        monkeypatch.setenv("XDG_CONFIG_HOME", "")
        paths = AppPaths.resolve()
        assert paths.config.parent.parent == Path.home()  # <home>/.config

    def test_derived_paths_live_under_their_roots(self, app_paths: AppPaths) -> None:
        assert app_paths.settings_file.parent == app_paths.config
        assert app_paths.profiles_dir.parent == app_paths.data
        assert app_paths.backups_dir.parent == app_paths.data
        assert app_paths.log_file.parent == app_paths.state

    def test_ensure_creates_private_directories(self, tmp_path: Path) -> None:
        paths = AppPaths.resolve(root=tmp_path / "root")
        paths.ensure()
        for directory in (paths.config, paths.data, paths.profiles_dir, paths.backups_dir):
            assert directory.is_dir()
            assert directory.stat().st_mode & 0o777 == 0o700

    def test_resolve_with_root_does_not_touch_home(self, tmp_path: Path) -> None:
        paths = AppPaths.resolve(root=tmp_path / "sandbox")
        assert str(paths.config).startswith(str(tmp_path))


class TestIsWithin:
    def test_child_is_within(self, tmp_path: Path) -> None:
        assert is_within(tmp_path / "a" / "b.txt", tmp_path)

    def test_root_is_not_within_itself(self, tmp_path: Path) -> None:
        assert not is_within(tmp_path, tmp_path)

    def test_parent_is_not_within_child(self, tmp_path: Path) -> None:
        assert not is_within(tmp_path, tmp_path / "sub")

    def test_traversal_escape_detected(self, tmp_path: Path) -> None:
        assert not is_within(tmp_path / ".." / "outside.txt", tmp_path)

    def test_symlink_escape_detected(self, tmp_path: Path) -> None:
        root = tmp_path / "root"
        root.mkdir()
        outside = tmp_path / "outside.txt"
        outside.write_text("secret")
        (root / "link").symlink_to(outside)
        assert not is_within(root / "link", root)


class TestSafeRemove:
    def test_removes_a_file_inside_the_root(self, tmp_path: Path) -> None:
        victim = tmp_path / "file.txt"
        victim.write_text("x")
        safe_remove(victim, tmp_path)
        assert not victim.exists()

    def test_missing_file_is_not_an_error(self, tmp_path: Path) -> None:
        safe_remove(tmp_path / "absent.txt", tmp_path)

    def test_refuses_outside_paths(self, tmp_path: Path) -> None:
        outside = tmp_path.parent / "outside.txt"
        outside.write_text("keep")
        try:
            with pytest.raises(PermissionDenied):
                safe_remove(outside, tmp_path)
            assert outside.exists()
        finally:
            outside.unlink(missing_ok=True)

    def test_refuses_the_root_itself(self, tmp_path: Path) -> None:
        with pytest.raises(PermissionDenied):
            safe_remove(tmp_path, tmp_path)

    def test_refuses_symlink_pointing_outside(self, tmp_path: Path) -> None:
        root = tmp_path / "root"
        root.mkdir()
        target = tmp_path / "target.txt"
        target.write_text("keep")
        link = root / "link.txt"
        link.symlink_to(target)
        with pytest.raises(PermissionDenied):
            safe_remove(link, root)
        assert target.exists()

    def test_allows_symlink_pointing_inside(self, tmp_path: Path) -> None:
        root = tmp_path / "root"
        root.mkdir()
        target = root / "target.txt"
        target.write_text("x")
        link = root / "link.txt"
        link.symlink_to(target)
        safe_remove(link, root)
        assert not link.exists()
        assert target.exists()


class TestFreeSpace:
    def test_reports_a_positive_number(self, tmp_path: Path) -> None:
        value = free_space_bytes(tmp_path)
        assert value is not None and value > 0

    def test_walks_up_for_missing_paths(self, tmp_path: Path) -> None:
        value = free_space_bytes(tmp_path / "does" / "not" / "exist")
        assert value is not None and value > 0


@pytest.mark.skipif(os.geteuid() == 0, reason="root ignores file permissions")
def test_unreadable_directory_is_reported_not_crashed(tmp_path: Path) -> None:
    blocked = tmp_path / "blocked"
    blocked.mkdir()
    blocked.chmod(0o000)
    try:
        assert free_space_bytes(blocked) is not None
    finally:
        blocked.chmod(0o700)
