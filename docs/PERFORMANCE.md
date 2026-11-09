# Performance

The specification asks for **measured** latency and explicitly forbids "0 ms"
promises. This document separates what was measured from what was not.

## The rule this project follows

> A number is only evidence for the thing it actually timed.

The end-to-end interaction latency a player feels is:

```
key press → LynxDroid engine → wire encoding → unix socket → Android agent
         → Android input dispatcher → application → SurfaceFlinger
         → Wayland client (Waydroid) → KWin → compositor output
```

LynxDroid controls the first three arrows. The rest belong to Android, the
container and the compositor. This machine has no container, no compositor and
no GPU (see `docs/KNOWN_ISSUES.md`), so those arrows **cannot** be measured
here and are not reported.

## What was measured, and where

Command, run in this repository's container (4 vCPU, Python 3.11, no display):

```console
$ lynxdroid perf --iterations 200
LynxDroid pipeline timing (milliseconds)

  iterations: 200    events per sample (max): 2

  stage       count       min    median       p95       max
  engine        200    0.0096    0.0102    0.0121    0.0419
  encode        200    0.0125    0.0129    0.0152    0.0451
  socket        200    0.0105    0.0109    0.0139    0.0536

  ! measured here: key event -> touch events, wire encoding, AF_UNIX write/read
  ! not measured: compositor, container, Android input dispatch, application frame
  ! no end-to-end latency figure is claimed; it cannot be obtained on this machine
```

| Stage | What it times | Typical |
|---|---|---|
| `engine` | key event → touch events: profile lookup, d-pad normalisation, coordinate transform, pointer allocation | ~10 µs |
| `encode` | those events → agent wire format (4-byte length + compact JSON) | ~13 µs |
| `socket` | `sendall` of that frame into a real `AF_UNIX` socket | ~11 µs |

**Reading these numbers honestly:**

* They are **microseconds**, and that is the point: the Python control layer is
  roughly three orders of magnitude below one 60 Hz frame (16.7 ms). Input
  handling will not be the bottleneck on the target machine.
* They do **not** include drawing, compositing, or anything inside Android.
* They are not a "latency of LynxDroid" figure. They are a budget check on the
  part we wrote, which is the only claim the data supports.
* Timings vary with CPU governor, container neighbours and Python version. The
  committed numbers are one run; re-run the command on your machine rather than
  trusting these.

### Why these three stages and not the whole path

Because these are the only stages that exist on this machine. Timing a path
whose middle is missing produces a small number that means nothing - exactly
the "0 ms" failure mode the specification warns against.

## What the GUI reports while you play

The status bar shows the same measurement, live:

```
send: 12.3 µs median · p95 41.0 µs · 1204 events (not end-to-end)
```

It comes from `input/pipeline.py`, which times handing a frame to the channel
and prints "(not end-to-end)" in the line itself so the number cannot be
mistaken for the interaction latency a player feels.

Two details are deliberate:

* work faster than the display precision is printed in **microseconds**, not
  rounded to `0.000 ms`, because "0 ms" is the claim stage 13 forbids;
* a failed delivery shows the error instead of a timing, so an unmeasured path
  can never look fast.

## Guard rails built into the tool

`lynxdroid perf` is not a demo; it refuses to produce a meaningless number:

* keys are taken **from the profile** (`profile_keys`), never assumed;
* a key that produces no touch events raises instead of timing an empty list
  (this was a real bug during development: the first version timed nothing and
  reported a flattering result);
* a profile with no bound keys is refused;
* `--iterations 0` is refused;
* the JSON has no field named `latency_ms` and no `total`; the stage series are
  reported separately so they cannot be added up into an end-to-end claim;
* every output - text and JSON - carries the scope note.

Tests: `tests/test_perf.py` (20 tests), including one that asserts the JSON
contains no aggregate latency field.

## What the target machine must measure instead

These are the numbers that matter and that **only** the CachyOS laptop can
produce. None of them has been run.

| Metric | How to get it on the laptop | Where it gets recorded |
|---|---|---|
| Key event → `MotionEvent` in Android | `adb shell getevent -t`, compare with the host timestamp; or the agent logs both ends | `docs/TESTING.md` tier 3 |
| Composer round trip | `weston-debug` / KWin frame timings, or a high-speed capture of 120 taps | tier 3 checklist |
| Frame rate in the running app | `dumpsys gfxinfo <package>` (frame timings), plus Waydroid's own logs | tier 3 |
| End-to-end feel | high-speed camera (240 fps) of key press vs. on-screen reaction, 30 trials, report median and p95 | tier 3, the only honest end-to-end method |
| CPU cost of the control layer | `perf stat -p <pid>` around the Python process during play | tier 3 |

The high-speed camera method is listed because it is the only way to measure
what a user perceives without trusting any of the software involved.

## Budgets this project holds itself to

Not promises about the finished product - design targets for the part we own,
chosen so that input handling is never the reason a game feels slow:

| Budget | Target | Current evidence |
|---|---|---|
| Control layer per key event (engine + encode + socket) | < 1 ms | measured ~34 µs total on this machine |
| Memory for the control layer | < 150 MiB RSS | not measured yet (needs a long session) |
| CPU while idle (no input) | < 1 % of one core | not measured yet |
| No input-path allocation per frame in steady state | aim | **not achieved**: the engine allocates events and JSON per press; acceptable at these sizes, revisit if tier 3 shows contention |

The last row is deliberate: it is the one place where the current design has a
known cost, recorded rather than hidden.
