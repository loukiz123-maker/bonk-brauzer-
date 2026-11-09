# LynxDroid touch agent (Android side)

This directory holds an **unverified Android-side touch helper prototype**.
It does not currently provide working, verified simultaneous touch injection.
See `docs/ANDROID_RUNTIME_REVIEW_RU.md` for blocking findings.

## Why it exists

The desktop input engine can simulate multiple contacts. Delivering them to
Android requires an appropriate input mechanism and permissions. The following
are possible approaches, not verified capabilities of the standalone launcher:

| Path | Mechanism | Simultaneous touches | Privilege needed |
|---|---|---|---|
| **A (prototype)** | Companion APK attempts to inject `MotionEvent`s | unverified; current implementation is broken for multiple pointers | app must have `INJECT_EVENTS`; grantability depends on the Android image and permission policy |
| B | `uinput` virtual touchscreen on the host | device-level | root + `/dev/uinput`, and KWin must route it to the right surface |
| C | `adb shell input tap` | **1** (sequential only) | none, but useless for games |

We do **not** claim path C is multitouch. `input/channels.py` reports
`simultaneous_touches=1` for it so the UI disables the overlay instead of
silently producing single taps.

## Protocol (v1)

The draft protocol uses length-prefixed JSON (`uint32` big-endian length +
UTF-8 JSON). The desktop expects a filesystem AF_UNIX socket, while the Java
server currently creates an Android abstract socket: they do not connect as
written. See the audit before attempting to run the helper.

Client → agent:

```json
{"op": "hello", "protocol": 1, "client": "lynxdroid"}
{"op": "touch", "protocol": 1, "events": [
  {"action": "down", "pointer": 1, "x": 812.0, "y": 455.5, "t": 148213, "seq": 41}
]}
{"op": "bye"}
```

Agent → client:

```json
{"protocol": 1, "max_touches": 10, "relative_motion": true, "latency_supported": true}
{"ok": true}
{"ok": false, "error": "no permission to inject events"}
```

A refused handshake answers with the agent's own version, never an echo of the
client's:

```json
{"ok": false, "error": "protocol mismatch", "protocol": 1, "supported": 2}
```

`protocol` is what the agent speaks; `supported` is what the client asked for.
The desktop client reads the pair to print which side to update. An agent that
predates this answers `{"ok": false}` with nothing else, and the client reports
that as a refusal rather than as a version it could not read.

The intended actions are `down`, `move`, `up`, `cancel`, with pointer IDs
`1..10`. The prototype maintains a bitmap, but its multi-pointer injection
and cleanup are not correct or verified; do not rely on them.

## Building

No build is available: the repository has no Gradle build scripts or wrapper.
The helper has not been compiled or installed. `INJECT_EVENTS` is a protected
permission; an `appops` command must not be assumed to grant it. No successful
permission grant or event injection has been verified.

## Status

`NOT BUILT, NOT VERIFIED` on the target machine. The new launcher only runs
offline input tests; no Android input channel is attached.

## Files

- `app/src/main/AndroidManifest.xml` - app definition
- `app/src/main/java/id/lynxdroid/agent/TouchAgentService.java` - socket server
  and `MotionEvent` injection
