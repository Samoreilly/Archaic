#!/usr/bin/env bash
# test_copy_install.sh - Contract tests for copy-install + config + README (#4).
#
# Verifies:
#   1. install.sh copies (never symlinks) binaries + plugins.
#   2. install.sh writes ~/.config/archaic/config.toml if missing.
#   3. `archaic-cli doctor --fix` writes config + installs all 3 plugins.
#   4. README matches reality (roots list, commands, socket paths).
#
# Hermetic: install.sh runs with a fake HOME + XDG_RUNTIME_DIR. The daemon
# it starts is killed during cleanup. Never touches the real HOME.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

PASS=0
FAIL=0
ok()  { PASS=$((PASS + 1)); echo "  PASS  $1"; }
bad() { FAIL=$((FAIL + 1)); echo "  FAIL  $1${2:+ ($2)}"; }

T="$(mktemp -d /tmp/archaic-install-test.XXXXXX)"
FAKEHOME="$T/home"
FAKERUN="$T/run"
mkdir -p "$FAKEHOME" "$FAKERUN"

cleanup() {
    if [ -f "$FAKERUN/archaic.sock.pid" ]; then
        DPID="$(cat "$FAKERUN/archaic.sock.pid" 2>/dev/null)"
        if [ -n "$DPID" ] && kill -0 "$DPID" 2>/dev/null; then
            kill "$DPID" 2>/dev/null || true
            for _ in $(seq 1 25); do kill -0 "$DPID" 2>/dev/null || break; sleep 0.2; done
            kill -9 "$DPID" 2>/dev/null || true
        fi
    fi
    # Backstop: the install-spawned daemon must never outlive the test.
    pkill -9 -f "archaic --daemon $FAKERUN/archaic.sock" 2>/dev/null || true
    sleep 0.3
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

# ── 1. No symlinks in install.sh ─────────────────────────────────────────────
if grep -qE '^[[:space:]]*ln\s+(-s|-[a-z]*s)' "$ROOT/install.sh"; then
    bad "install.sh never symlinks"
else
    ok "install.sh never symlinks"
fi
for f in "fish/archaic.fish" "bash/archaic.bash" "zsh/archaic.zsh"; do
    if grep -q "cp -f \"\$SCRIPT_DIR/$f\"" "$ROOT/install.sh"; then
        ok "install.sh copies $f"
    else
        bad "install.sh copies $f"
    fi
done

# ── 2. Full install into fake HOME (bash branch) ─────────────────────────────
if env -i HOME="$FAKEHOME" SHELL=/bin/bash XDG_RUNTIME_DIR="$FAKERUN" \
        PATH="/usr/local/bin:/usr/bin:/bin" \
        bash "$ROOT/install.sh" >"$T/install.log" 2>&1; then
    ok "install.sh exits 0 with fake HOME"
else
    bad "install.sh exits 0 with fake HOME" "see $T/install.log"
fi
for b in archaic archaic-cli archaic-helper; do
    if [ -f "$FAKEHOME/.local/bin/$b" ] && [ ! -L "$FAKEHOME/.local/bin/$b" ] \
        && [ -x "$FAKEHOME/.local/bin/$b" ]; then
        ok "installed binary is a real file: $b"
    else
        bad "installed binary is a real file: $b"
    fi
done
if [ -f "$FAKEHOME/.local/share/bash-completion/completions/archaic.bash" ] \
    && [ ! -L "$FAKEHOME/.local/share/bash-completion/completions/archaic.bash" ]; then
    ok "installed bash plugin is a real file"
else
    bad "installed bash plugin is a real file"
fi
if [ -f "$FAKEHOME/.config/archaic/config.toml" ]; then
    ok "install.sh writes config.toml"
else
    bad "install.sh writes config.toml"
fi
if "$ROOT/build/archaic-cli" --sock "$FAKERUN/archaic.sock" ping >/dev/null 2>&1; then
    ok "install.sh leaves a responding daemon"
else
    bad "install.sh leaves a responding daemon"
fi

# ── 3. doctor --fix from scratch ─────────────────────────────────────────────
rm -rf "$FAKEHOME/.config" "$FAKEHOME/.local"
mkdir -p "$FAKEHOME"
if env -i HOME="$FAKEHOME" XDG_RUNTIME_DIR="$FAKERUN" PATH="/usr/local/bin:/usr/bin:/bin" \
        "$ROOT/build/archaic-cli" doctor --fix >"$T/doctor.log" 2>&1; then
    ok "doctor --fix exits 0"
else
    bad "doctor --fix exits 0" "see $T/doctor.log"
fi
[ -f "$FAKEHOME/.config/archaic/config.toml" ] \
    && ok "doctor --fix writes config.toml" || bad "doctor --fix writes config.toml"
for p in ".config/fish/conf.d/archaic.fish" \
         ".local/share/bash-completion/completions/archaic.bash" \
         ".config/zsh/archaic.zsh"; do
    if [ -f "$FAKEHOME/$p" ] && [ ! -L "$FAKEHOME/$p" ]; then
        ok "doctor --fix installs $p"
    else
        bad "doctor --fix installs $p"
    fi
done

# ── 4. README matches reality ────────────────────────────────────────────────
for d in src projects project dev code git repos samdev work; do
    if grep -q "~/$d" "$ROOT/README.md"; then
        ok "README lists ~/$d"
    else
        bad "README lists ~/$d"
    fi
done
for cmd in "doctor --fix" "run.sh status" "run.sh rescan" "run.sh restart" "watch ~/" "roots"; do
    if grep -qF "$cmd" "$ROOT/README.md"; then
        ok "README documents '$cmd'"
    else
        bad "README documents '$cmd'"
    fi
done
if grep -q "XDG_RUNTIME_DIR/archaic.sock" "$ROOT/README.md" \
    && grep -q "config.example.toml" "$ROOT/README.md"; then
    ok "README documents socket path + example config"
else
    bad "README documents socket path + example config"
fi
if "$ROOT/build/archaic-cli" 2>&1 | grep -q "watch <path>" \
    && "$ROOT/build/archaic-cli" 2>&1 | grep -q "doctor \["; then
    ok "README commands exist in the CLI"
else
    bad "README commands exist in the CLI"
fi

echo ""
echo "copy-install: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
