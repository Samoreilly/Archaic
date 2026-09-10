#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
SCAN_PATH="${1:-}"
if [[ -n "$SCAN_PATH" && ! -d "$SCAN_PATH" ]]; then
    echo "Scan path does not exist: $SCAN_PATH" >&2
    exit 1
fi
if [[ -n "${XDG_RUNTIME_DIR:-}" ]]; then
    SOCK_PATH="$XDG_RUNTIME_DIR/archaic.sock"
else
    SOCK_PATH="/tmp/archaic-$(id -u).sock"
fi

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'
info()  { echo -e "${GREEN}[archaic]${NC} $*"; }
warn()  { echo -e "${YELLOW}[archaic]${NC} $*"; }
error() { echo -e "${RED}[archaic]${NC} $*"; }

need() {
    if ! command -v "$1" &>/dev/null; then
        error "$1 is required but not installed."
        exit 1
    fi
}

info "Checking prerequisites..."
need cmake
need make
if command -v gcc &>/dev/null; then
    CC="gcc"
elif command -v clang &>/dev/null; then
    CC="clang"
else
    error "Need gcc or clang (C23)."
    exit 1
fi
if ! pkg-config --exists fmt 2>/dev/null \
    && [[ ! -f /usr/include/fmt/core.h ]] \
    && [[ ! -f /usr/local/include/fmt/core.h ]] \
    && [[ ! -d /opt/homebrew/include/fmt ]]; then
    error "libfmt is required."
    error "  Debian/Ubuntu: sudo apt install cmake g++ libfmt-dev"
    error "  Fedora:        sudo dnf install cmake gcc fmt-devel"
    error "  macOS:         brew install cmake fmt"
    exit 1
fi
info "Compiler: $CC"

CURRENT_SHELL="$(basename "${SHELL:-}")"
case "$CURRENT_SHELL" in
    fish) SHELL_TYPE="fish" ;;
    bash) SHELL_TYPE="bash" ;;
    zsh)  SHELL_TYPE="zsh" ;;
    *)
        warn "Unsupported shell: ${CURRENT_SHELL:-unknown}. Plugins still install; source them yourself."
        SHELL_TYPE="unknown"
        ;;
esac
info "Shell: $SHELL_TYPE"

info "Building..."
mkdir -p "$BUILD_DIR"
cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
info "Build complete."

mkdir -p "$HOME/.local/bin"
ln -sf "$BUILD_DIR/archaic" "$HOME/.local/bin/archaic"
ln -sf "$BUILD_DIR/archaic-cli" "$HOME/.local/bin/archaic-cli"
ln -sf "$BUILD_DIR/archaic-helper" "$HOME/.local/bin/archaic-helper"

if [[ ":$PATH:" != *":$HOME/.local/bin:"* ]]; then
    warn "$HOME/.local/bin is not on PATH. Add it so archaic-cli works in new shells."
fi

case "$SHELL_TYPE" in
    fish)
        FISH_CONF_DIR="$HOME/.config/fish/conf.d"
        mkdir -p "$FISH_CONF_DIR"
        ln -sf "$SCRIPT_DIR/fish/archaic.fish" "$FISH_CONF_DIR/archaic.fish"
        rm -f "$FISH_CONF_DIR/99-archaic-reload.fish"
        info "Fish plugin: $FISH_CONF_DIR/archaic.fish"
        ;;
    bash)
        BASH_COMP_DIR="$HOME/.local/share/bash-completion/completions"
        mkdir -p "$BASH_COMP_DIR"
        ln -sf "$SCRIPT_DIR/bash/archaic.bash" "$BASH_COMP_DIR/archaic.bash"
        if [[ -f "$HOME/.bashrc" ]] && ! grep -q 'archaic.bash' "$HOME/.bashrc" 2>/dev/null; then
            echo '' >> "$HOME/.bashrc"
            echo 'source "$HOME/.local/share/bash-completion/completions/archaic.bash" 2>/dev/null' >> "$HOME/.bashrc"
            info "Added source line to ~/.bashrc"
        fi
        info "Bash plugin: $BASH_COMP_DIR/archaic.bash"
        ;;
    zsh)
        ZSH_CONF_DIR="$HOME/.config/zsh"
        mkdir -p "$ZSH_CONF_DIR"
        ln -sf "$SCRIPT_DIR/zsh/archaic.zsh" "$ZSH_CONF_DIR/archaic.zsh"
        if [[ -f "$HOME/.zshrc" ]] && ! grep -q 'archaic.zsh' "$HOME/.zshrc" 2>/dev/null; then
            echo '' >> "$HOME/.zshrc"
            echo 'source "$HOME/.config/zsh/archaic.zsh" 2>/dev/null' >> "$HOME/.zshrc"
            info "Added source line to ~/.zshrc"
        else
            warn "Add this to ~/.zshrc: source $ZSH_CONF_DIR/archaic.zsh"
        fi
        info "Zsh plugin: $ZSH_CONF_DIR/archaic.zsh"
        ;;
esac

USER_BUS="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/bus"
if command -v systemctl >/dev/null 2>&1 && [[ -S "$USER_BUS" ]]; then
    info "Enabling user service (login autostart)..."
    if [[ -n "$SCAN_PATH" ]]; then
        "$SCRIPT_DIR/run.sh" enable-service "$SCAN_PATH"
    else
        "$SCRIPT_DIR/run.sh" enable-service
    fi
else
    if [[ -S "$SOCK_PATH" ]]; then
        "$BUILD_DIR/archaic-cli" --sock "$SOCK_PATH" shutdown 2>/dev/null || true
        sleep 0.5
        rm -f "$SOCK_PATH" "${SOCK_PATH}.pid"
    fi
    if [[ -n "$SCAN_PATH" ]]; then
        mkdir -p "$HOME/.config/archaic"
        grep -qxF "$SCAN_PATH" "$HOME/.config/archaic/roots" 2>/dev/null || echo "$SCAN_PATH" >> "$HOME/.config/archaic/roots"
    fi
    info "Starting daemon"
    "$BUILD_DIR/archaic" --daemon "$SOCK_PATH" &
    echo $! > "${SOCK_PATH}.pid"
    disown $! 2>/dev/null || true
fi

ok=0
for _ in $(seq 1 20); do
    if "$BUILD_DIR/archaic-cli" --sock "$SOCK_PATH" ping &>/dev/null; then
        ok=1
        break
    fi
    sleep 0.25
done
if [[ "$ok" -ne 1 ]]; then
    error "Daemon did not respond. Run: ./build/archaic-cli doctor"
    exit 1
fi

echo ""
info "Ready. Add trees with: archaic-cli watch ~/src"
info "Socket: $SOCK_PATH"
echo ""
info "Open a new terminal, type:  cd "
info "then press Tab."
echo ""
info "Check:  archaic-cli doctor"
echo ""
