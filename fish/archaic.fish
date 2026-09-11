# archaic.fish - Fish shell integration for archaic autocomplete daemon
#
# Usage:
#   1. Start daemon: ./run.sh start /path/to/scan
#   2. Install plugin: ./run.sh install-fish
#   3. Restart fish or run: source ~/.config/fish/conf.d/archaic.fish

# ── Global state ──────────────────────────────────────────────────────────────
if set -q XDG_RUNTIME_DIR; and test -n "$XDG_RUNTIME_DIR"
    set -g archaic_sock_path "$XDG_RUNTIME_DIR/archaic.sock"
else
    set -g archaic_sock_path /tmp/archaic-(id -u).sock
end
set -g __archaic_daemon_healthy 1
set -g __archaic_version_checked 0
set -g __archaic_query_timeout 0.4
set -g __archaic_ping_timeout 0.3
set -g __archaic_last_query_time 0
set -g __archaic_debounce_ms 80
set -g __archaic_suggestion ""
set -g __archaic_max_completions 256
set -g __archaic_show_preview 0
set -g __archaic_cycle_completions ""
set -g __archaic_cycle_index 0
set -g __archaic_cycle_prefix ""
set -g __archaic_cycle_active 0
set -g __archaic_cycle_full_paths ""
set -g __archaic_cycle_types ""

# ── Resolve binary paths ─────────────────────────────────────────────────────
set -l plugin_path (status filename)
if test -L "$plugin_path"
    set plugin_path (readlink -f "$plugin_path")
end
set -g __archaic_repo_root (dirname (dirname "$plugin_path"))
if command -sq archaic-cli
    set -g archaic_cli_path (command -s archaic-cli)
else if test -x "$HOME/.local/bin/archaic-cli"
    set -g archaic_cli_path "$HOME/.local/bin/archaic-cli"
else
    set -g archaic_cli_path "$__archaic_repo_root/build/archaic-cli"
end
if command -sq archaic-helper
    set -g archaic_helper_path (command -s archaic-helper)
else if test -x "$HOME/.local/bin/archaic-helper"
    set -g archaic_helper_path "$HOME/.local/bin/archaic-helper"
else
    set -g archaic_helper_path "$__archaic_repo_root/build/archaic-helper"
end

# Empty <Tab> lists files. Other commands only if the token looks like a path.
set -g __archaic_file_cmds cd ls ll la l cat vim nvim lvim hx helix kak micro nano emacs less more bat rm mv cp mkdir rmdir pushd popd touch head tail chmod chown chgrp ln tar unzip zip gzip bzip2 xz 7z diff patch open xdg-open code cursor codium zed subl rg ag ack fd eza exa lsd tree dust tokei ncdu grep find file stat wc du realpath readlink dirname basename scp sftp rsync rclone sshfs source .
set -g __archaic_exec_cmds python python3 pytest lua ruby perl php node bun deno rustc go gcc g++ clang clang++ make cmake ninja meson just jq yq sqlite3 hexdump xxd strings pandoc ffmpeg ffplay mpv vlc feh zathura convert curl wget aria2c pip uv sudo doas env nohup timeout watch xargs tee strace gdb lldb objdump readelf nm install strip man which type
set -g __archaic_commands $__archaic_file_cmds $__archaic_exec_cmds

# ── Load config (socket path + command list) ─────────────────────────────────
set -l config_file ""
for p in "$ARCHAIC_CONFIG" "$HOME/.config/archaic/config.toml" "/etc/archaic/config.toml"
    if test -n "$p" -a -f "$p"
        set config_file "$p"
        break
    end
end

if test -n "$config_file"
    # Extract socket path from [daemon] section
    set -l cfg_sock (grep -A10 '^\[daemon\]' "$config_file" 2>/dev/null | grep 'socket_path' | string replace -r '.*=\s*"([^"]*)"' '$1')
    if test -n "$cfg_sock"
        set -g archaic_sock_path "$cfg_sock"
    end

    # Extract command list from [fish] section
    set -l cfg_cmds (grep -A10 '^\[fish\]' "$config_file" 2>/dev/null | grep 'commands' | string replace -r '.*=\s*\[(.*)\]' '$1' | string replace -r '"' '' | string split ',')
    if test (count $cfg_cmds) -gt 0
        # Trim whitespace from each command
        set -l trimmed_cmds
        for c in $cfg_cmds
            set trimmed_cmds $trimmed_cmds (string trim "$c")
        end
        if test (count $trimmed_cmds) -gt 0
            set -g __archaic_commands $trimmed_cmds
        end
    end

    # Extract max_completions from [daemon] section
    set -l cfg_max (grep -A10 '^\[daemon\]' "$config_file" 2>/dev/null | grep 'max_completions' | string replace -r '.*=\s*([0-9]+)' '$1')
    if test -n "$cfg_max"
        set -g __archaic_max_completions (math "max(1, min(256, $cfg_max))")
    end

    # Extract show_preview from [fish] section
    set -l cfg_preview (grep -A10 '^\[fish\]' "$config_file" 2>/dev/null | grep 'show_preview' | string replace -r '.*=\s*(true|false)' '$1')
    if test "$cfg_preview" = "true"
        set -g __archaic_show_preview 1
    end
