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
        _archaic_helper_listen="$XDG_RUNTIME_DIR/archaic-helper.sock"
    else
        _archaic_sock="/tmp/archaic-$(id -u).sock"
        _archaic_helper_listen="/tmp/archaic-helper-$(id -u).sock"
    fi

    # Single source of truth: ask archaic-cli (C TOML parser) first, fall
    # back to shell grep only if the CLI is missing or fails.
    if [[ -n "${_archaic_cli:-}" ]]; then
        local cli_sock=""
        if command -v timeout &>/dev/null; then
            cli_sock="$(timeout 0.3 "$_archaic_cli" print-socket 2>/dev/null)"
        else
            cli_sock="$("$_archaic_cli" print-socket 2>/dev/null)"
        fi
        if [[ -n "$cli_sock" ]]; then
            _archaic_sock="$cli_sock"
            return
        fi
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
        # Honor [daemon] max_completions like fish (default 256).
        local cfg_max
        cfg_max="$(sed -n '/^\[daemon\]/,/^\[/p' "$config_file" | grep 'max_completions' | sed 's/.*= *\([0-9]*\).*/\1/' 2>/dev/null)"
        if [[ "$cfg_max" =~ ^[0-9]+$ ]]; then
            (( cfg_max < 1 )) && cfg_max=1
            (( cfg_max > 256 )) && cfg_max=256
            _archaic_max_completions="$cfg_max"
        fi
    fi
}

# Default cap; _archaic_resolve_paths may raise it from config (max 256).
_archaic_max_completions=256
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

_archaic_ensure_serve() {
    if [[ -S "${_archaic_helper_listen:-}" ]]; then
        # Serve writes "$listen.pid". A stale socket (SIGKILLed serve) must
        # not pin every Tab to a failing --ask + one-shot fallback.
        local _pidf="${_archaic_helper_listen}.pid" _pid=""
        if [[ -f "$_pidf" ]]; then
            _pid="$(cat "$_pidf" 2>/dev/null)"
        fi
        if [[ -n "$_pid" ]] && kill -0 "$_pid" 2>/dev/null; then
            return 0
        fi
        # Stale or orphaned socket: clear it so we can re-serve below.
        rm -f "$_archaic_helper_listen" "$_pidf" 2>/dev/null
    fi
    [[ -x "${_archaic_helper:-}" ]] || return 1
    "$_archaic_helper" "$_archaic_sock" --serve "$_archaic_helper_listen" >/dev/null 2>&1 &
    disown 2>/dev/null || true
    local i
    for i in 1 2 3 4 5 6 7 8; do
        [[ -S "$_archaic_helper_listen" ]] && return 0
        sleep 0.05
    done
    return 1
}

_archaic_q() {
    [[ -x "${_archaic_helper:-}" ]] || return 1
    if _archaic_ensure_serve; then
        printf '%s\n' "$1" | "$_archaic_helper" --ask "$_archaic_helper_listen" 2>/dev/null && return 0
    fi
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

# ── Cleanup on exit ──────────────────────────────────────────────────────────
# Persistent helper is a per-user singleton (shared socket); it outlives a
# single shell, so there is no per-shell PID to reap here.

# ── Daemon health check ──────────────────────────────────────────────────────
# Self-healing like fish: a live socket always clears the latched state so
# a restarted daemon works without opening a new shell.
_archaic_daemon_healthy=1

_archaic_check_daemon() {
    if [[ -S "$_archaic_sock" ]]; then
        _archaic_daemon_healthy=1
        return 0
    fi
    if [[ "$_archaic_daemon_healthy" -eq 0 ]]; then
        return 1
    fi
    _archaic_daemon_healthy=0
    return 1
}

# ── Active command detection (multi-command lines) ─────────────────────────
# `cd foo && vim <Tab>` completes for vim, not cd. Unwraps sudo/doas/env.
_archaic_is_sep() {
    case "$1" in
        ';'|'&'|'|'|'&&'|'||'|'('|'{') return 0 ;;
    esac
    return 1
}

