"""Profile (de)serialisation with a strict, forward-versioned reader.

A reader never guesses: unknown element types, out-of-range numbers and
missing keys produce ``ProfileError`` with the JSON path of the offending
field, which the GUI shows verbatim.
"""

from __future__ import annotations

from typing import Any

from lynxdroid.profiles.model import (
    PROFILE_SCHEMA,
    Element,
    ElementType,
    Profile,
    ProfileError,
    ReferenceResolution,
    Target,
    parse_timestamp,
    utc_now,
)

_REQUIRED_TOP = ("schema", "id", "name", "target", "elements")

_DEFAULTS: dict[str, dict[str, Any]] = {
    "tap": {"radius": 0.035},
    "hold": {"radius": 0.04, "hold_delay_s": 0.15},
    "dpad": {"radius": 0.05, "dpad_radius": 0.09, "dead_zone": 0.15},
    "swipe": {"radius": 0.02, "swipe_duration_ms": 250},
    "mouse": {"radius": 0.03, "sensitivity": 1.0, "exit_key": "Escape"},
}


def profile_to_dict(profile: Profile) -> dict[str, Any]:
    return profile.to_dict()


def profile_from_dict(data: dict[str, Any]) -> Profile:
    """Parse and validate a stored profile. Raises ``ProfileError``."""
    if not isinstance(data, dict):
        raise ProfileError("profile must be a JSON object", path="")

    schema = _require_int(data, "schema", "schema")
    if schema > PROFILE_SCHEMA:
        raise ProfileError(
            f"profile schema {schema} was written by a newer LynxDroid; "
            f"this build understands up to {PROFILE_SCHEMA}",
            path="schema",
        )
    if schema < PROFILE_SCHEMA:
        data = migrate(data, schema)

    for key in _REQUIRED_TOP:
        if key not in data:
            raise ProfileError("missing required field", path=key)

    target_raw = data.get("target")
    if not isinstance(target_raw, dict):
        raise ProfileError("must be an object", path="target")
    target = Target(
        package=_require_str(target_raw, "package", "target.package"),
        package_version=_optional_str(target_raw, "package_version", "target.package_version"),
        instance=_require_str(target_raw, "instance", "target.instance", default="default"),
        title_hint=_optional_str(target_raw, "title_hint", "target.title_hint") or "",
    )

    reference_raw = data.get("reference") or {"width": 1920, "height": 1080, "dpi": 320}
    if not isinstance(reference_raw, dict):
        raise ProfileError("must be an object", path="reference")
    reference = ReferenceResolution(
        width=_require_int(reference_raw, "width", "reference.width"),
        height=_require_int(reference_raw, "height", "reference.height"),
        dpi=_require_int(reference_raw, "dpi", "reference.dpi", default=240),
    )

    elements_raw = data.get("elements")
    if not isinstance(elements_raw, list):
        raise ProfileError("must be a list", path="elements")
    elements = tuple(
        _element_from_dict(item, f"elements[{index}]") for index, item in enumerate(elements_raw)
    )

    created = _timestamp(data, "created_at", utc_now())
    updated = _timestamp(data, "updated_at", created)

    profile = Profile(
        id=_require_str(data, "id", "id"),
        name=_require_str(data, "name", "name"),
        target=target,
        elements=elements,
        reference=reference,
        schema=PROFILE_SCHEMA,
        created_at=created,
        updated_at=updated,
        notes=_optional_str(data, "notes", "notes") or "",
    )
    profile.validate()
    return profile


def migrate(data: dict[str, Any], from_schema: int) -> dict[str, Any]:
    """Bring an older profile up to the current schema, in memory.

    There is exactly one released version so far; the function exists so the
    upgrade path is explicit and testable rather than implied.
    """
    migrated = dict(data)
    schema = from_schema
    steps: list[str] = []

    # Example of the intended shape (no released v0 in the wild, kept for the
    # pattern the tests exercise):
    # if schema == 0:
    #     migrated["reference"] = migrated.pop("resolution", {...})
    #     schema = 1
    #     steps.append("0->1: rename resolution to reference")

    if schema < PROFILE_SCHEMA:
        raise ProfileError(
            f"no migration path from schema {from_schema} to {PROFILE_SCHEMA}",
            path="schema",
        )
    if steps:
        migrated["migration_log"] = steps
    return migrated


