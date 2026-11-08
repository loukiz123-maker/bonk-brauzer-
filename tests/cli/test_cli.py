"""CLI behaviour: exit codes, JSON output, consent gates, no side effects."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from lynxdroid.cli.main import main
from lynxdroid.core.errors import ExitCode
from lynxdroid.doctor import checks as doctor_checks


def run(capsys, args: list[str], root: Path) -> tuple[int, str, str]:
    code = main(["--root", str(root), *args])
    captured = capsys.readouterr()
    return code, captured.out, captured.err


@pytest.fixture()
def root(tmp_path: Path) -> Path:
    return tmp_path / "xdg"


class TestVersion:
    def test_prints_a_banner(self, capsys, root: Path) -> None:
        code, out, _ = run(capsys, ["version"], root)
        assert code == ExitCode.OK
        assert "LynxDroid" in out

    def test_json_is_machine_readable(self, capsys, root: Path) -> None:
        code, out, _ = run(capsys, ["version", "--json"], root)
        assert code == ExitCode.OK
        payload = json.loads(out)
        assert payload["schemas"]["profile"] == 1
        assert "python" in payload


class TestDoctor:
    def test_runs_read_only_and_exits(self, capsys, root: Path) -> None:
        code, out, _ = run(capsys, ["doctor", "--quiet"], root)
        assert code in (ExitCode.OK, ExitCode.FAIL)
        assert "LynxDroid doctor" in out

    def test_json_report_has_schema_and_checks(self, capsys, root: Path) -> None:
        _code, out, _ = run(capsys, ["doctor", "--json"], root)
        payload = json.loads(out)
        assert payload["schema"] == 1
        assert payload["checks"]
        for check in payload["checks"]:
            assert check["status"] in {"OK", "WARNING", "ERROR", "UNKNOWN"}
            assert check["title"]
            assert "observed" in check

    def test_strict_mode_blocks_on_missing_prerequisites(self, capsys, root: Path) -> None:
        code, out, _ = run(capsys, ["doctor", "--json", "--strict"], root)
        payload = json.loads(out)
        required = set(payload["meta"]["required_for_container"])
        blockers = [
            c
            for c in payload["checks"]
            if c["id"] in required and c["status"] in {"ERROR", "UNKNOWN"}
        ]
        expected = ExitCode.BLOCKED if blockers else ExitCode.OK
        assert code == expected

    def test_area_filter(self, capsys, root: Path) -> None:
        _code, out, _ = run(capsys, ["doctor", "--json", "--area", "environment"], root)
        payload = json.loads(out)
        assert all(c["id"].startswith(("os.", "session.")) for c in payload["checks"])

    def test_doctor_creates_no_config_outside_root(
        self, capsys, root: Path, tmp_path: Path
    ) -> None:
        run(capsys, ["doctor", "--json"], root)
        created = {p.name for p in (tmp_path / "home").rglob("*") if p.is_file()}
        assert not any(".json" in name for name in created)


class TestStatus:
    def test_fake_backend_status_is_json(self, capsys, root: Path) -> None:
        _code, out, _ = run(capsys, ["status", "--json"], root)
        payload = json.loads(out)
        assert "snapshot" in payload
        assert payload["backend"] in {"waydroid", "fake"}

    def test_capabilities_are_reported(self, capsys, root: Path) -> None:
        _code, out, _ = run(capsys, ["status", "--json"], root)
        payload = json.loads(out)
        assert payload["capabilities"]["features"]


class TestProfilesCli:
    def _seed(self, root: Path) -> str:
        from lynxdroid.core.paths import AppPaths
        from lynxdroid.profiles.model import Element, ElementType, Profile, Target
        from lynxdroid.profiles.store import ProfileStore

        paths = AppPaths.resolve(root=root)
        paths.ensure()
        store = ProfileStore(paths)
        profile = Profile.new(
            "CLI layout",
            Target(package="com.example.cli"),
            elements=(Element(id="a", type=ElementType.TAP, label="A", x=0.5, y=0.5, keys=("j",)),),
        )
        store.save(profile)
        return profile.id

    def test_list_empty(self, capsys, root: Path) -> None:
        code, out, _ = run(capsys, ["profiles", "list"], root)
        assert code == ExitCode.OK
        assert "no profiles" in out

    def test_list_json(self, capsys, root: Path) -> None:
        profile_id = self._seed(root)
        _code, out, _ = run(capsys, ["profiles", "list", "--json"], root)
        rows = json.loads(out)
        assert rows[0]["id"] == profile_id
        assert rows[0]["elements"] == 1

    def test_validate_ok(self, capsys, root: Path) -> None:
        self._seed(root)
        code, out, _ = run(capsys, ["profiles", "validate"], root)
        assert code == ExitCode.OK
        assert "✔" in out

    def test_validate_broken_file(self, capsys, root: Path) -> None:
        profile_id = self._seed(root)
        from lynxdroid.core.paths import AppPaths

        broken = AppPaths.resolve(root=root).profiles_dir / f"{profile_id}.json"
        broken.write_text("{broken", encoding="utf-8")
        code, out, _ = run(capsys, ["profiles", "validate"], root)
        assert code == ExitCode.FAIL
        assert "✘" in out

    def test_delete_requires_confirmation(self, capsys, root: Path) -> None:
        profile_id = self._seed(root)
        code, out, _ = run(capsys, ["profiles", "delete", profile_id], root)
        assert code == ExitCode.BLOCKED
        assert "re-run with --yes" in out
        # Nothing was deleted.
        code, out, _ = run(capsys, ["profiles", "list", "--json"], root)
        assert json.loads(out)[0]["id"] == profile_id

    def test_delete_with_confirmation_keeps_a_backup(self, capsys, root: Path) -> None:
        profile_id = self._seed(root)
        code, out, _ = run(capsys, ["profiles", "delete", profile_id, "--yes"], root)
        assert code == ExitCode.OK
        assert "backup" in out

    def test_rename_changes_the_name_and_keeps_the_id(self, capsys, root: Path) -> None:
        profile_id = self._seed(root)
        code, out, _ = run(capsys, ["profiles", "rename", profile_id, "Renamed"], root)
        assert code == ExitCode.OK
        assert "Renamed" in out
        # The id is the file's identity: renaming must not mint a new profile.
        code, out, _ = run(capsys, ["profiles", "list", "--json"], root)
        listed = json.loads(out)
        assert [p["id"] for p in listed] == [profile_id]
        assert listed[0]["name"] == "Renamed"

    def test_rename_with_an_empty_name_is_refused(self, capsys, root: Path) -> None:
        profile_id = self._seed(root)
        code, out, err = run(capsys, ["profiles", "rename", profile_id, "   "], root)
        assert code == ExitCode.FAIL
        # Errors go to stderr so a script can pipe stdout as data.
        assert "cannot rename" in err
        assert out == ""

    def test_rename_of_an_unknown_profile_fails_cleanly(self, capsys, root: Path) -> None:
        code, _, err = run(capsys, ["profiles", "rename", "nope", "X"], root)
        assert code != ExitCode.OK
        assert "Traceback" not in err

    def test_export_and_import_round_trip(self, capsys, root: Path, tmp_path: Path) -> None:
        profile_id = self._seed(root)
        destination = tmp_path / "exported.json"
        code, _, _ = run(capsys, ["profiles", "export", profile_id, str(destination)], root)
        assert code == ExitCode.OK
        assert destination.exists()
        run(capsys, ["profiles", "delete", profile_id, "--yes"], root)
        code, _, _ = run(capsys, ["profiles", "import", str(destination), "--yes"], root)
        assert code == ExitCode.OK

    def test_import_requires_confirmation(self, capsys, root: Path, tmp_path: Path) -> None:
        profile_id = self._seed(root)
        destination = tmp_path / "e.json"
        run(capsys, ["profiles", "export", profile_id, str(destination)], root)
        code, _out, _ = run(capsys, ["profiles", "import", str(destination)], root)
        assert code == ExitCode.BLOCKED


class TestDisplayCli:
    def test_presets_are_listed(self, capsys, root: Path) -> None:
        code, out, _ = run(capsys, ["display", "presets"], root)
        assert code == ExitCode.OK
        assert "1280x720" in out

    def test_invalid_values_are_rejected(self, capsys, root: Path) -> None:
        code, _out, err = run(capsys, ["display", "set", "0", "1080", "320", "--yes"], root)
        assert code == ExitCode.USAGE
        assert "positive" in err

    def test_set_requires_confirmation(self, capsys, root: Path) -> None:
        code, out, _ = run(capsys, ["display", "set", "1280", "720", "240"], root)
        assert code == ExitCode.BLOCKED
        assert "--yes" in out

    def test_show_with_fake_backend(self, capsys, root: Path) -> None:
        code, out, _ = run(capsys, ["display", "show", "--json"], root)
        assert code == ExitCode.OK
        assert isinstance(json.loads(out), dict)


class TestAppsCli:
    def test_install_requires_a_file(self, capsys, root: Path) -> None:
        # Missing input is a usage error (2), documented in --help.
        code, _, err = run(capsys, ["apps", "install", "/nonexistent.apk", "--yes"], root)
        assert code == ExitCode.USAGE
        assert "does not exist" in err

    def test_install_requires_confirmation(self, capsys, root: Path, tmp_path: Path) -> None:
        apk = tmp_path / "game.apk"
        apk.write_bytes(b"PK\x03\x04" + b"0" * 100)
        code, out, _ = run(capsys, ["apps", "install", str(apk)], root)
        assert code == ExitCode.BLOCKED
        assert "sha256" in out

    def test_list_json(self, capsys, root: Path) -> None:
        code, out, _ = run(capsys, ["apps", "list", "--json"], root)
        assert code == ExitCode.OK
        assert isinstance(json.loads(out), list)


class TestUsage:
    def test_no_command_is_a_usage_error(self, capsys, root: Path) -> None:
        with pytest.raises(SystemExit) as excinfo:
            main(["--root", str(root)])
        assert excinfo.value.code == 2

    def test_unknown_command_is_a_usage_error(self, capsys, root: Path) -> None:
        with pytest.raises(SystemExit) as excinfo:
            main(["--root", str(root), "frobnicate"])
        assert excinfo.value.code == 2

    def test_help_mentions_exit_codes(self, capsys, root: Path) -> None:
        with pytest.raises(SystemExit):
            main(["--help"])
        assert "exit codes" in capsys.readouterr().out.lower()


class TestGuiDependency:
    """The Qt probe must never lie, and a missing Qt must never traceback.

    Regression: ``import PySide6`` succeeds on a machine that is missing Qt's
    own shared libraries, so the old probe reported ``gui_available: true``.
    """

    def test_probe_reports_a_reason_when_qt_is_unusable(self, monkeypatch) -> None:
        from lynxdroid.gui import app as gui_app

        def broken_import():
            raise ImportError("libGL.so.1: cannot open shared object file")

        monkeypatch.setattr(gui_app, "_qt_module_present", lambda: True)
        monkeypatch.setattr(gui_app, "_load_qt_modules", broken_import)
        ok, reason = gui_app.qt_probe()
        assert ok is False
        assert "libGL" in reason
        assert "not loadable" in reason

    def test_absent_pyside6_is_reported_as_absent(self, monkeypatch) -> None:
        """An uninstalled PySide6 must not be blamed on "broken libraries"."""
        from lynxdroid.gui import app as gui_app

        monkeypatch.setattr(gui_app, "_qt_module_present", lambda: False)
        ok, reason = gui_app.qt_probe()
        assert ok is False
        assert reason == "PySide6 is not installed"

    def test_installed_but_broken_qt_says_so(self, monkeypatch) -> None:
        from lynxdroid.gui import app as gui_app

        def broken_import():
            raise ImportError("libEGL.so.1: cannot open shared object file")

        monkeypatch.setattr(gui_app, "_qt_module_present", lambda: True)
        monkeypatch.setattr(gui_app, "_load_qt_modules", broken_import)
        ok, reason = gui_app.qt_probe()
        assert ok is False
        assert "installed but" in reason
        assert "libEGL" in reason

    def test_probe_always_explains_itself(self) -> None:
        """Success and failure both carry a human-readable reason."""
        from lynxdroid.gui.app import qt_probe

        ok, reason = qt_probe()
        assert isinstance(ok, bool)
        assert reason.strip(), "the probe must always say why"
        if ok:
            assert "available" in reason.lower()

    def test_version_json_reports_availability_consistently(self, capsys, root: Path) -> None:
        from lynxdroid.gui.app import qt_probe

        code, out, _ = run(capsys, ["version", "--json"], root)
        assert code == ExitCode.OK
        payload = json.loads(out)
        assert payload["gui_available"] is qt_probe()[0]

    def test_gui_without_qt_is_a_clean_missing_dependency(
        self, capsys, root: Path, monkeypatch
    ) -> None:
        from lynxdroid.gui import app as gui_app

        def broken_import():
            raise ImportError("libGL.so.1: cannot open shared object file")

        monkeypatch.setattr(gui_app, "_qt_module_present", lambda: True)
        monkeypatch.setattr(gui_app, "_load_qt_modules", broken_import)
        code, _, err = run(capsys, ["gui", "--offscreen", "--backend", "fake"], root)
        assert code == ExitCode.MISSING_DEPENDENCY
        assert "libGL" in err
        assert "Traceback" not in err

    def test_text_version_prints_the_reason(self, capsys, root: Path, monkeypatch) -> None:
        from lynxdroid.gui import app as gui_app

        def broken_import():
            raise ImportError("libEGL.so.1 missing")

        monkeypatch.setattr(gui_app, "_qt_module_present", lambda: True)
        monkeypatch.setattr(gui_app, "_load_qt_modules", broken_import)
        code, out, _ = run(capsys, ["version"], root)
        assert code == ExitCode.OK
        assert "qt shell" in out
        assert "libEGL.so.1 missing" in out


class TestImageCommand:
    """The consent gate in front of the Android image download (stage 4)."""

    def test_plan_is_read_only_and_reports_the_target(self, capsys, root: Path) -> None:
        # `main()` creates the XDG skeleton for every command; what matters is
        # that `image plan` adds nothing on top of that baseline.
        run(capsys, ["version"], root)
        before = sorted(str(p.relative_to(root)) for p in root.rglob("*"))
        code, out, _ = run(capsys, ["image", "plan", "--json"], root)
        assert code == ExitCode.OK
        payload = json.loads(out)
        assert payload["channel"] == "lineage-vanilla"
        assert payload["checksums_available"] is False
        assert payload["requires_root_to_install"] is True
        assert payload["system_url"].startswith("https://")
        after = sorted(str(p.relative_to(root)) for p in root.rglob("*"))
        assert after == before, "image plan must not touch the filesystem"

    def test_plan_text_shows_licence_and_steps(self, capsys, root: Path) -> None:
        code, out, _ = run(capsys, ["image", "plan"], root)
        assert code == ExitCode.OK
        assert "Licence:" in out
        assert "Nothing is downloaded or installed until you confirm." in out

    def test_prepare_without_yes_is_blocked(self, capsys, root: Path) -> None:
        code, out, err = run(capsys, ["image", "prepare"], root)
        assert code == ExitCode.BLOCKED
        assert "explicit consent" in err
        # The plan is printed before the refusal, so the user can review it.
        assert "Licence:" in out

    def test_unverified_channel_needs_a_second_consent(self, capsys, root: Path) -> None:
        code, _, err = run(capsys, ["image", "prepare", "--yes"], root)
        assert code == ExitCode.BLOCKED
        assert "cannot be verified" in err
        assert "--allow-unverified" in err

    def test_unknown_channel_is_a_usage_error(self, capsys, root: Path) -> None:
        with pytest.raises(SystemExit) as excinfo:
            run(capsys, ["image", "plan", "--channel", "nope"], root)
        assert excinfo.value.code == 2

    def test_nothing_is_written_into_waydroid_paths(self, capsys, root: Path) -> None:
        """A refused download must leave no archive anywhere."""
        run(capsys, ["image", "prepare"], root)
        assert not list(root.glob("**/images/*.zip"))
        assert not list(root.glob("**/*.part"))


class TestHelpIsTruthful:
    """Regression: doctor once told users to run `lynxdroid image prepare`
    while no `image` command existed. Any hint that names a LynxDroid command
    must name one that the parser actually accepts.
    """

    @staticmethod
    def _command_tree(parser) -> dict[str, set[str]]:
        """Map every subcommand name to the set of words it accepts next."""

        def choices_of(p):
            for action in p._actions:
                if hasattr(action, "choices") and isinstance(action.choices, dict):
                    return action.choices
            return {}

        tree: dict[str, set[str]] = {}
        for name, subparser in choices_of(parser).items():
            tree[name] = set(choices_of(subparser))
        return tree

    def test_every_hint_names_a_real_command_and_subcommand(self) -> None:
        import re

        from lynxdroid.cli.main import build_parser

        tree = self._command_tree(build_parser())
        report = doctor_checks.full_report()
        problems: list[str] = []
        referenced = 0
        for check in report.checks:
            for text in (check.next_step or "", check.detail or ""):
                for match in re.finditer(r"lynxdroid\s+([a-z][a-z-]*)(?:\s+([a-z][a-z-]*))?", text):
                    referenced += 1
                    command, subcommand = match.group(1), match.group(2)
                    if command not in tree:
                        problems.append(f"unknown command '{command}' in: {text}")
                        continue
                    subs = tree[command]
                    if subcommand and subs and subcommand not in subs:
                        problems.append(f"unknown subcommand '{command} {subcommand}' in: {text}")
        assert referenced, "the doctor should point users at commands"
        assert not problems, "doctor gives unusable advice:\n" + "\n".join(problems)

    def test_documented_commands_all_parse(self) -> None:
        from lynxdroid.cli.main import build_parser

        known = set(self._command_tree(build_parser()))
        for command in (
            "doctor",
            "status",
            "start",
            "stop",
            "apps",
            "display",
            "profiles",
            "image",
            "gui",
            "version",
        ):
            assert command in known, f"{command} is documented but not implemented"


class TestLaunchReportsTheLayout:
    """Stage 9: a profile is bound to the app id and version, and the user is
    told about both. Before this the binding existed in the model but no code
    path ever looked it up, so `find_for_package` and `version_mismatch` were
    dead code.
    """

    @staticmethod
    def _save_profile(root: Path, package: str, version: str | None) -> None:
        from lynxdroid.core.paths import AppPaths
        from lynxdroid.profiles.model import Element, ElementType, Profile, Target
        from lynxdroid.profiles.store import ProfileStore

        paths = AppPaths.resolve(root)
        paths.ensure()
        store = ProfileStore(paths)
        store.save(
            Profile.new(
                "Shooter",
                Target(package=package, package_version=version),
                elements=(
                    Element(
                        id="tap", type=ElementType.TAP, label="Fire", x=0.5, y=0.5, keys=("j",)
                    ),
                ),
            )
        )

    @staticmethod
    def _use_fake_backend(root: Path) -> None:
        from lynxdroid.core.paths import AppPaths
        from lynxdroid.settings import SettingsStore

        paths = AppPaths.resolve(root)
        paths.ensure()
        SettingsStore(paths).update(backend="fake")

    def test_says_when_no_profile_is_bound(self, capsys, root: Path) -> None:
        self._use_fake_backend(root)
        code, out, _ = run(capsys, ["apps", "launch", "com.example.none"], root)
        assert code == ExitCode.OK
        assert "no layout profile is bound" in out

    def test_names_the_bound_profile(self, capsys, root: Path) -> None:
        self._save_profile(root, "com.example.game", "1.0.0")
        self._use_fake_backend(root)
        code, out, _ = run(capsys, ["apps", "launch", "com.example.game"], root)
        assert code == ExitCode.OK
        assert "1 layout profile(s) bound" in out
        assert "Shooter" in out, f"the profile name lost its case: {out!r}"

    def test_warns_when_the_app_version_changed(self, capsys, root: Path, monkeypatch) -> None:
        """The warning must fire on a real mismatch, not on every launch."""
        from lynxdroid.backend.waydroid import FakeBackend

        self._save_profile(root, "com.example.game", "1.0.0")
        self._use_fake_backend(root)
        monkeypatch.setattr(
            FakeBackend,
            "list_apps",
            lambda self: [{"package": "com.example.game", "version": "2.0.0"}],
        )
        code, out, _ = run(capsys, ["apps", "launch", "com.example.game"], root)
        assert code == ExitCode.OK
        assert "warning" in out.lower()
        assert "1.0.0" in out and "2.0.0" in out

    def test_no_warning_when_versions_agree(self, capsys, root: Path, monkeypatch) -> None:
        from lynxdroid.backend.waydroid import FakeBackend

        self._save_profile(root, "com.example.game", "1.0.0")
        self._use_fake_backend(root)
        monkeypatch.setattr(
            FakeBackend,
            "list_apps",
            lambda self: [{"package": "com.example.game", "version": "1.0.0"}],
        )
        code, out, _ = run(capsys, ["apps", "launch", "com.example.game"], root)
        assert code == ExitCode.OK
        assert "warning" not in out.lower()

    def test_no_warning_when_the_version_is_unknown(self, capsys, root: Path) -> None:
        """An unreadable version must never be reported as a mismatch."""
        self._save_profile(root, "com.example.game", "1.0.0")
        self._use_fake_backend(root)  # FakeBackend lists no apps
        code, out, _ = run(capsys, ["apps", "launch", "com.example.game"], root)
        assert code == ExitCode.OK
        assert "warning" not in out.lower()
