# CLI reference

```
lynxdroid [--root DIR] [-v] COMMAND ...
```

Global options:

| Option | Effect |
|---|---|
| `--root DIR` | use `DIR` instead of XDG paths (portable installs, tests) |
| `-v`, `--verbose` | debug logging to stderr |
| `--version` | print the version banner and exit |

## Exit codes

Stable, script-usable, documented in `--help`:

| Code | Meaning | Typical cause |
|---|---|---|
| 0 | OK | command succeeded |
| 1 | failure | backend error, corrupt file, validation failure |
| 2 | usage | bad arguments, invalid input value, unreadable file |
| 3 | missing dependency | Waydroid/adb not installed |
| 4 | permission | refused because of privileges |
| 5 | unsupported | the host cannot do this at all |
| 6 | blocked | a hard precondition failed; nothing was changed |

`--yes`-gated commands return **6** when confirmation is missing, and change
nothing.

---

## doctor

Read-only inspection. Never mounts, loads, installs or enables anything.

```
lynxdroid doctor [--json] [--area AREA]... [--quiet] [--strict]
```

| Option | Effect |
|---|---|
| `--json` | machine-readable report (`schema`, `version`, `checks[]`) |
| `--area` | limit to: `environment`, `session`, `kernel`, `gpu`, `waydroid`, `storage` (repeatable) |
| `--quiet` | hide `OK` rows in text mode |
| `--strict` | exit 6 if any **required** container check is not OK |

Report statuses: `OK`, `WARNING`, `ERROR`, `UNKNOWN`. `UNKNOWN` means *could not
determine* — never a disguised "unsupported".

Required checks (`meta.required_for_container`): `os.arch`, `session.wayland`,
`kernel.binder`, `container.namespaces`, `container.memfd`, `waydroid.binary`,
`waydroid.images`, `disk.space`.

```console
$ lynxdroid doctor --json | jq -r '.checks[] | "\(.status)\t\(.id)\t\(.detail)"'
OK      os.kernel       6.1.158+
ERROR   kernel.binder   this kernel was built without CONFIG_ANDROID_BINDER_IPC
UNKNOWN gpu.device      /sys/class/drm is absent (no DRM subsystem)
```

## profile

The stage-2 deliverable: a description of this machine and a **verdict** about
whether Android can run on it. Read-only, never privileged.

```
lynxdroid profile [--json]
```

Five preconditions are judged separately — `binder`, `wayland`, `gpu`,
`waydroid`, `disk` — and each carries the fact it was decided from, the reason
in user terms, and (when it is not satisfied) one command that would change it.

| Verdict | Meaning |
|---|---|
| `READY` | every precondition was checked and passed |
| `PARTIAL` | it can run, with a named limitation (single-touch input, no render node access) |
| `BLOCKED` | a precondition is known to fail; Android cannot boot |
| `UNKNOWN` | a precondition could not be checked. **Not** the same as `BLOCKED`, and never rounded up to `READY` |

The verdict is the worst of the five, and `UNKNOWN` counts as not-a-green-light
— the same rule `doctor --strict` applies. Exit code is `0` for `READY` or
`PARTIAL`, `6` otherwise, so a script can branch on it.

```console
$ lynxdroid profile
host       CachyOS Linux (cachyos) 6.12.4-1-cachyos x86_64
session    wayland on KDE
gpu        Iris Xe Graphics [i915]  (Iris Xe class)
waydroid   1.5.0

  + binder    Android binder devices are present
  + wayland   a Wayland session with a usable socket
  + gpu       Intel integrated graphics, the renderer the specification targets
  + waydroid  an installed backend with an image
  + disk      enough room for the container's data partition

verdict    READY - this host meets every precondition Android needs
```

`--json` is what to send back from a machine that cannot be reached otherwise:
the facts, the verdict and the evidence for it travel in one document.

```console
$ lynxdroid profile --json > profile.json
```

## status

```
lynxdroid status [--json]
```

Prints backend capabilities, the lifecycle state, and one readiness row per
layer (`process`, `container`, `android`). `--json` includes
`capabilities.features[]` with `support` ∈ {supported, conditional, unsupported,
untested}.

## start / stop

```
lynxdroid start [--timeout SECONDS] [--json]
lynxdroid stop  [--timeout SECONDS] [--json]
```

`start` waits for **real** readiness: `getprop sys.boot_completed == 1` inside
the guest (falling back to `dev.bootcomplete`). A running session process is
reported as `starting`, not `running`.

Failure codes: `boot-timeout`, `container-start-failed`, `service-unknown`,
`not-installed`, `already-starting`.

## show

```
lynxdroid show [--json]
```

Asks Waydroid to present the Android window (`waydroid show-full-ui`). This is
not the same operation as `start`: `start` boots the session, `show` puts a
window on screen for a session that is already running. It refuses with exit 6
when Android is not `running`, because asking for a window that cannot appear
only looks like a hang.

## game

```text
lynxdroid game explain MESSAGE [--json]
lynxdroid game fix NAME [--yes] [--json]
lynxdroid game revert [--yes] [--json]
```

`lynxdroid game explain MESSAGE [--json]` matches error text to a possible
category, not a confirmed cause. `lynxdroid game fix network --yes` runs
non-destructive ICMP and route probes inside Android; it does not test DNS or
the game's server. Legacy `hide-root`, `egl-fallback`, and `game revert` are
disabled because their effectiveness and safe restoration were not verified.
See `docs/COMPATIBILITY.md` for limitations and recovery guidance.