def _element_from_dict(raw: Any, path: str) -> Element:
    if not isinstance(raw, dict):
        raise ProfileError("element must be an object", path=path)
    raw_type = raw.get("type")
    if not isinstance(raw_type, str):
        raise ProfileError("type must be a string", path=f"{path}.type")
    try:
        element_type = ElementType(raw_type.strip().lower())
    except ValueError:
        raise ProfileError(f"unknown element type '{raw_type}'", path=f"{path}.type") from None

    defaults = _DEFAULTS[element_type.value]
    element_id = _require_str(raw, "id", f"{path}.id")
    label = _require_str(raw, "label", f"{path}.label", default="")
    if not label:
        label = _require_str(raw, "id", f"{path}.label").upper()[:12]

    x = _require_float(raw, "x", f"{path}.x")
    y = _require_float(raw, "y", f"{path}.y")

    keys_raw = raw.get("keys", [])
    if keys_raw is None:
        keys_raw = []
    if not isinstance(keys_raw, list) or not all(isinstance(k, str) for k in keys_raw):
        raise ProfileError("keys must be a list of strings", path=f"{path}.keys")

    dpad_raw = raw.get("dpad_keys", ["", "", "", ""])
    if (
        not isinstance(dpad_raw, list)
        or len(dpad_raw) != 4
        or not all(isinstance(k, str) for k in dpad_raw)
    ):
        raise ProfileError("dpad_keys must be a list of four strings", path=f"{path}.dpad_keys")

    swipe_raw = raw.get("swipe_to", [0.0, 0.0])
    if (
        not isinstance(swipe_raw, list)
        or len(swipe_raw) != 2
        or not all(isinstance(v, (int, float)) and not isinstance(v, bool) for v in swipe_raw)
    ):
        raise ProfileError("swipe_to must be [x, y]", path=f"{path}.swipe_to")

    element = Element(
        id=element_id,
        type=element_type,
        label=label,
        x=x,
        y=y,
        pointer=_require_int(raw, "pointer", f"{path}.pointer", default=0),
        radius=_require_float(raw, "radius", f"{path}.radius", default=defaults["radius"]),
        hold_delay_s=_require_float(
            raw, "hold_delay_s", f"{path}.hold_delay_s", default=defaults.get("hold_delay_s", 0.0)
        ),
        keys=tuple(keys_raw),
        dpad_keys=(dpad_raw[0], dpad_raw[1], dpad_raw[2], dpad_raw[3]),
        dpad_radius=_require_float(
            raw, "dpad_radius", f"{path}.dpad_radius", default=defaults.get("dpad_radius", 0.09)
        ),
        dead_zone=_require_float(
            raw, "dead_zone", f"{path}.dead_zone", default=defaults.get("dead_zone", 0.15)
        ),
        swipe_to=(float(swipe_raw[0]), float(swipe_raw[1])),
        swipe_duration_ms=_require_int(
            raw,
            "swipe_duration_ms",
            f"{path}.swipe_duration_ms",
            default=defaults.get("swipe_duration_ms", 250),
        ),
        sensitivity=_require_float(
            raw, "sensitivity", f"{path}.sensitivity", default=defaults.get("sensitivity", 1.0)
        ),
        exit_key=_optional_str(raw, "exit_key", f"{path}.exit_key")
        or defaults.get("exit_key", "Escape"),
    )
    element.validate()
    return element


# --- primitive readers -----------------------------------------------------


def _require_str(data: dict[str, Any], key: str, path: str, *, default: str | None = None) -> str:
    value = data.get(key, default)
    if value is None:
        raise ProfileError("missing required field", path=path)
    if not isinstance(value, str):
        raise ProfileError("must be a string", path=path)
    return value


def _optional_str(data: dict[str, Any], key: str, path: str) -> str | None:
    value = data.get(key)
    if value is None:
        return None
    if not isinstance(value, str):
        raise ProfileError("must be a string or null", path=path)
    return value


def _require_int(data: dict[str, Any], key: str, path: str, *, default: int | None = None) -> int:
    value = data.get(key, default)
    if value is None:
        raise ProfileError("missing required field", path=path)
    if isinstance(value, bool):
        raise ProfileError("must be an integer", path=path)
    if isinstance(value, float):
        if value != int(value):
            raise ProfileError("must be an integer", path=path)
        return int(value)
    if not isinstance(value, int):
        raise ProfileError("must be an integer", path=path)
    return value


def _require_float(
    data: dict[str, Any], key: str, path: str, *, default: float | None = None
) -> float:
    value = data.get(key, default)
    if value is None:
        raise ProfileError("missing required field", path=path)
    if isinstance(value, bool):
        raise ProfileError("must be a number", path=path)
    if isinstance(value, (int, float)):
        number = float(value)
        if number != number or number in (float("inf"), float("-inf")):
            raise ProfileError("must be a finite number", path=path)
        return number
    raise ProfileError("must be a number", path=path)


def _timestamp(data: dict[str, Any], key: str, default: Any) -> Any:
    raw = data.get(key)
    if raw is None:
        return default
    if not isinstance(raw, str):
        raise ProfileError("must be an ISO-8601 string", path=key)
    try:
        return parse_timestamp(raw)
    except (ValueError, ProfileError) as exc:
        raise ProfileError(f"invalid timestamp: {exc}", path=key) from None