end

# ── Bounded daemon calls (Tab must never block) ───────────────────────────────
# One-shot helper per query: no fifos, no persistent process, no fifo-open
# deadlocks. Every call is capped so a wedged daemon degrades to Fish's
# builtin completion instead of freezing the terminal.
function __archaic_helper_query -a line -d "One-shot helper query with timeout"
    if not test -x "$archaic_helper_path"
        return 1
    end
    if command -sq timeout
        printf '%s\n' "$line" | timeout $__archaic_query_timeout "$archaic_helper_path" "$archaic_sock_path" 2>/dev/null
    else
        printf '%s\n' "$line" | "$archaic_helper_path" "$archaic_sock_path" 2>/dev/null
    end
end

function __archaic_cli_query -d "Bounded archaic-cli call"
    if command -sq timeout
        timeout $__archaic_query_timeout command $archaic_cli_path $argv 2>/dev/null
    else
        command $archaic_cli_path $argv 2>/dev/null
    end
end

function __archaic_ping -d "Bounded daemon ping"
    if command -sq timeout
        timeout $__archaic_ping_timeout command $archaic_cli_path ping 2>/dev/null
    else
        command $archaic_cli_path ping 2>/dev/null
    end
end

function __archaic_learn_path -a p -d "Record accepted path without blocking"
    if not test -x "$archaic_helper_path"
        return
    end
    if command -sq timeout
        printf 'select %s\n' "$p" | timeout 0.2 "$archaic_helper_path" "$archaic_sock_path" >/dev/null 2>&1 &
    else
        printf 'select %s\n' "$p" | "$archaic_helper_path" "$archaic_sock_path" >/dev/null 2>&1 &
    end
    disown 2>/dev/null
end

# ── Daemon health check ───────────────────────────────────────────────────────
function __archaic_try_start_daemon -d "Start daemon in background if it is down"
    if test -S "$archaic_sock_path"
        return 0
    end
    if set -q __archaic_start_attempted
        return 1
    end
    set -g __archaic_start_attempted 1
    set -l bin ""
    if command -sq archaic
        set bin (command -s archaic)
    else if test -x "$HOME/.local/bin/archaic"
        set bin "$HOME/.local/bin/archaic"
    else
        set bin (dirname "$archaic_cli_path")/archaic
    end
    if not test -x "$bin"
        return 1
    end
    # No scan path: the daemon uses config roots (~/.config/archaic/roots
    # plus ~/src, ~/projects, …), never a whole-$HOME scan.
    $bin --daemon $archaic_sock_path >/dev/null 2>&1 &
    disown 2>/dev/null
    for i in 1 2 3 4 5 6 7 8
        if test -S "$archaic_sock_path"
            set -g __archaic_daemon_healthy 1
            return 0
        end
        sleep 0.05
    end
    return 1
end

function __archaic_check_daemon -d "Check if daemon socket exists and responds"
    if test -S "$archaic_sock_path"
        set -g __archaic_daemon_healthy 1
        return 0
    end
    if test "$__archaic_daemon_healthy" -eq 0
        return 1
    end
    if __archaic_try_start_daemon
        return 0
    end
    if not set -q __archaic_notified_down
        echo "archaic: daemon not running. Start with: systemctl --user start archaic  (or ./run.sh start)" >&2
        set -g __archaic_notified_down 1
    end
    set -g __archaic_daemon_healthy 0
    return 1
end

# ── Stale socket cleanup ──────────────────────────────────────────────────────
function __archaic_cleanup_socket -d "Remove stale socket file if daemon is not running"
    if test -e "$archaic_sock_path" -a ! -S "$archaic_sock_path"
        # File exists but is not a socket - stale, remove it
        rm -f "$archaic_sock_path" 2>/dev/null
        return
    end
    if test -S "$archaic_sock_path"
        # Socket exists - try ping to verify daemon is alive
        set -l ping_result (__archaic_ping)
        if test $status -ne 0
            # Daemon not responding - clean up stale socket
            rm -f "$archaic_sock_path" 2>/dev/null
            set -g __archaic_daemon_healthy 0
        end
    end
end

