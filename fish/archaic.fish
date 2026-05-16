# archaic.fish - Fish shell integration for archaic autocomplete daemon
#
# Usage:
#   1. Ensure archaic daemon is running: ./build/archaic --daemon /path/to/scan
#   2. Source this file or symlink to ~/.config/fish/conf.d/archaic.fish
#
# Features:
#   - Tab completion for paths using the daemon's scored completions
#   - archaic_suggest function returns the single best match
#   - archaic_complete function returns all completions

set -g archaic_sock_path /tmp/archaic-daemon.sock
set -g archaic_cli_path (dirname (status filename))/../build/archaic-cli

function archaic_suggest -d "Get the single best completion suggestion from archaic daemon"
    set -l prefix (commandline -ct)
    if test -z "$prefix"
        return
    end

    # Resolve relative paths
    if not string match -q '/*' -- "$prefix"
        set prefix (pwd)/"$prefix"
    end

    set -l result (command $archaic_cli_path suggest "$prefix" 2>/dev/null)
    if test $status -eq 0
        echo "$result"
    end
end

function archaic_complete -d "Get all completions from archaic daemon"
    set -l prefix (commandline -ct)
    if test -z "$prefix"
        return
    end

    # Resolve relative paths
    if not string match -q '/*' -- "$prefix"
        set prefix (pwd)/"$prefix"
    end

    command $archaic_cli_path complete "$prefix" 10 2>/dev/null | string match -r '^  \[' | string replace -r '.*?  (.*)' '$1'
end

# Fish completion for all commands
function __archaic_do_complete
    set -l prefix (commandline -ct)
    if test -z "$prefix"
        return
    end

    # Resolve relative paths
    if not string match -q '/*' -- "$prefix"
        set prefix (pwd)/"$prefix"
    end

    # Query daemon for completions
    set -l output (command $archaic_cli_path complete "$prefix" 20 2>/dev/null)
    if test $status -ne 0
        return
    end

    # Parse output: lines like "  [0] score=0.7332 freq=1 dir=no  /path/to/file"
    echo "$output" | while read -l line
        set -l path (echo "$line" | string match -r '  (/.*)$')
        if test -n "$path"
            echo "$path"
        end
    end
end

# Register completion for common commands
complete -c cd -f -a "(__archaic_do_complete)"
complete -c ls -f -a "(__archaic_do_complete)"
complete -c cat -f -a "(__archaic_do_complete)"
complete -c vim -f -a "(__archaic_do_complete)"
complete -c nvim -f -a "(__archaic_do_complete)"
complete -c less -f -a "(__archaic_do_complete)"
complete -c bat -f -a "(__archaic_do_complete)"
complete -c code -f -a "(__archaic_do_complete)"
complete -c rm -f -a "(__archaic_do_complete)"
complete -c mv -f -a "(__archaic_do_complete)"
complete -c cp -f -a "(__archaic_do_complete)"
complete -c mkdir -f -a "(__archaic_do_complete)"
complete -c touch -f -a "(__archaic_do_complete)"
complete -c git -f -a "(__archaic_do_complete)"

# Add suggest command to CLI
if test -n "$argv[1]" -a "$argv[1]" = "suggest"
    archaic_suggest
    exit
end
