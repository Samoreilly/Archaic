#compdef cd ls cat vim nvim less bat rm mv cp mkdir touch head tail chmod rg fd code hx grep find sudo docker git source .

# archaic.zsh - ZSH shell integration for archaic autocomplete daemon
#
# Usage:
#   1. Start daemon: ./run.sh start /path/to/scan
#   2. Install plugin: ./run.sh install-zsh
#   3. Restart zsh or run: compinit

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

    # Expand $VAR patterns
    local expanded="$path"
    while [[ "$expanded" =~ \$([A-Za-z_][A-Za-z0-9_]*) ]]; do
        local var_name="${BASH_REMATCH[1]}"
        local var_ref="\$$var_name"
        local val="${(P)var_name:-}"
        expanded="${expanded//$var_ref/$val}"
    done

    echo "$expanded"
}

# ── Metadata helpers for completion descriptions ──────────────────────────────
_archaic_time_ago() {
    local mtime=$1
    local now=$(date +%s)
    local diff=$((now - mtime))
    if (( diff < 60 )); then
        echo "${diff}s ago"
    elif (( diff < 3600 )); then
        echo "$((diff / 60))m ago"
    elif (( diff < 86400 )); then
        echo "$((diff / 3600))h ago"
    else
        echo "$((diff / 86400))d ago"
    fi
}

_archaic_human_size() {
    local bytes=$1
    if (( bytes >= 1073741824 )); then
        echo "$(printf '%.1f' $(echo "scale=1; $bytes / 1073741824" | bc 2>/dev/null || echo "$((bytes / 1073741824))"))G"
    elif (( bytes >= 1048576 )); then
        echo "$(printf '%.1f' $(echo "scale=1; $bytes / 1048576" | bc 2>/dev/null || echo "$((bytes / 1048576))"))M"
    elif (( bytes >= 1024 )); then
        echo "$(printf '%.1f' $(echo "scale=1; $bytes / 1024" | bc 2>/dev/null || echo "$((bytes / 1024))"))K"
    else
        echo "${bytes}B"
    fi
}

_archaic_dir_info() {
    local dir_path="$1"
    if [[ -d "$dir_path" ]]; then
        local count=$(ls -A "$dir_path" 2>/dev/null | wc -l)
        if (( count == 1 )); then
            echo "dir, 1 file"
        else
            echo "dir, $count files"
        fi
    else
        echo "dir"
    fi
}

_archaic_file_info() {
    local file_path="$1"
    if [[ -f "$file_path" ]]; then
        local stat_out
        stat_out=$(stat -c "%s %Y" "$file_path" 2>/dev/null)
        if [[ $? -eq 0 ]]; then
            local size=$(echo "$stat_out" | cut -d' ' -f1)
            local mtime=$(echo "$stat_out" | cut -d' ' -f2)
            local human_size=$(_archaic_human_size "$size")
            local time_ago=$(_archaic_time_ago "$mtime")
            echo "$human_size, $time_ago"
        else
            stat_out=$(stat -f "%z %m" "$file_path" 2>/dev/null)
            if [[ $? -eq 0 ]]; then
                local size=$(echo "$stat_out" | cut -d' ' -f1)
                local mtime=$(echo "$stat_out" | cut -d' ' -f2)
                local human_size=$(_archaic_human_size "$size")
                local time_ago=$(_archaic_time_ago "$mtime")
                echo "$human_size, $time_ago"
            else
                echo "file"
            fi
        fi
    else
        echo "file"
    fi
}

# ── Resolve binary and socket paths ──────────────────────────────────────────
_archaic_resolve_paths() {
    if [[ -x "$(command -v archaic-cli 2>/dev/null)" ]]; then
        _archaic_cli="archaic-cli"
    elif [[ -x "$HOME/.local/bin/archaic-cli" ]]; then
        _archaic_cli="$HOME/.local/bin/archaic-cli"
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
    elif [[ -x "$HOME/.local/bin/archaic-helper" ]]; then
        _archaic_helper="$HOME/.local/bin/archaic-helper"
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

    if [[ -n "${XDG_RUNTIME_DIR:-}" ]]; then
        _archaic_sock="$XDG_RUNTIME_DIR/archaic.sock"
    else
        _archaic_sock="/tmp/archaic-$(id -u).sock"
    fi

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

# ── Active command detection (multi-command lines) ─────────────────────────
_arhcaic_is_sep() {
    case "$1" in
        ';'|'&'|'|'|'&&'|'||'|'('|'{') return 0 ;;
    esac
    return 1
}

