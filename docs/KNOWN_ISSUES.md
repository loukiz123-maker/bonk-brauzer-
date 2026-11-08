# Known issues and limitations

Ordered by how likely they are to bite you.

## Verified-by-design but not run on the target machine

### 1. The GUI has never been displayed

**Symptom:** none here — PySide6 cannot even import in the development
container because `libGL.so.1`, `libEGL.so.1` and `libxkbcommon.so.0` are
absent.

**What is missing:** layout polish, DPI behaviour on a real panel, whether the
key overlay is legible at 1280x720 in a window, tab ordering on a 15" screen.

**What *is* tested:** all decision logic (`gui/viewmodel.py`), the painter
geometry maths, and WCAG contrast of the palette. The widget layer
(`main_window.py`) is straightforward glue but it is untested code.

**To close:** run `lynxdroid gui` on CachyOS with Qt 6.12 and file screenshots
plus a list of anything misaligned.

### 2. No real Android session has been started by this code

Everything in `backend/waydroid.py` is exercised against recorded command
output, not a live Waydroid 1.5.0. Specific risks:

* `waydroid status` field names differ between releases; we parse loosely but
  an unknown layout degrades to "stopped";
* `waydroid session start` may print a warning on stderr that we ignore;
* `waydroid app list` column format: we take the first token as the package,
  which is correct today but not a documented contract.

**To close:** run tier 2 of `docs/TESTING.md`.

### 3. Real multitouch is unproven

`android_agent/` is source only — no SDK here, and `INJECT_EVENTS` needs a
privileged install. Until it runs, the honest state is **single-touch
available, multitouch blocked**, and the UI says exactly that.

## Deliberate limitations

### 4. Android runs in a container, not a VM

Shared host kernel means weaker isolation than QEMU/KVM. We do not offer the VM
path (stage 2 says it is a consented fallback, not the main path), and tell the
user in `capabilities.notes` and `docs/SECURITY.md`.

### 5. Multiple independent instances: not implemented

Waydroid does not document isolated multi-container operation with separate
Binder, data and system services. Opening several app windows is **not** that,
and this project refuses to blur the distinction. Marked `UNTESTED` in the
capability table, which the GUI renders as a warning banner.

### 6. Split APK / AAB / XAPK: unsupported

`apps/catalog.inspect_apk` rejects `.apks`, `.xapk`, `.aab` with an explicit
message. Installing them correctly needs a session-based installer path.

### 7. ARM-only applications will not run

The target is x86_64. ARM translation needs a licensed translator that is not
bundled, downloaded or auto-installed. The UI shows the warning unless the user
turns it off.

### 8. Google Play is not installed and not guaranteed

The GAPPS image exists upstream and needs self-certification. Play Integrity
cannot be guaranteed. We do not help with either.

### 9. `persist.waydroid.*` semantics differ by version

* `width` / `height`: read at session start → needs a restart (we say so).
* `wm size`: a *logical* override some apps ignore (we say so).
* `multi_windows`: needs a container restart.
* `fake_touch`: turns mouse input into one touch; it is **not** multitouch.

We apply each through the documented command and report the exact steps taken,
but we cannot promise a given game honours them.

### 10. Windows are not embedded

Android renders into a Wayland surface Waydroid owns. On KWin you cannot embed
a foreign surface into a Qt window. So LynxDroid shows the Android window and
applies geometry/input transforms to it; the "surface" area in our window is a
reference plate showing where touches land, not a copy of the guest image.
Claiming otherwise would be the exact deception the specification forbids.

### 11. Latency is instrumented, not measured on hardware

`input/events.FrameStats` computes median/p95 of what we can observe (event
production → channel hand-off). End-to-end glass-to-glass latency needs a
high-speed camera or a photodiode and is not claimed.

## Smaller, real bugs and rough edges

