# LynxDroid

Standalone game launcher for Linux, with an independent Android runtime planned.
The current application is a launcher prototype, not an Android emulator yet.

## New direction: standalone launcher

The desktop entry and `lynxdroid gui` now open the **standalone launcher**,
without starting or contacting Waydroid. It can show a local APK library,
filter it, add one or several APK file references, remove references, copy a file path, open its folder,
and switch to full screen with F11
(Esc exits full screen). Adding an APK does **not** install or run it.
The left-hand sidebar is removed; the right-hand toolbar provides navigation,
settings, APK import and the control editor. The central desktop is intentionally
empty until an Android runtime exists. The editor opens
beside its canvas, and can save a layout draft,
including a fire marker. Unsaved edits are discarded on leaving the editor.
The draft does not inject input into games. A local dry-run connects supported
single-key tap and fire drafts to the existing input engine and encodes touch
messages using the scrcpy v5 protocol, without opening a connection. The web
preview demonstrates the same result independently; it does not run that engine. Engine
settings, Google Play and Android apps are not implemented.

The independent Android runtime, game compatibility, performance and actual
Android application execution are **not implemented or verified**. The
Waydroid-specific CLI and old code below are historical and are not the new
launcher engine. Existing user files are preserved.

Install the GUI and start it:

```bash
pip install -e '.[gui]'
lynxdroid gui
```

The Python Qt runtime and desktop graphics libraries must be installed. The
launcher stores its library under the user's XDG data directory and never
deletes an APK when removing its reference.

## Previous Waydroid implementation (historical)

The sections below document the earlier Waydroid-based implementation and CLI.
They are retained for reference; they are not requirements for the new launcher.

## Honest status first

Read this before anything else. It is the difference between "works" and
"should work".

| Area | State | Evidence |
|---|---|---|
| Unit tests (897) | **VERIFIED** | run in this repository, `pytest`; ruff + strict mypy clean |
| Geometry, profiles, storage, state machine | **VERIFIED** by tests | `tests/` |
| CLI + doctor on this machine | **VERIFIED** (headless Debian container) | `lynxdroid doctor` output below |
| GUI rendering on a display | **NOT VERIFIED** | this sandbox has no `libGL`/Wayland; widgets never instantiated |
| Waydroid session on CachyOS | **BLOCKED** | target laptop not reachable from here |
| Intel Iris Xe hardware rendering | **BLOCKED** | no `/dev/dri` here |
| Real multitouch injection | **BLOCKED** | needs the Android-side agent, see `android_agent/` |
| Multi-instance | **NOT STARTED** | deliberately, per stage 12 |

The specification is explicit: a stage is not finished because code exists, it
is finished when the documented acceptance test passed **on the target
machine**. Everything below the "VERIFIED" line above requires your laptop.

## What actually runs right now

Diagnostics are real and read-only. From this container:

```console
$ lynxdroid doctor --quiet
▲ WARNING Linux distribution    Debian GNU/Linux 12 (bookworm)
           next: this build targets CachyOS/Arch; other distributions need
                 different package and service handling …
? UNKNOWN Wayland compositor    no graphical session detected
✘ ERROR   XDG runtime directory missing
✘ ERROR   Android Binder IPC    this kernel was built without CONFIG_ANDROID_BINDER_IPC
? UNKNOWN DRM GPU device        /sys/class/drm is absent (no DRM subsystem)
? UNKNOWN waydroid CLI          waydroid not found and the package manager is unknown
✘ ERROR   Android images        no Android images installed yet

verdict: BLOCKED - these required checks are not OK:
  - kernel.binder: this kernel was built without CONFIG_ANDROID_BINDER_IPC
  - waydroid.binary: waydroid not found and the package manager is unknown
  …
```

Note the three-way distinction the specification demands: **ERROR** (checked,
missing), **UNKNOWN** (could not determine), **WARNING** (works with a caveat).
Nothing is reported as "unsupported" just because we could not look.

## Install

### On CachyOS / Arch (the target)

```bash
# 1. the Android backend itself (upstream package, not ours)
sudo pacman -S waydroid

# 2. LynxDroid (this project)
git clone <this repository> lynxdroid && cd lynxdroid
./scripts/install.sh                 # venv, install, and it verifies itself
. .venv/bin/activate
lynxdroid doctor                     # read-only, changes nothing
```

