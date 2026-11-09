# Licences and provenance

## This project

LynxDroid's own source: **MIT**. See `LICENSE` at the repository root.

## Runtime dependencies, and what we do with them

| Component | Licence | How we use it | Do we redistribute it? |
|---|---|---|---|
| **Waydroid** | GPL-3.0 | invoked as an installed program through its public CLI (`waydroid status/start/session/app/prop`), read via documented files | **No.** No code copied, no fork, not linked. If this ever changes, patches will be published here. |
| **LineageOS-based Android image** | Apache-2.0 + GPL-2.0 components | user downloads it themselves through Waydroid's OTA channel | **No.** Never bundled, never mirrored. |
| **GAPPS variant** | additionally contains Google-proprietary parts | not used by this project | **No.** Redistribution would be restricted; the UI warns about certification. |
| **PySide6 / Qt 6** | LGPL-3.0 (Qt), LGPL-3.0 (PySide6) | optional GUI extra, installed by the user via pip | **No.** A wheel we build does not embed Qt; it depends on it at runtime. If we later ship a binary that bundles Qt, the LGPL relinking obligation applies and will be documented. |
| **Python standard library** | PSF-2.0 | everything | No |
| **pytest, ruff, mypy** | MIT / MIT / MIT | development only | No |

The whole control layer therefore has **zero mandatory third-party runtime
dependencies**: `pip install lynxdroid` installs nothing but this project.

## Trademarks

"Android", "Google Play", "LineageOS", "Waydroid", "Intel", "Iris Xe", "KDE",
"Qt" are trademarks of their owners. LynxDroid is not affiliated with, endorsed
by, or sponsored by any of them.

## What we will not do

* redistribute Android images, Google components or a licensed ARM translator;
* ship a binary that links GPL code into a proprietary product;
* claim compatibility with a specific game or app store;
* bypass application protections or anti-cheat systems.

## Provenance of design decisions

The technical rules in `docs/ARCHITECTURE.md` and `docs/SECURITY.md` follow the
public Waydroid documentation and the Arch Wiki:

* <https://docs.waydro.id/> — Waydroid overview and usage
* <https://docs.waydro.id/debugging/getting-essential-information> — kernel/binder checks
* <https://docs.waydro.id/usage/waydroid-prop-options> — `persist.waydroid.*` properties
* <https://docs.waydro.id/usage/install-and-run-android-applications> — APK install, ARM note
* <https://wiki.archlinux.org/title/Waydroid> — kernel modules, binderfs, image channels

Those sources were used to check facts, not copied as implementation. Where
they disagree with what a specific machine reports, the machine wins — that is
why `doctor` reads `/proc`, `/sys` and the actual binaries instead of trusting a
table.