| # | Issue | Impact | Workaround |
|---|---|---|---|
| 12 | `doctor` on a non-Arch distro reports `pkgmanager=apt` and warns | none, it is honest | ignore on Arch/CachyOS |
| 13 | `Poller` keeps polling every 2 s while stopped | negligible CPU, one small subprocess every 2 s | none needed |
| 14 | Overlay is drawn over the reference plate, not over live Android video | keys appear on our plate while Android shows in its own window | use the plate for positioning, verify in-game |
| 15 | Drag-and-drop, selection and reset exist in `gui/editor.py` (37 tests) but have **never been exercised by a real mouse** — no Qt here | a drag may still behave differently under a real compositor | first thing to try on the laptop |
| 16 | D-pad analog mode (`analog_dpad`) has no GUI control | only keyboard-driven d-pad in the UI | API only |
| 17 | Mouse aim mode is implemented in the engine but not exposed in the UI | unusable for now | API only |
| 19 | The layout editor writes nothing until you press Save, and asks before discarding | one extra click | intended, but untested with a human |
| 20 | `realtime_capture` is never called: the window never requests pointer capture | aim mode cannot work even if exposed | needs stage 8 work on the laptop |
| 18 | `FakeBackend` is selectable with `--backend fake` in the GUI | could confuse a first-time user | it is labelled "test backend" everywhere |

## Defects found during development, kept on the record

These were real bugs in this repository, found by running it rather than
reading it. They are listed because a project that hides its own history is
not trustworthy about anything else.

| # | Defect | How it was caught | Status |
|---|---|---|---|
| 21 | The GUI computed touches and never sent them: `select_channel()` was never called | reading the event path while wiring the editor | fixed (`input/pipeline.py`, 23 tests) |
| 22 | `NullChannel` (a sink) claimed 10 simultaneous touches, enabling the overlay with no delivery | manual check of `capabilities()` | fixed, regression test added |
| 23 | `version --json` said `gui_available: true` while Qt's libraries could not load | running the CLI in this container | fixed, probe now load-tests |
| 24 | `doctor` told users to run `lynxdroid image prepare`, a command that did not exist | following the doctor's own advice | fixed, plus a test that resolves every hint against the parser |
| 25 | `InputError` derived from `RuntimeError`, so delivery failures printed tracebacks | writing the pipeline tests | fixed, now a `LynxError` |
| 26 | The latency readout printed `0.000 ms` for sub-microsecond work | a test asserting "0 ms" never appears | fixed, reported in µs |
| 27 | `find_for_package` / `version_mismatch` were dead code | audit for unreferenced functions | fixed, `apps launch` reports the binding |
| 28 | `hit_test` lived in a Qt module: untested and, in fact, never called | same audit | fixed (`gui/viewport.py`) |
| 29 | `ProfileStore.rename()` had no command: renaming a profile meant export → edit JSON → import | audit for unreferenced functions | fixed, `profiles rename` |
| 30 | Every profile command printed a Python traceback when the id named nothing | the rename tests, which used a wrong id | fixed, `ProfileNotFound` is reported as a sentence |
| 31 | The status bar's `latency: —` label was created and never updated, so it stayed empty forever | reading the widget list during the same audit | fixed, duplicate label removed |
| 32 | Choosing a row in the element list did not select the key on the canvas, though clicking the key did select the row | tracing `set_selected`, which nothing called | fixed, both directions now agree |
| 33 | A test asserted a measured send takes under a microsecond; it passed locally and failed on a loaded CI runner (and on 3.10, where the clock resolves to 1 µs) | the 3.10 CI job | fixed, rendering moved to the pure `format_latency_ms()` |

## Environment issues that are not bugs

* **Waydroid already installed by the user** → we never modify its config or
  images; `stop` stops the session, which may surprise someone who started it
  themselves. The status pill shows the discovered state before you act.
* **X11 session** → `session.wayland` reports ERROR with a pointer to the
  nested-compositor fallback (`cage`). Waydroid genuinely requires Wayland.
* **Secure Boot / SELinux** → we do not touch them. If binder is blocked by the
  kernel config, we say which config key is missing and stop.

## Reporting

Please include `lynxdroid doctor --json` output, `uname -r`, and the Waydroid
version. Without those three, most reports cannot be acted on.
