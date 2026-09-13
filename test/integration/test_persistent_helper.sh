#!/usr/bin/env bash
# test_persistent_helper.sh - Contract tests for persistent helper (#2).
#
# Verifies: one helper process keeps the daemon socket warm (--serve),
# shells can query it with a built-in timeout (--ask), output matches
# one-shot helper / archaic-cli, fallback works when serve is down, and
# serve survives a daemon restart without being restarted itself.
#
# Hermetic: unique temp dir, own XDG_CACHE_HOME + ARCHAIC_CONFIG, own
# sockets. Never touches the production daemon.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build/archaic"
CLI="$ROOT/build/archaic-cli"
HELPER="$ROOT/build/archaic-helper"

PASS=0
FAIL=0

# All CLI calls go through `timeout`: a wedged daemon must fail the test,
# never hang it.
cli() { timeout 5 "$CLI" --sock "$DSOCK" "$@" 2>/dev/null; }

ok()   { PASS=$((PASS + 1)); echo "  PASS  $1"; }
bad()  { FAIL=$((FAIL + 1)); echo "  FAIL  $1${2:+ ($2)}"; }

T="$(mktemp -d /tmp/archaic-persist-test.XXXXXX)"
FIX="$T/fixture"
CACHE="$T/cache"
DSOCK="$T/daemon.sock"
HSOCK="$T/helper.sock"
export XDG_CACHE_HOME="$CACHE"
export ARCHAIC_CONFIG="$T/config.toml"

