#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SOCK_PATH="/tmp/archaic-daemon.sock"
FISH_CONF_DIR="$HOME/.config/fish/conf.d"
FISH_PLUGIN="$SCRIPT_DIR/fish/archaic.fish"

usage() {
    echo "Usage: $0 {start|stop|status|install-fish|uninstall-fish|restart} [scan_path]"
    echo ""
    echo "Commands:"
    echo "  start [path]    Start the daemon scanning the given path (default: /home/sam/samdev)"
    echo "  stop            Stop the running daemon"
    echo "  status          Check if daemon is running"
    echo "  restart [path]  Restart the daemon"
    echo "  install-fish    Install fish shell plugin for tab completion"
    echo "  uninstall-fish  Remove fish shell plugin"
    exit 1
}

is_running() {
    [ -S "$SOCK_PATH" ] && kill -0 "$(cat "${SOCK_PATH}.pid" 2>/dev/null)" 2>/dev/null
}

start_daemon() {
    local scan_path="${1:-/home/sam/samdev}"

    if is_running; then
        echo "Daemon already running (socket: $SOCK_PATH)"
        return 0
    fi

    rm -f "$SOCK_PATH"

    echo "Starting archaic daemon, scanning: $scan_path"
    echo "Socket: $SOCK_PATH"

    "$SCRIPT_DIR/build/archaic" --daemon "$scan_path" "$SOCK_PATH" &
    echo $! > "${SOCK_PATH}.pid"

    sleep 1

    if is_running; then
        echo "Daemon started (PID: $!)"
    else
        echo "Failed to start daemon"
        return 1
    fi
}

stop_daemon() {
    if ! is_running; then
        echo "Daemon not running"
        rm -f "$SOCK_PATH" "${SOCK_PATH}.pid"
        return 0
    fi

    echo "Stopping daemon..."
    "$SCRIPT_DIR/build/archaic-cli" shutdown 2>/dev/null || true
    sleep 2

    if is_running; then
        echo "Daemon did not stop gracefully, killing..."
        kill "$(cat "${SOCK_PATH}.pid" 2>/dev/null)" 2>/dev/null || true
        sleep 1
    fi

    rm -f "$SOCK_PATH" "${SOCK_PATH}.pid"
    echo "Daemon stopped"
}

status_daemon() {
    if is_running; then
        local pid
        pid="$(cat "${SOCK_PATH}.pid" 2>/dev/null || echo "unknown")"
        echo "Daemon is running (PID: $pid, socket: $SOCK_PATH)"
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
    *)
        usage
        ;;
esac
