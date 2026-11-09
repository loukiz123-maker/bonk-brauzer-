"""Best-effort matching of game errors and non-destructive network probes.

A textual match is a hypothesis, not proof of the underlying cause.
"""

from __future__ import annotations

import logging
from collections.abc import Callable, Iterable, Sequence
from dataclasses import dataclass, field
from enum import Enum
from typing import Protocol

from lynxdroid.backend.contract import BackendError, Consent

log = logging.getLogger(__name__)


class Outcome(Enum):
    """What a fix did, in terms the user needs."""

    APPLIED = "applied"
    ALREADY = "already-in-place"
    REFUSED = "refused"
    FAILED = "failed"
    UNAVAILABLE = "unavailable"


class Support(Enum):
    """Whether a refusal is something we can do anything about."""

    FIXABLE = "fixable"
    #: The game demands something a property write cannot provide.
    NOT_FIXABLE = "not-fixable"
    #: We do not know: no signature matched. Never presented as fixed.
    UNKNOWN = "unknown"


@dataclass(frozen=True)
class Signature:
    """One known way a game refuses to start, with the fix that addresses it."""

    id: str
    #: Human name, shown in the UI and in the report.
    title: str
    #: Lower-case substrings; the game's own message is matched against these.
    markers: tuple[str, ...]
    #: Which fix (if any) is likely to help.
    fix: str | None
    support: Support
    #: Why, in one sentence a user can act on or argue with.
    explanation: str

    def matches(self, message: str) -> bool:
        lowered = message.lower()
        return any(marker in lowered for marker in self.markers)


# The list is deliberately short and each entry is a claim we can defend. A
# long list of guesses would look more capable and help nobody.
SIGNATURES: tuple[Signature, ...] = (
    Signature(
        id="root-detected",
        title="the game detected root access",
        markers=(
            "root-права",
            "root права",
            "rooted",
            "root access",
            "суперпользователь",
            "superuser",
            "su binary",
        ),
        fix=None,
        support=Support.UNKNOWN,
        explanation=(
            "The game reports root detection; the exact check is unknown. "
            "Property changes have not been verified to resolve it. Consult the game vendor."
        ),
    ),
    Signature(
        id="integrity-attestation",
        title="the game requires Play Integrity attestation",
        markers=(
            "play integrity",
            "integrity check",
            "device is not certified",
            "device not certified",
            "gms certificate",
            "attestation",
        ),
        fix=None,
        support=Support.NOT_FIXABLE,
        explanation=(
            "Attestation is signed by Google's servers about the whole boot chain. "
            "No setting in this container can produce a passing verdict, and an "
            "attempt to fake one is out of scope by policy (docs/SECURITY.md)."
        ),
    ),
    Signature(
        id="emulator-detected",
        title="the game refuses to run under an emulator",
        markers=(
            "эмулятор",
            "emulator",
        ),
        fix=None,
        support=Support.NOT_FIXABLE,
        explanation=(
            "Waydroid is a container, not a phone. A title that decides emulators "
            "are forbidden will keep refusing no matter what we set; the vendor's "
            "decision is the blocker, not our configuration."
        ),
    ),
    Signature(
        id="binder-root",
        title="the message mentions the Android container",
        markers=(
            "waydroid",
            "container",
            "binder",
        ),
        fix=None,
        support=Support.UNKNOWN,
        explanation=(
            "This message mentions the container but does not establish the cause. "
            "Run 'lynxdroid doctor' to gather more information."
        ),
    ),
    Signature(
        id="renderer",
        title="the renderer could not create a surface",
        markers=(
            "egl",
            "gles",
            "renderer",
            "surfaceflinger",
            "gralloc",
            "vulkan",
        ),
        fix=None,
        support=Support.UNKNOWN,
        explanation=(
            "A graphics-related term was found; the cause is not confirmed. "
            "Run lynxdroid doctor and inspect Waydroid logs."
        ),
    ),
    Signature(
        id="network",
        title="the game could not reach its server",
        markers=(
            "сетевое подключение",
            "не удалось войти на сервер",
            "ошибка сети",
            "network connection",
            "connection failed",
            "timed out",
            "timeout",
        ),
        fix="network",
        support=Support.UNKNOWN,
        explanation=(
            "The game reports a connection error. Its cause is unknown; "
            "check connectivity inside Android and consult the game vendor."
        ),
    ),
)


def match_failure(message: str) -> Signature | None:
    """The first signature matching *message*, or ``None``.

    Order matters: the attestation entry is checked before the plain root one,
    because a message can mention both and the stricter diagnosis is the
    honest one - telling a user to hide root when the game asks for a signed
    boot chain wastes their evening.
    """
    ordered = sorted(SIGNATURES, key=lambda s: s.id != "integrity-attestation")
    for signature in ordered:
        if signature.matches(message):
            return signature
    return None


@dataclass
class Step:
    """One thing a fix did, or found already done."""

    action: str
    done: bool
    detail: str = ""

    def as_dict(self) -> dict[str, object]:
        return {"action": self.action, "done": self.done, "detail": self.detail}


