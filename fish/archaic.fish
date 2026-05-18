# archaic.fish - Fish shell integration for archaic autocomplete daemon
#
# Usage:
#   1. Start daemon: ./run.sh start /path/to/scan
#   2. Install plugin: ./run.sh install-fish
#   3. Restart fish or run: source ~/.config/fish/conf.d/archaic.fish

# ── Global state ──────────────────────────────────────────────────────────────
set -g archaic_sock_path /tmp/archaic-daemon.sock
set -g __archaic_daemon_healthy 1
set -g __archaic_version_checked 0
set -g __archaic_helper_pid ""
set -g __archaic_last_query_time 0
set -g __archaic_debounce_ms 80
set -g __archaic_suggestion ""
set -g __archaic_max_completions 20
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
set -l repo_root (dirname (dirname "$plugin_path"))
set -g archaic_cli_path "$repo_root/build/archaic-cli"
set -g archaic_helper_path "$repo_root/build/archaic-helper"

# ── Completion pager colors ────────────────────────────────────────────────────
# Archaic does NOT override Fish's pager colors. Fish uses its defaults:
# blue/bold for directories, default color for files, etc. If you want to
# customize, set these in your own Fish config:
#   set -g fish_pager_color_completion blue
#   set -g fish_pager_color_description brblack
#   set -g fish_pager_color_prefix --bold --underline

# ── Default command list ──────────────────────────────────────────────────────
set -g __archaic_commands cd ls cat vim nvim less bat rm mv cp mkdir touch

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
        set -g __archaic_max_completions (math "max(1, min(100, $cfg_max))")
    end

    # Extract show_preview from [fish] section
    set -l cfg_preview (grep -A10 '^\[fish\]' "$config_file" 2>/dev/null | grep 'show_preview' | string replace -r '.*=\s*(true|false)' '$1')
    if test "$cfg_preview" = "true"
        set -g __archaic_show_preview 1
    end
end

# ── Helper lifecycle ──────────────────────────────────────────────────────────
function __archaic_ensure_helper -d "Start archaic-helper if not running"
    # Already running?
    if test -n "$__archaic_helper_pid"
        if kill -0 $__archaic_helper_pid 2>/dev/null
            return
        end
        # PID stale, clear
        set -g __archaic_helper_pid ""
    end

    # Helper binary must exist
    if not test -x "$archaic_helper_path"
        return
    end

    # Start helper with retry (3 attempts, exponential backoff)
    set -l max_retries 3
    set -l retry_delay 0.1
    for i in (seq 1 $max_retries)
        $archaic_helper_path "$archaic_sock_path" </dev/null >/dev/null 2>&1 &
        set -g __archaic_helper_pid $last_pid
        sleep $retry_delay
        if kill -0 $__archaic_helper_pid 2>/dev/null
            return
        end
        set retry_delay (math "$retry_delay * 2")
        echo "archaic: helper start attempt $i failed, retrying..." >&2
    end
    echo "archaic: helper failed to start after $max_retries attempts" >&2
    set -g __archaic_helper_pid ""
end

# ── Daemon health check ───────────────────────────────────────────────────────
function __archaic_check_daemon -d "Check if daemon socket exists and responds"
    if test "$__archaic_daemon_healthy" -eq 0
        return 1
    end
    if test -S "$archaic_sock_path"
        return 0
    end
    # Daemon not running - show notification once
    if not set -q __archaic_notified_down
        echo "archaic: daemon not running (completions unavailable). Start with: ./run.sh start" >&2
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
        set -l ping_result (command $archaic_cli_path ping 2>/dev/null)
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
    set -l ping_output (command $archaic_cli_path ping 2>/dev/null)
    if test $status -ne 0
        # Daemon might be unreachable - try cleanup
        __archaic_cleanup_socket
        return
    end

    set -g __archaic_version_checked 1
end

