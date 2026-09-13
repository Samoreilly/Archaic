#!/usr/bin/env bash
# test_per_root_unwatch.sh - Contract tests for per-root policy + live unwatch (#6).
#
# Verifies on a two-root fixture tree:
#   1. Per-root depth: shallow root omits deep paths from the index.
#   2. Per-root ignore_dirs: scoped to that root only.
#   3. watch=0: root is indexed but gets no inotify watches.
#   4. Live unwatch: drops scan list + watcher + index without restart,
#      and a later reindex does not re-add the root.
#
# NOTE: `complete` has a filesystem fallback, so index-membership is
# asserted via `suggest` (index-only) and health bucket counts.
#
# Hermetic: unique temp dir, own XDG_CONFIG_HOME/XDG_CACHE_HOME/
# ARCHAIC_CONFIG, own socket. Never touches the production daemon.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build/archaic"
CLI="$ROOT/build/archaic-cli"

PASS=0
FAIL=0
ok()  { PASS=$((PASS + 1)); echo "  PASS  $1"; }
bad() { FAIL=$((FAIL + 1)); echo "  FAIL  $1${2:+ ($2)}"; }

T="$(mktemp -d /tmp/archaic-roots-test.XXXXXX)"
R="$T/roots"
CACHE="$T/cache"
DSOCK="$T/daemon.sock"
export HOME="$T/fakehome"
export XDG_CONFIG_HOME="$T/config"
export XDG_CACHE_HOME="$CACHE"
export ARCHAIC_CONFIG="$T/config.toml"
mkdir -p "$R/A/x/y" "$R/A/keep" "$R/A/skipme" \
         "$R/B/deep1/deep2" "$R/B/skipme" "$CACHE" "$XDG_CONFIG_HOME/archaic" \
         "$T/fakehome"
touch "$R/A/x/y/deep.txt" "$R/A/keep/f.txt" "$R/A/skipme/s.txt" \
      "$R/B/deep1/deep2/f.txt" "$R/B/skipme/keep.txt"

cli() { timeout 5 "$CLI" --sock "$DSOCK" "$@" 2>/dev/null; }

