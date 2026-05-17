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

# ── Resolve binary paths ─────────────────────────────────────────────────────
set -l plugin_path (status filename)
if test -L "$plugin_path"
    set plugin_path (readlink -f "$plugin_path")
end
set -l repo_root (dirname (dirname "$plugin_path"))
set -g archaic_cli_path "$repo_root/build/archaic-cli"
set -g archaic_helper_path "$repo_root/build/archaic-helper"

# ── Default command list ─────────────────────────────────────────────────────
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
end

# ── Helper lifecycle ─────────────────────────────────────────────────────────
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

    # Start helper as background process with socket path
    $archaic_helper_path "$archaic_sock_path" </dev/null >/dev/null 2>&1 &
    set -g __archaic_helper_pid $last_pid
end

# ── Daemon health check ──────────────────────────────────────────────────────
function __archaic_check_daemon -d "Check if daemon socket exists"
    if test "$__archaic_daemon_healthy" -eq 0
        return 1
    end
    if test -S "$archaic_sock_path"
        return 0
    end
    set -g __archaic_daemon_healthy 0
    return 1
end

# ── Version check (first use only) ───────────────────────────────────────────
function __archaic_check_version -d "Verify CLI/helper version compatibility"
    if test "$__archaic_version_checked" -eq 1
        return
    end

    # Quick ping to verify daemon responds
    set -l ping_output (command $archaic_cli_path ping 2>/dev/null)
    if test $status -ne 0
        return
    end

    set -g __archaic_version_checked 1
end

# ── Core completion function ─────────────────────────────────────────────────
function __archaic_do_complete -d "Query archaic daemon for completions"
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
        set results (echo "complete $resolved 20" | $archaic_helper_path "$archaic_sock_path" 2>/dev/null)
    end

    if test -z "$results"
        # Helper not available, use CLI (forks but always works)
        set results (command $archaic_cli_path complete "$resolved" 20 2>/dev/null)
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

            if test "$type" = "D"
                echo -e "$display_path\t(dir)"
            else
                echo -e "$display_path\t(file)"
            end
            set found 1
        end
    end

    if test $found -eq 0
        # Try fuzzy matching
        set -l fuzzy_results ""
        if test -n "$__archaic_helper_pid" -a -d "/proc/$__archaic_helper_pid"
            set fuzzy_results (echo "fuzzy $resolved 20" | $archaic_helper_path "$archaic_sock_path" 2>/dev/null)
        end
        if test -z "$fuzzy_results"
            set fuzzy_results (command $archaic_cli_path fuzzy "$resolved" 20 2>/dev/null)
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

                if test "$type" = "D"
                    echo -e "$display_path\t(dir fuzzy)"
                else
                    echo -e "$display_path\t(file fuzzy)"
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

# ── Register completions for configured commands ─────────────────────────────
for cmd in $__archaic_commands
    complete -e -c $cmd
    complete -c $cmd -f -a "(__archaic_do_complete)"
end

# Catch-all for path-like arguments
complete -c "" -f -a "(__archaic_do_complete)"

# ── Inline autosuggestion via fish_right_prompt ──────────────────────────────
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
        set output (echo "complete $resolved 1" | $archaic_helper_path "$archaic_sock_path" 2>/dev/null)
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

# ── Key bindings to accept the suggestion ────────────────────────────────────
function __archaic_accept_suggestion
    if test -n "$__archaic_suggestion"
        commandline -t (commandline -t)"$__archaic_suggestion"
        set -g __archaic_suggestion ""
        commandline -f repaint
    end
end

# Alt+Right (works immediately)
bind \e\[1\;3C __archaic_accept_suggestion

# Ctrl+R: must use fish_user_key_bindings to override Fish's default history search
# (conf.d scripts load before default key bindings, so direct bind gets overridden)
function __archaic_user_key_bindings
    bind --mode insert \cr __archaic_accept_suggestion
    bind --mode default \cr __archaic_accept_suggestion
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

# ── Debug/status function ────────────────────────────────────────────────────
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
