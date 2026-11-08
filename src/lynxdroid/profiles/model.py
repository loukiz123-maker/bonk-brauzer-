"""Layout-profile data model (stage 9).

Design decisions that the tests pin down:

* **Identity is the package name**, never the visible title. Two different
  games can share a title; a package name plus optional version is what
  Android actually reports.
* **Coordinates are normalised (0..1) in Android buffer space**, so a profile
  survives a change of buffer resolution. Rotation is applied by
  ``display.geometry`` at the moment of injection, not baked into the file.
* **Every profile carries a schema version** and a stable id, so migration is
  always possible and never silently guesses.
* **Elements own a ``pointer`` slot** (0 = "auto") which is allocated by the
  input engine; profiles only describe intent.

The model is immutable; edits go through ``Profile.with_*`` helpers so a
half-edited profile can never be saved.
"""

from __future__ import annotations

import hashlib
import re
import uuid
from collections.abc import Iterable
from dataclasses import dataclass, field, replace
from datetime import datetime, timezone
from enum import Enum
from typing import Any

from lynxdroid.display.resolution import MAX_EDGE, MIN_EDGE, Orientation
from lynxdroid.version import SCHEMA_VERSIONS

PROFILE_SCHEMA = SCHEMA_VERSIONS["profile"]

_PACKAGE_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+$")
_KEY_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_\-+.,/ ]{0,31}$")
_ID_RE = re.compile(r"^[a-z0-9][a-z0-9\-]{0,63}$")
MIN_TOUCH_RADIUS = 0.005
MAX_TOUCH_RADIUS = 0.25


class ElementType(str, Enum):
    TAP = "tap"
    HOLD = "hold"
    DPAD = "dpad"
    SWIPE = "swipe"
    MOUSE = "mouse"