`scripts/install.sh` does the venv, the install and a self-check (the CLI
starts, the test suite passes). It never runs `sudo` and never touches anything
outside `.venv/`: installing Waydroid and picking a binder-capable kernel are
your decisions, so it prints them instead of performing them. The long form, if
you would rather do it yourself:

```bash
python -m venv .venv && . .venv/bin/activate
pip install -e '.[gui,dev]'          # [gui] pulls PySide6
```

Kernel requirement: `CONFIG_ANDROID_BINDER_IPC` and (preferably)
`CONFIG_ANDROID_BINDERFS`. `linux-cachyos`, `linux-cachyos-lts` and `linux-zen`
provide them; `lynxdroid doctor` tells you what **your** running kernel has
instead of assuming.

### Anywhere else

The CLI, doctor and the entire test suite need only the standard library:

```bash
pip install -e .                     # no PySide6, no Qt
lynxdroid doctor --json | jq .
pytest                               # 897 collected; Qt tests may skip without native libraries
```

The GUI is an optional extra on purpose: the interesting logic lives in
`gui/viewmodel.py` and is tested without Qt.

## First run

```bash
lynxdroid doctor                 # 1. what does this machine actually support?
lynxdroid image plan             # 2. sources, licence, size, disk check (no download)
lynxdroid status                 # 3. backend state, layers separately
lynxdroid start                  # 4. bring Android up (waits for real boot completion)
lynxdroid show                   #    optionally: open the Android window
lynxdroid apps list              # 5. installed packages
lynxdroid display presets        # 6. viewport options
lynxdroid display set 1280 712 240 --yes
lynxdroid gui                    # 7. the window
```

To start the window from the application menu instead of a terminal:

```bash
./scripts/install-desktop-entry.sh          # user-level, no sudo
./scripts/install-desktop-entry.sh --remove
```

The entry is written to `$XDG_DATA_HOME/applications` with an `Exec` line that
names the interpreter which can actually import the package, so it keeps
working from a session where no virtualenv is active.

`lynxdroid start` waits for `sys.boot_completed=1` inside the guest. A running
session process is **not** reported as "Android running" - see `Snapshot`
readiness layers (`process`, `container`, `android`).

## The four things people confuse

Stage 7 of the specification asks for this to be separated, and it is, in code:

| Concept | Changed by | Takes effect | Our type |
|---|---|---|---|
| Video buffer size | `persist.waydroid.width/height` | after session restart | `DisplayRequest` |
| Android logical viewport | `wm size` | live, ignored by some apps | `ApplyMode.LIVE` |
| Density | `wm density` | live | `DisplayRequest.dpi` |
| Window scale | our window, `fit` mode | immediately | `ViewportGeometry` |

`FitMode.CONTAIN` letterboxes with genuinely black bars, and a pointer on a bar
returns `None` from `widget_to_android()` - it can never become a touch.

## Keyboard → multitouch

In the desktop **Layout** tab, create a profile for the Android package, select
**Add key…**, choose Tap or Hold, enter a supported key, drag the new control
into place, and press **Save**. Use **Add movement…** to bind four keys in
up, right, down, left order (for example `W, D, S, A`). Select a control and
choose **Remove** to remove it. Hold and movement require a channel capable
of held contacts. The Qt workflow
and real Android input still need testing on the target desktop.

`input/engine.py` is the core of the product. Its guarantees are unit-tested:

1. Every contact that goes down eventually goes up **or** cancels - including
   on focus loss, profile switch, geometry change and backend failure.
2. A pointer id is never reused while its contact is open.
3. D-pad diagonals are normalised: `W+D` never exceeds the configured radius.
4. Releasing one of two held directions keeps the other.
5. A tap is one down/up pair at one point; OS auto-repeat cannot flood it.
6. `set_geometry` / `set_profile` release everything first - no event is ever
   computed against a half-changed mapping.

What the *transport* can do is a separate, honest question
(`input/channels.py`):

| Channel | Simultaneous touches | Why |
|---|---|---|
| `agent` (our APK) | up to 10 | real `MotionEvent`s; needs `INJECT_EVENTS` |
| `adb-shell` | **1** | `input tap` is sequential; the UI disables the overlay |
| `null` | 0 | no backend; events are recorded, not delivered |

