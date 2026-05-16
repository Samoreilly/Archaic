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

    # Only complete path-like inputs (starts with /, ./, ../, or contains /)
    if not string match -qr '^(\./|\.\./|/|[^/]+/)' -- "$prefix"
        return
    end

    # Resolve relative paths
    if not string match -q '/*' -- "$prefix"
        set prefix (pwd)/"$prefix"
    end

    # Query daemon
    set -l output (command $archaic_cli_path complete "$prefix" 20 2>/dev/null)
    if test $status -ne 0
        return
    end

    # Parse: "  [0] score=0.7332 freq=1 dir=no  /path/to/file"
    echo "$output" | string match -r '  (/.*)$'
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

# Catch-all: provide archaic completions for any unrecognized command
# that looks like a path
complete -c "" -f -a "(__archaic_do_complete)"