cleanup() {
    if [ -f "$DSOCK.pid" ]; then
        DPID="$(cat "$DSOCK.pid" 2>/dev/null)"
        if [ -n "$DPID" ] && kill -0 "$DPID" 2>/dev/null; then
            kill "$DPID" 2>/dev/null || true
            for _ in $(seq 1 25); do kill -0 "$DPID" 2>/dev/null || break; sleep 0.2; done
            kill -9 "$DPID" 2>/dev/null || true
        fi
    fi
    sleep 0.3
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

for b in "$BIN" "$CLI"; do
    if [ ! -x "$b" ]; then echo "missing binary: $b (run cmake --build build first)"; exit 1; fi
done

# Roots file with per-root policy annotations.
cat > "$XDG_CONFIG_HOME/archaic/roots" <<EOF
$R/A depth=1 ignore_dirs=skipme
$R/B watch=0
EOF
printf '[daemon]\nscan_threads = 1\nmax_depth = 10\nrescan_interval_seconds = 3600\n' \
    > "$ARCHAIC_CONFIG"

setsid "$BIN" --daemon "$DSOCK" >"$T/daemon.log" 2>&1 < /dev/null &
for _ in $(seq 1 40); do
    if [ -S "$DSOCK" ] && cli ping >/dev/null 2>&1; then break; fi
    sleep 0.2
done
if ! cli ping >/dev/null 2>&1; then bad "daemon starts"; exit 1; fi
# Wait for the initial scan to finish.
for _ in $(seq 1 100); do
    if cli doctor 2>/dev/null | grep -q "scanning:   no"; then break; fi
    sleep 0.2
done
ok "daemon starts and scans"

# Guard against vacuous passes: both fixture roots must be active.
if cli roots 2>/dev/null | grep -qF "$R/A" && cli roots 2>/dev/null | grep -qF "$R/B"; then
    ok "both fixture roots active"
else
    bad "both fixture roots active" "got: $(cli roots 2>/dev/null)"
    exit 1
fi

# inotify watch count for the daemon pid.
watch_count() {
    local pid="$1" total=0 fd
    for fd in /proc/"$pid"/fdinfo/*; do
        [ -f "$fd" ] || continue
        total=$((total + $(grep -c "^inotify wd:" "$fd" 2>/dev/null || true)))
    done
    echo "$total"
}

DPID="$(cat "$DSOCK.pid" 2>/dev/null)"
# The watcher follows the disk tree (scan depth/ignores don't prune watches):
# A, A/x, A/x/y, A/keep, A/skipme = 5. B has watch=0, so must contribute 0.
if [ "$(watch_count "$DPID")" = "5" ]; then
    ok "watch=0 root gets no inotify watches (5, A tree only)"
else
    bad "watch=0 root gets no inotify watches (5, A tree only)" "got $(watch_count "$DPID")"
fi

# ── 1. Per-root depth (index-only asserts via suggest) ──────────────────────
if [ "$(cli suggest "$R/A/x/" /tmp)" = "$R/A/x/y" ]; then
    ok "depth=1 indexes two levels (A/x/y found)"
else
    bad "depth=1 indexes two levels (A/x/y found)" "got '$(cli suggest "$R/A/x/" /tmp)'"
fi
if cli suggest "$R/A/x/y/" /tmp >/dev/null 2>&1; then
    bad "depth=1 omits third level (A/x/y/deep.txt not indexed)" \
        "got '$(cli suggest "$R/A/x/y/" /tmp)'"
else
    ok "depth=1 omits third level (A/x/y/deep.txt not indexed)"
fi
if [ "$(cli suggest "$R/B/deep1/deep2/" /tmp)" = "$R/B/deep1/deep2/f.txt" ]; then
    ok "default depth indexes deep paths under B"
else
    bad "default depth indexes deep paths under B" "got '$(cli suggest "$R/B/deep1/deep2/" /tmp)'"
fi

# ── 2. Per-root ignore_dirs, scoped to its root ─────────────────────────────
if cli suggest "$R/A/skipme/" /tmp >/dev/null 2>&1; then
    bad "ignore_dirs hides A/skipme" "got '$(cli suggest "$R/A/skipme/" /tmp)'"
else
    ok "ignore_dirs hides A/skipme"
fi
if [ "$(cli suggest "$R/B/skipme/" /tmp)" = "$R/B/skipme/keep.txt" ]; then
    ok "ignore_dirs does not leak into B"
else
    bad "ignore_dirs does not leak into B" "got '$(cli suggest "$R/B/skipme/" /tmp)'"
fi

# ── 3. Live unwatch ──────────────────────────────────────────────────────────
BEFORE="$(cli doctor 2>/dev/null | grep -E "indexed:" | head -1)"
if cli unwatch "$R/A" >/dev/null 2>&1; then
    ok "unwatch command succeeds"
else
    bad "unwatch command succeeds"
fi
if cli roots 2>/dev/null | grep -qF "$R/A"; then
    bad "unwatch removes root from roots file"
else
    ok "unwatch removes root from roots file"
fi
sleep 1
AFTER="$(cli doctor 2>/dev/null | grep -E "indexed:" | head -1)"
if [ "$BEFORE" != "$AFTER" ]; then
    ok "unwatch purges index buckets ($BEFORE -> $AFTER)"
else
    bad "unwatch purges index buckets" "unchanged: $BEFORE"
fi
if [ "$(watch_count "$DPID")" = "0" ]; then
    ok "unwatch drops the live watcher (0 watches)"
else
    bad "unwatch drops the live watcher (0 watches)" "got $(watch_count "$DPID")"
fi
# A later reindex must not re-add the unwatched root.
cli reindex >/dev/null 2>&1
for _ in $(seq 1 100); do
    if cli doctor 2>/dev/null | grep -q "scanning:   no"; then break; fi
    sleep 0.2
done
if cli suggest "$R/A/x/" /tmp >/dev/null 2>&1; then
    bad "reindex does not resurrect unwatched root" "got '$(cli suggest "$R/A/x/" /tmp)'"
else
    ok "reindex does not resurrect unwatched root"
fi
if [ "$(cli suggest "$R/B/deep1/deep2/" /tmp)" = "$R/B/deep1/deep2/f.txt" ]; then
    ok " surviving root still indexed after reindex"
else
    bad " surviving root still indexed after reindex"
fi

echo ""
echo "per-root-unwatch: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
