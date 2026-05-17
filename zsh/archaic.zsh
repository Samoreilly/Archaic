#compdef cd ls cat vim nvim less bat rm mv cp mkdir touch

# archaic.zsh - ZSH shell integration for archaic autocomplete daemon
#
# Usage:
#   1. Start daemon: ./run.sh start /path/to/scan
#   2. Install plugin: ./run.sh install-zsh
#   3. Restart zsh or run: compinit

# ── Resolve binary and socket paths ──────────────────────────────────────────
_archaic_resolve_paths() {
    if [[ -x "$(command -v archaic-cli 2>/dev/null)" ]]; then
        _archaic_cli="archaic-cli"
    else
        local script_path="${(%):-%x}"
        if [[ -n "$script_path" ]]; then
            local repo_root
            repo_root="$(cd "$(dirname "$script_path")/.." && pwd)"
            if [[ -x "$repo_root/build/archaic-cli" ]]; then
                _archaic_cli="$repo_root/build/archaic-cli"
            fi
        fi
    fi

    if [[ -x "$(command -v archaic-helper 2>/dev/null)" ]]; then
        _archaic_helper="archaic-helper"
    else
        local script_path="${(%):-%x}"
        if [[ -n "$script_path" ]]; then
            local repo_root
            repo_root="$(cd "$(dirname "$script_path")/.." && pwd)"
            if [[ -x "$repo_root/build/archaic-helper" ]]; then
                _archaic_helper="$repo_root/build/archaic-helper"
            fi
        fi
    fi

    _archaic_sock="/tmp/archaic-daemon.sock"

    local config_file=""
    local p
    for p in "${ARCHAIC_CONFIG:-}" "$HOME/.config/archaic/config.toml" "/etc/archaic/config.toml"; do
        if [[ -n "$p" && -f "$p" ]]; then
            config_file="$p"
            break
        fi
    done

    if [[ -n "$config_file" ]]; then
        local sock
        sock="$(sed -n '/^\[daemon\]/,/^\[/p' "$config_file" | grep 'socket_path' | sed 's/.*= *"\?\([^"]*\)"\?/\1/' 2>/dev/null)"
        if [[ -n "$sock" ]]; then
            _archaic_sock="$sock"
        fi
    fi
}

_archaic_resolve_paths

# ── Helper lifecycle ─────────────────────────────────────────────────────────
_archaic_helper_pid=""

_archaic_ensure_helper() {
    if [[ -n "$_archaic_helper_pid" ]]; then
        if kill -0 "$_archaic_helper_pid" 2>/dev/null; then
            return
        fi
        _archaic_helper_pid=""
    fi

    if [[ -z "$_archaic_helper" || ! -x "$_archaic_helper" ]]; then
        return
    fi

    "$_archaic_helper" "$_archaic_sock" </dev/null >/dev/null 2>&1 &
    _archaic_helper_pid=$!
}

# ── Daemon health check ──────────────────────────────────────────────────────
_archaic_daemon_healthy=1

_archaic_check_daemon() {
    if [[ "$_archaic_daemon_healthy" -eq 0 ]]; then
        return 1
    fi
    if [[ -S "$_archaic_sock" ]]; then
        return 0
    fi
    _archaic_daemon_healthy=0
    return 1
}

# ── Core completion function ─────────────────────────────────────────────────
_archaic_do_complete() {
    if [[ -z "$_archaic_cli" ]] || ! command -v "$_archaic_cli" &>/dev/null; then
        return
    fi

    if ! _archaic_check_daemon; then
        return
    fi

    local cur="${words[CURRENT]}"
    local cmd="${words[1]}"

    local dirs_only=0
    case "$cmd" in
        cd|mkdir|pushd|popd|rmdir) dirs_only=1 ;;
    esac

    local resolved=""
    local norm_prefix=""
    if [[ -z "$cur" ]]; then
        resolved="$(pwd)"
        norm_prefix=""
    else
        if [[ "$cur" != */* ]]; then
            if [[ ! "$cur" =~ ^[a-zA-Z0-9._-]+$ ]]; then
                return
            fi
        fi

        resolved="$cur"
        if [[ "$cur" != /* ]]; then
            local clean_prefix="${cur#./}"
            resolved="$(pwd)/$clean_prefix"
        fi
        while [[ "$resolved" == */../* ]]; do
            resolved="$(echo "$resolved" | sed 's|/[^/]*/\.\./|/|')"
        done
        resolved="${resolved%/./}"
        resolved="$(echo "$resolved" | sed 's|//\+|/|g')"
        norm_prefix="${cur%/}"
    fi

    local norm_resolved="${resolved%/}"
    local results=""

    _archaic_ensure_helper

    if [[ -n "$_archaic_helper_pid" && -n "$_archaic_helper" ]]; then
        results="$(echo "complete $resolved 50" | "$_archaic_helper" "$_archaic_sock" 2>/dev/null)"
    fi

    if [[ -z "$results" ]]; then
        results="$("$_archaic_cli" complete "$resolved" 50 2>/dev/null)"
    fi

    local found=0
    local -a matches=()
    local line

    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        local type="${line%% *}"
        local full_path="${line#* }"

        if [[ "$dirs_only" -eq 1 && "$type" != "D" ]]; then
            continue
        fi

        local display_path="$full_path"
        if [[ -z "$cur" ]]; then
            display_path="$(basename "$full_path")"
        elif [[ "$cur" != /* ]]; then
            display_path="${full_path#"$norm_resolved"}"
            display_path="$norm_prefix$display_path"
        fi

        if [[ "$type" == "D" ]]; then
            matches+=("${display_path%/}/")
        else
            matches+=("$display_path")
        fi
        found=1
    done <<< "$results"

    if [[ "$found" -eq 0 ]]; then
        local fuzzy_results=""
        if [[ -n "$_archaic_helper_pid" && -n "$_archaic_helper" ]]; then
            fuzzy_results="$(echo "fuzzy $resolved 50" | "$_archaic_helper" "$_archaic_sock" 2>/dev/null)"
        fi
        if [[ -z "$fuzzy_results" ]]; then
            fuzzy_results="$("$_archaic_cli" fuzzy "$resolved" 50 2>/dev/null)"
        fi

        while IFS= read -r line; do
            [[ -z "$line" ]] && continue
            local type="${line%% *}"
            local full_path="${line#* }"

            if [[ "$dirs_only" -eq 1 && "$type" != "D" ]]; then
                continue
            fi

            local display_path="$full_path"
            if [[ -z "$cur" ]]; then
                display_path="$(basename "$full_path")"
            elif [[ "$cur" != /* ]]; then
                display_path="${full_path#"$norm_resolved"}"
                display_path="$norm_prefix$display_path"
            fi

            if [[ "$type" == "D" ]]; then
                matches+=("${display_path%/}/")
            else
                matches+=("$display_path")
            fi
            found=1
        done <<< "$fuzzy_results"
    fi

    if [[ "$found" -eq 0 ]]; then
        _archaic_daemon_healthy=0
        return
    fi
    _archaic_daemon_healthy=1

    compadd -Q -a matches
}

# ── Default command list ─────────────────────────────────────────────────────
_archaic_commands=(cd ls cat vim nvim less bat rm mv cp mkdir touch)

_archaic_load_commands() {
    local config_file=""
    local p
    for p in "${ARCHAIC_CONFIG:-}" "$HOME/.config/archaic/config.toml" "/etc/archaic/config.toml"; do
        if [[ -n "$p" && -f "$p" ]]; then
            config_file="$p"
            break
        fi
    done

    if [[ -n "$config_file" ]]; then
        local cfg_cmds
        cfg_cmds="$(sed -n '/^\[zsh\]/,/^\[/p' "$config_file" | grep 'commands' | sed 's/.*= *\[\(.*\)\]/\1/' | tr -d '"' | tr ',' '\n' | sed 's/^ *//;s/ *$//' 2>/dev/null)"
        if [[ -n "$cfg_cmds" ]]; then
            _archaic_commands=()
            while IFS= read -r c; do
                [[ -n "$c" ]] && _archaic_commands+=("$c")
            done <<< "$cfg_cmds"
        fi
    fi
}

_archaic_load_commands

# ── Register completions ─────────────────────────────────────────────────────
compdef _archaic_do_complete ${_archaic_commands[@]}

# ── Status function ──────────────────────────────────────────────────────────
archaic-status() {
    if [[ -S "$_archaic_sock" ]]; then
        local ping_output
        ping_output="$("$_archaic_cli" ping 2>/dev/null)"
        if [[ $? -eq 0 ]]; then
            echo "Archaic daemon: running ($ping_output)"
        else
            echo "Archaic daemon: socket exists but unresponsive"
        fi
    else
        echo "Archaic daemon: not running"
    fi
    echo "CLI: $_archaic_cli"
    echo "Helper: ${_archaic_helper:-not found}"
    if [[ -n "$_archaic_helper_pid" ]] && kill -0 "$_archaic_helper_pid" 2>/dev/null; then
        echo "Helper PID: $_archaic_helper_pid (running)"
    else
        echo "Helper PID: (not running)"
    fi
    echo "Socket: $_archaic_sock"
    echo "Commands: ${_archaic_commands[*]}"
}
