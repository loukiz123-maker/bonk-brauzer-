# LynxDroid

An Android container on the **host Linux kernel**, with our own control layer:
lifecycle, diagnostics, viewport geometry, keyboard→multitouch mapping and
layout profiles. Built first in Python (this repository), with a documented
path to C++/Qt.

The Android userspace itself comes from **Waydroid** and a legally obtained
LineageOS-based image. LynxDroid does not bundle Android, does not mirror
images, and does not pretend to be a virtual machine.

```
                ┌──────────────────────────────┐
                │  LynxDroid GUI (PySide6)     │  ← our code
                │  session · display · layout  │
                └───────────────┬──────────────┘
                                │ backend contract (Python)
                ┌───────────────▼──────────────┐
                │  backend/waydroid            │  ← our adapter
                │  start · stop · props · apps │
                └───────────────┬──────────────┘
                                │ `waydroid`, `systemctl`, `waydroid shell`
                ┌───────────────▼──────────────┐
                │  Waydroid  ·  Android 13     │  ← upstream, not ours
                │  LineageOS image in LXC      │
                └───────────────┬──────────────┘
                                │ binder / namespaces / DRM
                ┌───────────────▼──────────────┐
                │  CachyOS kernel  ·  KWin     │  ← the host
                └──────────────────────────────┘
```

## Honest status first

Read this before anything else. It is the difference between "works" and
"should work".

| Area | State | Evidence |
|---|---|---|
| Unit tests (562) | **VERIFIED** | run in this repository, `pytest`; ruff + strict mypy clean |
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
python -m venv .venv && . .venv/bin/activate
pip install -e '.[gui,dev]'          # [gui] pulls PySide6
lynxdroid doctor                     # read-only, changes nothing
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
pytest                               # 562 tests, no display needed
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
lynxdroid display set 1280 720 240 --yes
lynxdroid gui                    # 7. the window
```

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
| `lynxdroid gui` | PySide6 shell |
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
