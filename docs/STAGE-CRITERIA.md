# Stage completion criteria

A stage is finished when its criteria below are *checked*, not when its code
exists. Two kinds of criterion appear here deliberately:

* **HERE** — provable in this environment (an automated test, a command whose
  output is recorded).
* **CI** — provable by a named job in `.github/workflows/ci.yml`; the case
  where a single local interpreter cannot stand in for the others (Python
  3.10 alongside 3.12, a clone with no venv).
* **TARGET** — provable only on the CachyOS machine, with the exact command
  whose output would settle it.

A stage with an open TARGET criterion is `PARTIAL` or `BLOCKED` in
`docs/ROADMAP.md`, never `DONE`. Nothing advances to the next stage while a
HERE criterion for the current one is open; TARGET criteria are recorded and
carried forward, because the machine is not available yet.

`tests/test_stage_criteria.py` checks this document against the code: every
criterion that claims to be verified HERE must cite a test that exists and
passes, and every stage row must say which criteria are open.

## Stage 1 — quality rules, requirement registry, stage gates

| # | Criterion | Kind | Evidence |
|---|---|---|---|
| 1.1 | No `TODO`, `FIXME` or `XXX` left in `src/` | HERE | `tests/test_stage_criteria.py::TestStageOneIsActuallySatisfied` |
| 1.2 | Every requirement `doctor` cites is declared in the registry | HERE | `tests/test_requirements_registry.py::TestChecksAreTraceable` |
| 1.3 | Every documented CLI command exists, and every command is documented | HERE | `tests/test_doc_counts.py::TestCliDocumentation` |
| 1.4 | Every test count quoted in a document matches `pytest --collect-only` | HERE | `tests/test_doc_counts.py` (whole module) |
| 1.5 | Every setting saved by the program is read by some code path | HERE | `tests/test_settings_are_read.py::TestEverySettingIsRead` |
| 1.6 | Every setting has a row in `docs/SETTINGS.md` naming the module that reads it | HERE | `tests/test_settings_are_read.py::TestSettingsTableMatchesTheCode` |
| 1.7 | Every module a docstring names exists (resolved by import) | HERE | `tests/test_layers.py::TestDocstringModuleReferences` |
| 1.8 | Nothing is written to `/var`, `/usr`, `/etc`, `/opt`, `/boot` by user code | HERE | `tests/test_layers.py::TestNothingIsWrittenToSystemDirectories` |
| 1.9 | `ruff check`, `ruff format --check`, `mypy --strict` are clean | CI | jobs `lint` and `types` |
| 1.10 | The suite passes on Python 3.10, 3.11 and 3.12 | CI | job `tests` (matrix 3.10, 3.11, 3.12) |
| 1.11 | A fresh clone installs and runs: `scripts/install.sh` then `lynxdroid version --json` | CI | job `install-script`, plus `tests/test_install_script.py` |
| 1.12 | The package works with no PySide6 present | CI | job `no-qt`, plus `tests/gui/test_viewmodel.py` |
| 1.13 | Every defect row says how it was caught and ends with a status | HERE | `tests/test_stage_criteria.py::TestStageOneIsActuallySatisfied` |

**Stage 1 status: criteria 1.1–1.13 are HERE and enforced by tests.** That is
what "DONE" means in the roadmap row, and it is all it means: it says nothing
about whether the product works on the target machine.

## Stage 2 — goal and target machine profile

The stage has a second half that the first draft of this table missed: knowing
facts is not the same as judging them. `doctor` lists what it observed;
`profile` decides what those observations mean for this machine. Both exist,
and they must not contradict each other.

| # | Criterion | Kind | Evidence |
|---|---|---|---|
| 2.1 | The platform profile is detected, never assumed | HERE | `tests/doctor/test_kernel_checks.py`, `tests/doctor/test_host_profile.py` |
| 2.2 | A container host is reported as such, with the limits named | HERE | `tests/test_requirements_registry.py::TestChecksAreTraceable` |
| 2.3 | `lynxdroid profile --json` on the CachyOS laptop, output attached to the issue | TARGET | `lynxdroid profile --json > profile.json` |
| 2.4 | Facts and verdict are separate: every precondition carries the fact it was decided from, and an unreadable fact is `None`, never a plausible default | HERE | `tests/doctor/test_host_profile.py::TestUnknownIsNotReady` |
| 2.5 | The verdict is the worst precondition; `UNKNOWN` is never rounded up to `READY` | HERE | `tests/doctor/test_host_profile.py::TestTheVerdictIsTheWorst` |
| 2.6 | Every `BLOCKED` and `PARTIAL` names the command that would change it | HERE | `tests/doctor/test_host_profile.py::TestEveryVerdictIsActionable` |
| 2.7 | `profile` and `doctor --strict` never disagree about the same host | HERE | `tests/doctor/test_host_profile.py::TestTheTwoVerdictsAgree`, `tests/cli/test_cli.py::TestProfileCommand` |
| 2.8 | A blocked host exits non-zero, so the answer is scriptable | HERE | `tests/cli/test_cli.py::TestProfileCommand` |

**Stage 2 status: 2.1–2.2 and 2.4–2.8 HERE, 2.3 open.** The verdict half of the
stage is complete and enforced; the target-laptop run is waiting on the user,
which is why `docs/ROADMAP.md` marks the stage `DONE` for detection and lists
2.3 as outstanding. Criterion 2.3 now points at `profile --json` rather than
`doctor --json`: a report of raw checks from the laptop would tell us what it
saw, but the point of the stage is a verdict about the machine, and the profile
document carries both.

One measured disagreement is worth recording, because it is the reason 2.7
exists. On this container `checks` reports `session.wayland` as `UNKNOWN`
("no graphical session detected") and `Report.blockers_for` counts `UNKNOWN` as
blocking, while a first version of `HostProfile` reported the same fact as
merely unchecked. Both behaviours were defensible in isolation; together they
were a defect, because a user asking "can I run Android here" would get
`BLOCKED` from one command and a non-answer from the other. The profile now
follows `doctor`: an unchecked prerequisite is not a green light, and the two
commands are pinned together by a test that runs both against the live host.

## What this document is for

The specification's rule is that a stage is not complete because the code was
written. This file turns that rule into a checklist that a test can read, so
the answer to "is stage N done?" is a command, not an opinion.
