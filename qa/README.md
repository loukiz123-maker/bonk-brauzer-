# qa/ — independent QA workspace

Owned by the QA agent. Nothing here is part of the build (`make`, CMake and the
launcher's packaging do not read this directory).

| Path | What it is |
|---|---|
| `REPORT-2026-11-10.md` | Audit report (Russian): findings, exact commands and results, what was not verified |
| `AGENTS-NOTICE.md` | Notice / prompt for the development agents: what QA fixed, how to take the fixes, what stays open |
| `patches/engine/` | `git format-patch` for the engine branch (`arena/906ff810-bonk-brauzer`, base `bb633d2`): documentation only - records the owner's decision that the Android runtime is a virtual machine, not yet written. The earlier Waydroid-removal patch is gone from here because the engine agent applied it (`04b034c`) |
| `patches/launcher/` | `git format-patch` series for the launcher branch (`arena/2dcca4f1-bonk-brauzer`, base `e805a17`), which has no common history with `main`: `0001`-`0004` bug fixes, `0005` removes Waydroid from the launcher, `0006` profile/doctor disagreement, `0007` the note in `LICENSE` (optional), `0008` two defects in the 8259 prototype |
| `tests/engine_parser_fuzz.cpp` | Sanitizer fuzzer for the host-side Android engine parsers (build instructions in the file) |
| `tests/launcher_input_fuzz.py` | Invariant fuzzer for the launcher's input mapping (run inside the launcher checkout) |

The engine bug fixes are ordinary commits on this branch (`fcc1c62`, `b381c61`);
the engine agent has cherry-picked both.

Owner's decisions of 2026-11-10 that everything here follows: Waydroid is
removed from the engine and from the launcher; the Android runtime is to be a
hardware-accelerated virtual machine ("like VirtualBox, as BlueStacks does").
That virtual machine does not exist yet, and which hypervisor to use is still
the owner's open question.

None of these tests involves a real Android system or a real virtual machine
(the sandbox has no `/dev/kvm`). They check host-side code only and are not
evidence that Android or any game runs.
