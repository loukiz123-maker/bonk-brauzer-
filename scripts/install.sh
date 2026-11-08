#!/usr/bin/env bash
# LynxDroid — reproducible install on CachyOS/Arch.
#
# What this script does:
#   1. checks the things it cannot fix (Python version, Waydroid package)
#   2. creates a virtual environment next to the source tree
#   3. installs LynxDroid into it, with the GUI extra
#   4. runs the same checks CI runs, so a broken install is caught here
#
# What it deliberately does NOT do:
#   * no sudo, no pacman, no kernel changes. Installing Waydroid and choosing
#     a binder-capable kernel are decisions about your machine, and the
#     project's own rule is that such changes need your consent, not a script's
#   * nothing is written outside .venv/ and the XDG directories LynxDroid
#     already owns
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

if [ -r /etc/os-release ] && ! grep -qiE '^(ID|ID_LIKE)=.*(arch|cachyos)' /etc/os-release; then
    # Not fatal: everything below works on any Linux. The target distro is
    # only needed for Waydroid itself.
    note "warning: this is not CachyOS/Arch; the CLI and tests will work,"
    note "         but the Waydroid packages below are Arch-specific"
fi

if ! command -v waydroid >/dev/null 2>&1; then
    note "warning: waydroid is not installed"
    note "         install it yourself when you are ready: sudo pacman -S waydroid"
    note "         the CLI and the test suite do not need it"
fi

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

# doctor is read-only by construction and is expected to exit non-zero when the
# host cannot run Android yet: a desktop without Waydroid is not a broken
# install, so its exit code is reported, never enforced.
doctor_status=0
python -m lynxdroid doctor --quiet >/dev/null 2>&1 || doctor_status=$?
note "doctor exited $doctor_status (non-zero is normal before Waydroid is set up)"

step "done"
cat <<SUMMARY
Activate the environment with:

    . $VENV/bin/activate

Then:

    lynxdroid doctor        # what this machine actually supports, read-only
    lynxdroid gui           # the window (needs a real display)

Next steps that need your decision, not this script's:
  * install Waydroid and a binder-capable kernel (see README, "Install")
  * obtain a LineageOS-based image: lynxdroid image plan
SUMMARY
