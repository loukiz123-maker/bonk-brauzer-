# LynxDroid touch agent (Android side)

This directory holds the **Android-side helper** that turns LynxDroid's touch
messages into real `MotionEvent`s with genuine simultaneous pointers.

## Why it exists

LynxDroid's desktop side can compute multitouch perfectly, but Android only
accepts injected multi-pointer input from a process with the right privilege.
There are exactly three honest options, and this project implements only the
first as a real path:

| Path | Mechanism | Simultaneous touches | Privilege needed |
|---|---|---|---|
| **A (this app)** | Companion APK injects `MotionEvent`s | up to 10 | app must hold `INJECT_EVENTS` (system/priv-app or `adb shell appops` grant on a debug image) |
| B | `uinput` virtual touchscreen on the host | device-level | root + `/dev/uinput`, and KWin must route it to the right surface |
| C | `adb shell input tap` | **1** (sequential only) | none, but useless for games |

We do **not** claim path C is multitouch. `input/channels.py` reports
`simultaneous_touches=1` for it so the UI disables the overlay instead of
silently producing single taps.

## Protocol (v1)

Length-prefixed JSON over an **AF_UNIX** socket in `$XDG_RUNTIME_DIR`
(never a TCP port - see stage 14). All frames are `uint32` big-endian length +
UTF-8 JSON.

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

Actions are `down`, `move`, `up`, `cancel`. `pointer` is `1..10` and must be
unique while the contact is open. The agent keeps a pointer-id bitmap and
rejects a `down` on an id that is already down, so a desktop bug cannot leave a
phantom finger on screen.

## Building

Requires an Android SDK (the host project does not need it; this is a separate
artefact):

```bash
cd android_agent
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

The APK is **not** committed here: building it needs a licensed SDK and, for
the full feature, a privileged install. A debug image where
`adb shell appops set id.lynxdroid.agent INJECT_EVENTS allow` works is enough
for path A.

## Status

`NOT BUILT, NOT VERIFIED` on the target machine. The desktop side treats a
missing agent as "no multitouch", shows the reason in the UI, and falls back to
single-touch taps rather than pretending.

## Files

- `app/src/main/AndroidManifest.xml` - app definition
- `app/src/main/java/id/lynxdroid/agent/TouchAgentService.java` - socket server
  and `MotionEvent` injection