class ProfileError(ValueError):
    """A profile violates the schema. Message is shown to the user verbatim."""

    def __init__(self, message: str, *, path: str = "") -> None:
        self.path = path
        super().__init__(f"{path}: {message}" if path else message)


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def isoformat(moment: datetime) -> str:
    return moment.astimezone(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def parse_timestamp(text: str) -> datetime:
    cleaned = text.strip()
    if cleaned.endswith("Z"):
        cleaned = cleaned[:-1] + "+00:00"
    moment = datetime.fromisoformat(cleaned)
    if moment.tzinfo is None:
        raise ProfileError("timestamp must be timezone aware", path="updated_at")
    return moment.astimezone(timezone.utc)


# --------------------------------------------------------------------------
# Elements
# --------------------------------------------------------------------------


@dataclass(frozen=True, slots=True)
class Target:
    """What the profile is attached to."""

    package: str
    package_version: str | None = None
    instance: str = "default"
    #: Display-only. Never used for matching (stage 9: no title-only binding).
    title_hint: str = ""

    def validate(self) -> None:
        if not _PACKAGE_RE.match(self.package or ""):
            raise ProfileError(
                f"'{self.package}' is not a valid Android package name",
                path="target.package",
            )
        if self.package_version and not re.fullmatch(
            r"[0-9][0-9A-Za-z._\-]{0,31}", self.package_version
        ):
            raise ProfileError("unsupported package_version format", path="target.package_version")
        if not _ID_RE.match(self.instance):
            raise ProfileError("instance must be a short lower-case id", path="target.instance")

    def matches(self, package: str, version: str | None = None) -> bool:
        """Whether this target can be applied to that package.

        A different app version does **not** disqualify the profile: the user
        keeps their layout and is warned instead (see ``version_mismatch``).
        Only the package identity decides applicability, which is why this is
        a plain comparison and not a scoring function.
        """
        return package == self.package

    def version_mismatch(self, version: str | None) -> bool:
        """True when the app was updated since the profile was authored."""
        return bool(self.package_version and version and self.package_version != version)


@dataclass(frozen=True, slots=True)
class ReferenceResolution:
    """The buffer the author placed the keys on. Used for warnings only."""

    width: int
    height: int
    dpi: int = 240

    @property
    def orientation(self) -> Orientation:
        return Orientation.of(self.width, self.height)

    def validate(self) -> None:
        for name, value in (("width", self.width), ("height", self.height)):
            if not MIN_EDGE <= value <= MAX_EDGE:
                raise ProfileError(
                    f"{name}={value} outside {MIN_EDGE}..{MAX_EDGE}",
                    path=f"reference.{name}",
                )
        if not 72 <= self.dpi <= 960:
            raise ProfileError("dpi outside 72..960", path="reference.dpi")

    @property
    def relayout_hint(self) -> str:
        """Warning shown when the reference no longer matches the live buffer."""
        return (
            "the profile was authored for "
            f"{self.width}x{self.height} ({self.orientation.value}); "
            "if the game's control layout changed you must re-check key positions"
        )


@dataclass(frozen=True, slots=True)
class Element:
    """One on-screen control.

    ``x``/``y`` are the anchor in normalised Android buffer coordinates.
    Additional geometry is per type and validated in ``validate``.
    """

    id: str
    type: ElementType
    label: str
    x: float
    y: float
    #: Pointer slot this element always uses, or 0 for automatic allocation.
    pointer: int = 0
    #: tap/hold/mouse: touch radius as a fraction of the shorter buffer edge.
    radius: float = 0.035
    #: hold: seconds before the press is treated as held (0 = immediately).
    hold_delay_s: float = 0.0
    #: tap: physical key names (profile keys are resolved by the input engine).
    keys: tuple[str, ...] = ()
    # --- dpad -----------------------------------------------------------
    #: dpad: four directional key names, clockwise from up.
    dpad_keys: tuple[str, str, str, str] = ("", "", "", "")
    #: dpad: travel radius in normalised units of the shorter edge.
    dpad_radius: float = 0.09
    #: dpad: dead zone as a fraction of the radius (prevents jitter at centre).
    dead_zone: float = 0.15
    # --- swipe ----------------------------------------------------------
    #: swipe: end point in normalised coordinates.
    swipe_to: tuple[float, float] = (0.0, 0.0)
    swipe_duration_ms: int = 250
    # --- mouse ----------------------------------------------------------
    #: mouse: sensitivity multiplier for relative (aim) mode.
    sensitivity: float = 1.0
    #: mouse: key that exits mouse mode.
    exit_key: str = "Escape"

    def validate(self) -> None:
        path = f"elements[{self.id}]"
        if not _ID_RE.match(self.id):
            raise ProfileError(
                "id must be lower-case letters, digits and dashes", path="elements[].id"
            )
        if not 0.0 <= self.x <= 1.0 or not 0.0 <= self.y <= 1.0:
            raise ProfileError(f"position ({self.x:.3f}, {self.y:.3f}) outside 0..1", path=path)
        if not self.label.strip():
            raise ProfileError("label must not be empty", path=f"{path}.label")
        if len(self.label) > 24:
            raise ProfileError("label longer than 24 characters", path=f"{path}.label")
        if self.pointer < 0 or self.pointer > 9:
            raise ProfileError("pointer slot must be 0..9 (0 = automatic)", path=f"{path}.pointer")
        if not MIN_TOUCH_RADIUS <= self.radius <= MAX_TOUCH_RADIUS:
            raise ProfileError(
                f"radius {self.radius} outside {MIN_TOUCH_RADIUS}..{MAX_TOUCH_RADIUS}",
                path=f"{path}.radius",
            )
        if not 0.0 <= self.hold_delay_s <= 5.0:
            raise ProfileError("hold_delay_s must be 0..5", path=f"{path}.hold_delay_s")

        if self.type in (ElementType.TAP, ElementType.HOLD):
            if not self.keys:
                raise ProfileError("needs at least one key", path=f"{path}.keys")
            _check_keys(self.keys, f"{path}.keys")
        if self.type is ElementType.DPAD:
            if not all(self.dpad_keys) or not any(self.dpad_keys):
                raise ProfileError("needs four directional keys", path=f"{path}.dpad_keys")
            _check_keys(self.dpad_keys, f"{path}.dpad_keys")
            if len(set(self.dpad_keys)) != 4:
                raise ProfileError("directional keys must be distinct", path=f"{path}.dpad_keys")
            if not 0.01 <= self.dpad_radius <= 0.5:
                raise ProfileError("dpad_radius must be 0.01..0.5", path=f"{path}.dpad_radius")
            if not 0.0 <= self.dead_zone < 1.0:
                raise ProfileError("dead_zone must be 0..1", path=f"{path}.dead_zone")
            if self.x - self.dpad_radius < -0.001 or self.x + self.dpad_radius > 1.001:
                raise ProfileError("dpad extends outside the screen horizontally", path=path)
            if self.y - self.dpad_radius < -0.001 or self.y + self.dpad_radius > 1.001:
                raise ProfileError("dpad extends outside the screen vertically", path=path)
        if self.type is ElementType.SWIPE:
            tx, ty = self.swipe_to
            if not (0.0 <= tx <= 1.0 and 0.0 <= ty <= 1.0):
                raise ProfileError("swipe end point outside 0..1", path=f"{path}.swipe_to")
            if abs(tx - self.x) < 1e-6 and abs(ty - self.y) < 1e-6:
                raise ProfileError("swipe start and end are identical", path=f"{path}.swipe_to")
            if not 30 <= self.swipe_duration_ms <= 3000:
                raise ProfileError(
                    "swipe_duration_ms must be 30..3000", path=f"{path}.swipe_duration_ms"
                )
        if self.type is ElementType.MOUSE:
            if not 0.1 <= self.sensitivity <= 10.0:
                raise ProfileError("sensitivity must be 0.1..10", path=f"{path}.sensitivity")
            if not self.exit_key:
                raise ProfileError("exit_key must not be empty", path=f"{path}.exit_key")

    def as_pointer_displacement(self) -> tuple[float, float]:
        """D-pad stick offset for the centre in normalised units (0, 0)."""
        return (0.0, 0.0)


def _check_keys(keys: Iterable[str], path: str) -> None:
    for key in keys:
        if not _KEY_NAME_RE.match(key):
            raise ProfileError(f"invalid key name '{key}'", path=path)


# --------------------------------------------------------------------------
# Profile
# --------------------------------------------------------------------------


#: Default reference resolution, defined once so a dataclass field cannot
#: accidentally share one mutable default (it is frozen, this is belt and braces).
DEFAULT_REFERENCE = ReferenceResolution(1920, 1080, 320)


@dataclass(frozen=True, slots=True)
class Profile:
    id: str
    name: str
    target: Target
    elements: tuple[Element, ...] = ()
    reference: ReferenceResolution = DEFAULT_REFERENCE
    schema: int = PROFILE_SCHEMA
    created_at: datetime = field(default_factory=utc_now)
    updated_at: datetime = field(default_factory=utc_now)
    notes: str = ""

    # -- construction ----------------------------------------------------
    @staticmethod
    def new(
        name: str,
        target: Target,
        *,
        reference: ReferenceResolution = DEFAULT_REFERENCE,
        elements: Iterable[Element] = (),
    ) -> Profile:
        profile = Profile(
            id=uuid.uuid4().hex[:16],
            name=_clean_name(name),
            target=target,
            elements=tuple(elements),
            reference=reference,
        )
        profile.validate()
        return profile

    # -- validation ------------------------------------------------------
    def validate(self) -> None:
        if not self.name.strip():
            raise ProfileError("name must not be empty", path="name")
        if len(self.name) > 64:
            raise ProfileError("name longer than 64 characters", path="name")
        if self.schema != PROFILE_SCHEMA:
            raise ProfileError(
                f"schema {self.schema} is not the current version {PROFILE_SCHEMA}",
                path="schema",
            )
        self.target.validate()
        self.reference.validate()
        seen: set[str] = set()
        for element in self.elements:
            element.validate()
            if element.id in seen:
                raise ProfileError(f"duplicate element id '{element.id}'", path="elements")
            seen.add(element.id)
        conflicts = key_conflicts(self.elements)
        if conflicts:
            raise ProfileError(
                "key assigned to more than one element: " + ", ".join(sorted(conflicts)),
                path="elements",
            )

    # -- editing ---------------------------------------------------------
    def with_elements(self, elements: Iterable[Element]) -> Profile:
        updated = replace(self, elements=tuple(elements), updated_at=utc_now())
        updated.validate()
        return updated

    def with_name(self, name: str) -> Profile:
        updated = replace(self, name=_clean_name(name), updated_at=utc_now())
        updated.validate()
        return updated

    def with_reference(self, reference: ReferenceResolution) -> Profile:
        updated = replace(self, reference=reference, updated_at=utc_now())
        updated.validate()
        return updated

    def with_target(self, target: Target) -> Profile:
        updated = replace(self, target=target, updated_at=utc_now())
        updated.validate()
        return updated

    def duplicate(self, new_name: str | None = None) -> Profile:
        """Copy with fresh identity so both can exist side by side."""
        return replace(
            self,
            id=uuid.uuid4().hex[:16],
            name=_clean_name(new_name or f"{self.name} (copy)"),
            created_at=utc_now(),
            updated_at=utc_now(),
        )

    # -- queries ---------------------------------------------------------
    def element(self, element_id: str) -> Element | None:
        return next((e for e in self.elements if e.id == element_id), None)

    def add_element(self, element: Element) -> Profile:
        return self.with_elements((*self.elements, element))

    def remove_element(self, element_id: str) -> Profile:
        return self.with_elements(e for e in self.elements if e.id != element_id)

    def replace_element(self, element: Element) -> Profile:
        if not any(e.id == element.id for e in self.elements):
            raise ProfileError(f"no element with id '{element.id}'", path="elements")
        return self.with_elements(element if e.id == element.id else e for e in self.elements)

    def move_element(self, element_id: str, x: float, y: float, *, clamp: bool = True) -> Profile:
        """Move an element (drag-and-drop), keeping it inside the screen.

        The d-pad is the awkward case: its *centre* must stay far enough from
        the edges for the whole stick to be reachable, otherwise a drag would
        produce a profile that cannot be saved. With ``clamp`` the position is
        pulled back to the nearest legal point instead of failing mid-gesture;
        without it, an out-of-range drag raises.
        """
        existing = self.element(element_id)
        if existing is None:
            raise ProfileError(f"no element with id '{element_id}'", path="elements")

        min_x, max_x, min_y, max_y = 0.0, 1.0, 0.0, 1.0
        if existing.type is ElementType.DPAD:
            radius = existing.dpad_radius
            min_x, max_x = radius, 1.0 - radius
            min_y, max_y = radius, 1.0 - radius
            if min_x > max_x or min_y > max_y:  # pragma: no cover - radius is capped at 0.5
                raise ProfileError("the stick is too large for this orientation", path="elements")

        if clamp:
            x = min(max(x, min_x), max_x)
            y = min(max(y, min_y), max_y)

        moved = replace(existing, x=x, y=y)
        moved.validate()
        return self.replace_element(moved)

    def checksum(self) -> str:
        """Stable digest of the semantic content (for change detection/UX)."""
        payload = "|".join(
            f"{e.id}:{e.type.value}:{e.x:.6f}:{e.y:.6f}:{','.join(e.keys)}"
            for e in sorted(self.elements, key=lambda item: item.id)
        )
        return hashlib.sha256(payload.encode("utf-8")).hexdigest()[:16]

    # -- serialisation ---------------------------------------------------
    def to_dict(self) -> dict[str, Any]:
        return {
            "schema": self.schema,
            "id": self.id,
            "name": self.name,
            "notes": self.notes,
            "created_at": isoformat(self.created_at),
            "updated_at": isoformat(self.updated_at),
            "target": {
                "package": self.target.package,
                "package_version": self.target.package_version,
                "instance": self.target.instance,
                "title_hint": self.target.title_hint,
            },
            "reference": {
                "width": self.reference.width,
                "height": self.reference.height,
                "dpi": self.reference.dpi,
            },
            "elements": [_element_to_dict(element) for element in self.elements],
        }


def key_conflicts(elements: Iterable[Element]) -> set[str]:
    """Physical keys bound to more than one element (stage 9 requires a warning)."""
    owner: dict[str, str] = {}
    conflicts: set[str] = set()
    for element in elements:
        keys: list[str] = []
        if element.type in (ElementType.TAP, ElementType.HOLD, ElementType.MOUSE):
            keys = list(element.keys)
        elif element.type is ElementType.DPAD:
            keys = list(element.dpad_keys)
        for key in keys:
            if not key:
                continue
            if key in owner and owner[key] != element.id:
                conflicts.add(key)
            owner[key] = element.id
    return conflicts


def _clean_name(name: str) -> str:
    cleaned = " ".join(name.split())
    if not cleaned:
        raise ProfileError("name must not be empty", path="name")
    return cleaned


def _element_to_dict(element: Element) -> dict[str, Any]:
    data: dict[str, Any] = {
        "id": element.id,
        "type": element.type.value,
        "label": element.label,
        "x": round(element.x, 6),
        "y": round(element.y, 6),
        "pointer": element.pointer,
        "radius": round(element.radius, 6),
    }
    if element.type in (ElementType.TAP, ElementType.HOLD, ElementType.MOUSE):
        data["keys"] = list(element.keys)
    if element.type is ElementType.HOLD:
        data["hold_delay_s"] = element.hold_delay_s
    if element.type is ElementType.DPAD:
        data["dpad_keys"] = list(element.dpad_keys)
        data["dpad_radius"] = round(element.dpad_radius, 6)
        data["dead_zone"] = round(element.dead_zone, 6)
    if element.type is ElementType.SWIPE:
        data["swipe_to"] = [round(element.swipe_to[0], 6), round(element.swipe_to[1], 6)]
        data["swipe_duration_ms"] = element.swipe_duration_ms
    if element.type is ElementType.MOUSE:
        data["sensitivity"] = round(element.sensitivity, 6)
        data["exit_key"] = element.exit_key
    return data
