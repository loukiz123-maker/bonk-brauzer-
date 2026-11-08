"""Subprocess execution without a shell.

``shell=True`` is banned project-wide: every external program receives an
explicit ``argv`` list, so package names, file paths and user input can never
be re-interpreted by ``/bin/sh`` (specification stage 14).

The wrapper also provides what the rest of the code needs to distinguish:

* the binary does not exist            -> ``available=False`` / FileNotFoundError
* it exists but we may not run it      -> ``PermissionDenied``
* it ran and failed                    -> ``RunResult(ok=False)``
* it ran too long                      -> ``RunResult(timed_out=True)``
"""

from __future__ import annotations

import contextlib
import os
import shlex
import shutil
import signal
import subprocess
import time
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path

DEFAULT_TIMEOUT = 15.0

#: Environment variables that never reach a child process. Waydroid and Android
#: tooling do not need them, and leaking them into logs/children is a risk.
_SCRUBBED_ENV_PREFIXES = ("LYNX_SECRET",)


@dataclass(frozen=True)
class RunResult:
    """Outcome of one command execution."""

    argv: tuple[str, ...]
    returncode: int
    stdout: str = ""
    stderr: str = ""
    duration_s: float = 0.0
    timed_out: bool = False
    error: str | None = None
    truncated: bool = False
    env_extras: Mapping[str, str] = field(default_factory=dict)

    @property
    def ok(self) -> bool:
        return self.error is None and not self.timed_out and self.returncode == 0

    @property
    def output(self) -> str:
        """stdout when the command succeeded, otherwise stderr or the error."""
        if self.error:
            return self.error
        return self.stdout if self.stdout.strip() else self.stderr

    def lines(self) -> list[str]:
        return list(self.stdout.splitlines())

    def describe(self) -> str:
        rendered = " ".join(shlex.quote(a) for a in self.argv)
        if self.error:
            return f"{rendered}: {self.error}"
        if self.timed_out:
            return f"{rendered}: timed out after {self.duration_s:.1f}s"
        return f"{rendered}: exit {self.returncode}"

    def as_dict(self) -> dict[str, object]:
        return {
            "argv": list(self.argv),
            "returncode": self.returncode,
            "ok": self.ok,
            "duration_s": round(self.duration_s, 3),
            "timed_out": self.timed_out,
            "error": self.error,
            "stdout": self.stdout,
            "stderr": self.stderr,
        }


def which(program: str) -> str | None:
    """Locate an executable, returning ``None`` when it is absent."""
    return shutil.which(program)


def have(program: str) -> bool:
    return which(program) is not None


def build_env(extras: Mapping[str, str] | None = None) -> dict[str, str]:
    """Child environment: inherited, minus secrets, plus explicit extras."""
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(_SCRUBBED_ENV_PREFIXES)
    }
    env.setdefault("LC_ALL", "C.UTF-8")
    if extras:
        for key, value in extras.items():
            if "\x00" in key or "\x00" in value:
                raise ValueError("environment entries must not contain NUL")
            env[key] = value
    return env


def run(
    argv: Sequence[str],
    *,
    timeout: float = DEFAULT_TIMEOUT,
    env: Mapping[str, str] | None = None,
    cwd: Path | None = None,
    stdin_data: str | None = None,
    max_output: int = 1_000_000,
    dry_run: bool = False,
) -> RunResult:
    """Run ``argv`` and never raise for external failures.

    Only programming errors (empty argv, NUL bytes) raise ``ValueError``.
    """
    args = _validate(argv)
    env_map = build_env(env)
    if dry_run:
        return RunResult(argv=args, returncode=0, env_extras=dict(env or {}), stdout="", error=None)

    started = time.monotonic()
    try:
        completed = subprocess.run(
            list(args),
            input=stdin_data,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            env=dict(env_map),
            cwd=str(cwd) if cwd else None,
            check=False,
            # Own process group: a hung child (or its children) can be killed
            # as a whole on timeout instead of leaking.
            start_new_session=True,
        )
    except FileNotFoundError:
        return RunResult(
            argv=args,
            returncode=127,
            error=f"{args[0]}: not found",
            duration_s=time.monotonic() - started,
        )
    except PermissionError as exc:
        return RunResult(
            argv=args,
            returncode=126,
            error=f"{args[0]}: not executable ({exc.strerror})",
            duration_s=time.monotonic() - started,
        )
    except subprocess.TimeoutExpired as exc:
        out = _as_text(exc.stdout)
        err = _as_text(exc.stderr)
        return RunResult(
            argv=args,
            returncode=124,
            stdout=out[:max_output],
            stderr=err[:max_output],
            timed_out=True,
            duration_s=time.monotonic() - started,
        )
    except OSError as exc:
        return RunResult(
            argv=args,
            returncode=126,
            error=f"{args[0]}: {exc}",
            duration_s=time.monotonic() - started,
        )

    stdout = completed.stdout or ""
    stderr = completed.stderr or ""
    truncated = len(stdout) > max_output or len(stderr) > max_output
    return RunResult(
        argv=args,
        returncode=completed.returncode,
        stdout=stdout[:max_output],
        stderr=stderr[:max_output],
        duration_s=time.monotonic() - started,
        truncated=truncated,
        env_extras=dict(env or {}),
    )


def spawn(
    argv: Sequence[str],
    *,
    env: Mapping[str, str] | None = None,
    stdout_path: Path | None = None,
) -> subprocess.Popen[bytes]:
    """Start a long-lived child (session, agent) with its own process group.

    Callers own the returned process and must terminate it; ``terminate_tree``
    does that safely.
    """
    args = _validate(argv)
    if stdout_path is None:
        return subprocess.Popen(
            list(args),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=build_env(env),
            start_new_session=True,
            close_fds=True,
        )
    # The file handle stays open in the child; the parent closes its own copy
    # so no descriptor (and no unflushed buffer) is left behind.
    with open(stdout_path, "ab") as out:  # noqa: PTH123
        return subprocess.Popen(
            list(args),
            stdin=subprocess.DEVNULL,
            stdout=out,
            stderr=subprocess.STDOUT,
            env=build_env(env),
            start_new_session=True,
            close_fds=True,
        )


def terminate_tree(process: subprocess.Popen[bytes], *, grace: float = 5.0) -> int | None:
    """Terminate a process group: SIGTERM, wait, then SIGKILL."""
    if process.poll() is not None:
        return process.returncode
    try:
        os.killpg(os.getpgid(process.pid), signal.SIGTERM)
    except ProcessLookupError:
        return process.poll()
    try:
        return process.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(os.getpgid(process.pid), signal.SIGKILL)
        try:
            return process.wait(timeout=grace)
        except subprocess.TimeoutExpired:  # pragma: no cover - stuck in D state
            return None


def _validate(argv: Sequence[str]) -> tuple[str, ...]:
    if not argv or not argv[0]:
        raise ValueError("argv must not be empty")
    args = tuple(str(part) for part in argv)
    for part in args:
        if "\x00" in part:
            raise ValueError("argv entries must not contain NUL bytes")
    return args


def _as_text(value: bytes | str | None) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", "replace")
    return value
