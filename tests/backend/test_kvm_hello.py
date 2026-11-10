"""Compile and check consent for the standalone KVM experiment.

The real KVM execution is opt-in and requires the target host.
"""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
from pathlib import Path

import pytest

SOURCE = Path(__file__).resolve().parents[2] / "runtime" / "kvm_hello.c"


@pytest.fixture()
def binary(tmp_path: Path) -> Path:
    compiler = shutil.which("cc")
    if platform.system() != "Linux" or compiler is None:
        pytest.skip("Linux C compiler and KVM headers required")
    target = tmp_path / "kvm-hello"
    result = subprocess.run(
        [
            compiler,
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-o",
            str(target),
            str(SOURCE),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    return target


def test_experiment_requires_explicit_confirmation(binary: Path) -> None:
    result = subprocess.run([str(binary)], capture_output=True, text=True, check=False)
    assert result.returncode == 2
    assert "--confirm" in result.stderr


def test_unrecognized_argument_cannot_run_guest(binary: Path) -> None:
    result = subprocess.run([str(binary), "--invalid"], capture_output=True, text=True, check=False)
    assert result.returncode == 2
    assert "--confirm" in result.stderr


@pytest.mark.target
def test_real_kvm_hlt_only_when_explicitly_enabled(binary: Path) -> None:
    if os.environ.get("LYNX_KVM_SMOKE") != "1":
        pytest.skip("real KVM execution requires explicit LYNX_KVM_SMOKE=1 on the target host")
    if platform.machine().lower() not in ("x86_64", "amd64"):
        pytest.skip("the experimental guest is x86_64 only")
    result = subprocess.run(
        [str(binary), "--confirm"], capture_output=True, text=True, timeout=15, check=False
    )
    assert result.returncode == 0, result.stderr
    assert "Android was not started" in result.stdout


@pytest.mark.parametrize("file_name", ["kvm_io_test.c", "serial8250_test.c"])
def test_guest_io_validation_on_synthetic_kvm_exits(tmp_path: Path, file_name: str) -> None:
    compiler = shutil.which("cc")
    if platform.system() != "Linux" or compiler is None:
        pytest.skip("Linux C compiler and KVM headers required")
    harness = Path(__file__).with_name(file_name)
    target = tmp_path / "kvm-io-test"
    result = subprocess.run(
        [
            compiler,
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(SOURCE.parent),
            "-o",
            str(target),
            str(harness),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    result = subprocess.run([str(target)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stderr
