"""Profile model and codec tests (stage 9 + 15)."""

from __future__ import annotations

import json

import pytest

from lynxdroid.profiles.codec import profile_from_dict, profile_to_dict
from lynxdroid.profiles.model import (
    PROFILE_SCHEMA,
    Element,
    ElementType,
    Profile,
    ProfileError,
    ReferenceResolution,
    Target,
    key_conflicts,
)

TARGET = Target(package="com.example.game", package_version="1.2.3")


def tap(**kwargs) -> Element:
    base = {
        "id": "tap-a",
        "type": ElementType.TAP,
        "label": "A",
        "x": 0.5,
        "y": 0.5,
        "keys": ("j",),
    }
    base.update(kwargs)
    return Element(**base)  # type: ignore[arg-type]


class TestTarget:
    @pytest.mark.parametrize(
        "package",
        [
            "com.example.game",
            "a.b",
            "com.a1.b2_c",
            "org.x.y.z",
        ],
    )
    def test_valid_packages(self, package: str) -> None:
        Target(package=package).validate()

    @pytest.mark.parametrize(
        "package",
        [
            "",
            "game",
            "com..game",
            ".com.game",
            "com.game.",
            "com/example",
            "com game",
            "1com.game",
        ],
    )
    def test_invalid_packages(self, package: str) -> None:
        with pytest.raises(ProfileError):
            Target(package=package).validate()

    def test_version_mismatch_is_detectable(self) -> None:
        target = Target(package="com.a.b", package_version="1.0")
        assert target.version_mismatch("2.0")
        assert not target.version_mismatch("1.0")
        assert not target.version_mismatch(None)


class TestElements:
    def test_tap_requires_keys(self) -> None:
        with pytest.raises(ProfileError, match="needs at least one key"):
            tap(keys=()).validate()

    def test_position_outside_unit_square_rejected(self) -> None:
        with pytest.raises(ProfileError, match=r"outside 0\.\.1"):
            tap(x=1.2).validate()
        with pytest.raises(ProfileError):
            tap(y=-0.1).validate()

    def test_radius_bounds(self) -> None:
        with pytest.raises(ProfileError):
            tap(radius=0.0001).validate()
        with pytest.raises(ProfileError):
            tap(radius=0.9).validate()
        tap(radius=0.02).validate()

    def test_pointer_slot_bounds(self) -> None:
        tap(pointer=0).validate()
        tap(pointer=9).validate()
        with pytest.raises(ProfileError):
            tap(pointer=10).validate()

    def test_invalid_key_names(self) -> None:
        with pytest.raises(ProfileError, match="invalid key name"):
            tap(keys=("",)).validate()
        with pytest.raises(ProfileError):
            tap(keys=("a" * 40,)).validate()

    def test_dpad_needs_four_distinct_keys(self) -> None:
        with pytest.raises(ProfileError, match="four directional keys"):
            Element(
                id="d", type=ElementType.DPAD, label="D", x=0.5, y=0.5, dpad_keys=("w", "d", "", "")
            ).validate()
        with pytest.raises(ProfileError, match="distinct"):
            Element(
                id="d",
                type=ElementType.DPAD,
                label="D",
                x=0.5,
                y=0.5,
                dpad_keys=("w", "w", "s", "a"),
            ).validate()

    def test_dpad_cannot_leave_the_screen(self) -> None:
        with pytest.raises(ProfileError, match="outside the screen"):
            Element(
                id="d",
                type=ElementType.DPAD,
                label="D",
                x=0.05,
                y=0.5,
                dpad_keys=("w", "d", "s", "a"),
                dpad_radius=0.2,
            ).validate()

    def test_swipe_requires_movement(self) -> None:
        with pytest.raises(ProfileError, match="identical"):
            Element(
                id="s",
                type=ElementType.SWIPE,
                label="S",
                x=0.5,
                y=0.5,
                keys=("z",),
                swipe_to=(0.5, 0.5),
            ).validate()

    def test_swipe_duration_bounds(self) -> None:
        with pytest.raises(ProfileError):
            Element(
                id="s",
                type=ElementType.SWIPE,
                label="S",
                x=0.2,
                y=0.5,
                keys=("z",),
                swipe_to=(0.8, 0.5),
                swipe_duration_ms=10,
            ).validate()

    def test_mouse_exit_key_required(self) -> None:
        with pytest.raises(ProfileError, match="exit_key"):
            Element(id="m", type=ElementType.MOUSE, label="M", x=0.5, y=0.5, exit_key="").validate()

    def test_label_length_limit(self) -> None:
        with pytest.raises(ProfileError, match="label longer"):
            tap(label="X" * 25).validate()