# Echoes active command from words[1..upto] (zsh 1-indexed, upto exclusive).
_arhcaic_is_assign() {
    case "$1" in
        -*|*=*) [[ "$1" == -* ]] && return 1; return 0 ;;
    esac
    return 1
}

_archaic_active_cmd_words() {
    local upto="${1:-$CURRENT}"
    local active=""
    local i
    for (( i=1; i<upto; i++ )); do
        if _arhcaic_is_sep "${words[i]:-}"; then
            continue
        fi
        if _arhcaic_is_assign "${words[i]:-}"; then
            continue
        fi
        active="${words[i]}"
        break
    done
    [[ -z "$active" ]] && active="${words[1]:-}"
    for (( i=1; i<upto; i++ )); do
        if _arhcaic_is_sep "${words[i]:-}"; then
            local j=$((i+1))
            while (( j < upto )); do
                if _arhcaic_is_sep "${words[j]:-}"; then j=$((j+1)); continue; fi
                if _arhcaic_is_assign "${words[j]:-}"; then j=$((j+1)); continue; fi
                case "${words[j]:-}" in
                    '') j=$((j+1)); continue ;;
                    *) active="${words[j]}"; break ;;
                esac
            done
        fi
    done
    case "$active" in
        sudo|doas|env|nohup|timeout|watch|xargs)
            for (( i=1; i<upto; i++ )); do
                if [[ "${words[i]:-}" == "$active" ]]; then
                    local j=$((i+1))
                    while (( j < upto )); do
                        if [[ "${words[j]:-}" == -* ]]; then j=$((j+1)); continue; fi
                        if _arhcaic_is_assign "${words[j]:-}"; then j=$((j+1)); continue; fi
                        active="${words[j]}"
                        break
                    done
                    break
                fi
            done
            ;;
    esac
    print -r -- "$active"
}

