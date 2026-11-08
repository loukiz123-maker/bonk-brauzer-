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

| # | Criterion | Kind | Evidence |
|---|---|---|---|
| 2.1 | The platform profile is detected, never assumed | HERE | `tests/doctor/test_kernel_checks.py` |
| 2.2 | A container host is reported as such, with the limits named | HERE | `tests/test_requirements_registry.py::TestChecksAreTraceable` |
| 2.3 | `lynxdroid doctor --json` on the CachyOS laptop, output attached to the issue | TARGET | `lynxdroid doctor --json > doctor.json` |

**Stage 2 status: 2.1–2.2 HERE, 2.3 open.** Hence `DONE` for detection, with
the target run recorded in `docs/ROADMAP.md` as outstanding.

## What this document is for

The specification's rule is that a stage is not complete because the code was
written. This file turns that rule into a checklist that a test can read, so
the answer to "is stage N done?" is a command, not an opinion.
