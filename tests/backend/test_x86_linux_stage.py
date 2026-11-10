"""Compile and exercise synthetic Linux/x86 staging without a VM."""

from __future__ import annotations

import platform
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


def test_synthetic_linux_header_and_transactional_staging(tmp_path: Path) -> None:
    compiler = shutil.which("cc")
    if platform.system() != "Linux" or compiler is None:
        pytest.skip("Linux C compiler required")
    binary = tmp_path / "x86-linux-stage-test"
    result = subprocess.run(
        [
            compiler,
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "runtime"),
            str(ROOT / "tests/backend/x86_linux_stage_test.c"),
            "-o",
            str(binary),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    result = subprocess.run([str(binary)], check=False, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
