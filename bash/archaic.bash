# archaic.bash - Bash shell integration for archaic autocomplete daemon
#
# Usage:
#  1. Start daemon: ./run.sh start /path/to/scan
#  2. Install plugin: ./run.sh install-bash
#  3. Restart bash or run: source ~/.local/share/bash-completion/completions/archaic.bash
#
# Or add to ~/.bashrc:
# source ~/.local/share/bash-completion/completions/archaic.bash

# ── Resolve binary and socket paths ──────────────────────────────────────────
_archaic_resolve_paths() {
    if [[ -x "$(command -v archaic-cli 2>/dev/null)" ]]; then
        _archaic_cli="archaic-cli"
    elif [[ -x "$HOME/.local/bin/archaic-cli" ]]; then
        _archaic_cli="$HOME/.local/bin/archaic-cli"
    else
        local script_path="${BASH_SOURCE[0]:-}"
        if [[ -n "$script_path" ]]; then
            while [[ -L "$script_path" ]]; do
                script_path="$(readlink -f "$script_path")"
            done
            local repo_root
            repo_root="$(cd "$(dirname "$script_path")/.." && pwd)"
            if [[ -x "$repo_root/build/archaic-cli" ]]; then
                _archaic_cli="$repo_root/build/archaic-cli"
            fi
        fi
    fi

    if [[ -x "$(command -v archaic-helper 2>/dev/null)" ]]; then
        _archaic_helper="archaic-helper"
    elif [[ -x "$HOME/.local/bin/archaic-helper" ]]; then
        _archaic_helper="$HOME/.local/bin/archaic-helper"
    else
        local script_path="${BASH_SOURCE[0]:-}"
        if [[ -n "$script_path" ]]; then
            while [[ -L "$script_path" ]]; do
                script_path="$(readlink -f "$script_path")"
            done
            local repo_root
            repo_root="$(cd "$(dirname "$script_path")/.." && pwd)"
            if [[ -x "$repo_root/build/archaic-helper" ]]; then
                _archaic_helper="$repo_root/build/archaic-helper"
            fi
        fi
    fi

    if [[ -n "${XDG_RUNTIME_DIR:-}" ]]; then
        _archaic_sock="$XDG_RUNTIME_DIR/archaic.sock"
    else
        _archaic_sock="/tmp/archaic-$(id -u).sock"
    fi

    local config_file=""
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

# ── Bounded daemon calls (Tab must never block) ──────────────────────────────
_archaic_to() {
    local _t="$1"
    shift
    if command -v timeout &>/dev/null; then
        timeout "$_t" "$@"
    else
        "$@"
    fi
}

_archaic_q() {
    [[ -x "${_archaic_helper:-}" ]] || return 1
    printf '%s\n' "$1" | _archaic_to 0.4 "$_archaic_helper" "$_archaic_sock" 2>/dev/null
}

_archaic_c() {
    _archaic_to 0.4 "$_archaic_cli" "$@" 2>/dev/null
}

_archaic_ping() {
    _archaic_to 0.3 "$_archaic_cli" ping 2>/dev/null
}

# ── Environment variable & tilde expansion ────────────────────────────────────
_archaic_expand_path() {
    local path="$1"

    # Handle ~ and ~user/ syntax
    if [[ "$path" == ~* ]]; then
        local tilde_part="${path%%/*}"
        local remainder="${path#"$tilde_part"}"
        if [[ "$tilde_part" == "~" || "$tilde_part" == "~/" ]]; then
            path="$HOME$remainder"
        else
            local uname="${tilde_part#\~}"
            local uhome
            uhome=$(getent passwd "$uname" 2>/dev/null | cut -d: -f6)
            if [[ -z "$uhome" ]]; then
                uhome=$(eval echo "~$uname" 2>/dev/null)
            fi
            if [[ -n "$uhome" ]]; then
                path="$uhome$remainder"
            fi
        fi
    fi

    # Expand ${VAR} patterns first (longer matches)
    local expanded="$path"
    while [[ "$expanded" =~ \$\{([A-Za-z_][A-Za-z0-9_]*)\} ]]; do
        local var_name="${BASH_REMATCH[1]}"
        local var_val="${!var_name:-}"
        expanded="${expanded//\$\{$var_name\}/$var_val}"
    done

    # Then expand $VAR patterns
    while [[ "$expanded" =~ \$([A-Za-z_][A-Za-z0-9_]*) ]]; do
        local var_name="${BASH_REMATCH[1]}"
        local var_val="${!var_name:-}"
        expanded="${expanded//\$$var_name/$var_val}"
    done

    echo "$expanded"
}

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

# ── Cleanup on exit ──────────────────────────────────────────────────────────
_archaic_cleanup() {
    if [[ -n "$_archaic_helper_pid" ]]; then
        kill "$_archaic_helper_pid" 2>/dev/null
    fi
}
trap _archaic_cleanup EXIT

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
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local prev="${COMP_WORDS[COMP_CWORD-1]}"
    local cmd="${COMP_WORDS[0]}"

    if [[ -z "$_archaic_cli" ]] || ! command -v "$_archaic_cli" &>/dev/null; then
        return
    fi

    if [[ "$cur" == -* ]]; then
        return
    fi

    case "$cmd" in
        cd|ls|ll|la|l|cat|vim|nvim|hx|nano|emacs|less|more|bat|rm|mv|cp|mkdir|rmdir|pushd|popd|touch|head|tail|chmod|chown|ln|tar|unzip|open|code|rg|fd|eza|grep|find)
            ;;
        *)
            if [[ -z "$cur" ]]; then
                return
            fi
            if [[ "$cur" != .* && "$cur" != ~* && "$cur" != /* && "$cur" != */* ]]; then
                return
            fi
            ;;
    esac

    if ! _archaic_check_daemon; then
        return
    fi

    local dirs_only=0
    case "$cmd" in
        cd|mkdir|pushd|popd|rmdir) dirs_only=1 ;;
    esac

    # Expand environment variables and ~user/ syntax
    local expanded_cur=$(_archaic_expand_path "$cur")

    local resolved=""
    local norm_prefix=""
    if [[ -z "$expanded_cur" ]]; then
        resolved="$(pwd)"
        norm_prefix=""
    else
        if [[ "$expanded_cur" != */* ]]; then
            if [[ ! "$expanded_cur" =~ ^[a-zA-Z0-9._-]+$ ]]; then
                return
            fi
        fi

        resolved="$expanded_cur"
        if [[ "$expanded_cur" != /* ]]; then
            local clean_prefix="${expanded_cur#./}"
            resolved="$(pwd)/$clean_prefix"
        fi
        while [[ "$resolved" == */../* ]]; do
            resolved="$(echo "$resolved" | sed 's|/[^/]*/\.\./|/|')"
        done
        # Handle trailing /.. (e.g., /home/sam/samdev/.. -> /home/sam)
        while [[ "$resolved" == */.. ]]; do
            resolved="$(echo "$resolved" | sed 's|/[^/]*/\.\.$|/|')"
        done
        resolved="${resolved%/./}"
        resolved="$(echo "$resolved" | sed 's|//\+|/|g')"
        # Remove trailing slash unless root
        if [[ "$resolved" != "/" ]]; then
            resolved="${resolved%/}"
        fi
        norm_prefix="${expanded_cur%/}"
    fi

    local norm_resolved="${resolved%/}"

    _archaic_ensure_helper

    local results=""
    if [[ -n "$_archaic_helper_pid" ]] && kill -0 "$_archaic_helper_pid" 2>/dev/null; then
        results="$(_archaic_q "$(printf 'complete\t%s\t%s\t%s\t%s' "$dirs_only" 50 "$PWD" "$resolved")")"
    fi
    if [[ -z "$results" ]]; then
        results="$(_archaic_c complete "$resolved" 50 "$PWD" "$dirs_only")" || return
    fi

    local found=0
    local -a completions=()

    if [[ -n "$expanded_cur" && "$expanded_cur" != */ && -d "$resolved" ]]; then
        completions+=("${expanded_cur%/}/")
        found=1
    fi

    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        local type="${line%% *}"
        local full_path="${line#* }"

        if [[ "$dirs_only" -eq 1 && "$type" != "D" ]]; then
            continue
        fi

        local display_path="$full_path"
        local typed="${COMP_WORDS[COMP_CWORD]}"
        if [[ -z "$typed" ]]; then
            display_path="$(basename "$full_path")"
        elif [[ "$typed" == ~* ]]; then
            if [[ "$full_path" == "$HOME" ]]; then
                display_path="~"
            elif [[ "$full_path" == "$HOME"/* ]]; then
                display_path="~${full_path#"$HOME"}"
            fi
        elif [[ "$typed" == /* ]]; then
            display_path="$full_path"
        else
            display_path="${full_path#"$norm_resolved"}"
            display_path="${typed%/}$display_path"
        fi

        if [[ "$type" == "D" ]]; then
            display_path="${display_path%/}/"
        fi

        completions+=("$display_path")
        found=1
    done <<< "$results"

    if [[ "$found" -eq 0 ]]; then
        local fuzzy_q="$resolved"
        local fuzzy_token=0
        if [[ -n "$expanded_cur" && "$expanded_cur" != */* ]]; then
            fuzzy_q="$expanded_cur"
            fuzzy_token=1
        fi
        local fuzzy_results=""
        if [[ -n "$_archaic_helper_pid" ]] && kill -0 "$_archaic_helper_pid" 2>/dev/null; then
            fuzzy_results="$(_archaic_q "fuzzy $fuzzy_q 50")"
        fi
        if [[ -z "$fuzzy_results" ]]; then
            fuzzy_results="$(_archaic_c fuzzy "$fuzzy_q" 50)" || return
        fi

        while IFS= read -r line; do
            [[ -z "$line" ]] && continue
            local type="${line%% *}"
            local full_path="${line#* }"

            if [[ "$fuzzy_token" -eq 0 && "$resolved" == /* ]]; then
                if [[ "$full_path" != "$norm_resolved"/* ]]; then
                    continue
                fi
                local remainder="${full_path#"$norm_resolved"/}"
                if [[ "$remainder" == */* ]]; then
                    continue
                fi
            fi

            if [[ "$dirs_only" -eq 1 && "$type" != "D" ]]; then
                continue
            fi

            local display_path="$full_path"
            if [[ "$fuzzy_token" -eq 1 ]]; then
                display_path="${full_path#"$PWD"/}"
            elif [[ -z "$expanded_cur" ]]; then
                display_path="$(basename "$full_path")"
            elif [[ "$expanded_cur" != /* ]]; then
                display_path="${full_path#"$norm_resolved"}"
                display_path="$norm_prefix$display_path"
            fi

            if [[ "$type" == "D" ]]; then
                display_path="${display_path%/}/"
            fi

            completions+=("$display_path")
            found=1
        done <<< "$fuzzy_results"
    fi

    if [[ "$found" -eq 0 ]]; then
        return
    fi

    COMPREPLY=("${completions[@]}")
}

# ── Default command list ─────────────────────────────────────────────────────
_archaic_commands=(cd ls ll la cat vim nvim hx nano emacs less more bat rm mv cp mkdir rmdir pushd popd touch head tail chmod chown ln tar unzip zip gzip diff open xdg-open code cursor rg fd eza exa lsd tree grep find file stat wc python python3 pytest node bun cargo go gcc g++ clang make cmake ninja scp rsync jq sudo docker kubectl npm pnpm yarn pip)

_archaic_load_commands() {
    local config_file=""
    for p in "${ARCHAIC_CONFIG:-}" "$HOME/.config/archaic/config.toml" "/etc/archaic/config.toml"; do
        if [[ -n "$p" && -f "$p" ]]; then
            config_file="$p"
            break
        fi
    done

    if [[ -n "$config_file" ]]; then
        local cfg_cmds
        cfg_cmds="$(sed -n '/^\[bash\]/,/^\[/p' "$config_file" | grep 'commands' | sed 's/.*= *\[\(.*\)\]/\1/' | tr -d '"' | tr ',' '\n' | sed 's/^ *//;s/ *$//' 2>/dev/null)"
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
for _archaic_cmd in "${_archaic_commands[@]}"; do
    complete -o nospace -o default -F _archaic_do_complete "$_archaic_cmd"
done
unset _archaic_cmd

# ── Status function ──────────────────────────────────────────────────────────
archaic-status() {
    if [[ -S "$_archaic_sock" ]]; then
        local ping_output
        ping_output="$(_archaic_ping)"
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

# ── Inline ghost-text suggestions (Bash 5.0+) ────────────────────────────────
# Uses READLINE_LINE / READLINE_POINT to show the best completion as dimmed
# text that the user can accept with Alt + Right Arrow.

_archaic_suggestion=""
_archaic_suggestion_full=""

_archaic_get_suggestion() {
    # Extract the word currently being typed (text before cursor, last token)
    local before_cursor="${READLINE_LINE:0:$READLINE_POINT}"
    local cur="${before_cursor##* }"

    [[ -z "$cur" ]] && { _archaic_suggestion=""; return; }

    # Only suggest for path-like inputs
    [[ "$cur" != */* ]] && { _archaic_suggestion=""; return; }

    _archaic_check_daemon || { _archaic_suggestion=""; return; }

    # Expand environment variables
    local expanded_cur=$(_archaic_expand_path "$cur")

    # Resolve to absolute path
    local resolved="$expanded_cur"
    [[ "$expanded_cur" != /* ]] && resolved="$(pwd)/$expanded_cur"
    resolved="${resolved%/}"

    # Detect command for context-aware queries
    local cmd="${READLINE_LINE%% *}"

    _archaic_ensure_helper

    # Query daemon (try helper first, then CLI)
    local output=""
    if [[ -n "$_archaic_helper_pid" ]] && kill -0 "$_archaic_helper_pid" 2>/dev/null; then
        output="$(_archaic_q "complete $resolved 1 $PWD $cmd")"
    fi
    if [[ -z "$output" ]]; then
        output="$(_archaic_c complete "$resolved" 1)" || return
    fi

    # Parse result: "D /path" or "F /path"
    local full_path="${output#* }"
    [[ -z "$full_path" || "$full_path" == "$output" ]] && { _archaic_suggestion=""; return; }

    # Calculate the remainder (ghost text)
    local norm_path="${full_path%/}"
    if [[ "$norm_path" == "$resolved"* ]]; then
        _archaic_suggestion="${norm_path#$resolved}"
        _archaic_suggestion_full="$full_path"
    else
        _archaic_suggestion=""
    fi
}

_archaic_accept_suggestion() {
    if [[ -n "$_archaic_suggestion" ]]; then
        READLINE_LINE="${READLINE_LINE}${_archaic_suggestion}"
        READLINE_POINT=${#READLINE_LINE}
        _archaic_suggestion=""
    fi
}

# Render ghost text by appending dimmed suggestion to the prompt.
# Called via PROMPT_COMMAND before each prompt display.
_archaic_render_suggestion() {
    if [[ -n "$_archaic_suggestion" ]]; then
        # \033[2m = dim, \033[0m = reset
        printf '\033[2m%s\033[0m' "$_archaic_suggestion"
    fi
}

# Hook into PROMPT_COMMAND: update suggestion + preserve existing hooks
_archaic_orig_prompt_command="${PROMPT_COMMAND:-}"
_archaic_prompt_hook() {
    _archaic_get_suggestion
    if [[ -n "$_archaic_orig_prompt_command" ]]; then
        eval "$_archaic_orig_prompt_command"
    fi
}
PROMPT_COMMAND="_archaic_prompt_hook"

# Bind Alt+Right Arrow to accept the inline suggestion
# \e[1;3C = Alt+Right (standard), \e\e[C = Alt+Right (alternate)
bind -x '"\e[1;3C": _archaic_accept_suggestion' 2>/dev/null
bind -x '"\e\e[C": _archaic_accept_suggestion' 2>/dev/null

# Also bind Ctrl+Right as alternative accept key
bind -x '"\e[1;5C": _archaic_accept_suggestion' 2>/dev/null
