"""The install script is part of the deliverable, so it is checked too.

Nothing here runs the script against a real host: it installs packages and
takes minutes. These tests catch the failure modes that actually happen to a
shell script in a repository - it stops parsing after an edit, it grows a
``sudo`` that the project forbids, or it drifts away from the commands the
documentation tells people to run.
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parent.parent / "scripts" / "install.sh"


class TestScriptExists:
    def test_it_is_executable(self) -> None:
        assert SCRIPT.is_file()
        # git tracks the executable bit; a bare "bash install.sh" would still
        # work, but the README tells people to run it directly.
        assert SCRIPT.stat().st_mode & 0o111, "scripts/install.sh is not executable"

    def test_it_has_a_shebang_and_strict_mode(self) -> None:
        lines = SCRIPT.read_text(encoding="utf-8").splitlines()
        assert lines[0].startswith("#!"), "no shebang"
        # strict mode must come before the first command that could fail,
        # not merely exist somewhere in the file
        body = "\n".join(lines[:40])
        assert "set -euo pipefail" in body, "the script must fail loudly"


class TestScriptParses:
    @pytest.mark.skipif(shutil.which("bash") is None, reason="bash is not available")
    def test_bash_accepts_it(self) -> None:
        result = subprocess.run(
            ["bash", "-n", str(SCRIPT)], capture_output=True, text=True, check=False
        )
        assert result.returncode == 0, result.stderr


class TestScriptRespectsTheProjectsRules:
    def test_it_never_uses_sudo(self) -> None:
        """The project's rule: system changes need explicit consent, and a
        script cannot give it. It may *print* a sudo command for the user."""
        text = SCRIPT.read_text(encoding="utf-8")
        executed = [
            line
            for line in text.splitlines()
            if "sudo" in line and not line.lstrip().startswith("#") and "note" not in line
        ]
        assert executed == [], f"sudo appears in executable lines: {executed}"

    def test_it_does_not_touch_the_filesystem_outside_the_venv(self) -> None:
        text = SCRIPT.read_text(encoding="utf-8")
        for forbidden in ("/usr/", "/etc/", "/var/lib"):
            writes = [
                line
                for line in text.splitlines()
                if forbidden in line and not line.lstrip().startswith("#")
            ]
            # Mentions inside messages are fine; a redirect or install into
            # those paths is not.
            assert not any((">" in line or "install" in line) for line in writes), forbidden

    def test_it_does_not_require_waydroid(self) -> None:
        text = SCRIPT.read_text(encoding="utf-8")
        assert "waydroid" not in text.lower()
        assert "lynxdroid doctor" not in text


class TestAgreesWithTheReadme:
    def test_the_install_target_matches_the_documented_one(self) -> None:
        """If the README and the script disagree, one of them is wrong and
        the user finds out the hard way."""
        readme = (SCRIPT.parent.parent / "README.md").read_text(encoding="utf-8")
        assert "pip install -e '.[gui,dev]'" in readme, (
            "README no longer documents the GUI install command"
        )
        script = SCRIPT.read_text(encoding="utf-8")
        assert "install -e '.[gui,dev]'" in script, (
            "the script installs something other than what the README documents"
        )
