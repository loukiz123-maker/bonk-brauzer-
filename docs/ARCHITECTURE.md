# Architecture

This document records **contracts**, not implementation trivia. It should stay
true while the code grows.

## 1. Layers and the dependency rule

```
core        stdlib-only: paths, subprocess, storage, diagnostics, errors
 ├── doctor          reads /proc, /sys, package manager, session env (read-only)
 ├── backend         AndroidBackend contract + Waydroid adapter + lifecycle
 ├── display         ViewportGeometry, resolution validation
 ├── input           engine (keys→touches), channels (touches→Android)
 ├── profiles        versioned model, codec, atomic store
 ├── apps            APK validation, package catalog
 ├── settings        typed, versioned, lenient reader
 ├── cli             text interface (everything above except gui)
 └── gui             PySide6 widgets + viewmodel (pure logic, no Qt)
```

Hard rules, checked by review (and by `tests/test_layers.py`):

1. `core` imports nothing from the project except `core`.
2. `doctor` never writes: no mounts, no module loads, no service changes.
3. `backend` never touches Qt; `gui` never parses command output itself.
4. `display`, `input`, `profiles` have **no** dependency on `backend` or `gui`.
   The geometry engine can be reasoned about without Waydroid, Android or Qt.
5. No module executes `shell=True`; see `core/proc.py`.
6. No module reads or writes outside `AppPaths` unless the user explicitly
   named the path (APK import, profile import/export).

## 2. The backend contract

`backend/contract.py` is the interface the GUI and CLI code against. It exists
so a future C++ backend, a QEMU fallback or a test double all look identical to
the rest of the program.

```python
class AndroidBackend:
    name: str
    capabilities() -> BackendCapabilities
    diagnose() -> Report
    snapshot() -> Snapshot
    start(timeout_s) -> Snapshot
    stop(timeout_s) -> Snapshot
    apply_display(width, height, dpi) -> list[str]
    read_display() -> dict[str, str]
    list_apps() -> list[dict[str, str]]
    launch_app(package) -> bool
    install_apk(path) -> tuple[bool, str]
    get_prop(name) -> str | None
    set_prop(name, value, consent) -> bool
    show_full_ui() -> bool
```

Three ideas make it honest:

**`Feature.support`** is `SUPPORTED | CONDITIONAL | UNSUPPORTED | UNTESTED`.
`UNTESTED` exists because "Waydroid might support multiple instances" is not
the same as "we implemented and verified it". The GUI renders unsupported and
untested controls disabled **with the reason**, never hidden - so a user can
tell "not built" from "broken right now".

**`Snapshot.readiness`** keeps four layers separate:

| Layer | Becomes READY when |
|---|---|
| `process` | `waydroid status` answered at all |
| `container` | the container reports `RUNNING` |
| `android` | `getprop sys.boot_completed` returns `1` |
| `graphics` / `input` | set by the display/input path (not by the adapter) |

A running process is explicitly **not** evidence that Android is usable. This
is a test (`test_session_running_without_boot_is_only_starting`), not a comment.

**Consent flags.** `set_prop(..., consent=False)` raises. Persistent Android
properties change system behaviour and must be a deliberate user decision.

## 3. Lifecycle state machine

`backend/lifecycle.py`:

```
not-configured ──configure──▶ stopped ──start──▶ starting ──boot──▶ running
      │                          ▲                   │                 │
      │                          │                   ▼                 │
      └──────discovered running──┴────────────── error ◀──stop── stopping
```

Properties:

* illegal transitions raise `ValueError` (a programming error, not a user error);
* every transition is recorded with a reason in a bounded history;
* `NOT_CONFIGURED → RUNNING` is allowed because a session started from a
  terminal is discovered by polling, and forcing a restart to satisfy the
  machine would be user-hostile;
* `request_stop_during_start()` is honoured: the stop happens right after the
  start attempt finishes instead of being ignored;
* observers are notified on every change and a failing observer cannot break
  the backend.

## 4. Coordinate pipeline

One immutable `ViewportGeometry` per (buffer size, window size, fit, rotation)
combination, replacing the previous one atomically.

```
Android buffer 1920×1080
      │  rotation (0/90/180/270)
      ▼
rotated picture
      │  fit mode (contain/fill/integer) → content rect + bars
      ▼
window logical pixels
      │  Qt mouse position
      ▼
widget_to_android() ──▶ Android pixels  ──▶ TouchEvent.pointer
```

