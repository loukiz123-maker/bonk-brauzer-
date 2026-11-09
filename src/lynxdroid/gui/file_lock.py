"""Non-blocking advisory locks for launcher-owned JSON files on Linux."""

from __future__ import annotations

import fcntl
import os
import stat
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path


@contextmanager
def exclusive_file_lock(path: Path) -> Iterator[None]:
    """Lock a persistent sidecar without following a symlink.

    All cooperating writers must use the same lock. Do not unlink the lock
    file after use: another process may already be holding its inode.
    """
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    lock_path = path.with_name(path.name + ".lock")
    flags = os.O_CREAT | os.O_RDWR | os.O_NONBLOCK | os.O_NOFOLLOW
    fd = os.open(lock_path, flags, 0o600)
    with os.fdopen(fd, "rb+") as lock:
        if not stat.S_ISREG(os.fstat(lock.fileno()).st_mode):
            raise OSError("Файл блокировки не является обычным файлом")
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield
