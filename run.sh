#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FISH_CONF_DIR="$HOME/.config/fish/conf.d"
FISH_PLUGIN="$SCRIPT_DIR/fish/archaic.fish"

# Extract a simple key = "value" or key = value from a TOML section
toml_get() {
    local file="$1" section="$2" key="$3"
    local in_section=0
    while IFS= read -r line; do
        line="${line%%#*}"
        line="$(echo "$line" | xargs 2>/dev/null || echo "$line")"
        [ -z "$line" ] && continue

        if [[ "$line" == "[$section]" ]]; then
            in_section=1
            continue
        elif [[ "$line" == "["* ]]; then
            in_section=0
            continue
        fi

        if [ "$in_section" -eq 1 ]; then
            local k="${line%%=*}"
            local v="${line#*=}"
            k="$(echo "$k" | xargs 2>/dev/null || echo "$k")"
            v="$(echo "$v" | xargs 2>/dev/null || echo "$v")"
            v="${v%\"}"
            v="${v#\"}"
            if [ "$k" = "$key" ]; then
                echo "$v"
                return
            fi
        fi
    done < "$file"
}

# Resolve config file and extract daemon socket_path
resolve_sock_path() {
    local config_paths=("${ARCHAIC_CONFIG:-}" "$HOME/.config/archaic/config.toml" "/etc/archaic/config.toml")
    local config_file=""
    for p in "${config_paths[@]}"; do
        [ -n "$p" ] && [ -f "$p" ] && config_file="$p" && break
    done

    if [ -n "$config_file" ]; then
        local sock
        sock="$(toml_get "$config_file" daemon socket_path)"
        [ -n "$sock" ] && echo "$sock" && return
    fi
    if [ -n "${XDG_RUNTIME_DIR:-}" ]; then
        echo "$XDG_RUNTIME_DIR/archaic.sock"
    else
        echo "/tmp/archaic-$(id -u).sock"
    fi
}

usage() {
    echo "Usage: $0 {start|stop|status|install-fish|uninstall-fish|install-bash|uninstall-bash|install-zsh|uninstall-zsh|restart|rescan|enable-service|disable-service} [scan_path]"
    echo ""
    echo "Commands:"
    echo "  start [path]    Start the daemon (default scan path: \$HOME)"
    echo "  stop            Stop the running daemon"
    echo "  status          Check if daemon is running"
    echo "  restart [path]  Restart the daemon"
    echo "  rescan [path]   Trigger an immediate rescan"
    echo "  install-fish    Install fish shell plugin for tab completion"
    echo "  uninstall-fish  Remove fish shell plugin"
    echo "  install-bash    Install bash completion script"
    echo "  uninstall-bash  Remove bash completion script"
    echo "  install         Build and install to system paths (requires sudo)"
    echo "  enable-service  Install systemd --user unit so the daemon starts at login"
    echo "  disable-service Remove the user systemd unit"
    echo ""
    echo "Quick start (build + install + run in one step):"
    echo "  ./install.sh [scan_path]"
    exit 1
}

is_running() {
    local sock="${1:-$(resolve_sock_path)}"
    [ -S "$sock" ] && kill -0 "$(cat "${sock}.pid" 2>/dev/null)" 2>/dev/null
}

start_daemon() {
    local config_paths=("${ARCHAIC_CONFIG:-}" "$HOME/.config/archaic/config.toml" "/etc/archaic/config.toml")
    local config_file=""
    for p in "${config_paths[@]}"; do
        [ -n "$p" ] && [ -f "$p" ] && config_file="$p" && break
    done

    local scan_path="${1:-}"
    if [ -z "$scan_path" ] && [ -n "$config_file" ]; then
        scan_path="$(toml_get "$config_file" daemon scan_path)"
    fi

    local sock_path
    sock_path="$(resolve_sock_path)"

    if is_running "$sock_path"; then
        echo "Daemon already running (socket: $sock_path)"
        return 0
    fi

    rm -f "$sock_path"

    if [ -n "$scan_path" ]; then
        echo "Starting archaic daemon, scanning: $scan_path"
        "$SCRIPT_DIR/build/archaic" --daemon "$scan_path" "$sock_path" &
    else
        echo "Starting archaic daemon (using config scan_paths)"
        "$SCRIPT_DIR/build/archaic" --daemon "" "$sock_path" &
    fi
    echo $! > "${sock_path}.pid"
    echo "Socket: $sock_path"

    sleep 1

    if is_running "$sock_path"; then
        echo "Daemon started (PID: $!)"
    else
        echo "Failed to start daemon"
        return 1
    fi
}

stop_daemon() {
    local sock_path
    sock_path="$(resolve_sock_path)"

    if ! is_running "$sock_path"; then
        echo "Daemon not running"
        rm -f "$sock_path" "${sock_path}.pid"
        return 0
    fi

    echo "Stopping daemon..."
    "$SCRIPT_DIR/build/archaic-cli" shutdown 2>/dev/null || true
    sleep 2

    if is_running "$sock_path"; then
        echo "Daemon did not stop gracefully, killing..."
        kill "$(cat "${sock_path}.pid" 2>/dev/null)" 2>/dev/null || true
        sleep 1
    fi

    rm -f "$sock_path" "${sock_path}.pid"
    echo "Daemon stopped"
}

status_daemon() {
    local sock_path
    sock_path="$(resolve_sock_path)"

    if is_running "$sock_path"; then
        local pid
        pid="$(cat "${sock_path}.pid" 2>/dev/null || echo "unknown")"
        echo "Daemon is running (PID: $pid, socket: $sock_path)"
    else
        echo "Daemon is not running"
    fi
}

install_fish() {
    if [ ! -d "$FISH_CONF_DIR" ]; then
        mkdir -p "$FISH_CONF_DIR"
    fi

    ln -sf "$FISH_PLUGIN" "$FISH_CONF_DIR/archaic.fish"
    echo "Fish plugin installed: $FISH_CONF_DIR/archaic.fish -> $FISH_PLUGIN"
    echo "Restart fish or run: source $FISH_CONF_DIR/archaic.fish"
}

uninstall_fish() {
    rm -f "$FISH_CONF_DIR/archaic.fish"
    echo "Fish plugin removed"
}

BASH_COMP_DIR="$HOME/.local/share/bash-completion/completions"
BASH_COMP_SCRIPT="$SCRIPT_DIR/bash/archaic.bash"

install_bash() {
    if [ ! -d "$BASH_COMP_DIR" ]; then
        mkdir -p "$BASH_COMP_DIR"
    fi

    ln -sf "$BASH_COMP_SCRIPT" "$BASH_COMP_DIR/archaic.bash"
    echo "Bash completion installed: $BASH_COMP_DIR/archaic.bash -> $BASH_COMP_SCRIPT"
    echo "Restart bash or run: source $BASH_COMP_DIR/archaic.bash"
}

uninstall_bash() {
    rm -f "$BASH_COMP_DIR/archaic.bash"
    echo "Bash completion removed"
}

ZSH_CONF_DIR="$HOME/.config/zsh"
ZSH_PLUGIN="$SCRIPT_DIR/zsh/archaic.zsh"

install_zsh() {
    mkdir -p "$ZSH_CONF_DIR"
    ln -sf "$ZSH_PLUGIN" "$ZSH_CONF_DIR/archaic.zsh"
    echo "Zsh plugin installed: $ZSH_CONF_DIR/archaic.zsh -> $ZSH_PLUGIN"
    echo "Add to ~/.zshrc: source $ZSH_CONF_DIR/archaic.zsh"
}

uninstall_zsh() {
    rm -f "$ZSH_CONF_DIR/archaic.zsh"
    echo "Zsh plugin removed"
}

install_user_service() {
    local bin="$SCRIPT_DIR/build/archaic"
    local cli="$SCRIPT_DIR/build/archaic-cli"
    local scan="${1:-}"

    if [ ! -x "$bin" ]; then
        echo "Build the daemon first (cmake --build build --target archaic)"
        return 1
    fi

    mkdir -p "$HOME/.local/bin"
    ln -sf "$bin" "$HOME/.local/bin/archaic"
    ln -sf "$cli" "$HOME/.local/bin/archaic-cli"
    ln -sf "$SCRIPT_DIR/build/archaic-helper" "$HOME/.local/bin/archaic-helper"
    mkdir -p "$HOME/.config/systemd/user"

    local sock
    if [ -n "${XDG_RUNTIME_DIR:-}" ]; then
        sock="$XDG_RUNTIME_DIR/archaic.sock"
    else
        sock="/tmp/archaic-$(id -u).sock"
    fi

    if [ -n "$scan" ] && [ -d "$scan" ]; then
        mkdir -p "$HOME/.config/archaic"
        if ! grep -qxF "$scan" "$HOME/.config/archaic/roots" 2>/dev/null; then
            echo "$scan" >> "$HOME/.config/archaic/roots"
        fi
    fi

    cat > "$HOME/.config/systemd/user/archaic.service" <<EOF
[Unit]
Description=Archaic path-complete daemon
Documentation=https://github.com/Samoreilly/Archaic

[Service]
Type=simple
ExecStart=$HOME/.local/bin/archaic --daemon $sock
ExecStop=$HOME/.local/bin/archaic-cli --sock $sock shutdown
Restart=on-failure
RestartSec=2

[Install]
WantedBy=default.target
EOF

    systemctl --user daemon-reload
    systemctl --user enable --now archaic.service
    loginctl enable-linger "$USER" 2>/dev/null || true
    echo "User service enabled (starts at login/boot)."
    echo "  systemctl --user status archaic"
}

disable_user_service() {
    systemctl --user disable --now archaic.service 2>/dev/null || true
    rm -f "$HOME/.config/systemd/user/archaic.service"
    systemctl --user daemon-reload 2>/dev/null || true
    echo "User service disabled."
}

case "${1:-}" in
    start)
        start_daemon "${2:-}"
        ;;
    stop)
        stop_daemon
        ;;
    status)
        status_daemon
        ;;
    restart)
        stop_daemon
        start_daemon "${2:-}"
        ;;
    install-fish)
        install_fish
        ;;
    uninstall-fish)
        uninstall_fish
        ;;
    install-bash)
        install_bash
        ;;
    uninstall-bash)
        uninstall_bash
        ;;
    install-zsh)
        install_zsh
        ;;
    uninstall-zsh)
        uninstall_zsh
        ;;
    install)
        echo "Installing archaic..."
        cd "$SCRIPT_DIR/build" && sudo make install
        echo "Installed. Enable with: sudo systemctl enable --now archaic@\$USER"
        echo "Or user service: ./run.sh enable-service"
        ;;
    enable-service)
        install_user_service "${2:-}"
        ;;
    disable-service)
        disable_user_service
        ;;
    rescan)
        echo "Triggering rescan..."
        "$SCRIPT_DIR/build/archaic-cli" scan "${2:-$HOME}"
        ;;
    *)
        usage
        ;;
esac
