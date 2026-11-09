"""The compatibility layer must be honest about what it cannot do.

Two failure modes matter here and they are opposite:

* claiming a fix worked when nothing changed - the user retries the game and
  blames us a second time;
* letting an unfixable refusal look fixable - the user spend an evening on
  property writes that were never going to satisfy a Play Integrity check.

Most of this file is about the second one, because it costs more.
"""

from __future__ import annotations

import pytest

from lynxdroid.backend import compat
from lynxdroid.backend.compat import (
    FIX_IDS,
    ROOT_PROPERTY_NAMES,
    CompatFixes,
    Outcome,
    Signature,
    Support,
    match_failure,
)
from lynxdroid.backend.contract import BackendError, user_consented


class FakeTarget:
    """A backend that records writes instead of performing them."""

    def __init__(self, props: dict[str, str | None] | None = None) -> None:
        self.props: dict[str, str | None] = dict(props or {})
        self.writes: list[tuple[str, str]] = []
        self.probe: list[tuple[list[str], int, str]] = []
        self.fail_on: set[str] = set()

    def get_prop(self, name: str) -> str | None:
        return self.props.get(name)

    def set_prop(self, name: str, value: str, *, consent: object = None) -> bool:
        if name in self.fail_on:
            return False
        self.writes.append((name, value))
        self.props[name] = value
        return True

    def shell_probe(self, command: list[str], *, timeout: float = 10.0) -> tuple[int, str]:
        for planned, code, output in self.probe:
            if planned == list(command):
                return code, output
        self.probe.append((list(command), 0, "ok"))
        return 0, "ok"


def fixes(target: FakeTarget) -> CompatFixes:
    return CompatFixes(target, consent=user_consented("test", "unit test"))


# ---------------------------------------------------------------------------
# Consent
# ---------------------------------------------------------------------------


class TestConsentIsNotABoolean:
    def test_a_bare_true_is_refused(self) -> None:
        with pytest.raises(BackendError) as caught:
            CompatFixes(FakeTarget(), consent=True)  # type: ignore[arg-type]
        assert caught.value.code == "consent-required"

    def test_a_missing_consent_is_refused(self) -> None:
        with pytest.raises(BackendError):
            CompatFixes(FakeTarget(), consent=None)  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# hide-root
# ---------------------------------------------------------------------------


class TestHideRoot:
    def test_it_writes_the_three_known_indicators(self) -> None:
        target = FakeTarget({"ro.debuggable": "1", "ro.secure": "0", "ro.build.tags": "test-keys"})
        result = fixes(target).hide_root()
        assert result.outcome is Outcome.APPLIED
        assert target.writes == [
            ("ro.debuggable", "0"),
            ("ro.secure", "1"),
            ("ro.build.tags", "release-keys"),
        ]

    def test_it_remembers_what_was_there_for_the_revert(self) -> None:
        target = FakeTarget({"ro.debuggable": "1", "ro.secure": "0", "ro.build.tags": "test-keys"})
        result = fixes(target).hide_root()
        assert result.restore == {
            "ro.debuggable": "1",
            "ro.secure": "0",
            "ro.build.tags": "test-keys",
        }

    def test_an_already_clean_image_is_not_reported_as_changed(self) -> None:
        target = FakeTarget(
            {"ro.debuggable": "0", "ro.secure": "1", "ro.build.tags": "release-keys"}
        )
        result = fixes(target).hide_root()
        assert result.outcome is Outcome.ALREADY
        assert not result.changed
        assert target.writes == []

    def test_a_refused_write_is_a_failure_not_a_success(self) -> None:
        target = FakeTarget({"ro.debuggable": "1", "ro.secure": "0", "ro.build.tags": "test-keys"})
        target.fail_on = {"ro.build.tags"}
        result = fixes(target).hide_root()
        assert result.outcome is Outcome.FAILED
        assert "allow_prop_writes" in result.message

    def test_it_does_not_claim_to_defeat_attestation(self) -> None:
        """The message must not oversell: Play Integrity is untouched."""
        target = FakeTarget({"ro.debuggable": "1"})
        result = fixes(target).hide_root()
        assert "Play Integrity" in result.message
        assert "not" in result.message


