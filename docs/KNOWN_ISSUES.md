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
| 15 | Drag-and-drop, selection and reset exist in `gui/editor.py` (40 tests) but have **never been exercised by a real mouse** — no Qt here | a drag may still behave differently under a real compositor | first thing to try on the laptop |
| 16 | D-pad analog mode (`analog_dpad`) has no GUI control | only keyboard-driven d-pad in the UI | API only |
| 17 | Mouse aim mode is wired end to end (key grabs, motion is delivered, exit key releases) but has **never been tried with a real mouse or against a real game** | relative-motion scaling under a compositor is guesswork until measured | first thing to try on the laptop |
| 18 | `FakeBackend` is selectable with `--backend fake` in the GUI | could confuse a first-time user | it is labelled "test backend" everywhere |
| 19 | The layout editor writes nothing until you press Save, and asks before discarding | one extra click | intended, but untested with a human |
| 20 | Pointer capture (`grabMouse`) is requested from the canvas and released on focus loss, exit key and profile switch — but only Qt's behaviour on the target compositor can confirm it really captures | if capture fails, the cursor will leave the window mid-aim | verify on the laptop |

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
| 34 | `binder_supported_in_filesystems()` was never called: on a kernel whose config is unreadable, `doctor` said "cannot tell" while `/proc/filesystems` could have answered | same audit | fixed, the doctor uses it as a second source |
| 35 | `kernel_report()` - the check that decides whether Android can boot at all on this kernel - had no tests | adding the `/proc/filesystems` branch | covered by `tests/doctor/test_kernel_checks.py` |
| 36 | `request_stop_during_start()` changed the detail but published no snapshot, so pressing Stop while STARTING left the screen saying "starting" | tracing `set_detail`, which nothing called on this path | fixed, observers are now told |
| 37 | Aim mode's first contact opened with MOVE, not DOWN: `_move()` let `_contact_for()` emit the DOWN into the queue but did not return it, and the window delivers only what is returned | writing the aim-mode tests | fixed, Android now sees the touch begin |
| 38 | The grab key of a mouse element was ignored by the engine while `key_conflicts()` counted it, so a profile could warn about a conflict that could never happen | same tests | fixed, `keys` and `exit_key` are both bound |
| 39 | `Report.ok_for()` and `doctor.prerequisites_state()` each implemented "which checks block us", so the CLI and the GUI could disagree about whether Android may start | same audit | fixed, one implementation (`blockers_for`) |
| 40 | The aspect-ratio warning said only "authored for a different aspect ratio"; `ReferenceResolution.relayout_hint` already had the sentence that tells the user to re-check key positions | same audit | fixed, the model's wording is shown |
| 41 | Held elements looked idle: the overlay was lit from live contacts only, so a HOLD or a mouse grab showed nothing while it was working | tracing `press_state`, which nothing called | fixed, contacts and held keys are shown together |
| 42 | `Element.as_pointer_displacement()` always returned `(0, 0)`, `DisplayRequest.as_async_clamp_string()`, `TouchEvent.with_seq()` and `EventQueue.peek_last()` were never called | same audit | removed rather than left as an API that promises nothing |
| 43 | `AndroidBackend.show_full_ui()` was implemented for both backends and never called: there was no way to put the Android window on screen | end of the same audit | fixed, `lynxdroid show` |
| 44 | `Poller.pause()`/`resume()` existed and nothing called them, while the docstring promised polling stops when the window is hidden | same audit | fixed, `hideEvent`/`showEvent` pause and resume it |
| 45 | `BackendState.is_transitional()`, `Action.ends_contact()`, `AppEntry.initials()` and `Profile.with_reference()/with_target()` were never called | same audit | removed |
| 46 | The schema-version claim ("profile JSON written by Python must load in C++ unchanged") was the stated gate for the C++ port and had no test; `migrate()` was never executed by anything | writing the gate tests | covered by `tests/profiles/test_compat.py`, which also pinned the deliberate whole-second timestamp precision |

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
