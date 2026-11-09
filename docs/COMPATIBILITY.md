# Game compatibility: what we can fix, and what we cannot

This page exists because of two screenshots. In both, LynxDroid had done its
job: Waydroid booted, the game launched, the picture appeared. The game then
closed itself. **"The emulator is broken" and "the application checked
something and refused" are different failures**, and only the first one is
ours to repair.

## The honest boundary

A game runs inside an Android image that is not a phone. Every title is free
to inspect that and refuse. Nothing in this project changes a decision a
vendor makes deliberately, and nothing here tries to defeat a hardware
attestation:

* **fixable** — the game asks a question we can honestly answer: is this build
  debuggable, does it have root, can it reach the network, can it create a
  rendering surface;
* **not fixable by us** — the game forbids emulators by policy, or demands a
  Play Integrity verdict signed by Google about the whole boot chain. Faking
  that is out of scope by `docs/SECURITY.md` and would break the trust the
  rest of the application is built on;
* **unknown** — the message matches nothing we know. It is reported as
  unknown, never as "try again", because that wastes the user's evening.

So the promise is not "any game runs". The promise is: **we tell you which of
the three you are looking at, in one command, before you change anything.**

## Diagnosing what the game said

```console
$ lynxdroid game explain "Не удалось войти на сервер: 3"
diagnosis  the game could not reach its server
  The title screen appeared, then a socket failed. Usually Waydroid's NAT or
  DNS inside the container, which is testable from inside Android rather than
  from the host.

  try: lynxdroid game fix network --yes
```

Exit codes make the answer scriptable:

| Code | Meaning |
|---|---|
| `0` | recognised, and there is something to try |
| `5` | recognised as **not fixable by us**, or not recognised at all |

## The three fixes

Each one changes persistent Android state, so each needs `--yes`, and the
change is recorded so it can be undone.

### `hide-root`

Waydroid runs Android as root, and the image is signed with test keys. Games
look at three properties:

| Property | Clean value | What it reveals when wrong |
|---|---|---|
| `ro.debuggable` | `0` | a debuggable build is an unlocked build |
| `ro.secure` | `1` | `0` means adb runs as root without asking |
| `ro.build.tags` | `release-keys` | `test-keys` is the first thing every check looks for |

```console
$ lynxdroid game fix hide-root --yes
hide-root: applied - root indicators are now hidden from ordinary checks;
  a Play Integrity attestation is not affected and not fooled
  [done] ro.debuggable = 0  (was 1)
  [done] ro.secure = 1  (was 0)
  [done] ro.build.tags = release-keys  (was test-keys)
```

That last sentence is the important one. This passes an ordinary root check.
It does **not** pass Play Integrity, and no property write can.

Undo it:

```console
$ lynxdroid game revert --yes
```

The revert writes back the stock values of the Waydroid image. Reverting a
system that was never changed reports `already-in-place` rather than claiming
to have restored something.

### `egl-fallback`

When the container cannot get a usable GPU node, it falls back to a renderer
path some engines cannot initialise — the picture is black, or the process
dies while creating its context. This asks for the software-friendly path:

```console
$ lynxdroid game fix egl-fallback --yes
```

The trade is explicit: **lower frame rate for a picture at all**. It is a
separate, undoable fix, never a default.

### `network`

Waydroid's own NAT and DNS are the usual reason a title screen appears and
then no server answers. This fix changes nothing — it **tests** the container's
own egress from inside Android and reports what it found:

```console
$ lynxdroid game fix network --yes
network: unavailable - the container has no working egress; Waydroid's NAT or
  DNS needs attention before the game can reach its server
  [not ] dns  Network is unreachable
  [done] route  192.168.240.0/24 dev eth0
```

A ping from the host proves nothing about the container's network, so the
probe runs inside Android.

## What to send us

If a game still refuses after `game explain`, send the text it displayed and
the output of:

```console
$ lynxdroid profile --json > profile.json
$ lynxdroid game explain "<the text>" --json
```

Two documents: what the machine is, and what the game said. That is enough to
tell a fixable refusal from a vendor decision without guessing.

## The known refusal list

| Signature | Recognised by | Support |
|---|---|---|
| `root-detected` | "root-права", "rooted", "superuser", "su binary" | fixable — `hide-root` |
| `integrity-attestation` | "play integrity", "device not certified", "attestation" | **not fixable** |
| `emulator-detected` | "эмулятор", "emulator", "not supported on this device" | **not fixable** |
| `binder-root` | "waydroid", "container", "binder" | unknown — run `doctor` first |
| `renderer` | "egl", "gles", "surfaceflinger", "gralloc", "vulkan" | fixable — `egl-fallback` |
| `network` | "сетевое подключение", "не удалось войти на сервер", "connection failed" | testable — `network` |

Attestation is matched *before* the plain root signature on purpose: a message
naming both must get the stricter answer, because sending a user to hide root
when the game wants a signed boot chain wastes their evening.

This table is a claim we can defend, not a complete taxonomy. Everything else
is reported as unknown, which is the truth.
