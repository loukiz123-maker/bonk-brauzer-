# LynxDroid: standalone Android gaming engine architecture

**Status:** architecture draft for a Linux-hosted Android gaming runtime. Waydroid is the current prototype candidate, not a guaranteed or final backend. This document does not declare compatibility with every game or with BlueStacks. It also does not authorize or implement a connection to the legacy Android agent.

## Product boundary

LynxDroid is a standalone runtime launcher and guest-control product. It must not depend on Bonk's browser window, DOM, X11 event loop, or browser storage. In this checkout, `bonk waydroid` is a command-line launcher around a user-installed Waydroid CLI; it opens Waydroid's own UI. It is not yet the standalone graphical launcher, an embedded display surface, or a complete game-control product.

```text
LynxDroid UI / CLI (user session; explicit consent)
           │ local, versioned control API — interface not frozen
           ▼
LynxDroid host controller (unprivileged)
   ├── Runtime backend: Waydroid session lifecycle
   ├── Guest bridge: selected Android guest / ADB operations
   ├── App workflow: selected APK + exact package ID
   ├── Frame provider: not implemented as a stream
   └── Input provider: coarse ADB commands; in-guest delivery unverified
           │                         │
           ▼                         ▼
Waydroid host service/container   Android guest/apps
```

The runtime process, Android guest, app, frame stream, and input delivery are separate capabilities and must not be collapsed into one `running` flag.

## State and ownership model

Keep at least these independent observations in API/UI responses:

- **Runtime:** stopped, starting, running, stopping, exited, or failed. This is host-process/service state only.
- **Guest:** disconnected, unauthorized/offline, booting, boot-property-complete, or unknown. ADB `sys.boot_completed=1` is not proof that the launcher or a game is ready.
- **App:** unknown, install requested, install command succeeded/failed, launch requested, or launch command succeeded/failed. A zero exit status does not establish a visible or responsive app.
- **Input:** requested, transport accepted/rejected, and—only with an in-guest witness—observed by the target app. A successful `adb shell input` exit code alone cannot establish delivery to the game.
- **Frames:** unavailable, snapshot available, or streaming. A single PNG screenshot is not a frame stream and must not be reported as interactive video.

These layers are now represented explicitly in `include/bonk/android/engine_state.hpp` and rendered by `bonk waydroid` as stable `engine ...` lines. The renderer is the contract the host runbook greps; a state is only printed as fact when it was actually observed. Only the controller that explicitly started a runtime may stop its owned foreground process. Waydroid session termination can affect a shared host container, so ownership and the stop consequence must be visible. Never take over or stop a pre-existing session implicitly.

## Startup and game-launch flow

1. **Inspect, read-only.** Query the installed Waydroid CLI and report session/container state. Do not initialize images, download system files, use `sudo`, or alter host permissions.
2. **Preflight the desktop session.** Check that the selected executable and current user Wayland/D-Bus environment are usable. Binder/GPU availability and image initialization need target-host validation; do not infer them from process creation.
3. **Obtain explicit start consent.** If Waydroid is stopped, start only after the user confirms. Supervise the foreground session and wait for `waydroid status` to report running, bounded by a timeout. If a session was already running, do not claim ownership.
4. **Open the runtime UI.** The initial supported display is Waydroid's own window. Embedding it in Bonk or another host window is a separate compositor/frame-transport project.
5. **Install only a user-selected APK.** Require a separate confirmation for that exact absolute path. Do not infer package IDs from filenames or silently replace/downgrade an install.
6. **Launch by exact package ID.** Treat the host command result as a request result, not app readiness. Verify the guest's foreground activity or an app-side smoke-test signal.
7. **Verify the input path inside Android.** Use a test app/agent that records received actions. Collect evidence for tap, swipe, key, and any later control mapping. Do not connect the legacy agent until its address namespace, peer/client verification, and protocol version are agreed.
8. **Stop deliberately.** On Ctrl-C/SIGTERM, stop only the process group owned by LynxDroid and report the final runtime and guest states. Record whether the shared Waydroid container also stopped.

