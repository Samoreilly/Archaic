#archaic.bash - Bash shell integration for archaic autocomplete daemon
#
#Usage:
# 1. Start daemon :./ run.sh start / path / to / scan
# 2. Install plugin :./ run.sh install - bash
# 3. Restart bash or run : source / usr / share / bash - completion / completions / archaic
#
#Or add to ~/.bashrc:
#source ~/.local / share / bash - completion / completions / archaic.bash

# ── Resolve binary and socket paths ──────────────────────────────────────────
_archaic_resolve_paths() {
    if
        [[-x "$(command -v archaic-cli 2>/dev/null)"]];
    then _archaic_cli = "archaic-cli" else local script_path =
        "${BASH_SOURCE[0]:-}" if[[-n "$script_path"]];
    then while[[-L "$script_path"]];
    do
        script_path = "$(readlink -f " $script_path ")" done local repo_root repo_root =
            "$(cd " $(dirname "$script_path") /.." && pwd)" if[[-x "$repo_root/build/archaic-cli"]]; then
                _archaic_cli="$repo_root/build/archaic-cli"
            fi
        fi
    fi

    _archaic_sock="/tmp/archaic-daemon.sock"

    local config_file=""
    for p in "${ARCHAIC_CONFIG:-}" "$HOME/.config/archaic/config.toml" "/etc/archaic/config.toml";
    do
        if
            [[-n "$p" && -f "$p"]];
    then config_file = "$p" break fi done

        if[[-n "$config_file"]];
    then local sock sock =
        "$(sed -n '/^\[daemon\]/,/^\[/p' " $config_file " | grep 'socket_path' | sed 's/.*= *"\
        ?\([^"]*\)"\? /\1 /' 2>/dev/null)" if[[-n "$sock"]]; then _archaic_sock = "$sock" fi fi
}

_archaic_resolve_paths

# ── Daemon health check ──────────────────────────────────────────────────────
    _archaic_daemon_healthy = 1

    _archaic_check_daemon() {
    if
        [["$_archaic_daemon_healthy" - eq 0]];
    then return 1 fi if[[-S "$_archaic_sock"]];
    then return 0 fi _archaic_daemon_healthy = 0 return 1
}

# ── Core completion function ─────────────────────────────────────────────────
_archaic_do_complete() {
    local cur = "${COMP_WORDS[COMP_CWORD]}" local prev = "${COMP_WORDS[COMP_CWORD-1]}" local cmd =
        "${COMP_WORDS[0]}"

        if[[-z "$_archaic_cli"]] ||
        !command - v "$_archaic_cli" & > / dev / null;
    then return fi

        if !_archaic_check_daemon; then
        return
    fi

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

    local results
    results="$("$_archaic_cli" complete "$resolved" 50 2>/dev/null)" || return

    local found=0
    local -a completions=()

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
            display_path="${display_path%/}/"
        fi

        completions+=("$display_path")
        found=1
    done <<< "$results"

    if [[ "$found" -eq 0 ]]; then
        local fuzzy_results
        fuzzy_results="$("$_archaic_cli" fuzzy "$resolved" 50 2>/dev/null)" || return

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
                display_path="${display_path%/}/"
            fi

            completions+=("$display_path")
            found=1
        done <<< "$fuzzy_results"
    fi

    if [[ "$found" -eq 0 ]]; then
        _archaic_daemon_healthy=0
        return
    fi
    _archaic_daemon_healthy=1

    COMPREPLY=("${completions[@]}")
}

# ── Default command list ─────────────────────────────────────────────────────
_archaic_commands=(cd ls cat vim nvim less bat rm mv cp mkdir touch)

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
    complete -r "$_archaic_cmd" 2>/dev/null
    complete -o nospace -F _archaic_do_complete "$_archaic_cmd"
done
unset _archaic_cmd

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
    echo "Socket: $_archaic_sock"
    echo "Commands: ${_archaic_commands[*]}"
}

# ── Inline ghost - text suggestions(Bash 5.0 +) ────────────────────────────────
#Uses READLINE_LINE / READLINE_POINT to show the best completion as dimmed
#text that the user can accept with Alt + Right Arrow.

_archaic_suggestion=""
_archaic_suggestion_full=""

_archaic_get_suggestion() {
#Extract the word currently being typed(text before cursor, last token)
    local before_cursor="${READLINE_LINE:0:$READLINE_POINT}"
    local cur="${before_cursor##* }"

    [[ -z "$cur" ]] && { _archaic_suggestion=""; return; }

#Only suggest for path - like inputs
    [[ "$cur" != */* ]] && { _archaic_suggestion=""; return; }

    _archaic_check_daemon || { _archaic_suggestion=""; return; }

    # Resolve to absolute path
    local resolved="$cur"
    [[ "$cur" != /* ]] && resolved="$(pwd)/$cur"
    resolved="${resolved%/}"

    # Detect command for context-aware queries
    local cmd="${READLINE_LINE%% *}"

    # Query daemon (try helper first, then CLI)
    local output=""
    if [[ -n "${_archaic_helper_pid:-}" ]] && kill -0 "$_archaic_helper_pid" 2>/dev/null; then
        output="$(echo "complete $resolved 1 $PWD $cmd" | "$_archaic_helper" "$_archaic_sock" 2>/dev/null)"
    fi
    if [[ -z "$output" ]]; then
        output="$("$_archaic_cli" complete "$resolved" 1 2>/dev/null)" || return
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
        # \033[2m = dim/italic off, \033[0m = reset
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
