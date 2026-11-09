#!/usr/bin/env bash
# LynxDroid — reproducible install on CachyOS/Arch.
#
# Creates a local virtual environment and installs the standalone launcher.
# No sudo, package manager, Android runtime, or system configuration changes.
# User APKs are never installed or deleted by this script.
#
# Exit codes follow docs/CLI.md: 0 ok, 1 failed, 2 usage, 3 missing dependency.

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly ROOT="$(dirname "$SCRIPT_DIR")"
readonly VENV="$ROOT/.venv"

die() {
    printf 'install: %s\n' "$1" >&2
    exit "${2:-1}"
}

note() { printf '  %s\n' "$1"; }
step() { printf '\n== %s\n' "$1"; }

# --------------------------------------------------------------------- checks
step "checking the host before touching anything"

command -v python3 >/dev/null 2>&1 || die "python3 is not on PATH (missing dependency)" 3

python_version=$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')
python_major=$(python3 -c 'import sys; print(sys.version_info[0])')
python_minor=$(python3 -c 'import sys; print(sys.version_info[1])')
if [ "$python_major" -lt 3 ] || { [ "$python_major" -eq 3 ] && [ "$python_minor" -lt 10 ]; }; then
    die "Python $python_version found; 3.10 or newer is required" 3
fi
note "python $python_version"

# -------------------------------------------------------------------- install
step "creating the virtual environment"
if [ -x "$VENV/bin/python" ]; then
    note "reusing $VENV"
else
    python3 -m venv "$VENV"
    note "created $VENV"
fi

step "installing LynxDroid (editable, with the GUI extra)"
# shellcheck disable=SC1091
. "$VENV/bin/activate"
python -m pip --quiet --disable-pip-version-check install --upgrade pip
python -m pip --quiet --disable-pip-version-check install -e '.[gui,dev]'
note "installed $(python -c 'import lynxdroid.version as v; print(v.__version__)')"

# --------------------------------------------------------------------- verify
step "verifying the install the way CI does"

python -m lynxdroid version --json >/dev/null
note "the CLI starts and reports its version"

if ! python -m pytest -q >/dev/null 2>&1; then
    printf '\ninstall: the test suite failed. Run it yourself to see why:\n' >&2
    printf '  . %s && pytest -x -q\n' "$VENV/bin/activate" >&2
    exit 1
fi
note "the test suite passes"

step "done"
cat <<SUMMARY
Activate the environment with:

    . $VENV/bin/activate

Then:

    lynxdroid gui           # standalone launcher (needs a real display)

The Android runtime is not implemented yet. Adding an APK only saves a file
reference; it does not install or run the app.
SUMMARY
