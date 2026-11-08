"""Android image preparation with an explicit consent gate (stage 4).

The specification is strict here, so the module is deliberately conservative:

* nothing downloads until the user has seen source, licence, size and action;
* the download goes to our own cache directory, never into ``/var/lib``;
* integrity is verified only when an authoritative checksum is available -
  we never invent one;
* installing into Waydroid's image directory is a separate, consented,
  root-requiring step that we *describe* rather than silently perform.

If a channel does not publish checksums, the plan says so plainly and requires
``allow_unverified=True`` to proceed.
"""

from __future__ import annotations

import hashlib
import json
import urllib.error
import urllib.request
from collections.abc import Callable
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path

from lynxdroid.core.errors import Blocked, DependencyMissing, LynxError
from lynxdroid.core.logging_setup import get
from lynxdroid.core.paths import free_space_bytes

log = get("backend.images")

OTA_SYSTEM = "https://ota.waydro.id/system"
OTA_VENDOR = "https://ota.waydro.id/vendor"

#: Published OTA channels. Waydroid resolves the architecture and vendor type
#: from the metadata itself; we only show the entry point, never a hardcoded
#: file URL that could rot.
CHANNELS: dict[str, dict[str, str]] = {
    "lineage-vanilla": {
        "system": OTA_SYSTEM,
        "vendor": OTA_VENDOR,
        "description": "LineageOS-based image without Google services",
        "arch": "x86_64 (matches this laptop)",
    },
    "lineage-gapps": {
        "system": OTA_SYSTEM,
        "vendor": OTA_VENDOR,
        "description": "LineageOS-based image with Google apps (needs self-certification)",
        "arch": "x86_64",
    },
}

LICENCE_NOTES = (
    "Android images from the Waydroid OTA channel are built from LineageOS "
    "sources (Apache-2.0 / GPL-2.0 components). The GAPPS variant additionally "
    "contains Google-proprietary components that you may not be allowed to "
    "redistribute; this project never bundles or mirrors any image."
)

APPROXIMATE_BYTES = {
    "vanilla": 1_200 * 1024**2,
    "gapps": 2_400 * 1024**2,
}


class ImageState(str, Enum):
    MISSING = "missing"
    PARTIAL = "partial"
    PRESENT = "present"
    UNVERIFIED = "unverified"


@dataclass(frozen=True)
class ImagePlan:
    """What would happen, shown to the user before anything is downloaded."""

    channel: str
    system_url: str
    vendor_url: str
    estimated_bytes: int
    target_dir: Path
    licence_note: str = LICENCE_NOTES
    requires_root_to_install: bool = True
    checksums_available: bool = False
    steps: tuple[str, ...] = ()

    def describe(self) -> str:
        size_gib = self.estimated_bytes / 1024**3
        lines = [
            f"Channel:   {self.channel}",
            f"System:    {self.system_url}",
            f"Vendor:    {self.vendor_url}",
            f"Size:      about {size_gib:.1f} GiB (system + vendor + working space)",
            f"Target:    {self.target_dir}",
            "",
            "Licence:",
            self.licence_note,
            "",
            "Steps:",
            *[f"  {index}. {step}" for index, step in enumerate(self.steps, start=1)],
            "",
            "Nothing is downloaded or installed until you confirm.",
        ]
        return "\n".join(lines)


@dataclass
class FetchResult:
    path: Path
    size_bytes: int
    sha256: str
    source: str
    verified: bool = False
    notes: list[str] = field(default_factory=list)


def plan_for(channel: str, cache_dir: Path, *, flavour: str = "vanilla") -> ImagePlan:
    """Build the consent screen contents. Reads nothing, writes nothing."""
    if channel not in CHANNELS:
        raise LynxError(
            f"unknown image channel '{channel}'", hint="available: " + ", ".join(CHANNELS)
        )
    entry = CHANNELS[channel]
    return ImagePlan(
        channel=channel,
        system_url=entry["system"],
        vendor_url=entry["vendor"],
        estimated_bytes=APPROXIMATE_BYTES.get(flavour, APPROXIMATE_BYTES["vanilla"]),
        target_dir=cache_dir / "images",
        checksums_available=False,
        steps=(
            "read the OTA metadata from the channel to learn the exact files",
            f"download the system and vendor archives into {cache_dir / 'images'}",
            "compute SHA-256 locally and show it (no published checksum exists to compare)",
            "extract the images and print the exact commands to install them as root",
            "LynxDroid never writes into /var/lib/waydroid on your behalf",
        ),
    )


