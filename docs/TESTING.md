# Testing

Three tiers, never confused with each other.

| Tier | Runs where | Count | Proves |
|---|---|---|---|
| 1. Unit + contract | any machine, no display | 419 | our logic is correct |
| 2. Integration | the target laptop | listed below | the components work together |
| 3. Target acceptance | CachyOS + Waydroid + Iris Xe | listed below | the product works for the user |

A green tier 1 is **not** evidence for tier 3. The specification says so
explicitly and this project follows it.

## Tier 1 — run this now

```bash
python -m venv .venv && . .venv/bin/activate
pip install -e '.[dev]'
pytest                                    # 419 passed
pytest -q --maxfail=1                     # fail fast
pytest tests/display -q                   # 80 display tests
pytest -m "not gui and not privileged"    # inside a restricted sandbox
```

Expected: `419 passed in ~2.5 s`, no display, no network, no Waydroid.

### What each suite pins down

| Suite | Tests | Key property |
|---|---|---|
| `tests/display/test_geometry.py` | 44 | input transform == image transform; bars reject touches; rotations round-trip |
| `tests/display/test_resolution.py` | 36 | zero/negative/overflow/absurd values never reach the backend |
| `tests/input/test_engine.py` | 32 | no stranded contacts; d-pad normalisation; slot exhaustion; focus-loss release |
| `tests/input/test_channels.py` | 22 | single-touch honesty; agent protocol; rejections surface |
| `tests/profiles/test_model.py` | 47 | schema, conflicts, paths in error messages |
| `tests/profiles/test_store.py` | 28 | atomic writes, backups, corrupt-file refusal, delete cannot escape |
| `tests/backend/test_lifecycle.py` | 20 | illegal transitions, observers, stop-during-start |
| `tests/backend/test_waydroid.py` | 28 | status parsing, consent gates, real boot check |
| `tests/backend/test_images.py` | 13 | nothing downloads without consent; checksum mismatch deletes |
| `tests/gui/test_viewmodel.py` | 49 | disabled-with-reason, banner logic, WCAG contrast |
| `tests/cli/test_cli.py` | 42 | exit codes, JSON contracts, consent gates, image command, Qt probe honesty, doctor hints name real commands |
| `tests/core/*` | 39 | XDG paths, symlink safety, no shell, timeouts |
| `tests/test_layers.py` | 19 | architecture: layer imports, no `shell=True`, doctor never writes, no stray `print`, docstrings, Qt-free core |

### Failure injection already covered

These are the negative paths from stages 5, 13 and 15:

* corrupted profile JSON → `DataCorrupted`, original untouched;
* `os.replace` raising `OSError` mid-save → previous file still valid;
* symlink pointing outside the app directory → `PermissionDenied`;
* oversized file → refused before reading;
* `waydroid status` returning garbage → parsed as empty, no crash;
* agent speaking a different protocol → connection refused;
* agent rejecting an event → `InputError` with the reason;
* 11th touch when 10 slots are used → `TouchSlotExhausted`, no zombie contact;
* stop requested during start → honoured after the start finishes;
* writing a property without consent → `BackendError`.

## Tier 2 — integration on the target laptop

Run on CachyOS with Waydroid installed, in a KDE Plasma **Wayland** session.
These have not been executed anywhere yet.

```bash
# 0. record the machine so the result is reproducible
uname -r; cat /etc/os-release | head -3
lynxdroid version --json
lynxdroid doctor --json > ~/lynxdroid-doctor.json

# 1. container + Android, waiting for real boot completion
lynxdroid start            # expects: "Android is running: ... boot completed"
lynxdroid status --json    # expects: readiness.android == "ready"
lynxdroid shell-less check:
  waydroid shell getprop sys.boot_completed    # expects: 1

# 2. repeatability: stop, start, stop, start
lynxdroid stop && lynxdroid start && lynxdroid stop && lynxdroid start

# 3. installed apps
lynxdroid apps list        # expects a list, not an error

# 4. viewport change and geometry agreement
lynxdroid display set 1280 720 240 --yes
lynxdroid display show     # persist.waydroid.* match, wm size reported
# then: measure that a click at the drawn position lands at that position

# 5. data persistence
#    install a test app, put data in it, `lynxdroid stop`, `lynxdroid start`,
#    verify the data is still there.

# 6. negative paths (each must produce a clear message, not a traceback)
lynxdroid start      # with binder removed / image deleted
```

### GPU-specific (stage 11)

```bash
lynxdroid doctor --area gpu --json | jq '.checks[] | {id, status, detail}'
# then, with a session running:
waydroid shell dumpsys SurfaceFlinger | grep -iE "GLES|renderer|Vulkan"
```

Record the **guest** renderer. Iris Xe appearing on the host proves nothing
about what Android uses; `llvmpipe`/`SwiftShader` must be labelled software.

### Input-specific (stages 8 and 10)

```bash
# With the agent installed (see android_agent/README.md):
#   adb install app-debug.apk
#   adb shell appops set id.lynxdroid.agent INJECT_EVENTS allow
#   adb shell am start-foreground-service id.lynxdroid.agent/.TouchAgentService
lynxdroid doctor --area kernel --json
# Acceptance: two independent contacts, correct down/move/up/cancel order,
# everything released when the window loses focus.
```

Without the agent, the correct result is: overlay disabled, reason
"single-touch input only". That is a pass for the honesty requirement and a
fail for multitouch.

## Tier 3 — target acceptance checklist

Copy this into the issue tracker and tick it on the machine:

- [ ] `lynxdroid doctor` exit 0 with no ERROR rows required for the container
- [ ] `lynxdroid start` reaches `state=running` with `readiness.android=ready`
- [ ] `waydroid shell getprop sys.boot_completed` → `1`
- [ ] window opens; black bars visible at the sides when the aspect differs
- [ ] clicking a bar produces **no** touch (verified in the GUI status line)
- [ ] a key drawn at a position produces a touch at that Android coordinate
- [ ] two simultaneous contacts reach a touch-test app
- [ ] focus loss releases everything (alt-tab away mid-hold)
- [ ] resolution change → restart → geometry still agrees with input
- [ ] profile survives an application restart
- [ ] `lynxdroid stop` leaves no `waydroid` processes (`pgrep -a waydroid`)
- [ ] no file in `~/.local/share/lynxdroid` is group/world readable
- [ ] `journalctl -u waydroid-container` shows no error introduced by us

## Continuous integration

`.github/workflows/ci.yml` runs, on every push:

1. `pytest` on Python 3.10, 3.11, 3.12 (Linux, headless);
2. `ruff check` and `ruff format --check`;
3. `mypy --strict` on `src/lynxdroid`;
4. a job proving the package imports **without** PySide6 (the GUI extra is
   optional by design);
5. a job that builds a wheel and runs `lynxdroid doctor --json` from it.

CI cannot substitute for tier 2/3: no binder, no GPU, no Wayland. The workflow
says so in a comment so nobody mistakes green CI for a working product.

## Reproducing a report

Every `doctor` run can be saved:

```bash
lynxdroid doctor --json > report.json
```

It includes `schema`, `version`, and one entry per check with `status`,
`observed` and `next_step`, plus the paths in use — no home-directory contents.
