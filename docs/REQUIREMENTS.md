# Requirement registry

Every requirement from the 20-stage specification, with priority, the test that
proves it, and an honest status.

**Status vocabulary** (stage 15):

| Status | Meaning |
|---|---|
| `НЕ НАЧАТ` | not started |
| `В РАБОТЕ` | in progress |
| `ЗАБЛОКИРОВАН` | cannot proceed without the target machine / a decision |
| `ПРОВЕРЕН` | acceptance test ran, with the command and result recorded |

A successful unit test is **not** evidence of a successful target run. Rows
marked `ПРОВЕРЕН` list the machine they were verified on.

Columns: **ID** · **P** (priority) · **Requirement** · **Test / evidence** · **Status**.

---

## P0 — must work, or the product does not exist

| ID | Requirement | Test / evidence | Status |
|---|---|---|---|
| P0-ARCH | Run Android userspace on the host kernel (no VM in the main path) | `docs/ARCHITECTURE.md §1`, backend contract | `ПРОВЕРЕН` (design), `ЗАБЛОКИРОВАН` (runtime, needs laptop) |
| P0-BINDER | Detect Android Binder properly | `doctor/checks.py::kernel_report`, `tests/cli` doctor tests | `ПРОВЕРЕН` in container: correctly reports `CONFIG_ANDROID_BINDER_IPC` absent |
| P0-WAYLAND | Detect Wayland session, distinguish "missing" from "cannot determine" | `doctor/platform.py::session_info`, `TestSession` | `ПРОВЕРЕН` in container (reports UNKNOWN, not "unsupported") |
| P0-KERNEL | Report the actual kernel, not an assumed one | `platform.os_info()` | `ПРОВЕРЕН` (6.1.158+ reported) |
| P0-NONROOT | GUI and CLI run as a normal user | `session.privileges` check; no `sudo` in any code path | `ПРОВЕРЕН` |
| P0-WAYDROID | Drive the installed backend, do not reinstall it | `backend/waydroid.py`, `test_waydroid.py` | `ПРОВЕРЕН` (unit), `ЗАБЛОКИРОВАН` (live) |
| P0-IMAGES | Show source/licence/size, get consent before any download | `backend/images.py`, `tests/backend/test_images.py` (13 tests) | `ПРОВЕРЕН` |
| P0-DISK | Check free space before an image download | `check_space()`, `disk.space` check | `ПРОВЕРЕН` |
| P0-MEMFD | Probe `memfd_create` without side effects | `platform.memfd_available()` via `ctypes` | `ПРОВЕРЕН` |
| P0-NS | Verify container namespaces exist | `platform.namespaces_available()` | `ПРОВЕРЕН` |
| P0-GPU | Identify the DRM device and renderer honestly | `gpu_report`, `guest_renderer` = UNKNOWN until session runs | `ЗАБЛОКИРОВАН` (no `/dev/dri` here) |
| P0-RENDERNODE | Report render-node access separately from presence | `gpu.render_node` check | `ЗАБЛОКИРОВАН` |
| P0-HWRENDER | Prove Android's renderer, not the host's | `gpu.guest_renderer` deliberately UNKNOWN pre-session | `НЕ НАЧАТ` (needs live Android) |
| P0-GEOMETRY | Input uses the same transform as the image | `tests/display/test_geometry.py` (44 tests incl. round-trips) | `ПРОВЕРЕН` |
| P0-BARS | Coordinates on black bars never produce touches | `test_pointer_on_bar_returns_none` | `ПРОВЕРЕН` |
| P0-RELEASE | No contact survives focus loss / profile switch / stop | `tests/input/test_engine.py::TestFocusLoss` | `ПРОВЕРЕН` |
| P0-POINTER-ID | Pointer ids never reused while open; exhaustion is an error | `test_slot_exhaustion_raises_and_leaves_no_zombie` | `ПРОВЕРЕН` |
| P0-DPAD | Diagonals normalised to the radius | `test_diagonal_is_normalised_to_the_radius` | `ПРОВЕРЕН` |
| P0-STATE | Lifecycle distinguishes container / Android / graphics readiness | `test_session_running_without_boot_is_only_starting` | `ПРОВЕРЕН` |
| P0-ATOMIC | Profile/settings writes are crash-safe; corrupt files never replaced | `tests/profiles/test_store.py` (write-failure test) | `ПРОВЕРЕН` |
| P0-NOSHELL | No `shell=True`, no shell concatenation | `tests/core/test_proc.py::test_shell_metacharacters_are_not_interpreted` | `ПРОВЕРЕН` |
| P0-PATHS | Deletion cannot escape the app directory or follow a symlink out | `test_delete_refuses_to_escape_the_directory` | `ПРОВЕРЕН` |
| P0-EXITCODES | Documented, stable exit codes | `docs/CLI.md`, `tests/cli` | `ПРОВЕРЕН` |
| P0-NO-SECRETS | Logs are redacted; exports contain no secrets | `test_export_contains_no_secret_fields`, `logging_setup.redact` | `ПРОВЕРЕН` |

## P1 — important, but the product survives without it