# ── Version check (first use only) ────────────────────────────────────────────
function __archaic_check_version -d "Verify CLI/helper version compatibility"
    if test "$__archaic_version_checked" -eq 1
        return
    end

    # Quick ping to verify daemon responds
    set -l ping_output (__archaic_ping)
    if test $status -ne 0
        # Daemon might be unreachable - try cleanup
        __archaic_cleanup_socket
        return
    end

    set -g __archaic_version_checked 1
end

function __archaic_expand_path -d "Expand ~ and \$VAR in a path prefix"
    set -l path $argv[1]
    if test -z "$path"
        echo ""
        return
    end
    if test "$path" = "~"
        echo "$HOME"
        return
    end
    if string match -q '~/*' -- "$path"
        echo "$HOME"(string replace -r '^~' '' "$path")
        return
    end
    if string match -qr '^\$\{[A-Za-z_][A-Za-z0-9_]*\}' -- "$path"
        set -l var (string replace -r '^\$\{([A-Za-z_][A-Za-z0-9_]*)\}.*' '$1' "$path")
        set -l val $$var
        echo (string replace -r '^\$\{[A-Za-z_][A-Za-z0-9_]*\}' "$val" "$path")
        return
    end
    if string match -qr '^\$[A-Za-z_][A-Za-z0-9_]*' -- "$path"
        set -l var (string replace -r '^\$([A-Za-z_][A-Za-z0-9_]*).*' '$1' "$path")
        set -l val $$var
        echo (string replace -r '^\$[A-Za-z_][A-Za-z0-9_]*' "$val" "$path")
        return
    end
    echo "$path"
end

function __archaic_parse_line -d "Split 'D /path with spaces' into type and path"
    set -l line $argv[1]
    set -l type (string split -m 1 -f 1 " " -- "$line")
    set -l rest (string split -m 1 -f 2 " " -- "$line")
    echo "$type"
    echo "$rest"
end

function __archaic_canon_path -d "Expand ~ so token rewriting can strip the resolved prefix"
    set -l p $argv[1]
    if test "$p" = "~"
        echo "$HOME"
        return
    end
    if string match -q '~/*' -- "$p"
        echo "$HOME"(string sub -s 2 -- "$p")
        return
    end
    echo "$p"
end

function __archaic_token_path -d "Turn an absolute result into the token the user should insert"
    set -l full (__archaic_canon_path $argv[1])
    set -l prefix $argv[2]
    set -l resolved $argv[3]
    set -l norm_prefix $argv[4]
    if test -z "$prefix"
        basename "$full"
        return
    end
    if string match -q '~*' -- "$prefix"
        if test "$full" = "$HOME"
            echo "~"
            return
        end
        if string match -q "$HOME/*" -- "$full"
            echo "~"(string replace -- "$HOME" "" "$full")
            return
        end
        echo "$full"
        return
    end
    if string match -q '/*' -- "$prefix"
        echo "$full"
        return
    end
    if test "$full" = "$resolved"
        echo "$norm_prefix"
        return
    end
    if string match -q "$resolved/*" -- "$full"
        echo "$norm_prefix"(string replace -- "$resolved" "" "$full")
        return
    end
    echo "$full"
end

# ── Active command detection (multi-command lines) ──────────────────────────
# `cd foo && vim <Tab>` should complete for vim, not cd. Returns the command
# after the last shell separator, unwrapping sudo/doas/env wrappers.
function __archaic_active_command -d "Active command in a possibly chained line"
    set -l tokens $argv
    if test (count $tokens) -eq 0
        echo ""
        return
    end
    # First non-separator, non-assignment token (handles `VAR=1 vim …`)
    set -l active ""
    for t in $tokens
        switch "$t"
            case ';' '&' '|' '&&' '||' '(' '{' 'if' 'while' 'for' 'begin' 'function' 'not' 'and' 'or'
                continue
            case '*=*'
                if not string match -q -- '-*' "$t"
                    continue
                end
                set active $t
                break
            case '*'
                set active $t
                break
        end
    end
    if test -z "$active"
        set active $tokens[1]
    end
    set -l expect_cmd 0
    for t in $tokens
        if test $expect_cmd -eq 1
            # Skip empty separator-adjacent tokens; first real token wins
            switch "$t"
                case ';' '&' '|' '&&' '||' '(' '{'
                    continue
                case '*=*'
                    if not string match -q -- '-*' "$t"
                        continue
                    end
                    set active $t
                    set expect_cmd 0
                case '*'
                    set active $t
                    set expect_cmd 0
            end
            continue
        end
        switch "$t"
            case ';' '&' '|' '&&' '||' '(' '{' 'if' 'while' 'for' 'begin' 'function' 'not' 'and' 'or'
                set expect_cmd 1
        end
    end
    # Unwrap privilege/env wrappers: `sudo vim <Tab>` completes for vim
    switch "$active"
        case sudo doas env nohup timeout watch xargs
            set -l idx 1
            for t in $tokens
                if test "$t" = "$active"
                    break
                end
                set idx (math $idx + 1)
            end
            # Find token right after the wrapper, skipping flags
            set -l j (math $idx + 1)
            while test $j -le (count $tokens)
                set -l cand $tokens[$j]
                switch "$cand"
                    case '-*'
                        set j (math $j + 1)
                        continue
                    case ';' '&' '|' '&&' '||' '(' '{' ''
                        break
                    case '*'
                        echo $cand
                        return
                end
                break
            end
    end
    echo $active
