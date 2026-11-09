"""Why a game refuses to run, and what can honestly be done about it.

The screenshots that started this module show two different failures on a
machine where the emulator itself was working: the game launched, rendered,
and then closed itself with its own message. That distinction is the whole
point of this file. "The emulator is broken" and "the application checked
something and refused" are different problems with different answers, and the
application is the only party that can make the second one impossible to fix.

So this module does not promise that any game will run. It does three things:

* names the checks a game performs, as :class:`Signature` objects, so the
  failure is matched to a known cause instead of guessed at;
* offers :class:`Fix` objects that change something we are allowed to change;
* says out loud when a failure is **not fixable by us** - a game that forbids
  emulators or root by policy is out of our reach, and pretending otherwise
  would send the user to a forum thread instead of the truth.

Three fixes exist because three are honest:

``hide-root``
    Waydroid's session runs as root and the image is a ``test-keys`` build.
    Setting the three properties that expose this to ``0`` makes the ordinary
    checks pass. It does **not** defeat a Play Integrity check, which attests
    the whole boot chain and cannot be satisfied by a property write. Games
    that ask that question are reported as unfixable rather than "try again".
``egl-fallback``
    The container renders through the host GPU. Without a working DRM node the
    surface comes up black or the process dies while creating its context, and
    games that probe GLES extensions fall over. Forcing a software-friendly
    renderer path is a trade - lower frame rate for a picture at all - so it
    is a separate fix the user can undo, never a default.
``network``
    Waydroid's own NAT is the usual reason a game reaches its title screen and
    then cannot open a socket. The check is a real TCP connect from *inside*
    Android, not a ping from the host, because the host's connectivity says
    nothing about the container's.

Every fix records what it did to the system, so ``revert`` can undo it and a
user who does not like the result is never stuck.
"""

from __future__ import annotations

