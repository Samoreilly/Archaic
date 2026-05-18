#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SOCK_PATH="/tmp/archaic-daemon.sock"
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
    echo "/tmp/archaic-daemon.sock"
}

usage() {
    echo "Usage: $0 {start|stop|status|install-fish|uninstall-fish|install-bash|uninstall-bash|restart|rescan|install} [scan_path]"
    echo ""
    echo "Commands:"
    echo "  start [path]    Start the daemon scanning the given path (default: /home/sam/samdev)"
    echo "  stop            Stop the running daemon"
    echo "  status          Check if daemon is running"
    echo "  restart [path]  Restart the daemon"
    echo "  rescan [path]   Trigger an immediate rescan"
    echo "  install-fish    Install fish shell plugin for tab completion"
    echo "  uninstall-fish  Remove fish shell plugin"
    echo "  install-bash    Install bash completion script"
    echo "  uninstall-bash  Remove bash completion script"
    echo "  install         Build and install to system paths (requires sudo)"
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
    install)
        echo "Installing archaic..."
        cd "$SCRIPT_DIR/build" && sudo make install
        echo "Installed. Enable with: sudo systemctl enable --now archaic@\$USER"
        ;;
    rescan)
        echo "Triggering rescan..."
        "$SCRIPT_DIR/build/archaic-cli" scan "${2:-/home/sam/samdev}"
        ;;
    *)
        usage
        ;;
esac
