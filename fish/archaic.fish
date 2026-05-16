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
    if test -z "$prefix"
        return
    end

    # Only complete path-like inputs or simple directory names
    if not string match -q '*/*' -- "$prefix"
        if not string match -qr '^[a-zA-Z0-9._-]+$' -- "$prefix"
            return
        end
    end

    # Resolve relative paths for the daemon query
    set -l resolved "$prefix"
    if not string match -q '/*' -- "$prefix"
        set -l clean_prefix (string replace -r '^\./' '' "$prefix")
        set resolved (pwd)/"$clean_prefix"
    else
        set resolved (string replace -r '//+' '/' "$resolved")
        set resolved (string replace -r '/\./' '/' "$resolved")
    end

    # Normalize prefixes for display conversion
    set -l norm_resolved (string replace -r '/+$' '' "$resolved")
    set -l norm_prefix (string replace -r '/+$' '' "$prefix")

    # Query daemon and parse output: "D /path" or "F /path" (one per line)
    set -l found 0
    for line in (command $archaic_cli_path complete "$resolved" 20 2>/dev/null)
        set -l parts (string split " " "$line")
        if test (count $parts) -ge 2
            set -l type $parts[1]
            set -l full_path $parts[2]
            
            # Convert to relative if user typed relative path
            set -l display_path "$full_path"
            if not string match -q '/*' -- "$prefix"
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