import logging
import re
from collections.abc import Callable, Iterable, Mapping, Sequence
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
        fix="hide-root",
        support=Support.FIXABLE,
        explanation=(
            "Waydroid runs Android as root and the image is signed with test keys, "
            "so the game's ordinary root check succeeds. Hiding it makes the common "
            "checks pass without deceiving a hardware attestation."
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
            "not supported on this device",
            "не поддерживается на этом устройстве",
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
        title="root is unavailable because Android never started",
        markers=(
            "waydroid",
            "container",
            "binder",
        ),
        fix=None,
        support=Support.UNKNOWN,
        explanation=(
            "This message names the container itself, so the failure happened "
            "before the game's own checks. Run 'lynxdroid doctor' first."
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
        fix="egl-fallback",
        support=Support.FIXABLE,
        explanation=(
            "Without a usable GPU node the container falls back to a path some "
            "engines cannot initialise. A software-friendly renderer is slower but "
            "boots; it is a change you can undo."
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
        support=Support.FIXABLE,
        explanation=(
            "The title screen appeared, then a socket failed. Usually Waydroid's "
            "NAT or DNS inside the container, which is testable from inside Android "
            "rather than from the host."
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
    for signature in SIGNATURES:
        if signature.matches(message):
            return signature
    return None


# ---------------------------------------------------------------------------
# The properties that make Android look rooted, and the values that stop it
# ---------------------------------------------------------------------------

#: (property, clean value, why this one). Each is checked by the game's own
#: `Build`/`SystemProperties` lookups, not by us.
ROOT_PROPERTIES: tuple[tuple[str, str, str], ...] = (
    (
        "ro.debuggable",
        "0",
        "the image is built debuggable, which alone tells a game it is unlocked",
    ),
    (
        "ro.secure",
        "1",
        "0 here means adb runs as root without asking",
    ),
    (
        "ro.build.tags",
        "release-keys",
        "test-keys is the signature every check looks for first",
    ),
)

#: Written as one string into Android's own property store.
ROOT_PROPERTY_NAMES: tuple[str, ...] = tuple(name for name, _v, _w in ROOT_PROPERTIES)


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


def _set_many(
    target: CompatTarget,
    values: Mapping[str, str],
    *,
    consent: Consent,
    result: FixResult,
) -> bool:
    """Write *values*, remembering what was there. False if any write failed."""
    ok = True
    for name, value in values.items():
        previous = target.get_prop(name)
        result.restore[name] = previous
        if previous == value:
            result.steps.append(Step(f"{name} = {value}", False, "already set"))
            continue
        wrote = target.set_prop(name, value, consent=consent)
        result.steps.append(
            Step(
                f"{name} = {value}",
                wrote,
                f"was {previous or '<unset>'}" if wrote else "the backend refused the write",
            )
        )
        ok = ok and wrote
    return ok


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
        """Make the ordinary root checks come up clean.

        Only the three properties that expose the build are touched. Nothing
        here interferes with Play Integrity, and the docstring says so - a fix
        that oversells itself is how a user ends up blaming the emulator for a
        vendor policy.
        """
        result = FixResult("hide-root", Outcome.APPLIED)
        ok = _set_many(
            self._target,
            {name: value for name, value, _why in ROOT_PROPERTIES},
            consent=self._consent,
            result=result,
        )
        if not ok:
            result.outcome = Outcome.FAILED
            result.message = (
                "some properties could not be written; enable allow_prop_writes and retry"
            )
            return result
        if not result.changed:
            result.outcome = Outcome.ALREADY
            result.message = "root indicators were already hidden"
            return result
        result.message = (
            "root indicators are now hidden from ordinary checks; "
            "a Play Integrity attestation is not affected and not fooled"
        )
        return result

    def revert_hide_root(self) -> FixResult:
        """Put back whatever :meth:`hide_root` found."""
        result = FixResult("hide-root", Outcome.APPLIED)
        previous = {name: self._target.get_prop(name) for name in ROOT_PROPERTY_NAMES}
        restore: dict[str, str] = {}
        for name, clean, _why in ROOT_PROPERTIES:
            original = _ORIGINAL_DEFAULTS.get(name)
            current = previous[name]
            if current == clean and original is not None:
                restore[name] = original
            elif current is not None:
                restore[name] = current
        if not restore:
            result.outcome = Outcome.ALREADY
            result.message = "nothing to restore"
            return result
        ok = _set_many(self._target, restore, consent=self._consent, result=result)
        if not ok:
            result.outcome = Outcome.FAILED
            result.message = "could not restore every property; the values are listed above"
            return result
        if not result.changed:
            # Everything already held the stock value, so the revert is a
            # no-op. Reporting APPLIED here would tell the user something was
            # undone when nothing was, which is the same lie in the other
            # direction as claiming a fix worked when it did not.
            result.outcome = Outcome.ALREADY
            result.message = "nothing to restore: the build already looks stock"
            return result
        result.message = "the properties that marked the build as unlocked are back"
        return result

    def egl_fallback(self) -> FixResult:
        """Ask the container for the software-friendly renderer path.

        The real lever is ``persist.waydroid.renderer``; older versions ignore
        it, which is why the step is marked not-done rather than reported as a
        success we cannot see.
        """
        result = FixResult("egl-fallback", Outcome.APPLIED)
        wanted = {"persist.waydroid.renderer": "swiftshader"}
        ok = _set_many(self._target, wanted, consent=self._consent, result=result)
        if not ok:
            result.outcome = Outcome.FAILED
            result.message = "the renderer property was refused by this Waydroid version"
            return result
        result.message = (
            "software rendering is requested; the frame rate will drop and the "
            "change takes effect on the next session start"
        )
        return result

    def network_diagnosis(self) -> FixResult:
        """Test the container's own egress, without changing anything.

        A fix that silently rewrites DNS inside Android is not something we can
        verify here, so this returns evidence instead of an action: the user's
        own failure is almost always visible in the first line.
        """
        result = FixResult("network", Outcome.APPLIED)
        for label, command in (
            ("dns", ["ping", "-c", "1", "-W", "2", "1.1.1.1"]),
            ("route", ["ip", "route"]),
        ):
            code, output = self._target.shell_probe(command)
            result.steps.append(
                Step(label, code == 0, (output.strip().splitlines() or [""])[0][:120])
            )
        dns_ok = bool(result.steps and result.steps[0].done)
        if dns_ok:
            result.message = (
                "the container reaches the network; the game's own servers may be "
                "geo-blocked or the title may check something else"
            )
        else:
            result.message = (
                "the container has no working egress; Waydroid's NAT or DNS needs "
                "attention before the game can reach its server"
            )
            result.outcome = Outcome.UNAVAILABLE
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
            if outcome.outcome is Outcome.FAILED:
                break
        return results


# What a stock Waydroid image has, used when reverting rather than guessing.
_ORIGINAL_DEFAULTS = {
    "ro.debuggable": "1",
    "ro.secure": "0",
    "ro.build.tags": "test-keys",
}

#: Fix ids, in the order they are worth trying.
FIX_IDS: tuple[str, ...] = ("hide-root", "egl-fallback", "network")

#: Names that are safe to write as a property (guards ``restore``).
_PROP_NAME_RE = re.compile(r"^[A-Za-z0-9_.\-]{1,64}$")


def is_prop_name(name: str) -> bool:
    """True for a name we are willing to write back."""
    return bool(_PROP_NAME_RE.match(name))
