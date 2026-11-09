"""Compatibility advice and safe failure behaviour."""

from __future__ import annotations

import pytest

from lynxdroid.backend.compat import CompatFixes, Outcome, Support, match_failure
from lynxdroid.backend.contract import BackendError, user_consented


class Target:
    def __init__(self, ping: int = 0) -> None:
        self.ping = ping
        self.writes: list[tuple[str, str]] = []

    def get_prop(self, name: str) -> str | None:
        return "release-keys"

    def set_prop(self, name: str, value: str, *, consent: object = None) -> bool:
        self.writes.append((name, value))
        return True

    def shell_probe(self, command: list[str], *, timeout: float = 10.0) -> tuple[int, str]:
        return (self.ping if command[0] == "ping" else 0, "probe output")


def fixes(target: Target) -> CompatFixes:
    return CompatFixes(target, consent=user_consented("test"))


@pytest.mark.parametrize("method", ["hide_root", "revert_hide_root", "egl_fallback"])
def test_unverified_changes_are_disabled(method: str) -> None:
    target = Target()
    result = getattr(fixes(target), method)()
    assert result.outcome is Outcome.UNAVAILABLE
    assert not result.changed
    assert not target.writes


def test_revert_does_not_guess_on_clean_device() -> None:
    target = Target()
    result = fixes(target).revert_hide_root()
    assert "did not save" in result.message
    assert target.writes == []


def test_refuses_boolean_consent() -> None:
    with pytest.raises(BackendError):
        CompatFixes(Target(), consent=True)  # type: ignore[arg-type]


def test_attestation_precedes_root() -> None:
    match = match_failure("root access; play integrity check failed")
    assert match is not None and match.id == "integrity-attestation"


def test_root_has_no_unverified_fix() -> None:
    match = match_failure("активны root-права")
    assert match is not None and match.support is Support.UNKNOWN and match.fix is None


def test_renderer_has_no_unverified_fix() -> None:
    match = match_failure("EGL error")
    assert match is not None and match.fix is None


def test_unknown_message() -> None:
    assert match_failure("Error 0xDEADBEEF") is None


def test_network_error_offers_probe() -> None:
    match = match_failure("Не удалось войти на сервер: 3")
    assert match is not None and match.fix == "network"


def test_ping_success_does_not_prove_dns_or_game_connectivity() -> None:
    result = fixes(Target()).network_diagnosis()
    assert result.outcome is Outcome.ALREADY
    assert not result.changed
    assert "does not verify the game" in result.message


def test_ping_failure_does_not_prove_network_failure() -> None:
    result = fixes(Target(ping=1)).network_diagnosis()
    assert result.outcome is Outcome.UNAVAILABLE
    assert "do not prove" in result.message


def test_apply_stops_after_unavailable() -> None:
    assert len(fixes(Target()).apply_all(["hide-root", "network"])) == 1


def test_unknown_fix_is_refused() -> None:
    assert fixes(Target()).apply_all(["unknown"])[0].outcome is Outcome.REFUSED


def test_generic_device_incompatibility_is_not_emulator_detection() -> None:
    assert match_failure("This app is not supported on this device") is None


def test_container_mention_does_not_assert_android_failed_to_start() -> None:
    match = match_failure("Waydroid issue")
    assert match is not None
    assert "does not establish the cause" in match.explanation


def test_network_diagnostic_is_not_claimed_to_be_a_fix() -> None:
    match = match_failure("network connection failed")
    assert match is not None and match.support is Support.UNKNOWN


def test_hostname_ping_is_a_separate_probe() -> None:
    target = Target()
    commands = []

    def probe(command, *, timeout=10.0):
        commands.append(command)
        return 0, "ok"

    target.shell_probe = probe  # type: ignore[method-assign]
    result = fixes(target).network_diagnosis()
    assert commands == [
        ["ping", "-c", "1", "-W", "2", "1.1.1.1"],
        ["ping", "-c", "1", "-W", "2", "example.com"],
        ["ip", "route"],
    ]
    assert len(result.steps) == 3
    assert not target.writes


def test_hostname_failure_is_inconclusive_even_if_ip_succeeds() -> None:
    target = Target()

    def probe(command, *, timeout=10.0):
        return (1 if "example.com" in command else 0, "probe")

    target.shell_probe = probe  # type: ignore[method-assign]
    result = fixes(target).network_diagnosis()
    assert result.outcome is Outcome.UNAVAILABLE
    assert "sudo waydroid shell --" in result.message
    assert "may not resolve" in result.message


def test_missing_shell_gives_actionable_manual_commands() -> None:
    target = Target()

    def no_shell(command, *, timeout=10.0):
        raise BackendError("no Android shell", code="no-shell")

    target.shell_probe = no_shell  # type: ignore[method-assign]
    result = fixes(target).network_diagnosis()
    assert result.outcome is Outcome.UNAVAILABLE
    assert len(result.steps) == 3 and not any(step.done for step in result.steps)
    assert "sudo waydroid shell -- ip route" in result.message
