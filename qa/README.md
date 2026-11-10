# qa/ — independent QA workspace

Owned by the QA agent. Nothing here is part of the build (`make`, CMake and the
launcher's packaging do not read this directory).

| Path | What it is |
|---|---|
| `REPORT-2026-11-10.md` | Audit report (Russian): findings, exact commands and results, what was not verified |
| `AGENTS-NOTICE.md` | Notice / prompt for the development agents: what QA fixed, how to take the fixes, what stays open |
| `patches/engine/` | `git format-patch` for the engine branch (`arena/906ff810-bonk-brauzer`, base `23b78b2`): removes the Waydroid runtime and the `bonk waydroid` command, on the project owner's decision of 2026-11-10 |
| `patches/launcher/` | `git format-patch` series for the launcher branch (`arena/2dcca4f1-bonk-brauzer`), which has no common history with `main` |
| `tests/engine_parser_fuzz.cpp` | Sanitizer fuzzer for the host-side Android engine parsers (build instructions in the file) |
| `tests/launcher_input_fuzz.py` | Invariant fuzzer for the launcher's input mapping (run inside the launcher checkout) |

The engine bug fixes are ordinary commits on this branch (`fcc1c62`, `b381c61`).
The Waydroid removal is a patch because the Waydroid code exists only on the
engine branch, not here or on `main`.

None of these tests involves a real Android system. They check host-side code
only and are not evidence that Android or any game runs.