_archaic_active_cmd_from_line() {
    local line="$1"
    local seg
    seg="$(print -r -- "$line" | sed -e 's/&&/\n/g' -e 's/\.\?||/\n/g' -e 's/[;|(){}&]/\n/g' | tail -n 1)"
    seg="${seg#"${seg%%[![:space:]]*}"}"
    local first=""
    local w0
    for w0 in ${(s: :)seg}; do
        case "$w0" in
            -*) first="$w0"; break ;;
            *=*) continue ;;
            *) first="$w0"; break ;;
        esac
    done
    case "$first" in
        sudo|doas|env|nohup|timeout|watch|xargs)
            local rest="${seg#*[[:space:]]}"
            local w
            for w in ${(s: :)rest}; do
                case "$w" in
                    -*) continue ;;
                    *=*) continue ;;
                    *) print -r -- "$w"; return ;;
                esac
            done
            print -r -- "$first"; return ;;
    esac
    print -r -- "$first"
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
    local cmd="$(_archaic_active_cmd_words "$CURRENT")"
    [[ -z "$cmd" ]] && cmd="${words[1]}"

    if [[ "$cur" == -* ]]; then
        return
    fi

    case "$cmd" in
        cd|ls|ll|la|l|cat|vim|nvim|hx|nano|emacs|less|more|bat|rm|mv|cp|mkdir|rmdir|pushd|popd|touch|head|tail|chmod|chown|ln|tar|unzip|open|code|rg|fd|eza|grep|find|source|.)
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

    # Expand environment variables and ~user/ syntax
    local expanded_cur=$(_archaic_expand_path "$cur")

    local dirs_only=0
    case "$cmd" in
        cd|mkdir|pushd|popd|rmdir) dirs_only=1 ;;
    esac

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
        while [[ "$resolved" == */.. ]]; do
            resolved="$(echo "$resolved" | sed 's|/[^/]*/\.\.$|/|')"
        done
        resolved="${resolved%/./}"
        resolved="$(echo "$resolved" | sed 's|//\+|/|g')"
        if [[ "$resolved" != "/" ]]; then
            resolved="${resolved%/}"
        fi
        norm_prefix="${expanded_cur%/}"
    fi

    local norm_resolved="${resolved%/}"
    local results=""
    local found=0
    local -a matches=()
    local line

    _archaic_ensure_helper

    if [[ -n "$expanded_cur" && "$expanded_cur" != */ && -d "$resolved" ]]; then
        matches+=("${expanded_cur%/}/\tdir")
        found=1
    fi

    if [[ -n "$_archaic_helper_pid" && -n "$_archaic_helper" ]]; then
        results="$(_archaic_q "$(printf 'complete\t%s\t%s\t%s\t%s' "$dirs_only" 50 "$PWD" "$resolved")")"
    fi

    if [[ -z "$results" ]]; then
        results="$(_archaic_c complete "$resolved" 50 "$PWD" "$dirs_only")"
    fi

    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        local type="${line%% *}"
        local full_path="${line#* }"

        if [[ "$dirs_only" -eq 1 && "$type" != "D" ]]; then
            continue
        fi

        local display_path="$full_path"
        if [[ -z "$expanded_cur" ]]; then
            display_path="$(basename "$full_path")"
        elif [[ "$expanded_cur" != /* ]]; then
            display_path="${full_path#"$norm_resolved"}"
            display_path="$norm_prefix$display_path"
        fi

        # Rich metadata in descriptions
        local desc=""
        if [[ "$type" == "D" ]]; then
            desc="$(_archaic_dir_info "$full_path")"
            matches+=("${display_path%/}/\t$desc")
        else
            desc="$(_archaic_file_info "$full_path")"
            matches+=("$display_path\t$desc")
        fi
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
        if [[ -n "$_archaic_helper_pid" && -n "$_archaic_helper" ]]; then
            fuzzy_results="$(_archaic_q "fuzzy $fuzzy_q 50")"
        fi
        if [[ -z "$fuzzy_results" ]]; then
            fuzzy_results="$(_archaic_c fuzzy "$fuzzy_q" 50)"
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

            local desc=""
            if [[ "$type" == "D" ]]; then
                desc="$(_archaic_dir_info "$full_path")"
                matches+=("${display_path%/}/\t$desc")
            else
                desc="$(_archaic_file_info "$full_path")"
                matches+=("$display_path\t$desc")
            fi
            found=1
        done <<< "$fuzzy_results"
    fi

    if [[ "$found" -eq 0 ]]; then
        return
    fi

    # Use _describe for rich descriptions
    _describe 'archaic' matches -Q
}

# ── Default command list ─────────────────────────────────────────────────────
_archaic_commands=(cd ls ll la cat vim nvim hx nano emacs less more bat rm mv cp mkdir rmdir pushd popd touch head tail chmod chown ln tar unzip zip gzip diff open xdg-open code cursor rg fd eza exa lsd tree grep find file stat wc python python3 pytest node bun cargo go gcc g++ clang make cmake ninja scp rsync jq sudo docker kubectl npm pnpm yarn pip source .)

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

# ── Inline suggestions via RPS1 (right prompt) ───────────────────────────────
_archaic_suggestion=""
_archaic_suggestion_full=""
_archaic_last_suggest_token=""
_archaic_last_suggest_result=""

_archaic_get_suggestion() {
    [[ "${ARCHAIC_SUGGEST_ON_PROMPT:-}" == "0" ]] && { _archaic_suggestion=""; return; }
    local cur="${LBUFFER##* }"
    [[ -z "$cur" ]] && { _archaic_suggestion=""; return; }
    [[ "$cur" != */* ]] && { _archaic_suggestion=""; return; }
    if [[ "$cur" == "$_archaic_last_suggest_token" ]]; then
        _archaic_suggestion="$_archaic_last_suggest_result"
        return
    fi
    _archaic_check_daemon || { _archaic_suggestion=""; _archaic_last_suggest_token="$cur"; _archaic_last_suggest_result=""; return; }

    local expanded_cur=$(_archaic_expand_path "$cur")
    local resolved="$expanded_cur"
    [[ "$expanded_cur" != /* ]] && resolved="$(pwd)/$expanded_cur"
    resolved="${resolved%/}"

    local cmd="$(_archaic_active_cmd_from_line "$LBUFFER")"
    [[ -z "$cmd" ]] && cmd="${LBUFFER%% *}"
    local output=""
    if [[ -n "$_archaic_helper_pid" && -n "$_archaic_helper" ]]; then
        output="$(_archaic_q "complete $resolved 1 $PWD $cmd")"
    fi
    if [[ -z "$output" ]]; then
        output="$(_archaic_c complete "$resolved" 1)" || return
    fi

    local full_path="${output#* }"
    [[ -z "$full_path" || "$full_path" == "$output" ]] && { _archaic_suggestion=""; _archaic_last_suggest_token="$cur"; _archaic_last_suggest_result=""; return; }

    local norm_path="${full_path%/}"
    if [[ "$norm_path" == "$resolved"* ]]; then
        _archaic_suggestion="${norm_path#$resolved}"
        _archaic_suggestion_full="$full_path"
    else
        _archaic_suggestion=""
    fi
    _archaic_last_suggest_token="$cur"
    _archaic_last_suggest_result="$_archaic_suggestion"
}

_archaic_accept_suggestion() {
    if [[ -n "$_archaic_suggestion" ]]; then
        LBUFFER="${LBUFFER}${_archaic_suggestion}"
        _archaic_suggestion=""
        zle reset-prompt
    fi
}

_archaic_cycle_next() {
    if [[ ${#_archaic_cycle_matches[@]} -eq 0 ]]; then
        _archaic_fetch_completions
    fi
    if [[ ${#_archaic_cycle_matches[@]} -eq 0 ]]; then
        return
    fi
    _archaic_cycle_index=$(( (_archaic_cycle_index + 1) % ${#_archaic_cycle_matches[@]} ))
    _archaic_suggestion="${_archaic_cycle_matches[$_archaic_cycle_index]}"
    zle reset-prompt
}

_archaic_cycle_prev() {
    if [[ ${#_archaic_cycle_matches[@]} -eq 0 ]]; then
        return
    fi
    _archaic_cycle_index=$(( (_archaic_cycle_index - 1 + ${#_archaic_cycle_matches[@]}) % ${#_archaic_cycle_matches[@]} ))
    _archaic_suggestion="${_archaic_cycle_matches[$_archaic_cycle_index]}"
    zle reset-prompt
}

_archaic_fetch_completions() {
    local cur="${LBUFFER##* }"
    [[ -z "$cur" || "$cur" != */* ]] && return
    _archaic_check_daemon || return

    local expanded_cur=$(_archaic_expand_path "$cur")
    local resolved="$expanded_cur"
    [[ "$expanded_cur" != /* ]] && resolved="$(pwd)/$expanded_cur"
    resolved="${resolved%/}"

    local cmd="$(_archaic_active_cmd_from_line "$LBUFFER")"
    [[ -z "$cmd" ]] && cmd="${LBUFFER%% *}"
    local results=""
    if [[ -n "$_archaic_helper_pid" && -n "$_archaic_helper" ]]; then
        results="$(_archaic_q "complete $resolved 20 $PWD $cmd")"
    fi
    if [[ -z "$results" ]]; then
        results="$(_archaic_c complete "$resolved" 20)" || return
    fi

    _archaic_cycle_matches=()
    _archaic_cycle_index=0
    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        local full_path="${line#* }"
        _archaic_cycle_matches+=("$(basename "$full_path")")
    done <<< "$results"
}

_archaic_reset_cycle() {
    _archaic_cycle_matches=()
    _archaic_cycle_index=0
    _archaic_suggestion=""
}

# Register zle widgets
zle -N _archaic_accept_suggestion
zle -N _archaic_cycle_next
zle -N _archaic_cycle_prev

# Bind keys: Ctrl+Space = accept (works in every terminal),
# Alt+Right = accept (where passed through),
# Alt+Down = cycle next, Alt+Up = cycle prev.
# (Ctrl+Left/Right are left alone for word jumping.)
bindkey '^ ' _archaic_accept_suggestion 2>/dev/null
bindkey '^[^[OC' _archaic_accept_suggestion 2>/dev/null
bindkey '^[[1;3C' _archaic_accept_suggestion 2>/dev/null
bindkey '^[^[[C' _archaic_accept_suggestion 2>/dev/null
bindkey '^[[1;3B' _archaic_cycle_next 2>/dev/null
bindkey '^[[1;3A' _archaic_cycle_prev 2>/dev/null

# Hook into precmd to update suggestion before each prompt
_archaic_orig_precmd_functions=(${precmd_functions[@]})
_archaic_precmd_hook() {
    _archaic_get_suggestion
}
precmd_functions=(_archaic_precmd_hook ${precmd_functions[@]})

# Append suggestion to RPS1 (right prompt)
_archaic_orig_rps1="${RPS1:-${RPROMPT:-}}"
_archaic_rps1() {
    if [[ -n "$_archaic_suggestion" ]]; then
        print -n "%F{244}${_archaic_suggestion}%f"
    fi
    if [[ -n "$_archaic_orig_rps1" ]]; then
        print -n "$_archaic_orig_rps1"
    fi
}
RPS1='$(_archaic_rps1)'

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
    echo "Accept hint: Ctrl+Space"
}
