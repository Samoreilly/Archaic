# archaic.fish - Fish shell integration for archaic autocomplete daemon
#
# Usage:
#   1. Start daemon: ./run.sh start /path/to/scan
#   2. Install plugin: ./run.sh install-fish
#   3. Restart fish or run: source ~/.config/fish/conf.d/archaic.fish

set -g archaic_sock_path /tmp/archaic-daemon.sock

# Resolve CLI path: follow symlinks to find actual location
set -l plugin_path (status filename)
if test -L "$plugin_path"
    set plugin_path (readlink -f "$plugin_path")
end
set -g archaic_cli_path (dirname (dirname "$plugin_path"))/build/archaic-cli

function __archaic_do_complete -d "Query archaic daemon for completions"
    set -l prefix (commandline -ct)
    
    # Detect the command being completed to apply rules
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

    # Handle empty prefix (e.g. "ls <tab>")
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

        # Resolve relative paths for the daemon query
        set resolved "$prefix"
        if not string match -q '/*' -- "$prefix"
            set -l clean_prefix (string replace -r '^\./' '' "$prefix")
            set resolved (pwd)/"$clean_prefix"
        end
        # Normalize path: resolve //, /./, and /../
        while string match -q '*/../*' -- "$resolved"
            set resolved (string replace -r '/[^/]+/\.\./' '/' "$resolved")
        end
        set resolved (string replace -r '/\.$' '/' "$resolved")
        set resolved (string replace -r '//+' '/' "$resolved")
        set resolved (string replace -r '/\./' '/' "$resolved")
        set norm_prefix (string replace -r '/+$' '' "$prefix")
    end

    # Normalize resolved prefix for display conversion
    set -l norm_resolved (string replace -r '/+$' '' "$resolved")

    # Query daemon and parse output: "D /path" or "F /path" (one per line)
    set -l found 0
    for line in (command $archaic_cli_path complete "$resolved" 20 2>/dev/null)
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
                # Empty prefix: show just the basename
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
        return
    end
end

# Replace completions for common commands
complete -e -c cd
complete -c cd -f -a "(__archaic_do_complete)"

complete -e -c ls
complete -c ls -f -a "(__archaic_do_complete)"

complete -e -c cat
complete -c cat -f -a "(__archaic_do_complete)"

complete -e -c vim
complete -c vim -f -a "(__archaic_do_complete)"

complete -e -c nvim
complete -c nvim -f -a "(__archaic_do_complete)"

complete -e -c less
complete -c less -f -a "(__archaic_do_complete)"

complete -e -c bat
complete -c bat -f -a "(__archaic_do_complete)"

complete -e -c rm
complete -c rm -f -a "(__archaic_do_complete)"

complete -e -c mv
complete -c mv -f -a "(__archaic_do_complete)"

complete -e -c cp
complete -c cp -f -a "(__archaic_do_complete)"

complete -e -c mkdir
complete -c mkdir -f -a "(__archaic_do_complete)"

complete -e -c touch
complete -c touch -f -a "(__archaic_do_complete)"

# Catch-all for any command with path-like arguments
complete -c "" -f -a "(__archaic_do_complete)"

# Inline autosuggestion via fish_right_prompt
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
    
    # Resolve relative paths
    set -l resolved "$prefix"
    if not string match -q '/*' -- "$prefix"
        set resolved (pwd)/"$prefix"
    end
    # Normalize: strip trailing slash for comparison
    set -l norm_resolved (string replace -r '/+$' '' "$resolved")
    
    # Use complete (not suggest) to get best match inside the directory
    set -l output (command $archaic_cli_path complete "$resolved" 1 2>/dev/null)
    if test $status -ne 0
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

# Key bindings to accept the suggestion
function __archaic_accept_suggestion
    if test -n "$__archaic_suggestion"
        commandline -t (commandline -t)"$__archaic_suggestion"
        set -g __archaic_suggestion ""
        commandline -f repaint
    end
end

bind \e\[1\;3C __archaic_accept_suggestion
bind \cr __archaic_accept_suggestion