## Process and module boundaries

- **Launcher/controller:** user-facing consent, runtime selection, bounded orchestration, status reporting, APK choice, package ID choice.
- **Runtime adapter:** Waydroid-specific status/start/UI/install/launch commands. It does not own Android boot semantics or change host configuration.
- **Guest bridge:** ADB transport selection, guest boot polling, package and input operations. ADB server auto-start remains a separate explicit consent boundary.
- **Frame provider:** future source of frames with width, height, pixel format, stride, rotation, sequence, and monotonic timestamp. Keep frame bytes separate from control messages; don't base64 a video stream into JSON.
- **Input provider:** future abstract actions (tap, path/gesture, key-down/up, cancellation, pointer identity). The current ADB shell implementation is a coarse prototype, not a low-latency game-control transport.
- **Profile storage/UI:** package-ID-based profiles and safe persistence/deletion are a separate layer. Do not edit the QA-owned profile validation, web preview persistence, or agent file-deletion areas in this branch.
- **IPC:** a private local Unix-domain transport is a candidate only. Before defining or connecting a protocol, agree versioning, message schema, socket namespace/address, peer authentication, limits, timeout/reconnect rules, and ownership. No unauthenticated TCP listener.

## Explicitly deferred integration

The old `TouchAgentService` and its Python client are not treated as a compatible bridge. The known abstract-namespace versus filesystem-path mismatch is a hard blocker until both sides agree on the exact address, client verification, and protocol version. This architecture draft does not alter either endpoint and does not claim agent compatibility.

## Acceptance gates

Each gate requires evidence from a real target Linux/Waydroid host; synthetic CLI/ADB fakes are useful regression tests but do not satisfy these gates.

| Gate | Required evidence |
|---|---|
| Guest boot | Waydroid session/container status plus ADB device state and `sys.boot_completed` (via the consent-gated `--adb ... --confirm-adb` probe); record image/version and host environment. Evidence must show `engine guest=boot-complete observed=yes`. |
| Test APK | APK source/hash, exact package ID, installation result, and a guest foreground witness: `engine app=launch-command-succeeded ... foreground-confirmed=yes` from `dumpsys activity activities`. A successful CLI exit alone is insufficient. |
| Input delivery | A test app inside Android records the expected tap/swipe/key event and coordinates/order. Capture its guest-side log or test result; host command success alone is insufficient. The `engine input ... observed-in-guest=` token must be `yes` for this gate. |
| Frame path | Repeated frames with format/stride/rotation/sequence/timestamps and measured latency; a static PNG fixture is not evidence. |
| Game compatibility | Test each named game on a documented image/ABI/GPU/driver configuration. Track ARM translation, Google services/Play Integrity, DRM and anti-cheat limitations; never promise “any game” without measured evidence. |

### Current Android evidence (this sandbox)

| Requested result | Status | Reason |
|---|---|---|
| Android guest boot | **Not run** | No Waydroid/ADB, Wayland/D-Bus session, Binder device, GPU render node, or `/dev/kvm` is available. |
| Test APK launch inside Android | **Not run** | No real guest or test APK is available; the synthetic CLI fixture is not an installable APK. |
| Input event observed inside Android | **Not run** | No guest-side test app/agent is available to record a tap/swipe/key event. |

These gates are still open; do not represent synthetic CLI/ADB fakes as live Android results.

## Parallel QA ownership

This branch's architecture work must avoid the QA-owned edits to `engine_session.cpp` / `engine_session.hpp` process-stop behavior, Adam7 decoding in `src/image/png.cpp`, CMake test working-directory configuration, and their associated regression tests. The separate QA branch owns the profile validation/persistence, Android-agent file-deletion, licensing metadata, and associated tests. Integrate those changes only after QA's patches and checks are available; resolve conflicts by preserving both sides, then rerun the complete build/test and sanitizer checks. This draft introduces no edits to those files.