| ID | Requirement | Test / evidence | Status |
|---|---|---|---|
| P1-OS | Warn on non-Arch distributions | `os.distro` check | `ПРОВЕРЕН` |
| P1-PKG | Use the real package manager (pacman), verify names | `platform.package_manager()` | `ПРОВЕРЕН` (reports `apt` here, and says so) |
| P1-KDE | KWin-specific window/embedding notes | `session.desktop`, `capabilities.notes` | `В РАБОТЕ` |
| P1-BINDERFS | Distinguish binderfs from classic binder nodes | `kernel.binderfs` check | `ПРОВЕРЕН` |
| P1-SERVICE | Report service state without changing the unit | `waydroid.service_state` | `ПРОВЕРЕН` |
| P1-CFG | Read `waydroid.cfg` without rewriting it | `_extract_cfg_summary` | `ПРОВЕРЕН` |
| P1-MESA | Identify the Mesa driver actually present | `gpu.mesa` | `ЗАБЛОКИРОВАН` |
| P1-VULKAN | Report Vulkan ICD presence separately from OpenGL | `gpu.vulkan` | `ЗАБЛОКИРОВАН` |
| P1-PSI | PSI is optional, reported as such | `kernel.psi` | `ПРОВЕРЕН` |
| P1-MULTITOUCH | Real simultaneous touches through a supported channel | `TestAgentChannel` (protocol, 10 ids), `android_agent/` | `ЗАБЛОКИРОВАН` (needs APK on device) |
| P1-SINGLE-TOUCH | The single-touch channel is labelled as such and disables the overlay | `test_reports_single_touch_only`, `overlay_control` | `ПРОВЕРЕН` |
| P1-DISPLAY-VALIDATE | Reject zero/negative/overflow/absurd resolutions | `tests/display/test_resolution.py` (36 tests) | `ПРОВЕРЕН` |
| P1-APPLY-MODE | Say which changes need a restart | `suggest_apply_mode`, `apply_display` steps | `ПРОВЕРЕН` |
| P1-PROFILES | Create/rename/copy/save/reset/delete with confirmation | `ProfileStore` + CLI + GUI actions | `ПРОВЕРЕН` (logic), `В РАБОТЕ` (GUI untested on a display) |
| P1-PROFILE-BINDING | Bind to package (+version warning), never to a title | `Target`, `version_mismatch`, `profile_rows` | `ПРОВЕРЕН` |
| P1-EXPORT | Export/import with validation and no secrets | `TestImportExport` | `ПРОВЕРЕН` |
| P1-UI-STATES | Unsupported controls disabled with a reason, never hidden | `tests/gui/test_viewmodel.py::TestControls` | `ПРОВЕРЕН` |
| P1-CONTRAST | Body text meets WCAG AA | `tests/gui/test_viewmodel.py::TestTheme` | `ПРОВЕРЕН` |
| P1-NONBLOCK | GUI never blocks on a subprocess | `gui/workers.py`, `Poller` | `В РАБОТЕ` (no display to test on) |
| P1-MEASURE | Measure latency/throughput rather than claim it | `FrameStats` (median/p95), no numbers invented | `В РАБОТЕ` |
| P1-APK | Validate APKs before install | `apps/catalog.inspect_apk`, CLI tests | `ПРОВЕРЕН` |
| P1-CI | CI builds and runs the suite | `.github/workflows/ci.yml` | `ПРОВЕРЕН` (workflow authored) |

## Explicitly deferred (stage 12 and later)

| Requirement | Why it is not done |
|---|---|
| Multiple independent Android instances | Waydroid does not document isolated multi-container operation; several windows ≠ several containers. Marked `UNTESTED` in capabilities. Stage 12: "not at the expense of the working main mode". |
| Split APK / AAB installs | Needs its own installer path and tests; reported `UNSUPPORTED`. |
| ARM-only app translation | Needs a licensed translator; not bundled, warned about in the UI. |
| Google Play / Play Integrity | Not guaranteed, not bundled. |
| Screen recording, screenshots, GPS spoofing, bots | Explicitly out of scope (stage 20). |
| C++ port | Stage 18-19: **must not start** until the Python baseline passes end-to-end on the target machine. Plan only: `docs/ARCHITECTURE.md §8`. |

## What blocks progress right now

The specification is unambiguous: if a critical test cannot run on the target
machine, stop and list the blockers rather than declaring the stage complete.

| Blocker | Consequence |
|---|---|
| No access to the CachyOS laptop (this sandbox is a Debian container) | `start`, GPU renderer, KWin embedding, real multitouch remain `ЗАБЛОКИРОВАН` |
| No Wayland compositor in the sandbox | GUI widgets are never instantiated; only `gui/viewmodel.py` is exercised |
| No `libGL`/`libEGL`/`libxkbcommon` in the sandbox | PySide6 imports fail here by design of the environment (documented, not worked around) |
| Android SDK absent | `android_agent/` cannot be compiled here |
| No `INJECT_EVENTS` grant possible | The multitouch path cannot be verified without a privileged install or a debug image |

Commands that would unblock each item are in `docs/TESTING.md § Target machine`.