# Assignment prefix (`VAR=1 vim …`): not a command, skip it.
_archaic_is_assign() {
    case "$1" in
        -*|*=*) [[ "$1" == -* ]] && return 1; return 0 ;;
    esac
    return 1
}
# Backward-compat aliases for the pre-fix typo (_arhcaic_*).
_arhcaic_is_sep() { _archaic_is_sep "$@"; }
_arhcaic_is_assign() { _archaic_is_assign "$@"; }
_archaic_active_cmd() {
    # $1 = index of current word (exclusive upper bound), defaults to COMP_CWORD
    local upto="${1:-$COMP_CWORD}"
    local active=""
    local i
    for (( i=0; i<upto; i++ )); do
        if _archaic_is_sep "${COMP_WORDS[i]:-}"; then
            continue
        fi
        if _archaic_is_assign "${COMP_WORDS[i]:-}"; then
            continue
        fi
        active="${COMP_WORDS[i]}"
        break
    done
    [[ -z "$active" ]] && active="${COMP_WORDS[0]:-}"
    for (( i=0; i<upto; i++ )); do
        if _archaic_is_sep "${COMP_WORDS[i]:-}"; then
            local j=$((i+1))
            while (( j < upto )); do
                if _archaic_is_sep "${COMP_WORDS[j]:-}"; then j=$((j+1)); continue; fi
                if _archaic_is_assign "${COMP_WORDS[j]:-}"; then j=$((j+1)); continue; fi
                case "${COMP_WORDS[j]:-}" in
                    '') j=$((j+1)); continue ;;
                    *) active="${COMP_WORDS[j]}"; break ;;
                esac
            done
        fi
    done
    # Unwrap wrappers: sudo/vim -> vim (skip flags after wrapper)
    case "$active" in
        sudo|doas|env|nohup|timeout|watch|xargs)
            for (( i=0; i<upto; i++ )); do
                if [[ "${COMP_WORDS[i]:-}" == "$active" ]]; then
                    local j=$((i+1))
                    while (( j < upto )); do
                        if [[ "${COMP_WORDS[j]:-}" == -* ]]; then j=$((j+1)); continue; fi
                        if _archaic_is_assign "${COMP_WORDS[j]:-}"; then j=$((j+1)); continue; fi
                        active="${COMP_WORDS[j]}"
                        break
                    done
                    break
                fi
            done
            ;;
    esac
    printf '%s' "$active"
}

_archaic_active_cmd_from_line() {
    # Parse a raw command line string (for ghost-text path where COMP_WORDS
    # is unavailable). Returns active command or "".
    local line="$1"
    local cur_pos="${2:-${#line}}"
    local before="${line:0:$cur_pos}"
    # Normalize separators to newlines, take last segment's first word
    local seg
    seg="$(printf '%s' "$before" | sed -e 's/&&/\n/g' -e 's/\.\?||/\n/g' -e 's/[;|(){}&]/\n/g' | tail -n 1)"
    seg="$(printf '%s' "$seg" | sed 's/^[[:space:]]*//')"
    # Skip leading VAR=assignments (`VAR=1 vim …` completes for vim)
    local first=""
    local w0
    for w0 in $seg; do
        case "$w0" in
            -*)
                first="$w0"; break ;;
            *=*)
                continue ;;
            *)
                first="$w0"; break ;;
        esac
    done
    # Unwrap wrappers + skip flags
    case "$first" in
        sudo|doas|env|nohup|timeout|watch|xargs)
            local rest="${seg#*[[:space:]]}"
            local w
            for w in $rest; do
                case "$w" in
                    -*) continue ;;
                    *=*) continue ;;
                    *) printf '%s' "$w"; return ;;
                esac
            done
            printf '%s' "$first"; return ;;
    esac
    printf '%s' "$first"
}

# ── Core completion function ─────────────────────────────────────────────────
# Does this command take path arguments unconditionally (empty token gets
# completions)? Other commands only complete path-like tokens.
_archaic_cmd_takes_paths() {
    case "$1" in
        cd|ls|ll|la|l|cat|vim|nvim|hx|nano|emacs|less|more|bat|rm|mv|cp|mkdir|rmdir|pushd|popd|touch|head|tail|chmod|chown|ln|tar|unzip|open|code|rg|fd|eza|grep|find|source|.)
            return 0 ;;
    esac
    return 1
}

