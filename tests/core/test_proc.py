"""Process execution: no shell, timeouts, missing binaries, environment hygiene."""

from __future__ import annotations

import sys
import time

import pytest

from lynxdroid.core import proc


class TestRun:
    def test_successful_command(self) -> None:
        result = proc.run([sys.executable, "-c", "print('hello')"])
        assert result.ok
        assert result.stdout.strip() == "hello"
        assert result.returncode == 0

    def test_failing_command_is_not_an_exception(self) -> None:
        result = proc.run([sys.executable, "-c", "import sys; sys.exit(3)"])
        assert not result.ok
        assert result.returncode == 3

    def test_missing_binary_is_reported(self) -> None:
        result = proc.run(["definitely-not-a-real-binary-xyz"])
        assert not result.ok
        assert result.returncode == 127
        assert "not found" in (result.error or "")

    def test_timeout_is_reported(self) -> None:
        result = proc.run([sys.executable, "-c", "import time; time.sleep(5)"], timeout=0.3)
        assert result.timed_out
        assert result.returncode == 124

    def test_empty_argv_rejected(self) -> None:
        with pytest.raises(ValueError):
            proc.run([])

    def test_nul_bytes_rejected(self) -> None:
        with pytest.raises(ValueError):
            proc.run(["echo", "a\x00b"])

    def test_output_is_capped(self) -> None:
        result = proc.run([sys.executable, "-c", "print('x' * 5000)"], max_output=100)
        assert result.truncated
        assert len(result.stdout) == 100

    def test_shell_metacharacters_are_not_interpreted(self) -> None:
        payload = "hello; echo INJECTED"
        result = proc.run([sys.executable, "-c", "import sys; print(sys.argv[1])", payload])
        assert result.stdout.strip() == payload
        assert "INJECTED" not in result.stdout.replace(payload, "")

    def test_environment_is_inherited_and_extended(self, monkeypatch) -> None:
        monkeypatch.setenv("LYNX_TEST_INHERITED", "yes")
        result = proc.run(
            [
                sys.executable,
                "-c",
                "import os; print(os.environ.get('LYNX_TEST_INHERITED'),"
                " os.environ.get('LYNX_TEST_EXTRA'))",
            ],
            env={"LYNX_TEST_EXTRA": "added"},
        )
        assert result.stdout.strip() == "yes added"

    def test_secret_environment_is_scrubbed(self, monkeypatch) -> None:
        monkeypatch.setenv("LYNX_SECRET_TOKEN", "s3cr3t")
        result = proc.run(
            [sys.executable, "-c", "import os; print(os.environ.get('LYNX_SECRET_TOKEN'))"]
        )
        assert result.stdout.strip() == "None"

    def test_dry_run_does_not_execute(self) -> None:
        marker = time.time()
        result = proc.run([sys.executable, "-c", "import time; time.sleep(2)"], dry_run=True)
        assert time.time() - marker < 1.0
        assert result.ok

    def test_describe_is_human_readable(self) -> None:
        result = proc.run([sys.executable, "-c", "raise SystemExit(7)"])
        assert "exit 7" in result.describe()

    def test_stdin_data_is_passed(self) -> None:
        result = proc.run(
            [sys.executable, "-c", "import sys; print(sys.stdin.read().upper())"], stdin_data="abc"
        )
        assert result.stdout.strip() == "ABC"

    def test_as_dict_is_jsonable(self) -> None:
        payload = proc.run([sys.executable, "-c", "print(1)"]).as_dict()
        assert payload["ok"] is True
        assert payload["argv"][0] == sys.executable


class TestSpawnTerminate:
    def test_spawn_and_terminate(self) -> None:
        child = proc.spawn([sys.executable, "-c", "import time; time.sleep(30)"])
        assert child.poll() is None
        code = proc.terminate_tree(child)
        assert code is not None
        assert child.poll() is not None

    def test_terminate_already_dead_process(self) -> None:
        child = proc.spawn([sys.executable, "-c", "pass"])
        child.wait(timeout=10)
        assert proc.terminate_tree(child) is not None

    def test_spawn_writes_output_to_file(self, tmp_path) -> None:
        log = tmp_path / "child.log"
        child = proc.spawn([sys.executable, "-c", "print('child ran')"], stdout_path=log)
        child.wait(timeout=10)
        for _ in range(50):
            if log.read_text().strip():
                break
            time.sleep(0.05)
        assert "child ran" in log.read_text()


class TestWhich:
    def test_finds_python(self) -> None:
        assert proc.which(sys.executable) is not None or proc.have("python3")

    def test_missing_tool(self) -> None:
        assert proc.which("lynxdroid-definitely-missing") is None
        assert not proc.have("lynxdroid-definitely-missing")
