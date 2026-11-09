#!/usr/bin/env bash
# Install (or remove) the menu entry and icon for the current user.
#
# Deliberately user-level: everything lands under $XDG_DATA_HOME, never in
# /usr/share, so no sudo is required and nothing outside the home directory is
# touched. That rule is the same one src/lynxdroid/core/paths.py states and
# tests/test_install_script.py checks for the other script.
#
#   ./scripts/install-desktop-entry.sh            # install
#   ./scripts/install-desktop-entry.sh --remove   # take it back out
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
APPS_DIR="$DATA_HOME/applications"
ICON_DIR="$DATA_HOME/icons/hicolor/scalable/apps"
ENTRY="$APPS_DIR/lynxdroid.desktop"
ICON="$ICON_DIR/lynxdroid.svg"

if [[ "${1:-}" == "--remove" ]]; then
    rm -f "$ENTRY" "$ICON"
    echo "removed $ENTRY"
    echo "removed $ICON"
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPS_DIR" 2>/dev/null || true
    exit 0
fi

# The entry runs `lynxdroid-gui`; installing a menu item that cannot start is
# worse than having none, so refuse when the command is not on PATH.
if ! command -v lynxdroid-gui >/dev/null 2>&1; then
    echo "lynxdroid-gui is not on PATH." >&2
    echo "Install the package first (scripts/install.sh) or activate its venv." >&2
    exit 1
fi

mkdir -p "$APPS_DIR" "$ICON_DIR"

# The Exec line must point at the interpreter that will find the package, not
# at whatever `lynxdroid-gui` happens to mean in this shell: a venv is often
# not active when the menu is clicked.
GUI_BIN="$(command -v lynxdroid-gui)"
python3 - "$GUI_BIN" "$REPO_ROOT/packaging/lynxdroid.desktop" "$ENTRY" <<'PYCODE'
import pathlib
import sys

binary, template, output = sys.argv[1:]
if any(c in binary for c in "\n\r"):
    raise SystemExit("Executable path contains a newline; cannot install desktop entry")
# Exec parsing follows desktop value unescaping. Escape for both layers and
# double percent signs so a literal path cannot introduce field codes.
quoted = ''.join('\\' + c if c in '\\"`$' else c for c in binary.replace('%', '%%'))
quoted = quoted.replace('\\', '\\\\')
text = pathlib.Path(template).read_text(encoding='utf-8')
lines = text.splitlines(keepends=True)
pathlib.Path(output).write_text(''.join('Exec="' + quoted + '"\n' if line.startswith('Exec=') else line for line in lines), encoding='utf-8')
PYCODE
install -m 0644 "$REPO_ROOT/packaging/lynxdroid.svg" "$ICON"

echo "installed $ENTRY"
echo "  Exec=$GUI_BIN"
echo "installed $ICON"
echo
echo "If the entry does not appear immediately, log out and back in, or run:"
echo "    update-desktop-database $APPS_DIR"

command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPS_DIR" 2>/dev/null || true
