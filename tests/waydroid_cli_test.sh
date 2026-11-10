#!/bin/sh
# Synthetic end-to-end coverage for the `bonk waydroid` executable command.
# This exercises launcher orchestration only; it is not a live Android test.
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: $0 /path/to/bonk" >&2
  exit 2
fi

bin=$1
case "$bin" in
  /*) ;;
  *) bin="$(pwd)/$bin" ;;
esac

tmp=$(mktemp -d "${TMPDIR:-/tmp}/bonk-waydroid-cli.XXXXXX")
launcher_pid=
cleanup() {
  if [ -n "$launcher_pid" ]; then
    kill -TERM "$launcher_pid" 2>/dev/null || true
    wait "$launcher_pid" 2>/dev/null || true
  fi
  rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

cat > "$tmp/waydroid" <<'FAKE'
#!/bin/sh
set -eu
state=${BONK_FAKE_WAYDROID_STATE:?}
log=${BONK_FAKE_WAYDROID_LOG:?}
printf '%s\n' "$*" >> "$log"
case "$*" in
  status)
    if [ -f "$state" ]; then current=$(cat "$state"); else current=STOPPED; fi
    printf 'Session:\t%s\nContainer:\t%s\n' "$current" "$current"
    ;;
  'session start')
    printf RUNNING > "$state"
    trap 'printf STOPPED > "$state"; exit 0' TERM INT
    while :; do sleep 0.05; done
    ;;
  show-full-ui|'app install '*|'app launch '*)
    [ "$(cat "$state")" = RUNNING ]
    ;;
  *) echo "unexpected args: $*" >&2; exit 2 ;;
esac
FAKE
chmod +x "$tmp/waydroid"
printf 'synthetic APK placeholder, never sent to Android\n' > "$tmp/game.apk"
export BONK_FAKE_WAYDROID_STATE="$tmp/state"
export BONK_FAKE_WAYDROID_LOG="$tmp/argv.log"

# Reject invalid package IDs and missing APK files before even querying runtime state.
: > "$BONK_FAKE_WAYDROID_LOG"
if "$bin" waydroid --waydroid "$tmp/waydroid" --launch-package invalid > "$tmp/invalid-package.log" 2>&1; then
  echo "expected invalid package ID to fail" >&2
  exit 1
fi
grep -q 'android.invalid_package_name' "$tmp/invalid-package.log"
[ ! -s "$BONK_FAKE_WAYDROID_LOG" ]
if "$bin" waydroid --waydroid "$tmp/waydroid" --confirm-start \
  --install-apk "$tmp/missing.apk" --confirm-install > "$tmp/missing-apk.log" 2>&1; then
  echo "expected missing APK to fail before starting Waydroid" >&2
  exit 1
fi
grep -q 'not an existing regular file' "$tmp/missing-apk.log"
[ ! -s "$BONK_FAKE_WAYDROID_LOG" ]

# A no-action invocation only reports status.
status_output=$("$bin" waydroid --waydroid "$tmp/waydroid")
printf '%s\n' "$status_output" | grep -q 'session=stopped'
if grep -q '^session start$' "$BONK_FAKE_WAYDROID_LOG"; then
  echo "status-only invocation unexpectedly started Waydroid" >&2
  exit 1
fi

# A stopped session cannot be acted on without start consent.
printf STOPPED > "$tmp/state"
: > "$BONK_FAKE_WAYDROID_LOG"
if "$bin" waydroid --waydroid "$tmp/waydroid" --show-ui > "$tmp/no-consent.log" 2>&1; then
  echo "expected --show-ui on a stopped session to fail without --confirm-start" >&2
  exit 1
fi
grep -q -- '--confirm-start' "$tmp/no-consent.log"
if grep -q '^session start$' "$BONK_FAKE_WAYDROID_LOG"; then
  echo "no-consent invocation unexpectedly started Waydroid" >&2
  exit 1
fi

# A running external session is used but never taken over or stopped.
printf RUNNING > "$tmp/state"
: > "$BONK_FAKE_WAYDROID_LOG"
"$bin" waydroid --waydroid "$tmp/waydroid" --show-ui > "$tmp/existing-session.log" 2>&1
if grep -q '^session start$' "$BONK_FAKE_WAYDROID_LOG"; then
  echo "launcher took ownership of an already-running Waydroid session" >&2
  exit 1
fi
grep -q '^show-full-ui$' "$BONK_FAKE_WAYDROID_LOG"

# Full CLI flow: explicit start, wait, UI, exact APK install and package launch,
# then clean up only the session process that this Bonk invocation owns.
printf STOPPED > "$tmp/state"
: > "$BONK_FAKE_WAYDROID_LOG"
"$bin" waydroid --waydroid "$tmp/waydroid" --confirm-start \
  --install-apk "$tmp/game.apk" --confirm-install --launch-package com.example.game \
  > "$tmp/owned-session.log" 2>&1 &
launcher_pid=$!

attempt=0
while [ "$attempt" -lt 200 ]; do
  if grep -q '^app launch com.example.game$' "$BONK_FAKE_WAYDROID_LOG" 2>/dev/null; then
    break
  fi
  if ! kill -0 "$launcher_pid" 2>/dev/null; then
    cat "$tmp/owned-session.log" >&2
    echo "Waydroid launcher exited before reaching the package launch" >&2
    exit 1
  fi
  attempt=$((attempt + 1))
  sleep 0.05
done
if ! grep -q '^app launch com.example.game$' "$BONK_FAKE_WAYDROID_LOG"; then
  cat "$tmp/owned-session.log" >&2
  echo "Waydroid launcher did not reach the package launch" >&2
  exit 1
fi
kill -TERM "$launcher_pid"
if ! wait "$launcher_pid"; then
  launcher_pid=
  cat "$tmp/owned-session.log" >&2
  echo "Bonk failed while cleaning up its owned Waydroid session" >&2
  exit 1
fi
launcher_pid=

grep -q '^session start$' "$BONK_FAKE_WAYDROID_LOG"
grep -q '^show-full-ui$' "$BONK_FAKE_WAYDROID_LOG"
grep -q "^app install $tmp/game.apk$" "$BONK_FAKE_WAYDROID_LOG"
grep -q '^app launch com.example.game$' "$BONK_FAKE_WAYDROID_LOG"
grep -q 'Bonk-managed Waydroid session stopped' "$tmp/owned-session.log"
[ "$(cat "$tmp/state")" = STOPPED ]

echo "Waydroid CLI synthetic end-to-end: PASS (not a live Waydroid test)"