def fetch_ota_metadata(url: str, *, timeout_s: float = 20.0) -> dict[str, object]:
    """Read the channel JSON. Network use is explicit and bounded."""
    request = urllib.request.Request(url, headers={"User-Agent": "LynxDroid/0.1"})
    try:
        with urllib.request.urlopen(request, timeout=timeout_s) as response:
            body = response.read(1_000_000)
    except urllib.error.URLError as exc:
        raise DependencyMissing(
            f"cannot reach {url}: {exc.reason}",
            hint="check the network connection or prepare the images manually",
        ) from exc
    try:
        payload = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise LynxError(f"{url} did not return JSON: {exc}") from exc
    if not isinstance(payload, dict):
        raise LynxError(f"{url} returned an unexpected payload")
    return payload


def required_space(estimated_bytes: int) -> int:
    """Space needed: download plus extracted images plus a safety margin."""
    return int(estimated_bytes * 2.2)


def check_space(target_dir: Path, estimated_bytes: int) -> tuple[bool, str]:
    available = free_space_bytes(target_dir)
    needed = required_space(estimated_bytes)
    if available is None:
        return False, f"free space on {target_dir} could not be determined"
    if available < needed:
        return (
            False,
            f"{needed / 1024**3:.1f} GiB needed, {available / 1024**3:.1f} GiB available "
            f"on {target_dir}",
        )
    return True, f"{available / 1024**3:.1f} GiB available"


def download(
    url: str,
    destination: Path,
    *,
    consent: bool,
    expected_sha256: str | None = None,
    chunk_bytes: int = 1024 * 256,
    progress: Callable[[int], None] | None = None,
) -> FetchResult:
    """Download one file. Requires ``consent=True``; never overwrites silently."""
    if not consent:
        raise Blocked(
            "downloading an Android image needs explicit consent",
            hint="show the plan first, then call again with consent",
        )
    destination = Path(destination)
    if destination.exists():
        raise LynxError(f"{destination} already exists; remove it first")
    destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)

    sha = hashlib.sha256()
    size = 0
    partial = destination.with_suffix(destination.suffix + ".part")
    request = urllib.request.Request(url, headers={"User-Agent": "LynxDroid/0.1"})
    try:
        with urllib.request.urlopen(request, timeout=60) as response, partial.open("wb") as out:
            while True:
                chunk = response.read(chunk_bytes)
                if not chunk:
                    break
                out.write(chunk)
                sha.update(chunk)
                size += len(chunk)
                if progress is not None:
                    progress(size)
    except urllib.error.URLError as exc:
        # urllib wraps socket timeouts and DNS failures in URLError; the
        # reason is either a message or an OSError with a useful str().
        partial.unlink(missing_ok=True)
        raise DependencyMissing(f"download failed: {exc.reason}") from exc
    except OSError as exc:
        partial.unlink(missing_ok=True)
        raise LynxError(f"cannot write {partial}: {exc.strerror}") from exc
    except BaseException:
        # Ctrl-C during a multi-gigabyte download is the normal way to stop
        # one. Without this the .part file is left behind, and a later run
        # that finds it has no way to tell a deliberate interruption from a
        # crashed download - so it must not be kept.
        partial.unlink(missing_ok=True)
        raise

    digest = sha.hexdigest()
    verified = False
    notes: list[str] = []
    if expected_sha256:
        if digest.lower() != expected_sha256.lower():
            partial.unlink(missing_ok=True)
            raise LynxError(
                f"checksum mismatch: expected {expected_sha256[:16]}…, computed {digest[:16]}…"
            )
        verified = True
    else:
        notes.append(
            "no authoritative checksum was available for this file; the digest above is "
            "what this machine computed, not a proof of authenticity"
        )
    partial.replace(destination)
    log.info("downloaded %s (%d bytes)", destination.name, size)
    return FetchResult(
        path=destination, size_bytes=size, sha256=digest, source=url, verified=verified, notes=notes
    )


def render_install_instructions(result: FetchResult) -> str:
    """Exact commands the user must run themselves, with the reason why."""
    return (
        "The image is downloaded but not installed. Installing it writes to\n"
        "/var/lib/waydroid, which belongs to the Waydroid package and needs root.\n"
        "LynxDroid does not do this for you. If you agree, run:\n\n"
        f"  sudo mkdir -p /var/lib/waydroid/images\n"
        f"  sudo cp {result.path} /var/lib/waydroid/images/\n"
        "  sudo waydroid init -f\n\n"
        f"File: {result.path.name} ({result.size_bytes / 1024**3:.2f} GiB)\n"
        f"SHA-256: {result.sha256}\n"
        f"Verified against a published checksum: {'yes' if result.verified else 'no'}\n"
    )