end

# ── Core completion function ──────────────────────────────────────────────────
function __archaic_do_complete -d "Query archaic daemon for completions"
    set -l typed (commandline -ct)
    set -l prefix $typed

    # Detect command being completed (chain-aware)
    set -l cmd_tokens (commandline -co)
    set -l cmd (__archaic_active_command $cmd_tokens)

    # Flags are not paths
    if string match -q -- '-*' "$prefix"
        return
    end

    # Commands that only accept directories
    set -l dir_only_cmds cd mkdir pushd popd rmdir chdir
    set -l dirs_only 0
    for dc in $dir_only_cmds
        if test "$cmd" = "$dc"
            set dirs_only 1
            break
        end
    end

    # Check daemon health
    if not __archaic_check_daemon
        return
    end

    # Version check on first use
    __archaic_check_version

    set prefix (__archaic_expand_path "$typed")
    set -l typed_norm $typed
    if test -n "$typed_norm"; and test "$typed_norm" != "/"
        set typed_norm (string replace -r '/+$' '' -- "$typed")
    end

    # Resolve prefix to absolute path
    set -l resolved ""
    set -l norm_prefix $typed_norm
    if test -z "$prefix"
        set resolved (pwd)
        set norm_prefix ""
    else
        # Only complete path-like inputs or simple directory names
        if not string match -q '*/*' -- "$prefix"
            if not string match -qr '^[a-zA-Z0-9._~-]+$' -- "$prefix"
                return
            end
        end

        set resolved "$prefix"
        if not string match -q '/*' -- "$prefix"
            set -l clean_prefix (string replace -r '^\./' '' "$prefix")
            set resolved (pwd)/"$clean_prefix"
        end
        # Normalize: resolve //, /./, and /../ (including trailing /..)
        while string match -q '*/../*' -- "$resolved"
            set resolved (string replace -r '/[^/]+/\.\./' '/' "$resolved")
        end
        # Handle trailing /.. (e.g., /home/sam/samdev/.. -> /home/sam)
        while string match -q '*/..' -- "$resolved"
            set resolved (string replace -r '/[^/]+/\.\.$' '/' "$resolved")
        end
        set resolved (string replace -r '/\.$' '/' "$resolved")
        set resolved (string replace -r '//+' '/' "$resolved")
        set resolved (string replace -r '/\./' '/' "$resolved")
        # Remove trailing slash for root
        if test "$resolved" = "/"
            # Keep as is
        else
            set resolved (string replace -r '/+$' '' "$resolved")
        end
    end

    set -l norm_resolved (string replace -r '/+$' '' "$resolved")

    if test -n "$typed"; and not string match -q '*/' -- "$typed"; and test -d "$resolved"
        echo "$typed/"\tdirectory
    end

    set -l req (printf 'complete\t%s\t%s\t%s\t%s' "$dirs_only" "$__archaic_max_completions" "$PWD" "$resolved")
    set -l results (__archaic_helper_query "$req")
    if test -z "$results"
        set results (__archaic_cli_query complete "$resolved" $__archaic_max_completions "$PWD" $dirs_only)
    end

    set -l scanning 0
    if test (count $results) -gt 0; and test "$results[1]" = "#scanning"
        set scanning 1
        set -e results[1]
    end

    set -l found 0
    for line in $results
        set -l type (string split -m 1 -f 1 " " -- "$line")
        set -l full_path (string split -m 1 -f 2 " " -- "$line")
        if test "$type" = "#scanning" -o "$type" = "#"
            set scanning 1
            continue
        end
        if test -z "$full_path"
            continue
        end

        if test "$dirs_only" -eq 1 -a "$type" != "D"
            continue
        end

        set -l display_path (__archaic_token_path "$full_path" "$typed" "$norm_resolved" "$norm_prefix")

        if test "$type" = "D"
            echo "$display_path"\tdirectory
        else
            echo "$display_path"\tfile
        end
        set found 1
    end

    if test $found -eq 0
        set -l fuzzy_q "$resolved"
        set -l fuzzy_token 0
        if not string match -q '*/*' -- "$prefix"; and test -n "$prefix"
            set fuzzy_q "$prefix"
            set fuzzy_token 1
        end
        set -l fuzzy_results (__archaic_helper_query "fuzzy $fuzzy_q $__archaic_max_completions")
        if test -z "$fuzzy_results"
            set fuzzy_results (__archaic_cli_query fuzzy "$fuzzy_q" $__archaic_max_completions)
        end

        for line in $fuzzy_results
            set -l type (string split -m 1 -f 1 " " -- "$line")
            set -l full_path (string split -m 1 -f 2 " " -- "$line")
            if test "$type" = "#scanning" -o "$type" = "#"
                set scanning 1
                continue
            end
            if test -z "$full_path"
                continue
            end

            if test "$fuzzy_token" -eq 0; and string match -q '/*' -- "$resolved"
                if not string match -q "$norm_resolved/*" -- "$full_path"
                    continue
                end
                set -l remainder (string replace "$norm_resolved/" "" "$full_path")
                if string match -q '*/*' -- "$remainder"
                    continue
                end
            end

            if test "$dirs_only" -eq 1 -a "$type" != "D"
                continue
            end

        set -l display_path (__archaic_token_path "$full_path" "$typed" "$norm_resolved" "$norm_prefix")
            if test "$fuzzy_token" -eq 1
                set display_path (string replace -- "$PWD/" "" (__archaic_canon_path "$full_path"))
            end

            if test "$type" = "D"
                echo "$display_path"\tdirectory
            else
                echo "$display_path"\tfile
            end
            set found 1
        end
    end

    if test $found -eq 0 -a $scanning -eq 1
        echo "indexing…"\tarchaic is still scanning
    end
