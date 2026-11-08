"""Application catalog: safe APK handling and package listing.

Stage 14 rules encoded here:

* only a file the user explicitly picked may be installed;
* the file's name, size and origin are shown before anything happens;
* symlinks are refused, size is bounded, and the extension must be ``.apk``;
* split APKs and AAB are reported as unsupported rather than half-implemented.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from pathlib import Path

from lynxdroid.core.errors import ExitCode, LynxError

MAX_APK_BYTES = 4 * 1024**3
APK_MAGIC = b"PK\x03\x04"


class ApkError(LynxError):
    """The chosen file cannot be installed; the message is user-facing."""

    code = "invalid-apk"
    exit_code = ExitCode.USAGE


@dataclass(frozen=True)
class ApkCandidate:
    """A file that passed every check and is ready for an explicit install."""

    path: Path
    size_bytes: int
    sha256: str
    package_hint: str

    @property
    def size_mb(self) -> float:
        return self.size_bytes / 1024**2

    def describe(self) -> str:
        return (
            f"{self.path.name} - {self.size_mb:.1f} MiB - sha256 {self.sha256[:16]}…\n"
            f"source: {self.path.parent}\n"
            "installing copies nothing outside the Android container"
        )


def inspect_apk(path: str | Path) -> ApkCandidate:
    """Validate an APK without executing or unpacking it."""
    apk = Path(path).expanduser()
    if apk.is_symlink():
        raise ApkError("refusing a symlink: copy the APK to a normal file first")
    if not apk.exists():
        raise ApkError(f"{apk} does not exist")
    if not apk.is_file():
        raise ApkError(f"{apk} is not a regular file")
    if apk.suffix.lower() != ".apk":
        suffix = apk.suffix.lower()
        if suffix in {".apks", ".xapk", ".aab"}:
            raise ApkError(
                f"{suffix} bundles are not supported: this build installs single APKs only"
            )
        raise ApkError(f"expected an .apk file, got '{apk.suffix or 'no extension'}'")
    size = apk.stat().st_size
    if size == 0:
        raise ApkError("file is empty")
    if size > MAX_APK_BYTES:
        raise ApkError(f"{size / 1024**3:.1f} GiB exceeds the 4 GiB limit")
    with apk.open("rb") as handle:
        header = handle.read(4)
    if header != APK_MAGIC:
        raise ApkError("not a ZIP/APK container (missing 'PK\\x03\\x04')")
    return ApkCandidate(
        path=apk,
        size_bytes=size,
        sha256=_sha256(apk),
        package_hint=_guess_package(apk),
    )


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _guess_package(path: Path) -> str:
    """Filename-based hint only. The real name comes from Android after install."""
    stem = path.stem
    cleaned = "".join(ch if ch.isalnum() else "_" for ch in stem).strip("_").lower()
    return cleaned or "unknown"


@dataclass(frozen=True)
class AppEntry:
    """One installed Android application."""

    package: str
    label: str
    running: bool = False
    has_profile: bool = False

    @property
    def vendor(self) -> str:
        parts = self.package.split(".")
        return parts[1] if len(parts) > 2 else (parts[0] if parts else "")


def merge_apps(
    raw: list[dict[str, str]],
    *,
    profiled_packages: set[str] | None = None,
) -> list[AppEntry]:
    """Normalise backend output into sorted entries, de-duplicated by package."""
    profiled = profiled_packages or set()
    seen: dict[str, AppEntry] = {}
    for item in raw:
        package = (item.get("package") or "").strip()
        if not package:
            continue
        label = (item.get("label") or "").strip() or package
        seen[package] = AppEntry(
            package=package,
            label=label,
            running=False,
            has_profile=package in profiled,
        )
    return sorted(seen.values(), key=lambda entry: (entry.label.lower(), entry.package))
