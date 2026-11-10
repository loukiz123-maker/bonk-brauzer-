"""Compile and exercise the synthetic 8254 subset without a VM."""

from __future__ import annotations

import platform
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


def test_timer_ticks_on_synthetic_elapsed_time(tmp_path: Path) -> None:
    compiler = shutil.which("cc")
    if platform.system() != "Linux" or compiler is None:
        pytest.skip("Linux C compiler required")
    binary = tmp_path / "i8254-test"
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
            str(ROOT / "tests/backend/i8254_test.c"),
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