end

function __archaic_is_path_token -d "Current token is not a flag"
    set -l tok (commandline -ct)
    if string match -q -- '-*' "$tok"
        return 1
    end
    return 0
end

function __archaic_complete_ok -d "File cmds allow empty token; others need a path-like token"
    set -l tok (commandline -ct)
    if string match -q -- '-*' "$tok"
        return 1
    end
    set -l toks (commandline -co)
    set -l cmd (__archaic_active_command $toks)
    if contains -- $cmd $__archaic_file_cmds
        return 0
    end
    __archaic_is_tool_path_token
end

function __archaic_is_tool_path_token -d "Token looks like a path (for git/docker/etc)"
    set -l tok (commandline -ct)
    if test -z "$tok"
        return 1
    end
    if string match -q -- '-*' "$tok"
        return 1
    end
    string match -qr -- '^(\.|~|/)|/' -- "$tok"
end

function __archaic_on_pwd --on-variable PWD -d "Learn from cd"
    if test -d "$PWD"
        __archaic_learn_path "$PWD"
    end
end

# ── Register completions for configured commands ────────────────────────────────
for cmd in $__archaic_commands
    complete -c $cmd -n '__archaic_complete_ok' -k -a "(__archaic_do_complete)"
end

complete -c git -n '__archaic_is_tool_path_token; and __fish_seen_subcommand_from add checkout switch restore diff rm mv show reset commit blame log grep apply am archive mergetool clean stash' -k -a "(__archaic_do_complete)"
complete -c docker -n '__archaic_is_tool_path_token; and __fish_seen_subcommand_from build cp run save load import export' -k -a "(__archaic_do_complete)"
complete -c podman -n '__archaic_is_tool_path_token; and __fish_seen_subcommand_from build cp run save load import export' -k -a "(__archaic_do_complete)"
complete -c kubectl -n '__archaic_is_tool_path_token; and __fish_seen_subcommand_from apply create delete replace diff kustomize' -k -a "(__archaic_do_complete)"
complete -c cargo -n '__archaic_is_tool_path_token; and __fish_seen_subcommand_from run build test bench install rustc' -k -a "(__archaic_do_complete)"
complete -c npm -n '__archaic_is_tool_path_token; and __fish_seen_subcommand_from run test exec pack publish' -k -a "(__archaic_do_complete)"
complete -c pnpm -n '__archaic_is_tool_path_token; and __fish_seen_subcommand_from run test exec' -k -a "(__archaic_do_complete)"
complete -c yarn -n '__archaic_is_tool_path_token; and __fish_seen_subcommand_from run test' -k -a "(__archaic_do_complete)"
complete -c terraform -n '__archaic_is_tool_path_token; and __fish_seen_subcommand_from apply plan destroy validate fmt' -k -a "(__archaic_do_complete)"
complete -c gh -n '__archaic_is_tool_path_token; and __fish_seen_subcommand_from repo gist pr issue' -k -a "(__archaic_do_complete)"

# ── Inline autosuggestion via fish_right_prompt ────────────────────────────────
set -g __archaic_suggestion ""
set -g __archaic_last_suggest_token ""
set -g __archaic_last_suggest_result ""

