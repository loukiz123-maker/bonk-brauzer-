# Testing

Three tiers, never confused with each other.

| Tier | Runs where | Count | Proves |
|---|---|---|---|
| 1. Unit + contract | any machine, no display | 903 | our logic is correct |
| 2. Integration | the target laptop | listed below | the components work together |
| 3. Target acceptance | CachyOS + Waydroid + Iris Xe | listed below | the product works for the user |

CI pins `ruff==0.17.4` on purpose: Ruff adds rules between minor releases,
so an unpinned upgrade can turn a green branch red with no code change. The
floor in `pyproject.toml` and the exact version in CI are recorded together.

A green tier 1 is **not** evidence for tier 3. The specification says so
explicitly and this project follows it.

## Tier 1 — run this now

```bash
python -m venv .venv && . .venv/bin/activate
pip install -e '.[dev]'
pytest                                    # 903 collected; Qt tests may skip without native libraries
pytest -q --maxfail=1                     # fail fast
pytest tests/display -q                   # 80 display tests
pytest -m "not gui and not privileged"    # inside a restricted sandbox
```

Expected: 903 collected; five Qt tests skip if native Qt libraries are missing. No display, network or Waydroid is required for the rest.

### What each suite pins down

| Suite | Tests | Key property |
|---|---|---|
| `tests/display/test_geometry.py` | 44 | input transform == image transform; bars reject touches; rotations round-trip |
| `tests/display/test_resolution.py` | 36 | zero/negative/overflow/absurd values never reach the backend |
| `tests/input/test_engine.py` | 47 | no stranded contacts; d-pad normalisation; slot exhaustion; focus-loss release; aim mode opens with DOWN |
| `tests/input/test_pipeline.py` | 37 | engine output actually reaches a channel; sub-microsecond work is never printed as "0 ms" |
| `tests/input/test_channels.py` | 25 | single-touch honesty; agent protocol; rejections surface |
| `tests/profiles/test_model.py` | 47 | schema, conflicts, paths in error messages |
| `tests/profiles/test_store.py` | 28 | atomic writes, backups, corrupt-file refusal, delete cannot escape |
| `tests/backend/test_lifecycle.py` | 21 | illegal transitions, observers, stop-during-start |
| `tests/backend/test_waydroid.py` | 46 | status parsing, consent gates, real boot check |
| `tests/backend/test_game_compat.py` | 39 | safe refusal of unverified fixes and limited network probes |
| `tests/backend/test_images.py` | 16 | nothing downloads without consent; checksum mismatch deletes |
| `tests/test_doc_counts.py` | 35 | every number quoted in README/TESTING/ROADMAP matches a real collection pass |
| `tests/input/test_agent_protocol.py` | 11 | the Java agent and the Python client agree on protocol version, frame limit, op names |
| `tests/profiles/test_compat.py` | 11 | the stored-format gate: a newer schema is refused, old files migrate, timestamps keep whole seconds |
| `tests/test_settings_are_read.py` | 9 | every setting has a reader, a row in SETTINGS.md, and that row names the right module |
| `tests/test_packaging.py` | 17 | desktop entry, icon, and the installer script run in a throwaway home |
| `tests/test_stage_criteria.py` | 11 | stage criteria: every HERE one cites a test that exists, every CI one a real job |
| `tests/test_install_script.py` | 7 | the install script parses, never runs sudo, and matches what the README documents |
| `tests/core/test_diagnostics.py` | 9 | the one definition of "this check blocks the container path" |
| `tests/doctor/test_kernel_checks.py` | 13 | binder/binderfs verdicts, including "cannot tell" staying distinct from "unsupported" |
| `tests/gui/test_editor.py` | 85 | editor logic plus extracted window callbacks for drag persistence and release delivery; full Qt remains untested |
| `tests/gui/test_viewmodel.py` | 71 | disabled-with-reason, banner logic, WCAG contrast |
| `tests/doctor/test_host_profile.py` | 26 | platform facts vs verdict, worst-verdict rule, unknown is not ready, agreement with `doctor` |
| `tests/cli/test_cli.py` | 78 | exit codes, JSON contracts, consent gates, image command, Qt probe honesty, doctor hints name real commands |
| `tests/test_requirements_registry.py` | 10 | the registry cites only tests and files that exist, and its counts are real |
| `tests/test_perf.py` | 20 | the timing tool measures real events and never reports an end-to-end figure |
| `tests/core/*` | 39 | XDG paths, symlink safety, no shell, timeouts |
| `tests/doctor/test_gpu_info.py` | 7 | GPU detection against described sysfs trees: one card, two cards, a virtual card, no node |
| `tests/test_layers.py` | 30 | architecture: layer imports, no `shell=True`, doctor never writes, no stray `print`, docstrings, Qt-free core |

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

### Nothing in tier 1 may depend on how fast the machine is

Two tests were written against a real measurement and both passed locally
while failing on a loaded CI runner: one assumed a single `deliver()` takes
under a microsecond, the other assumed it does not round to exactly 1 µs. The
clock decides that, not the code.

The rule that came out of it:

* formatting and rounding decisions belong in pure functions
  (`format_latency_ms`, `perf.percentile`) and are pinned there with literal
  inputs;
* a test on the integration path may assert only what is true regardless of
  speed — that a value exists, that the forbidden `0.000 ms` string never
  appears, that the line ends with its caveat;
* a bound on wall-clock time is allowed only with generous slack and only to
  prove something did *not* happen (a dry run did not sleep 2 s), never to
  prove how fast something was.

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
lynxdroid display set 1280 712 240 --yes
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