_archaic_do_complete() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local prev="${COMP_WORDS[COMP_CWORD-1]}"
    local cmd
    cmd="$(_archaic_active_cmd "$COMP_CWORD")"
    [[ -z "$cmd" ]] && cmd="${COMP_WORDS[0]}"

    if [[ -z "$_archaic_cli" ]] || ! command -v "$_archaic_cli" &>/dev/null; then
        return
    fi

    if [[ "$cur" == -* ]]; then
        return
    fi

    if _archaic_cmd_takes_paths "$cmd"; then
        :
    else
        if [[ -z "$cur" ]]; then
            return
        fi
        if [[ "$cur" != .* && "$cur" != ~* && "$cur" != /* && "$cur" != */* ]]; then
            return
        fi
    fi

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

    # Persistent helper first (one process per user, daemon socket kept warm);
    # falls back to one-shot helper, then to archaic-cli.
    local results=""
    results="$(_archaic_q "$(printf 'complete\t%s\t%s\t%s\t%s' "$dirs_only" "$_archaic_max_completions" "$PWD" "$resolved")")"
    if [[ -z "$results" ]]; then
        results="$(_archaic_c complete "$resolved" "$_archaic_max_completions" "$PWD" "$dirs_only")" || return
    fi

    local found=0
    local -a completions=()
    local first_full=""

    if [[ -n "$expanded_cur" && "$expanded_cur" != */ && -d "$resolved" ]]; then
        completions+=("${expanded_cur%/}/")
        found=1
        first_full="$resolved"
    fi

    local hint=""
    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        if [[ "$line" == \#hint\ * ]]; then
            hint="${line#\#hint }"
            continue
        fi
        if [[ "$line" == \#scanning || "$line" == \#* ]]; then
            continue
        fi
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
        [[ -z "$first_full" ]] && first_full="$full_path"
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
        fuzzy_results="$(_archaic_q "fuzzy $fuzzy_q $_archaic_max_completions")"
        if [[ -z "$fuzzy_results" ]]; then
            fuzzy_results="$(_archaic_c fuzzy "$fuzzy_q" "$_archaic_max_completions")" || return
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
            [[ -z "$first_full" ]] && first_full="$full_path"
            found=1
        done <<< "$fuzzy_results"
    fi

    if [[ "$found" -eq 0 ]]; then
        case "$hint" in
            outside-roots) echo "archaic: outside scan roots — archaic-cli watch $PWD" >&2 ;;
            ignored) echo "archaic: ignored by index rules" >&2 ;;
            scanning) echo "archaic: still indexing…" >&2 ;;
            empty) echo "archaic: no matches — archaic-cli explain ${COMP_WORDS[COMP_CWORD]}" >&2 ;;
        esac
        return
    fi

    # Ghost render: show the top-ranked remainder dimmed (stock bash has no
    # inline overlay; the accept key inserts exactly this remainder).
    if [[ -n "$cur" && -n "$first_full" ]]; then
        _archaic_render_ghost "$first_full" "$resolved"
    fi

    COMPREPLY=("${completions[@]}")
}

# ── Default command list ─────────────────────────────────────────────────────
_archaic_commands=(cd ls ll la cat vim nvim hx nano emacs less more bat rm mv cp mkdir rmdir pushd popd touch head tail chmod chown ln tar unzip zip gzip diff open xdg-open code cursor rg fd eza exa lsd tree grep find file stat wc python python3 pytest node bun cargo go gcc g++ clang make cmake ninja scp rsync jq sudo docker kubectl npm pnpm yarn pip source .)

_archaic_load_commands() {
    # Prefer the C parser via archaic-cli; fall back to shell grep.
    if [[ -n "${_archaic_cli:-}" ]]; then
        local cli_cmds=""
        if command -v timeout &>/dev/null; then
            cli_cmds="$(timeout 0.3 "$_archaic_cli" print-commands bash 2>/dev/null)"
        else
            cli_cmds="$("$_archaic_cli" print-commands bash 2>/dev/null)"
        fi
        if [[ -n "$cli_cmds" ]]; then
            # shellcheck disable=SC2206
            _archaic_commands=($cli_cmds)
            return
        fi
    fi
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
    echo "Helper: ${_archaic_helper:-not found} (persistent via ${_archaic_helper_listen:-?})"
    if [[ -S "${_archaic_helper_listen:-}" ]]; then
        echo "Helper serve: running (${_archaic_helper_listen})"
    else
        echo "Helper serve: not running (one-shot fallback)"
    fi
    echo "Socket: $_archaic_sock"
    echo "Commands: ${_archaic_commands[*]}"
    echo "Accept hint: Ctrl+Space"
}

# ── Ghost-text suggestions ───────────────────────────────────────────────────
# Stock bash cannot overlay inline text, so the ghost lives in two places:
#   * Tab renders the pending remainder dimmed on stderr (see do_complete).
#   * Ctrl+Space / Alt+Right computes the remainder live and inserts it.
# Bare tokens (no slash) qualify only as arguments of path-taking commands.

_archaic_suggestion=""
_archaic_suggestion_full=""
_archaic_last_suggest_token=""
_archaic_last_suggest_result=""

_archaic_learn_accept() {
    [[ -x "${_archaic_helper:-}" ]] || return 0
    printf 'select %s\n' "$1" | _archaic_to 0.2 "$_archaic_helper" "$_archaic_sock" >/dev/null 2>&1 &
    disown 2>/dev/null || true
}