function __archaic_get_suggestion -d "Get suggestion from archaic daemon"
    # Opt-out: set ARCHAIC_SUGGEST_ON_PROMPT=0 to disable per-prompt queries
    # (Tab completion keeps working). Reduces daemon traffic on every prompt.
    if set -q ARCHAIC_SUGGEST_ON_PROMPT; and test "$ARCHAIC_SUGGEST_ON_PROMPT" = "0"
        set -g __archaic_suggestion ""
        return
    end
    set -l prefix (commandline -t)
    if test -z "$prefix"
        set -g __archaic_suggestion ""
        return
    end

    # Memoize: repeated prompt repaints with the same token reuse the last
    # result instead of hitting the daemon again.
    if test "$prefix" = "$__archaic_last_suggest_token"
        set -g __archaic_suggestion "$__archaic_last_suggest_result"
        return
    end

    # Only suggest for path-like inputs
    if not string match -q '*/*' -- "$prefix"
        set -g __archaic_suggestion ""
        return
    end

    if not __archaic_check_daemon
        set -g __archaic_suggestion ""
        set -g __archaic_last_suggest_token "$prefix"
        set -g __archaic_last_suggest_result ""
        return
    end

    # Resolve relative paths
    set -l resolved "$prefix"
    if not string match -q '/*' -- "$prefix"
        set resolved (pwd)/"$prefix"
    end
    set -l norm_resolved (string replace -r '/+$' '' "$resolved")

    # Query: try helper first, fall back to CLI
    set -l output (__archaic_helper_query "complete $resolved 1 $PWD")
    if test -z "$output"
        set output (__archaic_cli_query complete "$resolved" 1)
    end

    if test $status -ne 0 -o -z "$output"
        set -g __archaic_suggestion ""
        set -g __archaic_last_suggest_token "$prefix"
        set -g __archaic_last_suggest_result ""
        return
    end

    # Parse: "D /path" or "F /path"
    set -l parts (string split " " "$output")
    if test (count $parts) -lt 2
        set -g __archaic_suggestion ""
        set -g __archaic_last_suggest_token "$prefix"
        set -g __archaic_last_suggest_result ""
        return
    end
    set -l suggestion $parts[2]

    # Normalize suggestion for comparison
    set -l norm_suggestion (string replace -r '/+$' '' "$suggestion")

    # Check if suggestion starts with the resolved path
    if string match -q "$norm_resolved*" -- "$norm_suggestion"
        set -l remainder (string sub -s (math (string length "$norm_resolved") + 1) "$norm_suggestion")
        # Strip leading slash from remainder if user typed trailing slash
        if string match -q '*/' -- "$prefix"
            set remainder (string replace -r '^/' '' "$remainder")
        end
        if test -n "$remainder"
            set -g __archaic_suggestion "$remainder"
        else
            set -g __archaic_suggestion ""
        end
    else
        set -g __archaic_suggestion ""
    end
    set -g __archaic_last_suggest_token "$prefix"
    set -g __archaic_last_suggest_result "$__archaic_suggestion"
end

function __archaic_right_prompt -d "Show archaic autosuggestion"
    __archaic_get_suggestion
    if test -n "$__archaic_suggestion"
        set_color --italics --dim
        echo -n "$__archaic_suggestion"
        set_color normal
    end
    __archaic_get_preview
end

# Append to existing fish_right_prompt if it exists, otherwise define it
if not set -q __archaic_prompt_wrapped
    if functions -q __archaic_orig_right_prompt
        set -g __archaic_prompt_wrapped 1
    else if functions -q fish_right_prompt
        functions --copy fish_right_prompt __archaic_orig_right_prompt
        function fish_right_prompt
            __archaic_orig_right_prompt
            __archaic_right_prompt
        end
        set -g __archaic_prompt_wrapped 1
    else
        function fish_right_prompt
            __archaic_right_prompt
        end
        set -g __archaic_prompt_wrapped 1
    end
end

# ── Completion cycling ────────────────────────────────────────────────────────
function __archaic_fetch_completions -d "Fetch completions for cycling"
    set -l prefix (commandline -t)
    if test -z "$prefix"
        return
    end
    if not string match -q '*/*' -- "$prefix"
        return
    end
    if not __archaic_check_daemon
        return
    end

    set -l resolved "$prefix"
    if not string match -q '/*' -- "$prefix"
        set resolved (pwd)/"$prefix"
    end
    set resolved (string replace -r '/+$' '' "$resolved")

    set -l results (__archaic_helper_query "complete $resolved $__archaic_max_completions $PWD")
    if test -z "$results"
        set results (__archaic_cli_query complete "$resolved" $__archaic_max_completions)
    end

    if test -z "$results"
        return
    end

    set -g __archaic_cycle_completions ""
    set -g __archaic_cycle_full_paths ""
    set -g __archaic_cycle_types ""
    set -g __archaic_cycle_index 0
    set -g __archaic_cycle_prefix "$prefix"
    set -g __archaic_cycle_active 1

    for line in $results
        set -l parts (string split " " "$line")
        if test (count $parts) -ge 2
            set -g __archaic_cycle_completions $__archaic_cycle_completions "$parts[2]"
            set -g __archaic_cycle_full_paths $__archaic_cycle_full_paths "$parts[2]"
            set -g __archaic_cycle_types $__archaic_cycle_types "$parts[1]"
        end
    end
