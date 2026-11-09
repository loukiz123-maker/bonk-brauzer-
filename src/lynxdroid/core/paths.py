"""XDG Base Directory handling and safe filesystem helpers.

Rules enforced here (stage 14 of the specification):

* configuration -> ``$XDG_CONFIG_HOME/lynxdroid``
* data          -> ``$XDG_DATA_HOME/lynxdroid``
* cache         -> ``$XDG_CACHE_HOME/lynxdroid``
* state (logs)  -> ``$XDG_STATE_HOME/lynxdroid``

Nothing is ever written outside those directories by the GUI/CLI user
process, and deletions are refused unless the path is demonstrably inside the
directory we own (symlinks resolved).
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

from lynxdroid.core.errors import PermissionDenied

APP_DIR_NAME = "lynxdroid"


def _xdg(env_var: str, default: str) -> Path:
    raw = os.environ.get(env_var, "").strip()
    if raw and Path(raw).is_absolute():
        return Path(raw)
    return Path.home() / default


def config_home() -> Path:
    return _xdg("XDG_CONFIG_HOME", ".config") / APP_DIR_NAME


def data_home() -> Path:
    return _xdg("XDG_DATA_HOME", ".local/share") / APP_DIR_NAME


def cache_home() -> Path:
    return _xdg("XDG_CACHE_HOME", ".cache") / APP_DIR_NAME


def state_home() -> Path:
    return _xdg("XDG_STATE_HOME", ".local/state") / APP_DIR_NAME


@dataclass(frozen=True)
class AppPaths:
    """Resolved, overridable view of every directory the app uses.

    Tests (and portable installs) inject a temporary root instead of touching
    the real home directory.
    """

    config: Path
    data: Path
    cache: Path
    state: Path

    @classmethod
    def resolve(cls, root: Path | None = None) -> AppPaths:
        """Return the live XDG layout, or a sandboxed layout under ``root``."""
        if root is not None:
            base = Path(root).expanduser()
            return cls(
                config=base / "config",
                data=base / "data",
                cache=base / "cache",
                state=base / "state",
            )
        return cls(
            config=config_home(),
            data=data_home(),
            cache=cache_home(),
            state=state_home(),
        )

    # --- derived locations -------------------------------------------------
    @property
    def settings_file(self) -> Path:
        return self.config / "settings.json"

    @property
    def profiles_dir(self) -> Path:
        return self.data / "profiles"

    @property
    def backups_dir(self) -> Path:
        return self.data / "backups"

    @property
    def log_file(self) -> Path:
        return self.state / "lynxdroid.log"

    @property
    def reports_dir(self) -> Path:
        return self.state / "reports"

    def ensure(self) -> None:
        """Create the directory tree with user-only permissions."""
        for directory in (
            self.config,
            self.data,
            self.cache,
            self.state,
            self.profiles_dir,
            self.backups_dir,
            self.reports_dir,
        ):
            directory.mkdir(parents=True, exist_ok=True, mode=0o700)


def is_within(path: Path, root: Path) -> bool:
    """True if ``path`` (after resolving symlinks) stays inside ``root``.

    Both sides are resolved with ``strict=False`` so the check also works for
    files that do not exist yet. This is the guard used for every delete.
    """
    try:
        resolved_path = Path(path).expanduser().resolve(strict=False)
        resolved_root = Path(root).expanduser().resolve(strict=False)
    except OSError:  # pragma: no cover - exotic filesystems
        return False
    if resolved_path == resolved_root:
        return False
    return resolved_root in resolved_path.parents


def safe_remove(path: Path, root: Path) -> None:
    """Delete one file under ``root`` after proving it cannot escape.

    Refuses: paths outside ``root``, the root itself, and symlinks that point
    outside ``root``. Never recurses: callers pass explicit files.
    """
    candidate = Path(path)
    if candidate.is_symlink():
        # Deleting a symlink only removes the link, but a link whose target
        # escapes the root is a strong signal something is wrong.
        target = candidate.resolve(strict=False)
        if not is_within(target, root):
            raise PermissionDenied(
                f"refusing to delete symlink {candidate} pointing outside {root}",
                hint="remove it manually after checking what it points to",
            )
        candidate.unlink()
        return
    if not is_within(candidate, root):
        raise PermissionDenied(f"refusing to delete {candidate}: outside {root}")
    candidate.unlink(missing_ok=True)


def free_space_bytes(path: Path) -> int | None:
    """Free bytes on the filesystem holding ``path`` (walking up to a parent)."""
    probe = Path(path).expanduser()
    while True:
        try:
            stats = os.statvfs(probe)
        except OSError:
            if probe.parent == probe:
                return None
            probe = probe.parent
            continue
        return stats.f_bavail * stats.f_frsize
