# Roadmap against the 20-stage plan

State of each stage, in the specification's own vocabulary. Nothing is marked
done on the basis of "the code exists".

Legend: **DONE** = implemented and verified by automated tests · **PARTIAL** =
implemented, target-machine verification outstanding · **BLOCKED** = needs the
CachyOS laptop · **NOT STARTED** / **DEFERRED** = intentionally absent.

| Stage | Subject | State | Notes |
|---|---|---|---|
| 1 | Quality rules, requirement registry, stage gates | **DONE** | `docs/REQUIREMENTS.md` with P0/P1, test, status; each row says what proves it |
| 2 | Goal and target machine profile | **DONE** | platform profile detected, not assumed; `doctor --area environment` |
| 3 | CachyOS / kernel diagnostics | **PARTIAL** | `lynxdroid doctor` complete and read-only; verified in a Debian container, target run pending |
| 4 | Architecture, images, backend contract | **DONE** | `backend/contract.py`, `backend/images.py` with consent gate; process/trust-boundary diagram in `ARCHITECTURE.md §1` |
| 5 | First real Android start | **BLOCKED** | start/stop implemented with real boot detection; cannot run without the laptop |
| 6 | Window, image, scaling | **PARTIAL** | geometry engine verified (44 tests); Qt window written, never displayed here |
| 7 | Resolution, DPI, properties | **DONE** | four concepts separated, validation + apply modes + rollback values; live verification blocked |
| 8 | Base input, Wayland verification | **PARTIAL** | engine + channels done; multitouch transport needs the agent on a device |
| 9 | Keyboard layout editor | **PARTIAL** | model, codec, store, CLI; drag-and-drop, selection and unsaved-changes handling implemented in Qt-free `gui/editor.py` (37 tests); the widgets themselves have never been displayed |
| 10 | Simultaneous presses, multitouch | **PARTIAL** | state machine and pointer allocation verified; real device test blocked |
| 11 | Intel Iris Xe, laptop optimisation | **BLOCKED** | host GPU checks written; nothing can be measured without the GPU |
| 12 | Android apps and instances | **PARTIAL** | app list/launch/install done; multi-instance deliberately `UNTESTED` |
| 13 | Performance and stability | **PARTIAL** | our own pipeline measured: ~10 µs/event/stage, `docs/PERFORMANCE.md`; end-to-end latency needs the laptop and is not claimed |
| 14 | Security, network, data | **DONE** | see `docs/SECURITY.md`; enforced by tests, not promises |
| 15 | Testing and quality criteria | **DONE** | 512 automated tests, ruff + strict mypy clean, documented tier 2/3 procedures |
| 16 | Full Python implementation | **PARTIAL** | the Python product is complete in scope for stages 1-10; three GUI surfaces are untested (stage 9 editor polish, aim mode, analog d-pad) |
| 17 | Python component integration | **BLOCKED** | the end-to-end scenario requires the laptop; the `gui` runs everything through real components, no mocks in the path |
| 18 | C++ migration plan | **DONE (plan only)** | module order and compatibility contract in `ARCHITECTURE.md §8`; **implementation must not start** until stage 17 passes |
| 19 | C++ port and verification | **NOT STARTED** | gated on stage 17, per stage 17's own rule |
| 20 | Build, results, sources | **PARTIAL** | README/ARCHITECTURE/SECURITY/TESTING/KNOWN_ISSUES/LICENSES written; a reproducible CachyOS build script and a verified install are pending |

## Why the C++ stage has not begun

Stage 17 is explicit: "Without an end-to-end check on the user's device, the
C++ migration does not begin." This environment has no Waydroid, no GPU, no
Wayland and no access to the laptop. Starting a C++ rewrite now would produce a
second unverified implementation, which is precisely what the specification
forbids.

The Python code is nevertheless structured for that migration: layers map 1:1
onto planned C++ targets, and the JSON profile schema is the compatibility
contract that both sides will be tested against.

## What would move each BLOCKED stage

| Stage | Unblocking evidence |
|---|---|
| 3, 5 | `lynxdroid doctor --json` and `lynxdroid start` output from the CachyOS laptop |
| 6, 17 | a screenshot of the window and a note on which coordinates landed where |
| 8, 10 | a touch-test app showing two simultaneous contacts, with the agent installed |
| 11 | `dumpsys SurfaceFlinger` renderer line plus a 1080p/720p comparison |
| 12 | confirmation that a second instance is not required for the MVP |
| 13 | a baseline run recorded with the procedure in `TESTING.md` |

## Next concrete steps

1. **Verify on the target machine.** Until then everything above is a claim,
   and this document says so.
2. Finish the layout editor: drag-and-drop, resize handles, per-element
   inspector, orientation-specific variants.
3. Expose aim mode and analog d-pad in the UI (engine support already exists
   and is tested).
4. Only then: the C++/Qt 6 port, module by module, each one gated on a
   Python-vs-C++ comparison on shared fixtures.