end

function __archaic_cycle_next -d "Show next completion in cycle"
    if test (count $__archaic_cycle_completions) -eq 0
        __archaic_fetch_completions
    end
    set -l n (count $__archaic_cycle_completions)
    if test $n -eq 0
        return
    end
    if not string match -qr '^-?\d+$' -- "$__archaic_cycle_index"
        set -g __archaic_cycle_index 0
    end

    set -g __archaic_cycle_index (math "($__archaic_cycle_index + 1) % $n")
    set -l idx (math "$__archaic_cycle_index + 1")
    set -l completion "$__archaic_cycle_completions[$idx]"
    set -g __archaic_suggestion (basename "$completion")
    commandline -f repaint
end

function __archaic_cycle_prev -d "Show previous completion in cycle"
    set -l n (count $__archaic_cycle_completions)
    if test $n -eq 0
        return
    end
    if not string match -qr '^-?\d+$' -- "$__archaic_cycle_index"
        set -g __archaic_cycle_index 0
    end

    set -g __archaic_cycle_index (math "($__archaic_cycle_index - 1 + $n) % $n")
    set -l idx (math "$__archaic_cycle_index + 1")
    set -l completion "$__archaic_cycle_completions[$idx]"
    set -g __archaic_suggestion (basename "$completion")
    commandline -f repaint
end

function __archaic_reset_cycle -d "Clear completion cycle state"
    set -g __archaic_cycle_completions ""
    set -g __archaic_cycle_index 0
    set -g __archaic_cycle_prefix ""
    set -g __archaic_cycle_active 0
    set -g __archaic_cycle_full_paths ""
    set -g __archaic_cycle_types ""
end

# ── Chain completions (accept and continue) ───────────────────────────────────
function __archaic_accept_and_continue -d "Accept suggestion and keep cursor ready for more"
    if test -n "$__archaic_suggestion"
        commandline -i "$__archaic_suggestion"
        set -g __archaic_suggestion ""
        __archaic_reset_cycle
        commandline -f end-of-line
        commandline -f repaint
    end
end

# ── Preview panel ─────────────────────────────────────────────────────────────
function __archaic_human_size -d "Convert bytes to human-readable size"
    set -l bytes $argv[1]
    if test "$bytes" -ge 1073741824
        echo (math "scale=1; $bytes / 1073741824")G
    else if test "$bytes" -ge 1048576
        echo (math "scale=1; $bytes / 1048576")M
    else if test "$bytes" -ge 1024
        echo (math "scale=1; $bytes / 1024")K
    else
        echo "$bytes"B
    end
end

function __archaic_get_preview -d "Get file preview for current completion"
    if test "$__archaic_show_preview" -eq 0
        return
    end
    if test "$__archaic_cycle_active" -eq 0
        return
    end
    if test (count $__archaic_cycle_full_paths) -eq 0
        return
    end

    set -l idx (math "$__archaic_cycle_index + 1")
    set -l full_path $__archaic_cycle_full_paths[$idx]
    set -l entry_type $__archaic_cycle_types[$idx]

    if test ! -e "$full_path"
        if test "$entry_type" = "D"
            set_color --dim yellow
            echo -n "[dir]"
        else
            set_color --dim yellow
            echo -n "[file]"
        end
        set_color normal
        return
    end

    set -l size ""
    set -l mtime ""
    if command -v stat >/dev/null 2>&1
        set -l stat_out (stat -c "%s %Y" "$full_path" 2>/dev/null)
        if test $status -eq 0
            set size (echo "$stat_out" | cut -d' ' -f1)
            set mtime (echo "$stat_out" | cut -d' ' -f2)
        else
            set -l stat_out (stat -f "%z %m" "$full_path" 2>/dev/null)
            if test $status -eq 0
                set size (echo "$stat_out" | cut -d' ' -f1)
                set mtime (echo "$stat_out" | cut -d' ' -f2)
            end
        end
    end

    set -l human_size ""
    if test -n "$size"
        set human_size (__archaic_human_size "$size")
    end

    set -l time_ago ""
    if test -n "$mtime"
        set -l now (date +%s)
        set -l diff (math "$now - $mtime")
        if test "$diff" -lt 60
            set time_ago "$diff"s" ago"
        else if test "$diff" -lt 3600
            set time_ago (math "$diff / 60")"m ago"
        else if test "$diff" -lt 86400
            set time_ago (math "$diff / 3600")"h ago"
        else
            set time_ago (math "$diff / 86400")"d ago"
        end
    end

    set_color --dim
    if test "$entry_type" = "D"
        set_color --dim blue
        echo -n "[dir"
    else
        echo -n "[file"
    end
    if test -n "$human_size"
        echo -n "  $human_size"
    end
    if test -n "$time_ago"
        echo -n "  $time_ago"
    end
    echo -n "]"
    set_color normal