class TestProfile:
    def test_new_profile_has_id_and_timestamps(self) -> None:
        profile = Profile.new("Shooter", TARGET, elements=(tap(),))
        assert len(profile.id) == 16
        assert profile.schema == PROFILE_SCHEMA
        assert profile.created_at <= profile.updated_at

    def test_duplicate_ids_rejected(self) -> None:
        with pytest.raises(ProfileError, match="duplicate element id"):
            Profile.new("Dup", TARGET, elements=(tap(), tap(y=0.7)))

    def test_key_conflicts_rejected_on_validate(self) -> None:
        first = tap(keys=("j",))
        second = Element(id="tap-b", type=ElementType.TAP, label="B", x=0.2, y=0.2, keys=("j",))
        with pytest.raises(ProfileError, match="key assigned to more than one"):
            Profile.new("Conflict", TARGET, elements=(first, second))

    def test_key_conflicts_reports_the_key(self) -> None:
        first = tap(keys=("j",))
        second = Element(id="tap-b", type=ElementType.TAP, label="B", x=0.2, y=0.2, keys=("j",))
        assert key_conflicts([first, second]) == {"j"}

    def test_duplicate_creates_a_new_identity(self) -> None:
        profile = Profile.new("Original", TARGET, elements=(tap(),))
        copy = profile.duplicate()
        assert copy.id != profile.id
        assert copy.name == "Original (copy)"
        assert copy.elements == profile.elements

    def test_editing_returns_new_instances(self) -> None:
        profile = Profile.new("A", TARGET, elements=(tap(),))
        renamed = profile.with_name("B")
        assert renamed.name == "B"
        assert profile.name == "A"
        assert renamed.updated_at >= profile.updated_at

    def test_add_and_remove_elements(self) -> None:
        profile = Profile.new("A", TARGET)
        extended = profile.add_element(tap())
        assert len(extended.elements) == 1
        trimmed = extended.remove_element("tap-a")
        assert trimmed.elements == ()

    def test_replace_missing_element_raises(self) -> None:
        profile = Profile.new("A", TARGET, elements=(tap(),))
        with pytest.raises(ProfileError, match="no element"):
            profile.replace_element(
                Element(id="nope", type=ElementType.TAP, label="N", x=0.1, y=0.1, keys=("n",))
            )

    def test_checksum_is_stable_and_change_sensitive(self) -> None:
        base = Profile.new("A", TARGET, elements=(tap(),))
        assert base.checksum() == base.checksum()
        moved = base.replace_element(tap(x=0.9))
        assert moved.checksum() != base.checksum()

    def test_name_is_normalised(self) -> None:
        profile = Profile.new("  My    Game  ", TARGET)
        assert profile.name == "My Game"

    def test_empty_name_rejected(self) -> None:
        with pytest.raises(ProfileError):
            Profile.new("   ", TARGET)

    def test_reference_resolution_bounds(self) -> None:
        with pytest.raises(ProfileError):
            ReferenceResolution(100, 100).validate()
        with pytest.raises(ProfileError):
            ReferenceResolution(1920, 1080, dpi=10).validate()
        ReferenceResolution(1280, 720, 240).validate()


class TestCodec:
    def test_round_trip_preserves_everything(self) -> None:
        dpad = Element(
            id="mv", type=ElementType.DPAD, label="MV", x=0.2, y=0.8, dpad_keys=("w", "d", "s", "a")
        )
        profile = Profile.new("Round", TARGET, elements=(tap(), dpad))
        payload = json.loads(json.dumps(profile_to_dict(profile)))
        restored = profile_from_dict(payload)
        assert restored.to_dict() == profile.to_dict()
        assert restored.checksum() == profile.checksum()

    def test_unknown_element_type_is_reported_with_path(self) -> None:
        profile = Profile.new("X", TARGET, elements=(tap(),))
        payload = profile_to_dict(profile)
        payload["elements"][0]["type"] = "joystick"
        with pytest.raises(ProfileError, match=r"elements\[0\]\.type"):
            profile_from_dict(payload)

    def test_missing_required_field_names_the_field(self) -> None:
        payload = profile_to_dict(Profile.new("X", TARGET))
        del payload["target"]
        with pytest.raises(ProfileError, match="target"):
            profile_from_dict(payload)

    def test_future_schema_is_refused_not_guessed(self) -> None:
        payload = profile_to_dict(Profile.new("X", TARGET))
        payload["schema"] = PROFILE_SCHEMA + 5
        with pytest.raises(ProfileError, match="newer LynxDroid"):
            profile_from_dict(payload)

    def test_non_numeric_coordinate_rejected(self) -> None:
        payload = profile_to_dict(Profile.new("X", TARGET, elements=(tap(),)))
        payload["elements"][0]["x"] = "left"
        with pytest.raises(ProfileError, match=r"elements\[0\]\.x"):
            profile_from_dict(payload)

    def test_nan_and_infinity_rejected(self) -> None:
        payload = profile_to_dict(Profile.new("X", TARGET, elements=(tap(),)))
        payload["elements"][0]["y"] = float("nan")
        with pytest.raises(ProfileError):
            profile_from_dict(payload)

    def test_bad_timestamp_rejected(self) -> None:
        payload = profile_to_dict(Profile.new("X", TARGET))
        payload["updated_at"] = "yesterday"
        with pytest.raises(ProfileError, match="timestamp"):
            profile_from_dict(payload)

    def test_naive_timestamp_rejected(self) -> None:
        payload = profile_to_dict(Profile.new("X", TARGET))
        payload["created_at"] = "2024-01-01T00:00:00"
        with pytest.raises(ProfileError):
            profile_from_dict(payload)

    def test_elements_must_be_a_list(self) -> None:
        payload = profile_to_dict(Profile.new("X", TARGET))
        payload["elements"] = {"a": 1}
        with pytest.raises(ProfileError, match="must be a list"):
            profile_from_dict(payload)

    def test_export_contains_no_secret_fields(self) -> None:
        payload = profile_to_dict(Profile.new("X", TARGET, elements=(tap(),)))
        text = json.dumps(payload).lower()
        for forbidden in ("password", "token", "secret", "cookie", "auth"):
            assert forbidden not in text

    def test_label_defaults_from_id_when_missing(self) -> None:
        payload = profile_to_dict(Profile.new("X", TARGET, elements=(tap(label="A"),)))
        payload["elements"][0]["label"] = ""
        restored = profile_from_dict(payload)
        assert restored.elements[0].label