@dataclass
class FixResult:
    """The outcome of one fix, including everything needed to undo it."""

    fix: str
    outcome: Outcome
    steps: list[Step] = field(default_factory=list)
    #: Property name -> value it had before we touched it (``None`` = unset).
    restore: dict[str, str | None] = field(default_factory=dict)
    message: str = ""

    @property
    def changed(self) -> bool:
        return self.outcome is Outcome.APPLIED and any(step.done for step in self.steps)

    def as_dict(self) -> dict[str, object]:
        return {
            "fix": self.fix,
            "outcome": self.outcome.value,
            "changed": self.changed,
            "message": self.message,
            "steps": [step.as_dict() for step in self.steps],
            "restore": dict(self.restore),
        }


#: The narrow view of a backend that the fixes need. A protocol rather than the
#: concrete class so the tests can run without Waydroid and so this module
#: cannot quietly grow a dependency on everything the backend can do.
class CompatTarget(Protocol):
    """What a fix is allowed to do to Android.

    A ``Protocol`` rather than a base class: the backend must not inherit from
    a module whose whole job is optional compatibility work, and the tests must
    be able to pass a dict-backed stand-in. ``AndroidBackend`` satisfies this
    structurally, which mypy checks at every call site.
    """

    def get_prop(self, name: str) -> str | None:
        raise NotImplementedError

    def set_prop(self, name: str, value: str, *, consent: Consent | None = None) -> bool:
        raise NotImplementedError

    def shell_probe(self, command: Sequence[str], *, timeout: float = 10.0) -> tuple[int, str]:
        raise NotImplementedError


class CompatFixes:
    """The fixes, and the judgement about what is worth trying.

    Constructed with a target and the user's consent, so that no method can be
    called into a state where a persistent change happens without one.
    """

    def __init__(self, target: CompatTarget, *, consent: Consent) -> None:
        if not isinstance(consent, Consent):
            raise BackendError(
                "compatibility fixes change persistent Android state",
                code="consent-required",
                hint="ask the user, then pass what user_consented returned",
            )
        self._target = target
        self._consent = consent

    # -- individual fixes ---------------------------------------------------
    def hide_root(self) -> FixResult:
        """Disabled: property changes and their reversal were not verified."""
        return FixResult(
            "hide-root",
            Outcome.UNAVAILABLE,
            message="Disabled: root detection cannot be reliably fixed by these properties. "
            "No changes made; consult the game vendor.",
        )

    def revert_hide_root(self) -> FixResult:
        """Never guess previous values; older versions did not save them."""
        return FixResult(
            "hide-root",
            Outcome.UNAVAILABLE,
            message="Cannot restore automatically: earlier versions did not save original values. "
            "No changes made. Check your Waydroid configuration and original values manually.",
        )

    def egl_fallback(self) -> FixResult:
        return FixResult(
            "egl-fallback",
            Outcome.UNAVAILABLE,
            message="Disabled: this renderer property was not verified on Waydroid. "
            "No changes made; run lynxdroid doctor.",
        )

    def network_diagnosis(self) -> FixResult:
        """Collect limited network evidence without changing settings."""
        result = FixResult("network", Outcome.UNAVAILABLE)
        for label, command in (
            ("ICMP to 1.1.1.1", ["ping", "-c", "1", "-W", "2", "1.1.1.1"]),
            ("routes", ["ip", "route"]),
        ):
            code, output = self._target.shell_probe(command)
            result.steps.append(Step(label, code == 0, output.strip()[:500]))
        if result.steps[0].done:
            result.outcome = Outcome.ALREADY
            result.message = (
                "ICMP to 1.1.1.1 succeeded. This does not verify DNS, "
                "TCP, or the game's server; no settings changed."
            )
        else:
            result.message = (
                "ICMP to 1.1.1.1 failed; ICMP may be blocked. "
                "This alone does not prove network or DNS failure; "
                "check routes and consult the game vendor."
            )
        return result

    # -- the judgement ------------------------------------------------------
    def advise(self, message: str) -> dict[str, object]:
        """Map a game's own error text to a diagnosis the user can act on."""
        signature = match_failure(message)
        if signature is None:
            return {
                "match": None,
                "support": Support.UNKNOWN.value,
                "advice": (
                    "this message does not match anything we know. That means we do "
                    "not know the cause, not that there is no cause: copy the text and "
                    "run 'lynxdroid doctor' before changing settings"
                ),
                "fix": None,
            }
        return {
            "match": signature.id,
            "title": signature.title,
            "support": signature.support.value,
            "advice": signature.explanation,
            "fix": signature.fix,
        }

    def apply_all(self, fixes: Iterable[str]) -> list[FixResult]:
        """Run the named fixes in order, stopping at the first hard failure."""
        known: dict[str, Callable[[], FixResult]] = {
            "hide-root": self.hide_root,
            "egl-fallback": self.egl_fallback,
            "network": self.network_diagnosis,
        }
        results: list[FixResult] = []
        for name in fixes:
            action = known.get(name)
            if action is None:
                results.append(FixResult(name, Outcome.REFUSED, message=f"no such fix '{name}'"))
                continue
            outcome = action()
            results.append(outcome)
            if outcome.outcome in (Outcome.FAILED, Outcome.REFUSED, Outcome.UNAVAILABLE):
                break
        return results


#: Legacy command names remain accepted but unsafe operations are disabled.
FIX_IDS: tuple[str, ...] = ("hide-root", "egl-fallback", "network")
