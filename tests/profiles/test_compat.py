"""The stored-format compatibility gate.

ARCHITECTURE.md §8 says the schema version *is* the contract: a profile JSON
written by one build must load in the next, and a round trip must not lose
anything a later reader depends on. That claim was untested - the migration
hook existed but no test ever ran it, so a change to the schema would have
been discovered by a user with an unreadable profile.

These tests treat a serialised profile as an external input, which is what it
is: a file another program (or a future C++ port) may have written.
"""

from __future__ import annotations

import json
from dataclasses import replace

import pytest

from lynxdroid.profiles.codec import (
    PROFILE_SCHEMA,
    migrate,
    profile_from_dict,
    profile_to_dict,
)
from lynxdroid.profiles.model import (
    Element,
    ElementType,
    Profile,
    ProfileError,
    Target,
)


@pytest.fixture()
def sample() -> Profile:
    element = Element(
        id="jump", type=ElementType.TAP, label="Jump", x=0.25, y=0.75, keys=("space",)
    )
    return Profile.new("Platformer", Target(package="org.example.game"), elements=(element,))


class TestCurrentSchemaRoundTrip:
    def test_timestamps_are_stored_to_the_second(self, sample: Profile) -> None:
        """The format fixes the precision, so a round trip drops microseconds.

        This is a property of the stored format, not an accident: written
        timestamps carry whole seconds, so a future reader does not have to
        deal with sub-second noise. Everything else must survive untouched.
        """
        payload = profile_to_dict(sample)
        assert payload["created_at"].endswith("Z")
        assert "." not in payload["created_at"], payload["created_at"]
        restored = profile_from_dict(json.loads(json.dumps(payload)))
        assert restored.created_at == sample.created_at.replace(microsecond=0)
        assert restored.updated_at == sample.updated_at.replace(microsecond=0)

    def test_everything_but_the_sub_second_digits_survives(self, sample: Profile) -> None:
        restored = profile_from_dict(json.loads(json.dumps(profile_to_dict(sample))))
        whole_seconds = sample.created_at.replace(microsecond=0)
        assert restored == replace(
            sample, created_at=whole_seconds, updated_at=sample.updated_at.replace(microsecond=0)
        )

    def test_the_written_schema_is_the_current_one(self, sample: Profile) -> None:
        payload = profile_to_dict(sample)
        assert payload["schema"] == PROFILE_SCHEMA

    def test_the_payload_is_json_serialisable_without_custom_encoders(
        self, sample: Profile
    ) -> None:
        # A future C++ reader parses plain JSON; no Python-only types allowed.
        text = json.dumps(profile_to_dict(sample))
        assert json.loads(text) == profile_to_dict(sample)

    def test_keys_are_lists_not_tuples_in_the_file(self, sample: Profile) -> None:
        payload = profile_to_dict(sample)
        assert isinstance(payload["elements"][0]["keys"], list)


class TestUnknownFuture:
    def test_a_newer_schema_is_refused_with_a_clear_message(self, sample: Profile) -> None:
        payload = profile_to_dict(sample)
        payload["schema"] = PROFILE_SCHEMA + 1
        with pytest.raises(ProfileError) as caught:
            profile_from_dict(payload)
        # The user must be told which side is old.
        assert "newer" in str(caught.value)
        assert str(PROFILE_SCHEMA + 1) in str(caught.value)

    def test_a_missing_schema_is_an_error_not_a_guess(self, sample: Profile) -> None:
        payload = profile_to_dict(sample)
        del payload["schema"]
        with pytest.raises(ProfileError):
            profile_from_dict(payload)

    def test_a_non_integer_schema_is_refused(self, sample: Profile) -> None:
        payload = profile_to_dict(sample)
        payload["schema"] = "1"
        with pytest.raises(ProfileError):
            profile_from_dict(payload)


class TestMigrate:
    def test_migrating_to_the_current_version_is_a_no_op(self) -> None:
        data = {"schema": PROFILE_SCHEMA, "keep": True}
        assert migrate(data, PROFILE_SCHEMA) == data

    def test_migrating_from_an_unknown_older_version_refuses(self) -> None:
        """Guessing how an old file was shaped is how data gets destroyed."""
        with pytest.raises(ProfileError) as caught:
            migrate({"schema": -1}, -1)
        assert "no migration path" in str(caught.value)

    def test_migrate_does_not_mutate_its_input(self) -> None:
        data = {"schema": PROFILE_SCHEMA}
        before = dict(data)
        migrate(data, PROFILE_SCHEMA)
        assert data == before