class TestRevert:
    def test_revert_restores_the_stock_values(self) -> None:
        target = FakeTarget(
            {"ro.debuggable": "0", "ro.secure": "1", "ro.build.tags": "release-keys"}
        )
        fixes(target).revert_hide_root()
        assert target.props == {
            "ro.debuggable": "1",
            "ro.secure": "0",
            "ro.build.tags": "test-keys",
        }

    def test_revert_of_an_untouched_system_changes_nothing(self) -> None:
        target = FakeTarget({"ro.debuggable": "1", "ro.secure": "0", "ro.build.tags": "test-keys"})
        result = fixes(target).revert_hide_root()
        assert result.outcome is Outcome.ALREADY
        assert target.writes == []

    def test_every_root_property_has_a_known_stock_value(self) -> None:
        """A revert that does not know the original value would guess."""
        for name in ROOT_PROPERTY_NAMES:
            assert name in compat._ORIGINAL_DEFAULTS


# ---------------------------------------------------------------------------
# egl-fallback
# ---------------------------------------------------------------------------


class TestRendererFallback:
    def test_it_requests_software_rendering(self) -> None:
        target = FakeTarget()
        result = fixes(target).egl_fallback()
        assert ("persist.waydroid.renderer", "swiftshader") in target.writes
        assert result.outcome is Outcome.APPLIED

    def test_it_says_the_frame_rate_will_drop(self) -> None:
        result = fixes(FakeTarget()).egl_fallback()
        assert "frame rate" in result.message

    def test_a_refusal_is_reported_as_a_failure(self) -> None:
        target = FakeTarget()
        target.fail_on = {"persist.waydroid.renderer"}
        result = fixes(target).egl_fallback()
        assert result.outcome is Outcome.FAILED


# ---------------------------------------------------------------------------
# network
# ---------------------------------------------------------------------------


class TestNetworkDiagnosis:
    def test_a_reachable_container_says_so(self) -> None:
        target = FakeTarget()
        target.probe = [(["ping", "-c", "1", "-W", "2", "1.1.1.1"], 0, "1 packets transmitted")]
        result = fixes(target).network_diagnosis()
        assert result.outcome is Outcome.APPLIED
        assert "reaches the network" in result.message

    def test_no_egress_is_reported_as_no_answer_not_as_fixed(self) -> None:
        target = FakeTarget()
        target.probe = [(["ping", "-c", "1", "-W", "2", "1.1.1.1"], 1, "Network is unreachable")]
        result = fixes(target).network_diagnosis()
        assert result.outcome is Outcome.UNAVAILABLE
        assert "no working egress" in result.message

    def test_it_probes_from_inside_android(self) -> None:
        """A host ping proves nothing about the container's own network."""
        target = FakeTarget()
        fixes(target).network_diagnosis()
        commands = [command for command, _code, _out in target.probe]
        assert any("ping" in command for command in commands)


# ---------------------------------------------------------------------------
# Matching a game's own message
# ---------------------------------------------------------------------------


