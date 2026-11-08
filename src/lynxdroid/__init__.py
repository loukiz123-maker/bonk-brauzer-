"""LynxDroid - own control layer for an Android userspace on the host Linux kernel.

The package is deliberately split into layers that never import "upwards":

    core       stdlib-only primitives: paths, process execution, diagnostics
    doctor     read-only system inspection built on ``core``
    backend    Android container adapters (Waydroid) and the lifecycle machine
    display    viewport/DPI/rotation geometry and resolution validation
    input      multitouch/keyboard mapping engine and injection channels
    profiles   versioned, atomically stored layout profiles
    apps       installed-package listing and launching
    gui        PySide6 shell (optional, imports everything above)
    cli        text interface (imports everything above except ``gui``)
"""

from __future__ import annotations

from lynxdroid.version import (
    BUILD_STAGE,
    SCHEMA_VERSIONS,
    __version__,
    version_string,
)

__all__ = ["BUILD_STAGE", "SCHEMA_VERSIONS", "__version__", "version_string"]
