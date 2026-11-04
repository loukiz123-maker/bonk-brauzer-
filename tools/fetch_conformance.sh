#!/usr/bin/env bash
# Downloads the conformance corpora into tests/conformance/corpus/.
#
#   ./tools/fetch_conformance.sh [DEST]
#
# The corpora are deliberately NOT vendored in this repository: they are large,
# they have their own licences (html5lib-tests: MIT, test262: BSD-style) and they
# move. `bonk_conformance` simply skips a suite when its directory is missing, so
# the unit-test suite and `make verify` stay offline and reproducible.
#
# Sources:
#   html5lib-tests tokenizer/*.test     https://github.com/html5lib/html5lib-tests
#   html5lib-tests tree-construction/   same repository; the tree-construction
#                                       files moved to web-platform-tests in 2019,
#                                       so we pin the last commit that has them
#   test262 test/                       https://github.com/tc39/test262
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-$ROOT/tests/conformance/corpus}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Pinned so that a report is reproducible: the tokenizer suite from master,
# tree-construction from the commit before it moved to WPT.
HTML5LIB_TOKENIZER="refs/heads/master"
HTML5LIB_TREE_COMMIT="9329e64694e7835d0dcff9811e22856ef6ad16f9"
TEST262="refs/heads/main"

mkdir -p "$DEST"

echo "==> html5lib-tests (tokenizer)"
curl -fsSL "https://codeload.github.com/html5lib/html5lib-tests/tar.gz/$HTML5LIB_TOKENIZER" -o "$TMP/tokenizer.tar.gz"
mkdir -p "$TMP/tokenizer"
tar xzf "$TMP/tokenizer.tar.gz" -C "$TMP/tokenizer" --strip-components=1
rm -rf "$DEST/tokenizer"
cp -r "$TMP/tokenizer/tokenizer" "$DEST/tokenizer"

echo "==> html5lib-tests (tree construction, $HTML5LIB_TREE_COMMIT)"
curl -fsSL "https://codeload.github.com/html5lib/html5lib-tests/tar.gz/$HTML5LIB_TREE_COMMIT" -o "$TMP/tree.tar.gz"
mkdir -p "$TMP/tree"
tar xzf "$TMP/tree.tar.gz" -C "$TMP/tree" --strip-components=1
rm -rf "$DEST/tree"
cp -r "$TMP/tree/tree-construction" "$DEST/tree"

echo "==> test262"
curl -fsSL "https://codeload.github.com/tc39/test262/tar.gz/$TEST262" -o "$TMP/test262.tar.gz"
mkdir -p "$TMP/test262"
tar xzf "$TMP/test262.tar.gz" -C "$TMP/test262" --strip-components=1
rm -rf "$DEST/test262"
mkdir -p "$DEST/test262"
cp -r "$TMP/test262/test" "$DEST/test262/test"

echo "==> reftests (in-repo, nothing to download)"
rm -rf "$DEST/reftests"
cp -r "$ROOT/tests/conformance/reftests" "$DEST/reftests"

echo
echo "corpora in $DEST:"
du -sh "$DEST"/* 2>/dev/null || true
echo
echo "next: make conformance   # writes docs/conformance.md"