class TestSignatureMatching:
    def test_the_standoff_screenshot_is_recognised(self) -> None:
        message = (
            "На вашем устройстве активны root-права. Они могут использоваться для "
            "изменения файлов игры, что является нарушением."
        )
        signature = match_failure(message)
        assert signature is not None
        assert signature.id == "root-detected"
        assert signature.fix == "hide-root"
        assert signature.support is Support.FIXABLE

    def test_the_free_fire_screenshot_is_recognised(self) -> None:
        message = "Ошибка сетевого подключения\nНе удалось войти на сервер: 3"
        signature = match_failure(message)
        assert signature is not None
        assert signature.id == "network"
        assert signature.fix == "network"

    def test_attestation_is_matched_before_plain_root(self) -> None:
        """A message naming both must get the stricter, honest diagnosis."""
        message = "root detected; play integrity check failed"
        signature = match_failure(message)
        assert signature is not None
        assert signature.id == "integrity-attestation"
        assert signature.support is Support.NOT_FIXABLE

    def test_an_unknown_message_matches_nothing(self) -> None:
        assert match_failure("Error code 0x5F3759DF") is None

    def test_advice_for_an_unknown_message_does_not_pretend_to_know(self) -> None:
        advice = fixes(FakeTarget()).advise("Error code 0x5F3759DF")
        assert advice["match"] is None
        assert advice["support"] == "unknown"
        assert advice["fix"] is None
        assert "do not know the cause" in str(advice["advice"])

    def test_every_signature_has_a_reason(self) -> None:
        for signature in compat.SIGNATURES:
            assert signature.explanation.strip(), f"{signature.id} has no explanation"
            assert signature.markers, f"{signature.id} can never match"

    def test_an_unfixable_signature_names_no_fix(self) -> None:
        for signature in compat.SIGNATURES:
            if signature.support is Support.NOT_FIXABLE:
                assert signature.fix is None, (
                    f"{signature.id} is unfixable but offers '{signature.fix}'"
                )

    def test_markers_are_lower_case_so_matching_is_not_case_blind(self) -> None:
        for signature in compat.SIGNATURES:
            for marker in signature.markers:
                assert marker == marker.lower(), f"{signature.id}: {marker!r}"


class TestEmulatorDetection:
    def test_it_is_declared_unfixable(self) -> None:
        signature = match_failure("This application does not run on an emulator")
        assert signature is not None
        assert signature.support is Support.NOT_FIXABLE

    def test_the_advice_says_whose_decision_it_is(self) -> None:
        advice = fixes(FakeTarget()).advise("emulator detected")
        assert "vendor" in str(advice["advice"])


# ---------------------------------------------------------------------------
# Applying a set of fixes
# ---------------------------------------------------------------------------


class TestApplyAll:
    def test_it_runs_the_named_fixes_in_order(self) -> None:
        target = FakeTarget()
        results = fixes(target).apply_all(["hide-root", "egl-fallback"])
        assert [r.fix for r in results] == ["hide-root", "egl-fallback"]

    def test_an_unknown_fix_is_refused_not_ignored(self) -> None:
        results = fixes(FakeTarget()).apply_all(["defeat-anticheat"])
        assert results[0].outcome is Outcome.REFUSED
        assert "no such fix" in results[0].message

    def test_it_stops_after_a_failure(self) -> None:
        target = FakeTarget({"ro.debuggable": "1", "ro.secure": "0", "ro.build.tags": "test-keys"})
        target.fail_on = {"ro.build.tags"}
        results = fixes(target).apply_all(["hide-root", "egl-fallback"])
        assert len(results) == 1, "a failed prerequisite must stop the sequence"

    def test_every_advertised_fix_id_is_callable(self) -> None:
        """FIX_IDS is what the CLI offers; each must exist on the class."""
        target = FakeTarget()
        instance = fixes(target)
        for fix_id in FIX_IDS:
            assert callable(
                getattr(
                    instance,
                    {
                        "hide-root": "hide_root",
                        "egl-fallback": "egl_fallback",
                        "network": "network_diagnosis",
                    }[fix_id],
                )
            )


class TestSerialisation:
    def test_a_result_serialises_for_json(self) -> None:
        target = FakeTarget({"ro.debuggable": "1"})
        payload = fixes(target).hide_root().as_dict()
        assert payload["fix"] == "hide-root"
        assert payload["outcome"] == "applied"
        assert payload["changed"] is True
        assert isinstance(payload["steps"], list)
        assert payload["steps"], "a fix that reports no steps is not evidence"

    def test_a_signature_is_immutable(self) -> None:
        """The list of known refusals is shared state; nobody may mutate it."""
        import dataclasses

        signature = Signature("x", "t", ("m",), None, Support.UNKNOWN, "why")
        with pytest.raises(dataclasses.FrozenInstanceError):
            signature.id = "y"  # type: ignore[misc]