Why it is one object: stage 6 requires that *input uses the same transforms as
the image*. A key drawn at `normalized_to_widget(u, v)` and a click at that
pixel both go through the same maths, and the round-trip is a test.

Failure modes handled explicitly:

| Input | Result |
|---|---|
| pointer on a black bar | `None` → no touch (never clamped) |
| zero-size window | empty content rect, `None` |
| extreme aspect (3840×100) | content clamped inside the surface |
| key authored at the exact corner | drawn 0.001 px inside, so it is clickable |
| rotation changed mid-touch | `InputEngine.set_geometry()` cancels first |

Display *values* are validated separately (`display/resolution.py`), because a
number can be geometrically fine and still unsafe: `validate_display()` rejects
zero, negative, out-of-range, >8.3 Mpx and >2³¹ stride, and returns `None`
rather than a "mostly valid" request.

## 5. Input pipeline

```
physical key ──▶ InputEngine ──▶ list[TouchEvent] ──▶ Channel ──▶ Android
                    │                                   │
                    ├ Profile (geometry, bindings)      ├ AgentChannel (unix socket, 10 touches)
                    └ ViewportGeometry                  ├ AdbShellChannel (1 touch, complete taps)
                                                        └ NullChannel (tests)
```

The engine owns pointer allocation, the d-pad vector maths, swipe interpolation
and — most importantly — release-on-focus-loss. The channel owns the *transport*
and reports what it can actually carry. Splitting them is what allows the
overlay to be tested to death without a device.

**Why a unix socket and not a port:** stage 14 forbids debug ports reachable
from outside. `AF_UNIX` in `$XDG_RUNTIME_DIR` with `0600` still means other
processes *of the same user* can connect - the documentation says so instead of
implying otherwise.

## 6. Profile format

One JSON file per profile under `$XDG_DATA_HOME/lynxdroid/profiles/<id>.json`.

```json
{
  "schema": 1,
  "id": "3f2a…",
  "name": "Shooter",
  "target": {"package": "com.company.game", "package_version": "1.2.3",
              "instance": "default", "title_hint": ""},
  "reference": {"width": 1920, "height": 1080, "dpi": 320},
  "elements": [
    {"id": "tap-a", "type": "tap", "label": "A", "x": 0.82, "y": 0.55,
     "pointer": 0, "radius": 0.035, "keys": ["j"]},
    {"id": "dpad-move", "type": "dpad", "label": "MOVE", "x": 0.18, "y": 0.72,
     "dpad_keys": ["w", "d", "s", "a"], "dpad_radius": 0.08, "dead_zone": 0.15}
  ]
}
```

Decisions, each with a test:

* **binding is the package name**, never the visible title;
* **coordinates are normalised in Android buffer space**, so a resolution change
  does not move keys and rotation does not rewrite the file;
* **`schema` is mandatory**; a newer schema is refused with a clear message
  instead of being half-parsed;
* **writes are atomic** (`tmp` → `fsync` → `os.replace`) and the previous file
  is copied to `backups/<id>/<timestamp>.json` first;
* **deletion keeps a backup** unless `hard=True`;
* **key conflicts are rejected at validate time** and additionally surfaced in
  the UI list.

## 7. Threading model (GUI)

* the Qt event loop never runs a subprocess;
* `gui/workers.py` runs backend calls on a `QThreadPool` (3 threads) and returns
  through signals;
* the status poller asks for a snapshot every 2 s; `boot_completed()` is cached
  for 1 s so a poll cannot spawn a subprocess storm;
* input handling happens on the GUI thread, where it belongs — it is pure
  arithmetic on an immutable geometry.

## 8. What a C++ port would keep

The stage-18 migration order maps 1:1 onto this tree, which is why the layers
are separated this way:

| Order | Python module | C++ target | Reason |
|---|---|---|---|
| 1 | `profiles/model.py`, `codec.py` | `lynx::profiles` | data + validation, no Qt |
| 2 | `display/geometry.py`, `input/engine.py` | `lynx::input` | hot path, pure maths |
| 3 | `doctor/*`, `backend/waydroid.py` | `lynx::backend` | system adapters |
| 4 | `backend/lifecycle.py` | `lynx::core` | state machine |
| 5 | `gui/*` | Qt 6 widgets + `QML` | UI |
| 6 | packaging | CMake + CPack | distribution |

Schema versions (`version.SCHEMA_VERSIONS`) are the compatibility contract:
profile JSON written by Python must load in C++ unchanged, and a round-trip
test on both sides is the gate.