## apps

```
lynxdroid apps list [--json]
lynxdroid apps launch PACKAGE
lynxdroid apps install PATH.apk [--yes]
```

`launch` also reports which layout profile is bound to that package, and warns
when the profile was written for a different app version. If the version cannot
be read, nothing is claimed - an unknown version is never reported as a
mismatch.

`install` prints the file name, size, SHA-256 and the source directory, then
refuses without `--yes`. It rejects symlinks, non-`.apk` files, empty files,
files over 4 GiB and anything without the `PK\x03\x04` magic. Split APK
(`.apks`, `.xapk`) and `.aab` are refused with an explicit message.

## display

```
lynxdroid display show    [--json]
lynxdroid display set WIDTH HEIGHT DPI [--yes] [--json]
lynxdroid display presets
```

`set` validates first (`320..3840` per edge, `72..960` dpi, ≤ 8.3 Mpx, safe
32-bit stride). Then:

1. records the previous `persist.waydroid.width/height`;
2. writes the new values (needs `--yes`);
3. runs `wm density`;
4. prints which changes need a session restart.

Nothing is written without `--yes`; without it the command prints the plan and
exits 6.

## profiles

```
lynxdroid profiles list     [--json]
lynxdroid profiles validate [PATH]
lynxdroid profiles export   PROFILE_ID DESTINATION
lynxdroid profiles rename   PROFILE_ID NEW_NAME
lynxdroid profiles import   SOURCE [--yes]
lynxdroid profiles delete   PROFILE_ID [--yes]
```

* `validate` with no argument checks every stored profile and reports each file
  as `✔` or `✘` with the reason (exit 1 if any failed).
* `export` writes `0600` JSON with no secrets.
* `import` refuses symlinks and validates before storing; a colliding id is
  imported as a copy rather than overwriting.
* `delete` keeps a timestamped backup unless hard-deleted through the API.
* `rename` changes only the display name; the id identifies the file and is
  never reissued, so a binding in the GUI survives the rename.
* An id that names nothing exits 1 with a sentence pointing at
  `profiles list`, not a traceback.

## perf

```
lynxdroid perf [--iterations N] [--json]
```

Times the stages LynxDroid owns: key event → touch events, wire encoding, and
the `AF_UNIX` write. It prints, in both text and JSON, what it did **not**
measure (compositor, container, Android dispatch, application frame) and makes
no end-to-end claim. Keys are taken from the profile; a key that produces no
events is an error rather than a flattering zero. See `docs/PERFORMANCE.md`.

## image

```
lynxdroid image plan    [--channel CHANNEL] [--json]
lynxdroid image prepare [--channel CHANNEL] [--yes] [--allow-unverified] [--json]
```

Channels: `lineage-vanilla` (no Google services) and `lineage-gapps`.

* `plan` is **read-only**: it prints the OTA endpoints, the licence note, an
  estimated size, the free-space verdict and the exact steps, then exits 0
  (or 6 when there is not enough disk space). Nothing is downloaded.
* `prepare` refuses without `--yes` (exit 6) *after* printing the plan.
* The OTA channel publishes no checksum, so `prepare` additionally refuses
  without `--allow-unverified` (exit 6) rather than pretending a locally
  computed SHA-256 proves authenticity.
* Downloads land in `$XDG_CACHE_HOME/lynxdroid/images`, never in
  `/var/lib/waydroid`. Installation is printed as `sudo` commands for the user
  to run, because it is a root action on a package-owned directory.

## gui

```
lynxdroid gui [--backend waydroid|fake] [--offscreen]
```

Requires the `gui` extra (`pip install -e '.[gui]'`). `--offscreen` renders with
Qt's offscreen platform, useful for smoke tests on a headless box.

## version

```
lynxdroid version [--json]
```

`--json` prints `version`, `stage`, `schemas` (profile/settings/agent-protocol/
doctor-report) and `gui_available`. That last field is a real load test, not an
`import PySide6` check: a wheel whose Qt shared libraries are missing reports
`false` and `version` prints the loader's reason. `gui` exits 3 in that case.

---

## Environment variables

| Variable | Used for |
|---|---|
| `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_CACHE_HOME`, `XDG_STATE_HOME` | state location (defaults per XDG spec) |
| `XDG_RUNTIME_DIR` | touch-agent socket directory |
| `WAYLAND_DISPLAY`, `XDG_SESSION_TYPE`, `XDG_CURRENT_DESKTOP` | read by `doctor` only |
| `LYNX_SECRET*` | stripped from child environments |

## Programmatic use

Everything the CLI does is available as a library:

```python
from lynxdroid.core.paths import AppPaths
from lynxdroid.profiles.store import ProfileStore
from lynxdroid.display.geometry import ViewportGeometry, Size, Rotation
from lynxdroid.input.engine import InputEngine

paths = AppPaths.resolve()
profile = ProfileStore(paths).load_all()[0]
geometry = ViewportGeometry(buffer=Size(1920, 1080), surface=Size(960, 540))
engine = InputEngine(profile, geometry)
events = engine.key_down("w")  # -> [TouchEvent(down), TouchEvent(move)]
```
