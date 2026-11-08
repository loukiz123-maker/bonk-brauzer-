"""``lynxdroid`` command line interface.

Design rules:

* exit codes are documented and stable (see ``core.errors.ExitCode``);
* ``--json`` works on every inspection command for scripts and CI;
* nothing destructive happens without ``--yes``;
* the GUI is never required: every diagnostic works headless.

Commands
    doctor          read-only host and backend diagnostics
    status          backend lifecycle snapshot
    start / stop    session control (consent-gated where root is needed)
    apps            list / launch / install Android packages
    display         show and change viewport properties
    profiles        list / show / validate / export / import / delete
    gui             launch the Qt shell (needs PySide6)
    version         version banner
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from collections.abc import Sequence
from pathlib import Path

from lynxdroid import version as version_mod
from lynxdroid.apps.catalog import inspect_apk, merge_apps
from lynxdroid.backend import images
from lynxdroid.backend.contract import AndroidBackend
from lynxdroid.core import proc
from lynxdroid.core.diagnostics import Status
from lynxdroid.core.errors import Blocked, DependencyMissing, ExitCode, LynxError, UsageError
from lynxdroid.core.logging_setup import configure as configure_logging
from lynxdroid.core.paths import AppPaths
from lynxdroid.display.geometry import ViewportGeometry
from lynxdroid.display.resolution import PRESETS, validate_display
from lynxdroid.doctor import checks as doctor_checks
from lynxdroid.profiles.model import Profile, ProfileError, ProfileNotFound
from lynxdroid.profiles.store import ProfileStore
from lynxdroid.settings import SettingsStore

# --------------------------------------------------------------------------
# entry point
# --------------------------------------------------------------------------


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(list(argv) if argv is not None else None)

    paths = AppPaths.resolve(Path(args.root).expanduser() if args.root else None)
    try:
        paths.ensure()
    except OSError as exc:
        print(f"lynxdroid: cannot create {paths.config}: {exc}", file=sys.stderr)
        return int(ExitCode.FAIL)

    settings_store = SettingsStore(paths)
    log_level = "DEBUG" if args.verbose else None
    settings = None
    if args.command != "version":
        try:
            settings, _ = settings_store.load()
        except LynxError:
            settings = None
    configure_logging(
        paths.log_file,
        level=_level(settings.log_level if settings else "INFO"),
        console_level=_level(log_level) if log_level else None,
    )

    handler = args.func
    try:
        return int(handler(args, paths, settings_store))
    except LynxError as exc:
        if getattr(args, "json", False):
            print(json.dumps({"error": exc.as_dict()}, indent=2, ensure_ascii=False))
        else:
            print(f"lynxdroid: {exc.message}", file=sys.stderr)
            if exc.hint:
                print(f"  hint: {exc.hint}", file=sys.stderr)
        return int(exc.exit_code)
    except ProfileNotFound as exc:
        print(f"lynxdroid: {exc}", file=sys.stderr)
        return int(ExitCode.FAIL)
    except ProfileError as exc:
        print(f"lynxdroid: invalid profile: {exc}", file=sys.stderr)
        return int(ExitCode.FAIL)
    except KeyboardInterrupt:
        print("lynxdroid: interrupted", file=sys.stderr)
        return int(ExitCode.FAIL)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="lynxdroid",
        description="Own control layer for an Android container on the host Linux kernel.",
        epilog="Documented exit codes: 0 ok, 1 failure, 2 usage, 3 missing dependency, "
        "4 permission, 5 unsupported, 6 blocked.",
    )
    parser.add_argument("--root", help="use this directory instead of XDG paths (testing)")
    parser.add_argument("-v", "--verbose", action="store_true", help="debug logging to stderr")
    parser.add_argument("--version", action="version", version=version_mod.version_string())
    sub = parser.add_subparsers(dest="command", required=True)

    # doctor ------------------------------------------------------------
    p_doctor = sub.add_parser("doctor", help="inspect the host without changing anything")
    p_doctor.add_argument("--json", action="store_true", help="machine-readable report")
    p_doctor.add_argument(
        "--area",
        action="append",
        choices=["environment", "session", "kernel", "gpu", "waydroid", "storage"],
        help="limit the report to specific areas (repeatable)",
    )
    p_doctor.add_argument("--quiet", action="store_true", help="hide OK rows in text mode")
    p_doctor.add_argument(
        "--strict", action="store_true", help="exit 6 when a required check is not OK"
    )
    p_doctor.set_defaults(func=cmd_doctor)

    # status ------------------------------------------------------------
    p_status = sub.add_parser("status", help="backend lifecycle snapshot")
    p_status.add_argument("--json", action="store_true")
    p_status.set_defaults(func=cmd_status)

    # start / stop ------------------------------------------------------
    p_start = sub.add_parser("start", help="start the Android session")
    p_start.add_argument("--timeout", type=float, default=90.0)
    p_start.add_argument("--json", action="store_true")
    p_start.set_defaults(func=cmd_start)

    p_stop = sub.add_parser("stop", help="stop the Android session")
    p_stop.add_argument("--timeout", type=float, default=45.0)
    p_stop.add_argument("--json", action="store_true")
    p_stop.set_defaults(func=cmd_stop)

    # apps --------------------------------------------------------------
    p_apps = sub.add_parser("apps", help="installed Android applications")
    apps_sub = p_apps.add_subparsers(dest="apps_command", required=True)
    p_apps_list = apps_sub.add_parser("list")
    p_apps_list.add_argument("--json", action="store_true")
    p_apps_list.set_defaults(func=cmd_apps_list)

    p_apps_launch = apps_sub.add_parser("launch")
    p_apps_launch.add_argument("package")
    p_apps_launch.set_defaults(func=cmd_apps_launch)

    p_apps_install = apps_sub.add_parser("install")
    p_apps_install.add_argument("apk")
    p_apps_install.add_argument("--yes", action="store_true", help="confirm the install")
    p_apps_install.set_defaults(func=cmd_apps_install)

    # display -----------------------------------------------------------
    p_display = sub.add_parser("display", help="viewport resolution and DPI")
    display_sub = p_display.add_subparsers(dest="display_command", required=True)
    p_display_show = display_sub.add_parser("show")
    p_display_show.add_argument("--json", action="store_true")
    p_display_show.set_defaults(func=cmd_display_show)

    p_display_set = display_sub.add_parser("set")
    p_display_set.add_argument("width", type=int)
    p_display_set.add_argument("height", type=int)
    p_display_set.add_argument("dpi", type=int)
    p_display_set.add_argument(
        "--yes", action="store_true", help="confirm the persistent property change"
    )
    p_display_set.add_argument("--json", action="store_true")
    p_display_set.set_defaults(func=cmd_display_set)

    p_display_presets = display_sub.add_parser("presets")
    p_display_presets.set_defaults(func=cmd_display_presets)

    # profiles ----------------------------------------------------------
    p_profiles = sub.add_parser("profiles", help="layout profiles")
    prof_sub = p_profiles.add_subparsers(dest="profiles_command", required=True)
    p_prof_list = prof_sub.add_parser("list")
    p_prof_list.add_argument("--json", action="store_true")
    p_prof_list.set_defaults(func=cmd_profiles_list)

    p_prof_validate = prof_sub.add_parser("validate")
    p_prof_validate.add_argument("path", nargs="?")
    p_prof_validate.set_defaults(func=cmd_profiles_validate)

    p_prof_export = prof_sub.add_parser("export")
    p_prof_export.add_argument("profile_id")
    p_prof_export.add_argument("destination")
    p_prof_export.set_defaults(func=cmd_profiles_export)

    p_prof_import = prof_sub.add_parser("import")
    p_prof_import.add_argument("source")
    p_prof_import.add_argument("--yes", action="store_true")
    p_prof_import.set_defaults(func=cmd_profiles_import)

    p_prof_rename = prof_sub.add_parser("rename")
    p_prof_rename.add_argument("profile_id")
    p_prof_rename.add_argument("new_name")
    p_prof_rename.set_defaults(func=cmd_profiles_rename)

    p_prof_delete = prof_sub.add_parser("delete")
    p_prof_delete.add_argument("profile_id")
    p_prof_delete.add_argument("--yes", action="store_true")
    p_prof_delete.set_defaults(func=cmd_profiles_delete)

    # image -------------------------------------------------------------
    p_image = sub.add_parser("image", help="Android images (consent-gated)")
    image_sub = p_image.add_subparsers(dest="image_command", required=True)

    p_image_plan = image_sub.add_parser(
        "plan", help="show sources, licence, size and steps; downloads nothing"
    )
    p_image_plan.add_argument(
        "--channel", choices=sorted(images.CHANNELS), default="lineage-vanilla"
    )
    p_image_plan.add_argument("--json", action="store_true")
    p_image_plan.set_defaults(func=cmd_image_plan)

    p_image_prepare = image_sub.add_parser(
        "prepare", help="download the OTA archives into the LynxDroid cache"
    )
    p_image_prepare.add_argument(
        "--channel", choices=sorted(images.CHANNELS), default="lineage-vanilla"
    )
    p_image_prepare.add_argument(
        "--yes", action="store_true", help="confirm the download after reading the plan"
    )
    p_image_prepare.add_argument(
        "--allow-unverified",
        action="store_true",
        help="proceed although the channel publishes no checksum",
    )
    p_image_prepare.add_argument("--json", action="store_true")
    p_image_prepare.set_defaults(func=cmd_image_prepare)

    # perf --------------------------------------------------------------
    p_perf = sub.add_parser(
        "perf", help="measure the latency of the pipeline we own (never end-to-end)"
    )
    p_perf.add_argument("--iterations", type=int, default=200)
    p_perf.add_argument("--json", action="store_true")
    p_perf.set_defaults(func=cmd_perf)

    # gui ---------------------------------------------------------------
    p_gui = sub.add_parser("gui", help="launch the graphical shell")
    p_gui.add_argument("--backend", choices=["waydroid", "fake"], default=None)
    p_gui.add_argument(
        "--offscreen",
        action="store_true",
        help="render with the offscreen platform (screenshots/tests)",
    )
    p_gui.set_defaults(func=cmd_gui)

    # version -----------------------------------------------------------
    p_version = sub.add_parser("version", help="print the version banner")
    p_version.add_argument("--json", action="store_true")
    p_version.set_defaults(func=cmd_version)

    return parser


# --------------------------------------------------------------------------
# commands
# --------------------------------------------------------------------------


def cmd_version(args: argparse.Namespace, paths: AppPaths, store: SettingsStore) -> int:
    if args.json:
        print(
            json.dumps(
                {
                    "version": version_mod.__version__,
                    "stage": version_mod.BUILD_STAGE,
                    "schemas": version_mod.SCHEMA_VERSIONS,
                    "python": ".".join(map(str, sys.version_info[:3])),
                    "gui_available": shutil.which("python3") is not None and _qt_available(),
                },
                indent=2,
            )
        )
        return int(ExitCode.OK)
    print(version_mod.version_string())
    print(f"  python       {'.'.join(map(str, sys.version_info[:3]))}")
    available, reason = _qt_status()
    print(f"  qt shell     {'available' if available else reason}")
    print(f"  config       {paths.config}")
    print(f"  profiles     {paths.profiles_dir}")
    return int(ExitCode.OK)


def cmd_doctor(args: argparse.Namespace, paths: AppPaths, store: SettingsStore) -> int:
    areas = tuple(args.area) if args.area else None
    report = doctor_checks.full_report(areas=areas)
    report.meta["paths"] = {
        "config": str(paths.config),
        "data": str(paths.data),
        "state": str(paths.state),
    }
    if args.json:
        print(report.to_json())
    else:
        print(f"LynxDroid doctor - {version_mod.version_string()}")
        print(f"collected: {report.meta.get('areas', {})}\n")
        print(report.to_text(show_ok=not args.quiet))
    ready, blockers = doctor_checks.prerequisites_state(report)
    if not args.json:
        print()
        if ready:
            print("verdict: required container prerequisites are OK")
        else:
            print("verdict: BLOCKED - these required checks are not OK:")
            for check in blockers:
                print(f"  - {check.id}: {check.detail}")
                if check.next_step:
                    print(f"      next: {check.next_step}")
    if args.strict and not ready:
        return int(ExitCode.BLOCKED)
    return int(ExitCode.OK if report.worst is not Status.ERROR else ExitCode.FAIL)


def _backend(args: argparse.Namespace, settings_store: SettingsStore) -> AndroidBackend:
    from lynxdroid.backend.waydroid import BackendConfig, FakeBackend, WaydroidBackend

    settings, _ = settings_store.load()
    kind = getattr(args, "backend", None) or settings.backend
    if kind == "fake":
        return FakeBackend()
    return WaydroidBackend(BackendConfig())


def cmd_status(args: argparse.Namespace, paths: AppPaths, store: SettingsStore) -> int:
    backend = _backend(args, store)
    snapshot = backend.snapshot()
    payload = {
        "backend": backend.name,
        "capabilities": backend.capabilities().as_dict(),
        "snapshot": snapshot.as_dict(),
    }
    if args.json:
        print(json.dumps(payload, indent=2, ensure_ascii=False))
    else:
        print(f"backend: {backend.name} {backend.capabilities().version}")
        print(f"state:   {snapshot.state.value}  ({snapshot.detail})")
        for layer, readiness in snapshot.readiness.items():
            print(f"  {layer:<10} {readiness.value}")
        if snapshot.raw_status:
            print("reported by backend:")
            for key, value in sorted(snapshot.raw_status.items()):
                print(f"  {key:<20} {value}")
        if snapshot.error:
            print(f"error: {snapshot.error}")
    return int(ExitCode.OK if snapshot.state.value != "error" else ExitCode.FAIL)


def cmd_start(args: argparse.Namespace, paths: AppPaths, store: SettingsStore) -> int:
    backend = _backend(args, store)
    snapshot = backend.start(timeout_s=args.timeout)
    if args.json:
        print(json.dumps(snapshot.as_dict(), indent=2))
    else:
        print(f"Android is {snapshot.state.value}: {snapshot.detail}")
    return int(ExitCode.OK)


def cmd_stop(args: argparse.Namespace, paths: AppPaths, store: SettingsStore) -> int:
    backend = _backend(args, store)
    snapshot = backend.stop(timeout_s=args.timeout)
    if args.json:
        print(json.dumps(snapshot.as_dict(), indent=2))
    else:
        print(f"Android is {snapshot.state.value}: {snapshot.detail}")
    return int(ExitCode.OK)


def cmd_apps_list(args: argparse.Namespace, paths: AppPaths, store: SettingsStore) -> int:
    backend = _backend(args, store)
    store_profiles = ProfileStore(paths)
    profiled = {p.target.package for p in store_profiles.load_all()}
    try:
        raw_apps = backend.list_apps()
    except LynxError:
        raw_apps = []
    if not raw_apps and not backend.capabilities().supports("app-list"):
        if args.json:
            print("[]")
            return int(ExitCode.OK)
        raise LynxError(
            "this backend cannot list installed applications",
            hint="start the session first, then retry",
        )
    entries = merge_apps(raw_apps, profiled_packages=profiled)
    if args.json:
        print(json.dumps([entry.__dict__ for entry in entries], indent=2, ensure_ascii=False))
        return int(ExitCode.OK)
    if not entries:
        print("no installed packages reported by the backend")
        return int(ExitCode.OK)
    width = max(len(entry.package) for entry in entries)
    for entry in entries:
        marker = "*" if entry.has_profile else " "
        print(f"{marker} {entry.package:<{width}}  {entry.label}")
    print(f"\n{len(entries)} package(s); '*' marks packages with a layout profile")
    return int(ExitCode.OK)


def cmd_apps_launch(args: argparse.Namespace, paths: AppPaths, store: SettingsStore) -> int:
    """Launch a package and report which layout applies to it.

    Stage 9 binds a profile to the application id and version and requires a
    warning when the version changes. Without this the binding lived in the
    model and never reached the user.
    """
    backend = _backend(args, store)
    backend.launch_app(args.package)
    print(f"launched {args.package}")

    profile_store = ProfileStore(paths)
    matches = profile_store.find_for_package(args.package)
    if not matches:
        print(
            "  no layout profile is bound to this package yet; create one in the "
            "GUI or import one with 'lynxdroid profiles import'"
        )
        return int(ExitCode.OK)

    installed = _installed_version(backend, args.package)
    print(f"  {len(matches)} layout profile(s) bound to this package:")
    for profile in matches:
        print(f"    {profile.name}  ({profile.id})")
        if profile.target.version_mismatch(installed):
            print(
                "      warning: written for version "
                f"{profile.target.package_version}, installed is "
                f"{installed or 'unknown'} - key positions may no longer match "
                "the game's controls"
            )
    return int(ExitCode.OK)


def _installed_version(backend: AndroidBackend, package: str) -> str | None:
    """Best-effort version from the real app list, or ``None``.

    A missing answer must never be reported as a version mismatch, so the
    caller treats ``None`` as "do not warn". ``list_apps`` is the only source
    the backend contract offers, and it may not carry a version at all.
    """
    try:
        for entry in backend.list_apps():
            if entry.get("package") == package:
                version = entry.get("version")
                return str(version) if version else None
    except (LynxError, NotImplementedError):
        return None
    return None


def cmd_apps_install(args: argparse.Namespace, paths: AppPaths, store: SettingsStore) -> int:
    candidate = inspect_apk(args.apk)
    print(candidate.describe())
    if not args.yes:
        print("\nrefusing to install without confirmation; re-run with --yes")
        return int(ExitCode.BLOCKED)
    backend = _backend(args, store)
    ok, message = backend.install_apk(str(candidate.path))
    print(message)
    return int(ExitCode.OK if ok else ExitCode.FAIL)


def cmd_display_show(args: argparse.Namespace, paths: AppPaths, store: SettingsStore) -> int:
    backend = _backend(args, store)
    try:
        values = backend.read_display()
    except LynxError as exc:
        if args.json:
            print(json.dumps({"error": exc.as_dict()}, indent=2, ensure_ascii=False))
            return int(ExitCode.OK)
        raise
    if args.json:
        print(json.dumps(values, indent=2))
        return int(ExitCode.OK)
    if not values:
        print("backend reported no display values (is the session running?)")
        return int(ExitCode.OK)
    for key, value in sorted(values.items()):
        print(f"{key:<32} {value}")
    return int(ExitCode.OK)


def cmd_display_set(args: argparse.Namespace, paths: AppPaths, store: SettingsStore) -> int:
    request, issues = validate_display(args.width, args.height, args.dpi)
    for issue in issues:
        print(f"{issue.severity}: {issue.field}: {issue.message}", file=sys.stderr)
    if request is None:
        return int(ExitCode.USAGE)
    if not args.yes:
        print(
            f"would set the Android viewport to {request.width}x{request.height}@{request.dpi}\n"
            "this writes persist.waydroid.width/height (needs a session restart) and runs\n"
            "'wm density'. re-run with --yes to apply",
        )
        return int(ExitCode.BLOCKED)
    backend = _backend(args, store)
    steps = backend.apply_display(request.width, request.height, request.dpi)
    if args.json:
        print(json.dumps({"steps": steps}, indent=2))
    else:
        for step in steps:
            print(f"  {step}")
    return int(ExitCode.OK)


def cmd_display_presets(args: argparse.Namespace, paths: AppPaths, store: SettingsStore) -> int:
    for preset in PRESETS:
        note = f"  # {preset.note}" if preset.note else ""
        print(f"{preset.name:<20} {preset.width}x{preset.height} @ {preset.dpi}dpi{note}")
    return int(ExitCode.OK)


def cmd_profiles_list(args: argparse.Namespace, paths: AppPaths, store: SettingsStore) -> int:
    profile_store = ProfileStore(paths)
    rows: list[dict[str, object]] = []
    for stored, path, error in profile_store.iter_profiles():
        if stored is None:
            rows.append({"file": path.name, "error": error})
            continue
        profile = stored.profile
        rows.append(
            {
                "id": profile.id,
                "name": profile.name,
                "package": profile.target.package,
                "elements": len(profile.elements),
                "reference": f"{profile.reference.width}x{profile.reference.height}",
                "updated": profile.updated_at.isoformat(),
            }
        )
    if args.json:
        print(json.dumps(rows, indent=2, ensure_ascii=False))
        return int(ExitCode.OK)
    if not rows:
        print(f"no profiles yet in {profile_store.directory}")
        return int(ExitCode.OK)
    for row in rows:
        if "error" in row:
            print(f"! {row['file']}: {row['error']}")
            continue
        print(
            f"{row['id']}  {row['name']:<28} {row['package']:<32} "
            f"{row['elements']:>2} keys  {row['reference']}"
        )
    return int(ExitCode.OK)


def cmd_profiles_validate(args: argparse.Namespace, paths: AppPaths, store: SettingsStore) -> int:
    profile_store = ProfileStore(paths)
    failed = 0
    if args.path:
        raw = Path(args.path)
        try:
            from lynxdroid.core.storage import read_json
            from lynxdroid.profiles.codec import profile_from_dict

            profile = profile_from_dict(read_json(raw))
        except (LynxError, ProfileError, FileNotFoundError) as exc:
            print(f"✘ {raw}: {exc}")
            return int(ExitCode.FAIL)
        print(f"✔ {raw}: {profile.name} ({len(profile.elements)} elements)")
        return int(ExitCode.OK)
    for stored, path, error in profile_store.iter_profiles():
        if stored is None:
            print(f"✘ {path.name}: {error}")
            failed += 1
        else:
            print(f"✔ {path.name}: {stored.profile.name}")
    return int(ExitCode.FAIL if failed else ExitCode.OK)


def cmd_profiles_export(args: argparse.Namespace, paths: AppPaths, store: SettingsStore) -> int:
    profile_store = ProfileStore(paths)
    profile = profile_store.load(args.profile_id)
    destination = profile_store.export(profile, Path(args.destination))
    print(f"exported '{profile.name}' to {destination}")
    return int(ExitCode.OK)


def cmd_profiles_import(args: argparse.Namespace, paths: AppPaths, store: SettingsStore) -> int:
    if not args.yes:
        print(f"will validate and import {args.source}; re-run with --yes")
        return int(ExitCode.BLOCKED)
    profile_store = ProfileStore(paths)
    profile = profile_store.import_file(Path(args.source))
    print(f"imported '{profile.name}' ({profile.id}) for {profile.target.package}")
    return int(ExitCode.OK)


def cmd_profiles_rename(args: argparse.Namespace, paths: AppPaths, store: SettingsStore) -> int:
    profile_store = ProfileStore(paths)
    before = profile_store.load(args.profile_id)
    try:
        after = profile_store.rename(args.profile_id, args.new_name)
    except ProfileError as exc:
        print(f"lynxdroid: cannot rename: {exc}", file=sys.stderr)
        return int(ExitCode.FAIL)
    print(f"renamed '{before.name}' ({after.id}) to '{after.name}'")
    return int(ExitCode.OK)


def cmd_profiles_delete(args: argparse.Namespace, paths: AppPaths, store: SettingsStore) -> int:
    profile_store = ProfileStore(paths)
    profile = profile_store.load(args.profile_id)
    if not args.yes:
        print(f"will delete '{profile.name}' (a backup is kept); re-run with --yes")
        return int(ExitCode.BLOCKED)
    backup = profile_store.delete(args.profile_id)
    print(f"deleted; backup at {backup}" if backup else "deleted")
    return int(ExitCode.OK)


def _image_channel(args: argparse.Namespace) -> str:
    return str(args.channel)


def cmd_image_plan(args: argparse.Namespace, paths: AppPaths, store: SettingsStore) -> int:
    """Read-only: describe the download, check disk space, change nothing."""
    plan = images.plan_for(_image_channel(args), paths.cache)
    enough, space_note = images.check_space(plan.target_dir, plan.estimated_bytes)
    if args.json:
        print(
            json.dumps(
                {
                    "schema": version_mod.SCHEMA_VERSIONS["doctor-report"],
                    "channel": plan.channel,
                    "system_url": plan.system_url,
                    "vendor_url": plan.vendor_url,
                    "estimated_bytes": plan.estimated_bytes,
                    "target_dir": str(plan.target_dir),
                    "licence": plan.licence_note,
                    "checksums_available": plan.checksums_available,
                    "requires_root_to_install": plan.requires_root_to_install,
                    "space_ok": enough,
                    "space": space_note,
                    "steps": list(plan.steps),
                },
                indent=2,
                ensure_ascii=False,
            )
        )
        return int(ExitCode.OK if enough else ExitCode.BLOCKED)
    print(plan.describe())
    print()
    print(f"Disk space: {space_note}")
    if not enough:
        print("Not enough free space: nothing would be downloaded.", file=sys.stderr)
        return int(ExitCode.BLOCKED)
    print()
    print("To download: lynxdroid image prepare --channel " + plan.channel + " --yes")
    return int(ExitCode.OK)


def cmd_image_prepare(args: argparse.Namespace, paths: AppPaths, store: SettingsStore) -> int:
    """Download, with consent. Never installs, never touches /var/lib."""
    channel = _image_channel(args)
    plan = images.plan_for(channel, paths.cache)
    enough, space_note = images.check_space(plan.target_dir, plan.estimated_bytes)
    if not enough:
        raise Blocked(
            f"not enough free space: {space_note}",
            hint="free space under the LynxDroid cache or choose another --root",
        )
    if not args.yes:
        print(plan.describe())
        print()
        print(f"Disk space: {space_note}")
        print()
        raise Blocked(
            "downloading needs explicit consent",
            hint="review the plan above, then add --yes",
        )
    if not plan.checksums_available and not args.allow_unverified:
        raise Blocked(
            "this channel publishes no checksum, so the download cannot be verified",
            hint="re-run with --allow-unverified if you accept a locally computed SHA-256",
        )
    metadata = images.fetch_ota_metadata(plan.system_url)
    results: list[images.FetchResult] = []
    for key in ("system", "vendor"):
        entry = metadata.get(key)
        if not isinstance(entry, dict):
            raise LynxError(f"the OTA metadata has no '{key}' entry")
        url = str(entry.get("url") or "")
        if not url.startswith("https://"):
            # The OTA metadata is remote input: never let it point us at
            # file://, http:// or anything else the user did not consent to.
            raise LynxError(f"refusing a non-https image URL: {url!r}")
        destination = plan.target_dir / f"{key}.zip"
        if destination.exists():
            print(f"already present, skipped: {destination}")
            continue
        result = images.download(url, destination, consent=True)
        results.append(result)
        print(f"downloaded {result.path.name} ({result.size_bytes} bytes)")
        print(f"  sha256 {result.sha256} (verified: {result.verified})")
    if args.json:
        print(
            json.dumps(
                {"channel": channel, "files": [str(r.path) for r in results]},
                indent=2,
            )
        )
        return int(ExitCode.OK)
    if results:
        print()
        print(images.render_install_instructions(results[0]))
    return int(ExitCode.OK)


def _demo_profile_and_geometry() -> tuple[Profile, ViewportGeometry]:
    """A fixed, representative profile so numbers are comparable run to run."""
    from lynxdroid.display.geometry import FitMode, Rotation, Size
    from lynxdroid.profiles.model import Element, ElementType, Profile, Target

    elements = (
        Element(id="tap-fire", type=ElementType.TAP, label="Fire", x=0.86, y=0.82, keys=("space",)),
        Element(id="tap-jump", type=ElementType.TAP, label="Jump", x=0.92, y=0.66, keys=("j",)),
    )
    profile = Profile.new("Perf sample", Target(package="org.example.perf"), elements=elements)
    geometry = ViewportGeometry(
        buffer=Size(1920, 1080),
        surface=Size(1200, 675),
        fit=FitMode.CONTAIN,
        rotation=Rotation.NONE,
    )
    return profile, geometry


def cmd_perf(args: argparse.Namespace, paths: AppPaths, store: SettingsStore) -> int:
    """Numbers about our own work only; see ``lynxdroid.perf.SCOPE_NOTES``."""
    from lynxdroid import perf

    profile, geometry = _demo_profile_and_geometry()
    result = perf.measure(profile=profile, geometry=geometry, iterations=max(1, args.iterations))
    if args.json:
        print(json.dumps(result.as_dict(), indent=2, ensure_ascii=False))
        return int(ExitCode.OK)
    perf.render(result)
    return int(ExitCode.OK)


def cmd_gui(args: argparse.Namespace, paths: AppPaths, store: SettingsStore) -> int:
    if args.offscreen:
        import os

        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    try:
        from lynxdroid.gui.app import run_gui

        return run_gui(paths=paths, backend_kind=args.backend)
    except ImportError as exc:
        # Both "PySide6 is not installed" and "the wheel is there but its Qt
        # libraries are not loadable" land here. It is a missing dependency
        # (exit 3), not a generic failure, and must never be a traceback.
        raise DependencyMissing(
            f"the graphical shell is not usable: {exc}",
            hint=(
                "install PySide6 *and* Qt's runtime libraries "
                "(libGL, libEGL, libxkbcommon), or use the CLI commands instead"
            ),
        ) from exc


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------


def _qt_status() -> tuple[bool, str]:
    """Load-test Qt through the GUI package, which owns the real probe."""
    from lynxdroid.gui.app import qt_probe

    return qt_probe()


def _qt_available() -> bool:
    return _qt_status()[0]


def _level(name: str) -> int:
    import logging

    return getattr(logging, name.upper(), logging.INFO)


def _require_binary(names: Sequence[str]) -> str:
    for name in names:
        found = proc.which(name)
        if found:
            return found
    raise UsageError(f"none of {', '.join(names)} is installed")


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
