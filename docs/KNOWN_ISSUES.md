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
| 47 | The test counts in README/TESTING/ROADMAP had drifted again (four per-file rows and the total were stale); I had been fixing them by hand, which is why | writing a check for them | fixed, `tests/test_doc_counts.py` compares every quoted number with a real collection pass |
| 48 | SECURITY.md claims `core/proc.py` is the only place a subprocess is created and that nothing sets `shell=True`, and nothing verified either | reading §3 against the tests | fixed, two static checks in `tests/test_layers.py` |
| 49 | The Java touch agent and the Python client implement one protocol with nothing comparing them; the agent answered a refused handshake with `{ok: false}` while the client only expected a `protocol` field | writing the cross-implementation test | fixed on both sides, and `tests/input/test_agent_protocol.py` now holds them together |
| 50 | A protocol refusal could print "the agent speaks protocol 1, this build speaks 1" - both halves naming us, which reads as if nothing were wrong, because the agent echoed the client's version back | the test above, which failed on exactly that string | fixed: each side reports its own version |
| 51 | A `doctor` check cited requirement `P1-REALHOST`, which the registry never declared: the report said "every requirement is covered" while a check pointed at nothing | checking the registry against the code instead of against itself | fixed, the requirement is declared and the link is now enforced by a test |
| 52 | The Waydroid session log was written to `/var/lib/waydroid`, which is root-owned, so `start()` raised "cannot spawn waydroid session: No such file or directory" for every normal user - a message that blames a missing binary for what is a permission problem | starting a session in a test instead of stubbing the spawn | fixed: the log goes to `$XDG_STATE_HOME/lynxdroid/waydroid-session.log`; the `state_dir` setting that could only fail is gone, and an AST test now refuses writes to system directories anywhere in `src/` |
| 53 | A start showed "user requested start" for the whole wait (up to 90 s with a cold boot), indistinguishable from a hung start | writing a test that watched the lifecycle during a start | fixed: `_wait_boot` announces the session wait and then the boot wait; `Lifecycle.set_detail`, which had no caller, now has one |
| 54 | The GPU report paired a card with a render node using `link.endswith("0")` - the card's *index*, not part of a PCI address - so on a machine with two GPUs it could report Intel while naming NVIDIA's node, and `gpu_info()` had no test at all (125 unexercised lines) because its sysfs paths were hard-coded | describing a two-GPU machine instead of running on one | fixed: the pairing uses the PCI address; `gpu_info(drm_root=, module_root=)` takes its roots as parameters, and `tests/doctor/test_gpu_info.py` covers six described machines |
| 55 | `core/storage.py` carried two wrappers, `patched_file` and `atomic_update`, with no caller in `src/` or `tests/` - 20 unexercised lines that looked like part of the save path | the coverage report, then a grep for both names | removed; `read_json`/`write_json` are the two primitives the app uses |
| 56 | `version.py` told the reader that bumping a schema needs "a migration entry in `profiles.migration`" - a module that does not exist; the migration lives in `profiles.codec.migrate` | a test that imports every module a docstring names | fixed, and `tests/test_layers.py` now resolves those names by import |
| 57 | Ctrl-C during an image download left the `.part` file on disk: only `URLError` and `OSError` cleaned up, and `KeyboardInterrupt` is neither, so a later run found a partial image with no way to tell it from a crashed transfer | stopping a download the way a user does | fixed: any `BaseException` removes the partial file and re-raises; the interrupt still reaches the caller |
| 58 | `theme = "system"` was an accepted setting that resolved to the dark palette unconditionally: choosing "follow my desktop" gave dark on a light desktop, silently | asking what each accepted value actually resolves to | fixed: `palette_for` takes the desktop's colour scheme and `main_window` reads it from `QGuiApplication.styleHints()`; the palette itself still needs a display to be seen, so the rendered result stays `ЗАБЛОКИРОВАН` |
| 59 | "Explicit consent" for root-level actions was a `consent: bool = False` argument, so any caller could pass `True`; `apply_display` did exactly that when calling its own `set_prop`, and the guarantee rested on four call sites staying honest rather than on anything the code could check | asking what would stop a caller from asserting the user's agreement | fixed: `Consent` can only be produced by `user_consented()`, both `set_prop` and `images.download` reject anything that is not one (checked with `isinstance`, since annotations are not enforced at run time), and the CLI/GUI create it at the point where the user answered yes |
| 60 | `apply_display` without consent still ran two `prop get` calls and put "1280 -> 1920" into the steps it returned: it checked at the first write instead of before doing anything, so an unauthorised call reported a change that never happened | the new test, which asserted no subprocess ran at all | fixed: the refusal is the first thing in the function |
| 61 | Six settings were saved, clamped, documented in no place, and read by nothing: `input_mode`, `fit_mode`, `fullscreen`, `capture_pointer`, `allow_service_control`, `allow_prop_writes`. Changing any of them did nothing, and two of them were the opt-ins SECURITY.md promised | asking, for each field, which line reads it | fixed: each has a reader, `tests/test_settings_are_read.py` fails when a field has none, and `docs/SETTINGS.md` names the reader for every row |
| 62 | "Root actions require opt-in" was documented while `_ensure_container` ran `systemctl start` unconditionally; `apply_display` passed `consent=True` to its own `set_prop` | reading the promise next to the code | fixed: both opt-ins are off by default and enforced with `isinstance`, and the refusal names the exact command the user can run instead |
| 63 | `fit_mode` was saved on every change and never restored, so the combo box always began at `contain` while the file said otherwise; `fullscreen` could not be set at all because the only code that knows (the F11 toggle) never saved | opening the program twice with the same settings | fixed: both are restored at startup and written back on change |
| 64 | `lynxdroid-gui` was declared in `pyproject.toml` and appeared in no document, and there was no `.desktop` entry: on KDE the product could only be started from a terminal | looking for the app in the menu it was never installed into | fixed: `packaging/lynxdroid.desktop` plus `scripts/install-desktop-entry.sh` (user-level, no sudo), both covered by `tests/test_packaging.py` |
| 65 | `desktop-file-validate` was not available in this sandbox, so the entry was checked by parsing it rather than by the freedesktop tool | trying to run the validator | **open**: the entry passes the project's own checks; the upstream validator must be run on the CachyOS machine |
| 66 | `tests/test_packaging.py` imported `tomllib` at module level; it is stdlib only from Python 3.11 and the package supports 3.10, so `pytest (3.10)` failed at collection and every test in that file was lost - 3.11 and 3.12 were green | the `pytest (3.10)` CI job, which is the only place the difference shows | fixed: the import is inside a function with a small fallback parser, and `tests/test_layers.py::TestPython310Compatibility` now refuses module-level post-3.10 imports anywhere in the tree |
| 67 | Two commands answered "can this machine run Android" differently: `doctor --strict` counted `session.wayland = UNKNOWN` as blocking, while the first `HostProfile` reported the same fact as merely unexamined and could stay non-blocking | a live comparison of the two verdicts on the same container | fixed: the profile treats an unchecked prerequisite as not-a-green-light, matching `Report.blockers_for`, and `tests/doctor/test_host_profile.py::TestTheTwoVerdictsAgree` plus `tests/cli/test_cli.py::TestProfileCommand` now compare both commands on the live host |

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
