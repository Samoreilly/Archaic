#!/usr/bin/env bash
# test_memory_budget.sh - Contract tests for memory budget (#5).
#
# Verifies:
#   1. doctor reports RSS vs the configured max_memory_mb.
#   2. Unit tests cover enforcement (see archaic-unit: memory_budget_*).
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

T="$(mktemp -d /tmp/archaic-mem-test.XXXXXX)"
FIX="$T/fixture"
CACHE="$T/cache"
DSOCK="$T/daemon.sock"
export XDG_CACHE_HOME="$CACHE"
export ARCHAIC_CONFIG="$T/config.toml"

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

mkdir -p "$FIX/sub" "$CACHE"
touch "$FIX/sub/f.txt"
cat > "$ARCHAIC_CONFIG" <<EOF
[daemon]
scan_threads = 1
max_depth = 8
rescan_interval_seconds = 3600

[storage]
max_memory_mb = 128
EOF

for b in "$BIN" "$CLI"; do
    if [ ! -x "$b" ]; then echo "missing binary: $b (run cmake --build build first)"; exit 1; fi
done

setsid "$BIN" --daemon "$FIX" "$DSOCK" >"$T/daemon.log" 2>&1 < /dev/null &
for _ in $(seq 1 40); do
    if [ -S "$DSOCK" ] && cli ping >/dev/null 2>&1; then break; fi
    sleep 0.2
done
if ! cli ping >/dev/null 2>&1; then bad "daemon starts"; exit 1; fi
sleep 2

DOC="$(cli doctor)"
if echo "$DOC" | grep -q "budget:.*128 MB"; then
    ok "doctor reports the configured 128 MB budget"
else
    bad "doctor reports the configured 128 MB budget" "got: $(echo "$DOC" | grep -i budget)"
fi
if echo "$DOC" | grep -qE "rss:[[:space:]]+[0-9]+ bytes"; then
    ok "doctor reports RSS"
else
    bad "doctor reports RSS"
fi
if echo "$DOC" | grep -qE "% used"; then
    ok "doctor reports budget usage percent"
else
    bad "doctor reports budget usage percent"
fi
# Daemon enforces the cap at runtime: it must stay alive and answering
# on a small fixture with a small budget.
if cli ping >/dev/null 2>&1 && cli complete "$FIX/" 5 /tmp | grep -qF "$FIX/sub"; then
    ok "daemon serves queries under the budget"
else
    bad "daemon serves queries under the budget"
fi

# ── Save containment (security) ────────────────────────────────────────────
if cli save /tmp/archaic-evil-save.bin >/dev/null 2>&1; then
    bad "save rejects paths outside the state dir"
else
    ok "save rejects paths outside the state dir"
fi
[ ! -e /tmp/archaic-evil-save.bin ] \
    && ok "rejected save wrote nothing" || bad "rejected save wrote nothing"
if cli save "$CACHE/archaic/backup.bin" >/dev/null 2>&1 \
    && [ -f "$CACHE/archaic/backup.bin" ]; then
    ok "save allows paths inside the state dir"
else
    bad "save allows paths inside the state dir"
fi

# ── doctor --fix respawns a dead daemon without system() ───────────────────
OLDPID="$(cat "$DSOCK.pid" 2>/dev/null)"
kill "$OLDPID" 2>/dev/null || true
for _ in $(seq 1 40); do
    { [ -z "$OLDPID" ] || ! kill -0 "$OLDPID" 2>/dev/null; } && break
    sleep 0.2
done
if [ -n "$OLDPID" ] && kill -0 "$OLDPID" 2>/dev/null; then
    kill -9 "$OLDPID" 2>/dev/null || true
    for _ in $(seq 1 25); do kill -0 "$OLDPID" 2>/dev/null || break; sleep 0.2; done
fi
rm -f "$DSOCK" "$DSOCK.pid"
# Point the user bus at nowhere so systemctl fails and doctor must use
# its fork/exec spawn path (never touches the real user bus).
if XDG_RUNTIME_DIR="$T/run" cli doctor --fix 2>&1 | grep -q "started/restarted daemon"; then
    ok "doctor --fix respawns dead daemon"
else
    bad "doctor --fix respawns dead daemon"
fi
if cli ping >/dev/null 2>&1; then
    ok "respawned daemon answers"
else
    bad "respawned daemon answers"
fi

echo ""
echo "memory-budget: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