cleanup() {
    if [ -f "$DSOCK.pid" ]; then
        DPID="$(cat "$DSOCK.pid" 2>/dev/null)"
        if [ -n "$DPID" ] && kill -0 "$DPID" 2>/dev/null; then
            kill "$DPID" 2>/dev/null || true
            for _ in $(seq 1 25); do kill -0 "$DPID" 2>/dev/null || break; sleep 0.2; done
            kill -9 "$DPID" 2>/dev/null || true
        fi
    fi
    if [ -f "$HSOCK.pid" ]; then
        kill "$(cat "$HSOCK.pid" 2>/dev/null)" 2>/dev/null || true
    fi
    sleep 0.3
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

# ── Fixture + binaries ───────────────────────────────────────────────────────
mkdir -p "$FIX/alpha" "$FIX/beta" "$CACHE"
touch "$FIX/alpha/a1.txt" "$FIX/alpha/a2.txt" "$FIX/beta/b1.txt"
printf '[daemon]\nscan_threads = 1\nmax_depth = 8\nrescan_interval_seconds = 3600\n' > "$ARCHAIC_CONFIG"

for b in "$BIN" "$CLI" "$HELPER"; do
    if [ ! -x "$b" ]; then echo "missing binary: $b (run cmake --build build first)"; exit 1; fi
done

# ── Daemon ───────────────────────────────────────────────────────────────────
setsid "$BIN" --daemon "$FIX" "$DSOCK" >"$T/daemon.log" 2>&1 < /dev/null &
for _ in $(seq 1 40); do
    if [ -S "$DSOCK" ] && cli ping >/dev/null 2>&1; then break; fi
    sleep 0.2
done
if cli ping >/dev/null 2>&1; then ok "daemon starts on fixture tree"; else bad "daemon starts on fixture tree"; exit 1; fi
sleep 2  # let the initial scan index the fixture

# ── Serve ────────────────────────────────────────────────────────────────────
setsid "$HELPER" "$DSOCK" --serve "$HSOCK" >"$T/serve.log" 2>&1 < /dev/null &
for _ in $(seq 1 20); do
    [ -S "$HSOCK" ] && break
    sleep 0.2
done
if [ -S "$HSOCK" ]; then ok "serve creates listen socket"; else bad "serve creates listen socket"; exit 1; fi
if [ -f "$HSOCK.pid" ] && kill -0 "$(cat "$HSOCK.pid")" 2>/dev/null; then
    ok "serve writes live pid file"
else
    bad "serve writes live pid file"
fi

TABQ="$(printf 'complete\t0\t20\t/tmp\t%s/al' "$FIX")"
SPACEQ="complete $FIX/al 20 /tmp"

# ── --ask matches one-shot + CLI ─────────────────────────────────────────────
ASK_TAB="$(printf '%s\n' "$TABQ" | "$HELPER" --ask "$HSOCK" 2>/dev/null)"
ONE_TAB="$(printf '%s\n' "$TABQ" | timeout 10 "$HELPER" "$DSOCK" 2>/dev/null)"
if [ -n "$ASK_TAB" ] && [ "$ASK_TAB" = "$ONE_TAB" ]; then
    ok "ask(tab query) == one-shot helper"
else
    bad "ask(tab query) == one-shot helper" "ask='$ASK_TAB' one='$ONE_TAB'"
fi

ASK_SPACE="$(printf '%s\n' "$SPACEQ" | "$HELPER" --ask "$HSOCK" 2>/dev/null)"
CLI_OUT="$(cli complete "$FIX/al" 20)"
if [ -n "$ASK_SPACE" ] && [ "$ASK_SPACE" = "$CLI_OUT" ]; then
    ok "ask(space query) == archaic-cli"
else
    bad "ask(space query) == archaic-cli" "ask='$ASK_SPACE' cli='$CLI_OUT'"
fi

if printf 'ping\n' | "$HELPER" --ask "$HSOCK" 2>/dev/null | grep -qE '^[0-9]+$'; then
    ok "ask ping returns uptime"
else
    bad "ask ping returns uptime"
fi

if echo "$ASK_TAB" | grep -qF "$FIX/alpha"; then
    ok "ask returns fixture paths"
else
    bad "ask returns fixture paths" "got='$ASK_TAB'"
fi

# ── Fallback when serve is down ──────────────────────────────────────────────
kill "$(cat "$HSOCK.pid")" 2>/dev/null
sleep 1
if [ ! -S "$HSOCK" ]; then
    ok "serve removes socket on SIGTERM"
else
    bad "serve removes socket on SIGTERM"
fi
if printf '%s\n' "$TABQ" | "$HELPER" --ask "$HSOCK" >/dev/null 2>&1; then
    bad "ask fails without serve"
else
    ok "ask fails without serve"
fi
FALLBACK="$(printf '%s\n' "$TABQ" | timeout 10 "$HELPER" "$DSOCK" 2>/dev/null)"
if [ -n "$FALLBACK" ] && [ "$FALLBACK" = "$ONE_TAB" ]; then
    ok "one-shot fallback works while serve is down"
else
    bad "one-shot fallback works while serve is down" "got='$FALLBACK'"
fi

# ── Stale socket recovery (SIGKILL leaves socket+pid behind) ────────────────
setsid "$HELPER" "$DSOCK" --serve "$HSOCK" >"$T/serve.log" 2>&1 < /dev/null &
for _ in $(seq 1 20); do [ -S "$HSOCK" ] && break; sleep 0.2; done
SERVEPID="$(cat "$HSOCK.pid" 2>/dev/null)"
kill -9 "$SERVEPID" 2>/dev/null
sleep 0.5
if [ -S "$HSOCK" ] && ! kill -0 "$SERVEPID" 2>/dev/null; then
    ok "SIGKILL leaves stale socket (test precondition)"
else
    bad "SIGKILL leaves stale socket (test precondition)"
fi
# Shell ensure_serve logic: pid dead -> rm socket+pid, re-serve.
if ! kill -0 "$SERVEPID" 2>/dev/null; then rm -f "$HSOCK" "$HSOCK.pid"; fi
setsid "$HELPER" "$DSOCK" --serve "$HSOCK" >"$T/serve.log" 2>&1 < /dev/null &
for _ in $(seq 1 20); do [ -S "$HSOCK" ] && break; sleep 0.2; done
RECOVERED="$(printf '%s\n' "$TABQ" | "$HELPER" --ask "$HSOCK" 2>/dev/null)"
if [ -n "$RECOVERED" ] && [ "$RECOVERED" = "$ONE_TAB" ]; then
    ok "serve recovers after stale socket cleanup"
else
    bad "serve recovers after stale socket cleanup" "got='$RECOVERED'"
fi

# ── Daemon restart without restarting serve ──────────────────────────────────
# NOTE: SIGTERM is the graceful stop path (IPC `shutdown` only stops the
# listener; the process exits on SIGTERM, same as systemd sends). SIGTERM
# during an active scan can wedge the daemon (shutdown-path deadlock), so
# escalate to SIGKILL like run.sh stop does.
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
setsid env XDG_CACHE_HOME="$CACHE" ARCHAIC_CONFIG="$ARCHAIC_CONFIG" \
    "$BIN" --daemon "$FIX" "$DSOCK" >"$T/daemon.log" 2>&1 < /dev/null &
for _ in $(seq 1 40); do
    if [ -S "$DSOCK" ] && cli ping >/dev/null 2>&1; then break; fi
    sleep 0.2
done
# Fresh daemon rescans in background: wait until the fixture is indexed.
for _ in $(seq 1 100); do
    cli complete "$FIX/al" 20 | grep -qF "$FIX/alpha" && break
    sleep 0.2
done
AFTER="$(printf '%s\n' "$TABQ" | "$HELPER" --ask "$HSOCK" 2>/dev/null)"
if [ -n "$AFTER" ] && echo "$AFTER" | grep -qF "$FIX/alpha"; then
    ok "serve reconnects after daemon restart"
else
    bad "serve reconnects after daemon restart" "got='$AFTER'"
fi

# ── Shell plugins use persistent path, no per-Tab one-shot gate ─────────────
if grep -q -- "--serve" "$ROOT/fish/archaic.fish" \
    && grep -q -- "--ask" "$ROOT/fish/archaic.fish" \
    && grep -q -- "--serve" "$ROOT/bash/archaic.bash" \
    && grep -q -- "--ask" "$ROOT/bash/archaic.bash" \
    && grep -q -- "--serve" "$ROOT/zsh/archaic.zsh" \
    && grep -q -- "--ask" "$ROOT/zsh/archaic.zsh"; then
    ok "fish/bash/zsh plugins use --serve/--ask"
else
    bad "fish/bash/zsh plugins use --serve/--ask"
fi
if grep -q "_archaic_helper_pid" "$ROOT/bash/archaic.bash" \
    || grep -q "_archaic_helper_pid" "$ROOT/zsh/archaic.zsh" \
    || grep -q "_archaic_ensure_helper" "$ROOT/bash/archaic.bash" \
    || grep -q "_archaic_ensure_helper" "$ROOT/zsh/archaic.zsh"; then
    bad "bash/zsh free of per-Tab one-shot helper gate"
else
    ok "bash/zsh free of per-Tab one-shot helper gate"
fi

echo ""
echo "persistent-helper: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