end
function __archaic_accept_suggestion
    if test -n "$__archaic_suggestion"
        set -l tok (commandline -t)"$__archaic_suggestion"
        commandline -t "$tok"
        set -l p $tok
        if not string match -q '/*' -- "$p"
            set p "$PWD/$p"
        end
        __archaic_learn_path "$p"
        set -g __archaic_suggestion ""
        __archaic_reset_cycle
        commandline -f repaint
    else
        # No archaic remainder (e.g. the grey hint came from fish's own
        # history autosuggestion, or the path is already complete):
        # fall through to fish's native accept instead of silently
        # doing nothing.
        commandline -f accept-autosuggestion
    end
end

# ── Accept the ghost-text suggestion ─────────────────────────────────────────
# Ctrl+Space is the accept key: easy to reach, unbound by default, and
# sent reliably as NUL by virtually every terminal. (Alt+Right also
# works where the terminal passes it through; Alt+Shift+Right accepts
# and keeps completing (chain), Ctrl+R too.)
# (`ctrl-space` on fish 4+, `-k nul` on fish 3.x)
bind ctrl-space __archaic_accept_suggestion 2>/dev/null
bind -k nul __archaic_accept_suggestion 2>/dev/null

# Alt+Right (works where the terminal passes it through)
bind \e\[1\;3C __archaic_accept_suggestion
bind \e\e\[C __archaic_accept_suggestion

# Alt+Down: cycle next completion
bind \e\[1\;3B __archaic_cycle_next

# Alt+Up: cycle previous completion
bind \e\[1\;3A __archaic_cycle_prev

# Alt+Shift+Right: accept and continue (chain completions)
bind \e\[1\;4C __archaic_accept_and_continue

# Ctrl+Shift+Space: accept and continue (alternative)
bind \e\[27\;6\;32~ __archaic_accept_and_continue

# Ctrl+R: must use fish_user_key_bindings to override Fish's default history search
# (conf.d scripts load before default key bindings, so direct bind gets overridden)
function __archaic_user_key_bindings
    bind --mode insert \cr __archaic_accept_suggestion
    bind --mode default \cr __archaic_accept_suggestion
    bind --mode insert ctrl-space __archaic_accept_suggestion 2>/dev/null
    bind --mode default ctrl-space __archaic_accept_suggestion 2>/dev/null
    bind --mode insert -k nul __archaic_accept_suggestion 2>/dev/null
    bind --mode default -k nul __archaic_accept_suggestion 2>/dev/null
    # Cycle bindings survive Fish defaults
    bind --mode insert \e\[1\;3B __archaic_cycle_next
    bind --mode insert \e\[1\;3A __archaic_cycle_prev
    bind --mode insert \e\[1\;4C __archaic_accept_and_continue
end

# Register to run after Fish's default key bindings are loaded
if not set -q __archaic_bindings_wrapped
    if functions -q __archaic_orig_user_key_bindings
        set -g __archaic_bindings_wrapped 1
    else if functions -q fish_user_key_bindings
        functions --copy fish_user_key_bindings __archaic_orig_user_key_bindings
        function fish_user_key_bindings
            __archaic_orig_user_key_bindings
            __archaic_user_key_bindings
        end
        set -g __archaic_bindings_wrapped 1
    else
        function fish_user_key_bindings
            __archaic_user_key_bindings
        end
        set -g __archaic_bindings_wrapped 1
    end
end

# ── Debug/status function ─────────────────────────────────────────────────────
function __archaic_status -d "Show archaic daemon status"
    if test -S "$archaic_sock_path"
        set -l ping_output (__archaic_ping)
        if test $status -eq 0
            echo "Archaic daemon: running ($ping_output)"
        else
            echo "Archaic daemon: socket exists but unresponsive"
        end
    else
        echo "Archaic daemon: not running"
    end
    echo "CLI: $archaic_cli_path"
    echo "Helper: $archaic_helper_path (one-shot per Tab)"
    echo "Socket: $archaic_sock_path"
    echo "Commands: $__archaic_commands"
    echo "Accept hint: Ctrl+Space (Alt+Down/Up to cycle)"
    echo "Version checked: $__archaic_version_checked"
end