_archaic_get_suggestion() {
    # Opt-out: ARCHAIC_SUGGEST_ON_PROMPT=0 disables ghost queries
    # (Tab completion keeps working).
    [[ "${ARCHAIC_SUGGEST_ON_PROMPT:-}" == "0" ]] && { _archaic_suggestion=""; return; }
    # Extract the word currently being typed (text before cursor, last token)
    local before_cursor="${READLINE_LINE:0:$READLINE_POINT}"
    local cur="${before_cursor##* }"

    [[ -z "$cur" ]] && { _archaic_suggestion=""; return; }

    # Detect command for context-aware queries (chain-aware)
    local cmd
    cmd="$(_archaic_active_cmd_from_line "$READLINE_LINE" "$READLINE_POINT")"
    [[ -z "$cmd" ]] && cmd="${READLINE_LINE%% *}"

    if [[ "$cur" != */* ]]; then
        # Bare token: must look like a path fragment, sit in argument
        # position, and belong to a path-taking command.
        [[ "$cur" =~ ^[a-zA-Z0-9._~-]+$ ]] || { _archaic_suggestion=""; return; }
        [[ "$before_cursor" == *" "* ]] || { _archaic_suggestion=""; return; }
        _archaic_cmd_takes_paths "$cmd" || { _archaic_suggestion=""; return; }
    fi

    # Memoize: same token reuses the last result instead of re-querying.
    if [[ "$cur" == "$_archaic_last_suggest_token" ]]; then
        _archaic_suggestion="$_archaic_last_suggest_result"
        return
    fi

    _archaic_check_daemon || { _archaic_suggestion=""; _archaic_last_suggest_token="$cur"; _archaic_last_suggest_result=""; return; }

    # Expand environment variables
    local expanded_cur=$(_archaic_expand_path "$cur")

    # Resolve to absolute path
    local resolved="$expanded_cur"
    [[ "$expanded_cur" != /* ]] && resolved="$(pwd)/$expanded_cur"
    resolved="${resolved%/}"

    # Query daemon (persistent helper first, then CLI)
    local output=""
    output="$(_archaic_q "complete $resolved 1 $PWD $cmd")"
    if [[ -z "$output" ]]; then
        output="$(_archaic_c complete "$resolved" 1)" || return
    fi

    # Parse result: "D /path" or "F /path"
    local full_path="${output#* }"
    [[ -z "$full_path" || "$full_path" == "$output" ]] && { _archaic_suggestion=""; _archaic_last_suggest_token="$cur"; _archaic_last_suggest_result=""; return; }

    # Calculate the remainder (ghost text)
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
    # Compute live: READLINE_LINE is only meaningful inside this widget.
    _archaic_get_suggestion
    if [[ -n "$_archaic_suggestion" ]]; then
        READLINE_LINE="${READLINE_LINE}${_archaic_suggestion}"
        READLINE_POINT=${#READLINE_LINE}
        [[ -n "$_archaic_suggestion_full" ]] && _archaic_learn_accept "$_archaic_suggestion_full"
        _archaic_suggestion=""
        _archaic_suggestion_full=""
    fi
}

# Legacy no-op: ghost text used to hook PROMPT_COMMAND (which runs outside
# readline, so it could never see the live line). Kept so shells upgrading
# from that version don't error on their saved PROMPT_COMMAND.
_archaic_prompt_hook() {
    :
}
# Back-compat for the pre-append layout: if a previous version saved the
# original hook, keep running it once (avoids dropping user hooks that
# existed before upgrade).
if [[ -n "${_archaic_orig_prompt_command:-}" && "${_archaic_orig_prompt_command}" != *"_archaic_prompt_hook"* ]]; then
    case "${PROMPT_COMMAND:-}" in
        *"_archaic_orig_prompt_command"*) ;;
        *) PROMPT_COMMAND="${PROMPT_COMMAND:+${PROMPT_COMMAND}; }eval \"\$_archaic_orig_prompt_command\"" ;;
    esac
fi

# Render the ghost remainder dimmed on stderr. Called from Tab completion
# with the top-ranked result (same ordering the accept key inserts).
_archaic_render_ghost() {
    local full_path="$1" resolved="$2"
    [[ "${ARCHAIC_SUGGEST_ON_PROMPT:-}" == "0" ]] && return 0
    local norm_path="${full_path%/}"
    local norm_resolved="${resolved%/}"
    if [[ "$norm_path" == "$norm_resolved"* ]]; then
        local remainder="${norm_path#$norm_resolved}"
        if [[ -n "$remainder" ]]; then
            printf '\033[2m→ %s  (Ctrl+Space to accept)\033[0m\n' "$remainder" >&2
        fi
    fi
    return 0
}

# Accept the ghost-text suggestion with Ctrl+Space (reliable in every
# terminal, unbound by default). Alt+Right also works where the
# terminal passes it through. (Ctrl+Left/Right are left alone for
# word jumping.)
bind -x '"\C-@": _archaic_accept_suggestion' 2>/dev/null
bind -x '"\e[1;3C": _archaic_accept_suggestion' 2>/dev/null
bind -x '"\e\e[C": _archaic_accept_suggestion' 2>/dev/null
