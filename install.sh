#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
SCAN_PATH="${1:-$HOME}"
SOCK_PATH="/tmp/archaic-daemon.sock"

# ── Color output ─────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[archaic]${NC} $*"; }
warn()  { echo -e "${YELLOW}[archaic]${NC} $*"; }
error() { echo -e "${RED}[archaic]${NC} $*"; }

# ── Prerequisites ────────────────────────────────────────────────────────────
check_prereq() {
    if ! command -v "$1" &>/dev/null; then
        error "$1 is required but not installed."
        exit 1
    fi
}

info "Checking prerequisites..."
check_prereq cmake
check_prereq make

if command -v gcc &>/dev/null; then
    CC="gcc"
elif command -v clang &>/dev/null; then
    CC="clang"
else
    error "No C compiler found (gcc or clang required)."
    exit 1
fi
info "Using C compiler: $CC"

# ── Detect shell ─────────────────────────────────────────────────────────────
CURRENT_SHELL="$(basename "$SHELL")"

if [[ "$CURRENT_SHELL" == "fish" ]]; then
    SHELL_TYPE="fish"
elif [[ "$CURRENT_SHELL" == "bash" ]]; then
    SHELL_TYPE="bash"
else
    warn "Unsupported shell: $CURRENT_SHELL. Installing anyway — manual sourcing required."
    SHELL_TYPE="unknown"
fi
info "Detected shell: $SHELL_TYPE"

# ── Build ─────────────────────────────────────────────────────────────────────
info "Building archaic..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)" >/dev/null 2>&1
info "Build complete."

# ── Check if daemon already running ──────────────────────────────────────────
if [[ -S "$SOCK_PATH" ]] && kill -0 "$(cat "${SOCK_PATH}.pid" 2>/dev/null)" 2>/dev/null; then
    info "Daemon already running. Restarting..."
    "$BUILD_DIR/archaic-cli" shutdown 2>/dev/null || true
    sleep 1
    rm -f "$SOCK_PATH" "${SOCK_PATH}.pid"
fi

# ── Start daemon ─────────────────────────────────────────────────────────────
info "Starting daemon, scanning: $SCAN_PATH"
"$BUILD_DIR/archaic" --daemon "$SCAN_PATH" "$SOCK_PATH" &
echo $! > "${SOCK_PATH}.pid"

# Wait for daemon to be ready
for i in $(seq 1 10); do
    if [[ -S "$SOCK_PATH" ]]; then
        if "$BUILD_DIR/archaic-cli" ping &>/dev/null; then
            break
        fi
    fi
    sleep 0.5
done

if [[ -S "$SOCK_PATH" ]] && "$BUILD_DIR/archaic-cli" ping &>/dev/null; then
    info "Daemon started (PID: $(cat "${SOCK_PATH}.pid"))"
else
    error "Daemon failed to start."
    exit 1
fi

# ── Install shell plugin ─────────────────────────────────────────────────────
case "$SHELL_TYPE" in
    fish)
        FISH_CONF_DIR="$HOME/.config/fish/conf.d"
        mkdir -p "$FISH_CONF_DIR"
        ln -sf "$SCRIPT_DIR/fish/archaic.fish" "$FISH_CONF_DIR/archaic.fish"
        info "Fish plugin installed."

        # Source immediately if running in fish
        if [[ "$CURRENT_SHELL" == "fish" ]]; then
            fish -c "source $FISH_CONF_DIR/archaic.fish" 2>/dev/null || true
            info "Fish plugin sourced."
        fi
        ;;
    bash)
        BASH_COMP_DIR="$HOME/.local/share/bash-completion/completions"
        mkdir -p "$BASH_COMP_DIR"
        ln -sf "$SCRIPT_DIR/bash/archaic.bash" "$BASH_COMP_DIR/archaic.bash"
        info "Bash completion installed."

        # Source immediately if running in bash
        if [[ "$CURRENT_SHELL" == "bash" ]]; then
            source "$BASH_COMP_DIR/archaic.bash" 2>/dev/null || true
            info "Bash completion sourced."
        fi
        ;;
    *)
        warn "Shell plugin not installed for $CURRENT_SHELL."
        warn "Manual setup required. See README.md for instructions."
        ;;
esac

# ── Done ─────────────────────────────────────────────────────────────────────
echo ""
info "Archaic is ready!"
info "  Scan path: $SCAN_PATH"
info "  Socket:    $SOCK_PATH"
info "  Shell:     $SHELL_TYPE"
echo ""
info "Commands:"
info "  ./run.sh status    — Check daemon status"
info "  ./run.sh stop      — Stop daemon"
info "  ./run.sh restart   — Restart daemon"
info "  ./run.sh rescan    — Trigger rescan"
echo ""
