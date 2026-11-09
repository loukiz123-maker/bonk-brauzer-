# Settings

Everything the program remembers between runs lives in one file:

```
$XDG_CONFIG_HOME/lynxdroid/settings.json      (default: ~/.config/lynxdroid/)
```

It is plain JSON with a `schema` number, written atomically, mode `0600`.
Unknown keys are kept and reported rather than silently dropped; a wrong type
or an out-of-range number is replaced with the default and the user is told in
a warning that the GUI shows as a note.

**Nothing in this file is a promise the code does not keep.** Every setting
below is read by a specific place, and `tests/test_settings_are_read.py`
fails if a field is added without a reader or without a line in this document.
That test exists because six settings — `input_mode`, `fit_mode`,
`fullscreen`, `capture_pointer`, `allow_service_control`,
`allow_prop_writes` — were once saved and never read, so changing them did
nothing at all.

## Viewport

| Key | Default | Meaning | Read by |
|---|---|---|---|
| `buffer_width` | `1920` | Android buffer width in pixels | `gui/viewmodel.py::DisplayForm.from_settings`, `gui/main_window.py` (initial surface) |
| `buffer_height` | `1080` | Android buffer height in pixels | `gui/viewmodel.py::DisplayForm.from_settings`, `gui/main_window.py` |
| `dpi` | `320` | Android density; changes the `wm density` value | `gui/viewmodel.py::DisplayForm.from_settings` |
| `fit_mode` | `"contain"` | `contain`, `fill` or `integer` — how Android is scaled into the window and where the letterbox bars fall | `gui/main_window.py` → `GameSurface.set_fit_mode` (saved *and* restored) |
| `fullscreen` | `false` | Start the window full screen. F11 toggles it and writes this value back | `gui/main_window.py` (first `showEvent`) |

`fit_mode` is restored at startup, not only saved: the combo box used to
always begin at `contain`, so choosing `integer` lasted until the next launch
while the file still had `integer` in it.

## Input

| Key | Default | Meaning | Read by |
|---|---|---|---|
| `input_mode` | `"auto"` | `auto`, `agent`, `adb` or `none` — which touch transport to prefer. `auto` means "no preference"; the selector then chooses by capability, not by order | `input/pipeline.py::channel_for_mode` → `select_channel(prefer=...)` |
| `capture_pointer` | `false` | Allow the pointer grab that aim mode performs. Off: aim mode still works, but the pointer is neither hidden nor warped back to the centre, and the status bar says so | `gui/main_window.py::_sync_aim_mode` |
| `release_on_focus_loss` | `true` | Release held touches when the window loses focus, so no finger stays down after an Alt-Tab | `gui/main_window.py` |

An unknown `input_mode` is treated as `auto` rather than as "no input": a typo
in a config file must not mute the overlay.

## Backend and privileges

| Key | Default | Meaning | Read by |
|---|---|---|---|
| `backend` | `"waydroid"` | `waydroid` or `fake`. The fake backend exists for the GUI on a machine without Waydroid and is labelled as fake everywhere it appears | `cli/main.py::_backend`, `gui/main_window.py` |
| `allow_service_control` | `false` | Permit starting the **root-owned** `waydroid-container.service` unit. Off by default: without it the user is told the exact `systemctl` command to run themselves | `backend/waydroid.py::_ensure_container` |
| `allow_prop_writes` | `false` | Permit writing persistent Android properties at all. A confirmation dialog is not a substitute for this switch | `backend/waydroid.py::set_prop`, `apply_display` |
| `arm_translation_warning` | `true` | Show the warning that Android apps shipping only arm64 code will not run on this host | `gui/viewmodel.py::banners` |

The two `allow_*` keys are the opt-ins `docs/SECURITY.md` has always
described. Until they were wired up the code performed those actions whenever
a caller asked, so the documented default of "off" was enforced by nothing.

## Interface

| Key | Default | Meaning | Read by |
|---|---|---|---|
| `theme` | `"dark"` | `dark`, `light` or `system`. `system` follows the desktop's colour scheme as reported by Qt | `gui/theme.py::palette_for` |
| `show_bar_thickness` | `true` | Write each letterbox bar's thickness in logical pixels on the bar itself. The bars are always drawn — they mark where a press is refused | `gui/overlay.py::set_bar_labels` |
| `confirm_destructive` | `true` | Ask before deleting a profile or an image | `gui/main_window.py` |

## Diagnostics

| Key | Default | Meaning | Read by |
|---|---|---|---|
| `doctor_on_start` | `true` | Run the read-only doctor checks when the window opens | `gui/main_window.py` |
| `log_level` | `"INFO"` | `DEBUG`, `INFO`, `WARNING` or `ERROR`. Logs go to `$XDG_STATE_HOME/lynxdroid/lynxdroid.log` | `cli/main.py` → `core/logging_setup.py` |

## What is deliberately not a setting

* **The Waydroid binary and image paths.** They come from the installed
  Waydroid, not from us. A setting would let LynxDroid point at files it has no
  business reading.
* **Anything that needs root.** There is no "run as root" switch; the two
  `allow_*` keys decide whether a *user-level* command may be attempted, and
  root commands are never spawned by this process.
