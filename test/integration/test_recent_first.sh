#!/usr/bin/env bash
# test_recent_first.sh - Contract tests for recent-first ranking (#3).
#
# Verifies on a fixture tree:
#   1. Baseline completion is alphabetical for equal siblings.
#   2. `select` (accept learning: Tab/ghost/cd) moves the accepted path
#      ahead of alphabetical siblings on the next query.
#   3. cwd-proximate paths beat alphabetical siblings.
#
# Hermetic: unique temp dir, own XDG_CACHE_HOME + ARCHAIC_CONFIG, own
# socket. Never touches the production daemon.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build/archaic"
CLI="$ROOT/build/archaic-cli"

PASS=0
FAIL=0
ok()  { PASS=$((PASS + 1)); echo "  PASS  $1"; }
bad() { FAIL=$((FAIL + 1)); echo "  FAIL  $1${2:+ ($2)}"; }

T="$(mktemp -d /tmp/archaic-recent-test.XXXXXX)"
FIX="$T/fixture"
CACHE="$T/cache"
DSOCK="$T/daemon.sock"
export XDG_CACHE_HOME="$CACHE"
export ARCHAIC_CONFIG="$T/config.toml"

# All CLI calls go through `timeout`: a wedged daemon must fail, never hang.
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

mkdir -p "$FIX/aaa" "$FIX/zzz" "$CACHE"
touch "$FIX/aaa/file.txt" "$FIX/zzz/file.txt"
printf '[daemon]\nscan_threads = 1\nmax_depth = 8\nrescan_interval_seconds = 3600\n' > "$ARCHAIC_CONFIG"

for b in "$BIN" "$CLI"; do
    if [ ! -x "$b" ]; then echo "missing binary: $b (run cmake --build build first)"; exit 1; fi
done

setsid "$BIN" --daemon "$FIX" "$DSOCK" >"$T/daemon.log" 2>&1 < /dev/null &
for _ in $(seq 1 40); do
    if [ -S "$DSOCK" ] && cli ping >/dev/null 2>&1; then break; fi
    sleep 0.2
done
if ! cli ping >/dev/null 2>&1; then bad "daemon starts"; exit 1; fi
# Wait until both siblings are indexed.
for _ in $(seq 1 100); do
    OUT="$(cli complete "$FIX/" 20 /tmp)"
    echo "$OUT" | grep -qF "$FIX/aaa" && echo "$OUT" | grep -qF "$FIX/zzz" && break
    sleep 0.2
done

first_of() { echo "$1" | grep -E '^[DF] ' | head -1 | awk '{print $2}'; }

# ── 0. cwd proximity beats alphabetical (no selects yet) ────────────────────
NEAR="$(cli complete "$FIX/" 20 "$FIX/zzz")"
if [ "$(first_of "$NEAR")" = "$FIX/zzz" ]; then
    ok "cwd-proximate path beats alphabetical siblings"
else
    bad "cwd-proximate path beats alphabetical siblings" "got='$(first_of "$NEAR")'"
fi

# ── 1. Alphabetical baseline (neutral cwd, no selects) ──────────────────────
BASE="$(cli complete "$FIX/" 20 /tmp)"
if [ "$(first_of "$BASE")" = "$FIX/aaa" ]; then
    ok "baseline ranks alphabetical siblings (aaa first)"
else
    bad "baseline ranks alphabetical siblings (aaa first)" "got='$(first_of "$BASE")'"
fi

# ── 2. Accept learning: select zzz, it leads next Tab ───────────────────────
if cli select "$FIX/zzz" >/dev/null 2>&1; then
    ok "select accepts a path"
else
    bad "select accepts a path"
fi
AFTER="$(cli complete "$FIX/" 20 /tmp)"
if [ "$(first_of "$AFTER")" = "$FIX/zzz" ]; then
    ok "last-selected path beats alphabetical siblings"
else
    bad "last-selected path beats alphabetical siblings" "got='$(first_of "$AFTER")'"
fi
# A different selection wins over the older one (recency, not just frequency).
cli select "$FIX/aaa" >/dev/null 2>&1
AGAIN="$(cli complete "$FIX/" 20 /tmp)"
if [ "$(first_of "$AGAIN")" = "$FIX/aaa" ]; then
    ok "most-recent selection wins"
else
    bad "most-recent selection wins" "got='$(first_of "$AGAIN")'"
fi

echo ""
echo "recent-first: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