The overlay refuses to activate on a single-contact channel instead of
silently producing taps that make a game unplayable.

## Commands

| Command | What it does |
|---|---|
| `lynxdroid doctor [--json] [--strict] [--area X]` | read-only host + backend report |
| `lynxdroid status [--json]` | lifecycle state with per-layer readiness |
| `lynxdroid start` / `stop` / `show` | session control, waits for real readiness; `show` opens the Android window |
| `lynxdroid apps list/launch/install` | package management with consent gates |
| `lynxdroid display show/set/presets` | viewport properties, previous values recorded |
| `lynxdroid profiles list/validate/export/rename/import/delete` | layout profiles, atomic writes |
| `lynxdroid image plan/prepare` | OTA images: consent, checksum honesty, never writes `/var/lib` |
| `lynxdroid perf` | times our own pipeline in µs; states what it did **not** measure |
| `lynxdroid gui` | PySide6 shell (also installed as `lynxdroid-gui`) |
| `lynxdroid version --json` | version and schema numbers |

Exit codes are documented in `--help`: `0` ok, `1` failure, `2` usage,
`3` missing dependency, `4` permission, `5` unsupported, `6` blocked.

## Security posture in one paragraph

Everything runs as your user. The GUI never needs root. Commands are executed
with explicit `argv` lists (`shell=True` is banned project-wide), properties
are namespaced and validated, persistent writes require `consent=True`, APKs
must be regular files with `PK` magic and a size bound, profile paths are
resolved and checked against symlink escapes, and the touch agent speaks
length-prefixed JSON over a **unix socket**, never a TCP port. Details and
threat model: [`docs/SECURITY.md`](docs/SECURITY.md).

## Documentation

| File | Contents |
|---|---|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | layers, dependency rules, backend contract, data flow |
| [`docs/REQUIREMENTS.md`](docs/REQUIREMENTS.md) | requirement registry with P0/P1, test, status |
| [`docs/TESTING.md`](docs/TESTING.md) | what runs where, how to reproduce, target-machine procedures |
| [`docs/SECURITY.md`](docs/SECURITY.md) | trust boundaries, privileged paths, threat model |
| [`docs/KNOWN_ISSUES.md`](docs/KNOWN_ISSUES.md) | every limitation, honestly |
| [`docs/CLI.md`](docs/CLI.md) | full command and exit-code reference |
| [`docs/SETTINGS.md`](docs/SETTINGS.md) | every setting, its default, and the module that reads it |
| [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md) | what was measured, in µs; what was not measured and why |
| [`docs/ROADMAP.md`](docs/ROADMAP.md) | stage-by-stage state against the 20-stage plan |
| [`android_agent/README.md`](android_agent/README.md) | the Android-side touch agent and its protocol |

## Licences

LynxDroid's own code: MIT (see `LICENSE`). Waydroid: GPL-3.0, used as an
installed dependency through its public CLI, not linked or vendored. Android
images: built from LineageOS sources; the GAPPS variant contains
Google-proprietary parts that this project never redistributes. See
[`docs/LICENSES.md`](docs/LICENSES.md).

## Not in scope

Screen recording, screenshots, GPS spoofing, bots, anti-cheat circumvention,
bundled Google Play, redistributed images. The product goal is a working
Android container with proven input and graphics, not a pretty installer.

### Browser preview of the launcher

Run `python scripts/preview_server.py` and open port 8080 to see the HTML/CSS/JS
visual copy. It automatically reloads when preview files change. Keep
`web-preview/` synchronized whenever the Qt launcher changes. The browser
preview has a separate local library of filenames only; files are not uploaded,
installed, or shared with the desktop launcher. Browser security prevents it
from opening the APK folder, showing its real path, or controlling the window.
The preview does not run Android applications. The artwork is a copy of
`src/lynxdroid/data/launcher-bg.jpg` and must be updated alongside it.
The same applies to `desktop-bg.jpg`. Control templates in the preview are
separate browser data, not shared with the desktop launcher.

The touch-message encoder is adapted from [scrcpy](https://github.com/Genymobile/scrcpy)
revision `a60891aea193d92e7e5c3942700eca63f9d19a5f`, Apache-2.0; its license
is in `licenses/scrcpy-APACHE-2.0.txt`. It is a codec, not an Android runtime,
and has not been tested against a real scrcpy server.