# ── Core completion function ──────────────────────────────────────────────────
function __archaic_do_complete -d "Query archaic daemon for completions"
    # Debounce: skip if last query was too recent
    set -l now (date +%s%3N 2>/dev/null; or date +%s)
    if test -n "$__archaic_last_query_time"
        set -l elapsed (math "$now - $__archaic_last_query_time")
        if test $elapsed -lt $__archaic_debounce_ms 2>/dev/null
            return
        end
    end
    set -g __archaic_last_query_time "$now"

    set -l prefix (commandline -ct)

    # Detect command being completed
    set -l cmd_tokens (commandline -co)
    set -l cmd ""
    if test (count $cmd_tokens) -gt 0
        set cmd $cmd_tokens[1]
    end

    # Commands that only accept directories
    set -l dir_only_cmds cd mkdir pushd popd rmdir
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

    # Resolve prefix to absolute path
    set -l resolved ""
    set -l norm_prefix ""
    if test -z "$prefix"
        set resolved (pwd)
        set norm_prefix ""
    else
        # Only complete path-like inputs or simple directory names
        if not string match -q '*/*' -- "$prefix"
            if not string match -qr '^[a-zA-Z0-9._-]+$' -- "$prefix"
                return
            end
        end

        set resolved "$prefix"
        if not string match -q '/*' -- "$prefix"
            set -l clean_prefix (string replace -r '^\./' '' "$prefix")
            set resolved (pwd)/"$clean_prefix"
        end
        # Normalize: resolve //, /./, and /../
        while string match -q '*/../*' -- "$resolved"
            set resolved (string replace -r '/[^/]+/\.\./' '/' "$resolved")
        end
        set resolved (string replace -r '/\.$' '/' "$resolved")
        set resolved (string replace -r '//+' '/' "$resolved")
        set resolved (string replace -r '/\./' '/' "$resolved")
        set norm_prefix (string replace -r '/+$' '' "$prefix")
    end

    set -l norm_resolved (string replace -r '/+$' '' "$resolved")

    # Query: try helper first (persistent), fall back to CLI
    set -l results ""
    if test -n "$__archaic_helper_pid" -a -d "/proc/$__archaic_helper_pid"
        set results (echo "complete $resolved $__archaic_max_completions $PWD" | $archaic_helper_path "$archaic_sock_path" 2>/dev/null)
    end

    if test -z "$results"
        # Helper not available, use CLI (forks but always works)
        set results (command $archaic_cli_path complete "$resolved" $__archaic_max_completions 2>/dev/null)
    end

    # Parse output: "D /path" or "F /path" (one per line)
    set -l found 0
    for line in $results
        set -l parts (string split " " "$line")
        if test (count $parts) -ge 2
            set -l type $parts[1]
            set -l full_path $parts[2]

            # Skip files for directory-only commands
            if test "$dirs_only" -eq 1 -a "$type" != "D"
                continue
            end

            # Convert to relative/basename for display
            set -l display_path "$full_path"
            if test -z "$prefix"
                set display_path (basename "$full_path")
            else if not string match -q '/*' -- "$prefix"
                set display_path (string replace "$norm_resolved" "" "$full_path")
                set display_path "$norm_prefix$display_path"
            end

            # Completion output: plain text only, no ANSI codes.
            # Fish wraps descriptions in () automatically, so we use bare words
            # to avoid double-parentheses like ((dir)).
            if test "$type" = "D"
                echo "$display_path"\tdirectory
            else
                echo "$display_path"\tfile
            end
            set found 1
        end
    end

    if test $found -eq 0
        # Try fuzzy matching
        set -l fuzzy_results ""
        if test -n "$__archaic_helper_pid" -a -d "/proc/$__archaic_helper_pid"
            set fuzzy_results (echo "fuzzy $resolved $__archaic_max_completions" | $archaic_helper_path "$archaic_sock_path" 2>/dev/null)
        end
        if test -z "$fuzzy_results"
            set fuzzy_results (command $archaic_cli_path fuzzy "$resolved" $__archaic_max_completions 2>/dev/null)
        end

        for line in $fuzzy_results
            set -l parts (string split " " "$line")
            if test (count $parts) -ge 2
                set -l type $parts[1]
                set -l full_path $parts[2]

                if test "$dirs_only" -eq 1 -a "$type" != "D"
                    continue
                end

                set -l display_path "$full_path"
                if test -z "$prefix"
                    set display_path (basename "$full_path")
                else if not string match -q '/*' -- "$prefix"
                    set -l parent_dir (dirname "$norm_resolved")
                    set display_path (string replace "$parent_dir/" "" "$full_path")
                end

                # Plain text fuzzy completions (no ANSI codes)
                if test "$type" = "D"
                    echo "$display_path"\tdirectory
                else
                    echo "$display_path"\tfile
                end
                set found 1
            end
        end
    end

    if test $found -eq 0
        set -g __archaic_daemon_healthy 0
        return
    end
    set -g __archaic_daemon_healthy 1
end

# ── Register completions for configured commands ────────────────────────────────
for cmd in $__archaic_commands
    complete -e -c $cmd
    complete -c $cmd -f -a "(__archaic_do_complete)"
end

# Catch-all for path-like arguments
complete -c "" -f -a "(__archaic_do_complete)"

# ── Inline autosuggestion via fish_right_prompt ────────────────────────────────
set -g __archaic_suggestion ""

function __archaic_get_suggestion -d "Get suggestion from archaic daemon"
    set -l prefix (commandline -t)
    if test -z "$prefix"
        set -g __archaic_suggestion ""
        return
    end

    # Only suggest for path-like inputs
    if not string match -q '*/*' -- "$prefix"
        set -g __archaic_suggestion ""
        return
    end

    if not __archaic_check_daemon
        set -g __archaic_suggestion ""
        return
    end

    # Resolve relative paths
    set -l resolved "$prefix"
    if not string match -q '/*' -- "$prefix"
        set resolved (pwd)/"$prefix"
    end
    set -l norm_resolved (string replace -r '/+$' '' "$resolved")

    # Query: try helper first, fall back to CLI
    set -l output ""
    if test -n "$__archaic_helper_pid" -a -d "/proc/$__archaic_helper_pid"
        set output (echo "complete $resolved 1 $PWD" | $archaic_helper_path "$archaic_sock_path" 2>/dev/null)
    end
    if test -z "$output"
        set output (command $archaic_cli_path complete "$resolved" 1 2>/dev/null)
    end

    if test $status -ne 0 -o -z "$output"
        set -g __archaic_suggestion ""
        return
    end

    # Parse: "D /path" or "F /path"
    set -l parts (string split " " "$output")
    if test (count $parts) -lt 2
        set -g __archaic_suggestion ""
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
if functions -q fish_right_prompt
    if not functions -q __archaic_orig_right_prompt
        functions --copy fish_right_prompt __archaic_orig_right_prompt
    end
    function fish_right_prompt
        __archaic_orig_right_prompt
        __archaic_right_prompt
    end
else
    function fish_right_prompt
        __archaic_right_prompt
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

    set -l results ""
    if test -n "$__archaic_helper_pid" -a -d "/proc/$__archaic_helper_pid"
        set results (echo "complete $resolved $__archaic_max_completions $PWD" | $archaic_helper_path "$archaic_sock_path" 2>/dev/null)
    end
    if test -z "$results"
        set results (command $archaic_cli_path complete "$resolved" $__archaic_max_completions 2>/dev/null)
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
    if test (count $__archaic_cycle_completions) -eq 0
        return
    end

    set -g __archaic_cycle_index (math "($__archaic_cycle_index + 1) % (count $__archaic_cycle_completions)")
    set -l idx (math "$__archaic_cycle_index + 1")
    set -l completion $__archaic_cycle_completions[$idx]
    set -g __archaic_suggestion (basename "$completion")
    commandline -f repaint
end

function __archaic_cycle_prev -d "Show previous completion in cycle"
    if test (count $__archaic_cycle_completions) -eq 0
        return
    end

    set -g __archaic_cycle_index (math "($__archaic_cycle_index - 1 + count $__archaic_cycle_completions) % (count $__archaic_cycle_completions)")
    set -l idx (math "$__archaic_cycle_index + 1")
    set -l completion $__archaic_cycle_completions[$idx]
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
        commandline -t (commandline -t)"$__archaic_suggestion"
        set -g __archaic_suggestion ""
        __archaic_reset_cycle
        commandline -f repaint
    end
end

# Alt+Right (works immediately)
bind \e\[1\;3C __archaic_accept_suggestion

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
    # Cycle bindings survive Fish defaults
    bind --mode insert \e\[1\;3B __archaic_cycle_next
    bind --mode insert \e\[1\;3A __archaic_cycle_prev
    bind --mode insert \e\[1\;4C __archaic_accept_and_continue
end

# Register to run after Fish's default key bindings are loaded
if functions -q fish_user_key_bindings
    # User already has fish_user_key_bindings - wrap it
    functions --copy fish_user_key_bindings __archaic_orig_user_key_bindings
    function fish_user_key_bindings
        __archaic_orig_user_key_bindings
        __archaic_user_key_bindings
    end
else
    function fish_user_key_bindings
        __archaic_user_key_bindings
    end
end

# ── Debug/status function ─────────────────────────────────────────────────────
function __archaic_status -d "Show archaic daemon status"
    if test -S "$archaic_sock_path"
        set -l ping_output (command $archaic_cli_path ping 2>/dev/null)
        if test $status -eq 0
            echo "Archaic daemon: running ($ping_output)"
        else
            echo "Archaic daemon: socket exists but unresponsive"
        end
    else
        echo "Archaic daemon: not running"
    end
    echo "CLI: $archaic_cli_path"
    echo "Helper: $archaic_helper_path"
    if test -n "$__archaic_helper_pid" -a -d "/proc/$__archaic_helper_pid"
        echo "Helper PID: $__archaic_helper_pid (running)"
    else
        echo "Helper PID: (not running)"
    end
    echo "Socket: $archaic_sock_path"
    echo "Commands: $__archaic_commands"
    echo "Version checked: $__archaic_version_checked"
end