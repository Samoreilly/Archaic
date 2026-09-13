#!/usr/bin/env bash
# test_shell_contract.sh - Shell contract tests on a fixture tree (#8).
#
# fish, bash, zsh and archaic-cli must AGREE on the same fixture queries:
# Tab completion sets match, bare (slash-less) tokens work everywhere,
# dirs_only filters files, and ghost affordances behave.
#
# Methods: fish via `complete -C`, bash by invoking _archaic_do_complete
# with COMP_* set, zsh via its suggestion/accept functions headlessly
# (LBUFFER/POSTDISPLAY need no live zle), CLI directly. Ghost rendering
# (bash stderr) and ghost accept (bash/zsh insertion) are asserted too.
#
# Hermetic: unique temp dir, own XDG_CONFIG_HOME/XDG_CACHE_HOME/
# ARCHAIC_CONFIG, own sockets (incl. per-shell helper sockets). The
# production daemon is never touched.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build/archaic"
CLI="$ROOT/build/archaic-cli"

PASS=0
FAIL=0
ok()  { PASS=$((PASS + 1)); echo "  PASS  $1"; }
bad() { FAIL=$((FAIL + 1)); echo "  FAIL  $1${2:+ ($2)}"; }

T="$(mktemp -d /tmp/archaic-contract-test.XXXXXX)"
FIX="$T/fixture"
CACHE="$T/cache"
DSOCK="$T/daemon.sock"
export XDG_CONFIG_HOME="$T/config"
export XDG_CACHE_HOME="$CACHE"
export ARCHAIC_CONFIG="$T/config.toml"
mkdir -p "$FIX/alpha" "$FIX/beta" "$CACHE" "$XDG_CONFIG_HOME"
touch "$FIX/alpha/a.txt" "$FIX/beta/b.txt" "$FIX/gamma.txt" "$FIX/delta.md"

cli() { timeout 5 "$CLI" --sock "$DSOCK" "$@" 2>/dev/null; }

cleanup() {
    for p in "$T"/*.sock.pid; do
        [ -f "$p" ] || continue
        SPID="$(cat "$p" 2>/dev/null)"
        if [ -n "$SPID" ] && kill -0 "$SPID" 2>/dev/null; then
            kill "$SPID" 2>/dev/null || true
        fi
    done
    if [ -f "$DSOCK.pid" ]; then
        DPID="$(cat "$DSOCK.pid" 2>/dev/null)"
        if [ -n "$DPID" ] && kill -0 "$DPID" 2>/dev/null; then
            kill "$DPID" 2>/dev/null || true
            for _ in $(seq 1 25); do kill -0 "$DPID" 2>/dev/null || break; sleep 0.2; done
            kill -9 "$DPID" 2>/dev/null || true
        fi
    fi
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

for b in "$BIN" "$CLI"; do
    if [ ! -x "$b" ]; then echo "missing binary: $b (run cmake --build build first)"; exit 1; fi
done
for sh in fish bash zsh; do
    command -v $sh >/dev/null 2>&1 || { echo "missing shell: $sh"; exit 1; }
done

cat > "$ARCHAIC_CONFIG" <<EOF
[daemon]
socket_path = "$DSOCK"
scan_threads = 1
max_depth = 8
rescan_interval_seconds = 3600
EOF

setsid "$BIN" --daemon "$FIX" "$DSOCK" >"$T/daemon.log" 2>&1 < /dev/null &
for _ in $(seq 1 40); do
    if [ -S "$DSOCK" ] && cli ping >/dev/null 2>&1; then break; fi
    sleep 0.2
done
if ! cli ping >/dev/null 2>&1; then bad "daemon starts"; exit 1; fi
for _ in $(seq 1 100); do
    cli complete "$FIX/" 10 "$FIX" 2>/dev/null | grep -qF "$FIX/alpha" && break
    sleep 0.2
done

ENV="ARCHAIC_CONFIG=$T/config.toml XDG_CACHE_HOME=$CACHE PATH=$ROOT/build:$PATH"

# Normalize a path list to absolute, slash-insensitive, sorted unique set.
normset() {
    local cwd="$1"
    sed -e 's/[[:space:]]*$//' | while IFS= read -r p; do
        [ -z "$p" ] && continue
        p="${p%/}"
        case "$p" in
            /*) printf '%s\n' "$p" ;;
            *) printf '%s/%s\n' "$cwd" "$p" ;;
        esac
    done | sort -u
}

fish_complete() { # $1 = full command line
    # complete -C also prints fish's native file completions (no
    # description); archaic's lines always carry a tab-separated
    # description, so filter on TAB.
    env $ENV fish -c "
        source $ROOT/fish/archaic.fish
        set -g archaic_helper_listen $T/fish.sock
        cd $FIX
        complete -C'$1'
    " 2>/dev/null | grep -P '\t' | cut -f1 | normset "$FIX"
}

bash_complete() { # $1 $2... = COMP_WORDS
    local words="$1"
    env $ENV bash -c "
        source $ROOT/bash/archaic.bash
        _archaic_helper_listen=$T/bash.sock
        cd $FIX
        COMP_WORDS=($words)
        COMP_CWORD=\$((\${#COMP_WORDS[@]} - 1))
        COMP_LINE='$1'
        COMP_POINT=\${#COMP_LINE}
        COMPREPLY=()
        _archaic_do_complete >/dev/null 2>$T/bash-err.txt
        printf '%s\n' \"\${COMPREPLY[@]}\"
    " 2>/dev/null | normset "$FIX"
}

cli_set() { # remaining args: complete args; prints normalized set
    cli "$@" | grep -E '^[DF] ' | awk '{print $2}' | normset "$FIX"
}

check_agree() { # $1 = label, $2 = expected set, $3... = actual sets
    local label="$1" expected="$2"
    shift 2
    local a
    for a in "$@"; do
        if [ "$a" != "$expected" ]; then
            bad "$label" "expected [$(echo "$expected" | tr '\n' ' ')] got [$(echo "$a" | tr '\n' ' ')]"
            return
        fi
    done
    ok "$label"
}

# ── 1. Absolute subdir prefix ────────────────────────────────────────────────
EXP="$(cli_set complete "$FIX/al" 20 "$FIX")"
check_agree "absolute prefix agrees" "$EXP" \
    "$(fish_complete "cd $FIX/al")" \
    "$(bash_complete "cd $FIX/al")"

# ── 2. Bare token (no slash) ─────────────────────────────────────────────────
EXP="$(cli_set complete "$FIX/al" 20 "$FIX")"
check_agree "bare token agrees" "$EXP" \
    "$(fish_complete "cd al")" \
    "$(bash_complete "cd al")"

# ── 3. File prefix via vim ───────────────────────────────────────────────────
EXP="$(cli_set complete "$FIX/gam" 20 "$FIX")"
check_agree "file prefix agrees" "$EXP" \
    "$(fish_complete "vim gam")" \
    "$(bash_complete "vim gam")"

# ── 4. Empty token lists the directory ───────────────────────────────────────
# The dir itself renders absolute in the CLI but as a basename in shells;
# contract is the children sets.
SELF="$(basename "$FIX")"
EXP="$(cli_set complete "$FIX" 20 "$FIX" 1 | grep -v "^$FIX$")"
check_agree "empty token agrees" "$EXP" \
    "$(fish_complete "cd " | grep -v "^$FIX/$SELF$")" \
    "$(bash_complete "cd " | grep -v "^$FIX/$SELF$")"

# ── 5. dirs_only filters files ───────────────────────────────────────────────
EXP_EMPTY="$(cli_set complete "$FIX/gam" 20 "$FIX" 1)"
if [ -z "$EXP_EMPTY" ]; then
    ok "cli dirs_only excludes gamma.txt"
else
    bad "cli dirs_only excludes gamma.txt" "got $EXP_EMPTY"
fi
check_agree "dirs_only agrees (empty)" "$EXP_EMPTY" \
    "$(fish_complete "cd gam")" \
    "$(bash_complete "cd gam")"

# ── 6. Ghost: bash renders the remainder on Tab (stderr) ────────────────────
env $ENV bash -c "
    source $ROOT/bash/archaic.bash
    _archaic_helper_listen=$T/bash.sock
    cd $FIX
    COMP_WORDS=(cd $FIX/al); COMP_CWORD=1; COMP_LINE='cd $FIX/al'; COMP_POINT=\${#COMP_LINE}
    COMPREPLY=()
    _archaic_do_complete >/dev/null 2>$T/ghost-err.txt
" 2>/dev/null
if grep -q "pha" "$T/ghost-err.txt" && grep -q "Ctrl+Space" "$T/ghost-err.txt"; then
    ok "bash renders ghost remainder on Tab"
else
    bad "bash renders ghost remainder on Tab" "got: $(cat "$T/ghost-err.txt")"
fi

# ── 7. Ghost: bash accept inserts the remainder ──────────────────────────────
GOT="$(env $ENV bash -c "
    source $ROOT/bash/archaic.bash
    _archaic_helper_listen=$T/bash.sock
    cd $FIX
    READLINE_LINE='cd $FIX/al'; READLINE_POINT=\${#READLINE_LINE}
    _archaic_accept_suggestion
    printf '%s' \"\$READLINE_LINE\"
" 2>/dev/null)"
if [ "$GOT" = "cd $FIX/alpha" ]; then
    ok "bash accept inserts ghost remainder"
else
    bad "bash accept inserts ghost remainder" "got '$GOT'"
fi

# ── 8. Ghost: zsh POSTDISPLAY for slash + bare tokens ────────────────────────
ZGOT="$(env $ENV zsh -f -c "
    autoload -Uz compinit && compinit -u -d $T/zcompdump >/dev/null 2>&1
    source $ROOT/zsh/archaic.zsh >/dev/null 2>&1
    _archaic_helper_listen=$T/zsh.sock
    cd $FIX
    LBUFFER='cd $FIX/al'
    _archaic_zle_update
    printf '%s' \"\$POSTDISPLAY\"
" 2>/dev/null)"
[ "$ZGOT" = "pha" ] && ok "zsh ghost for slash token" || bad "zsh ghost for slash token" "got '$ZGOT'"
ZGOT2="$(env $ENV zsh -f -c "
    autoload -Uz compinit && compinit -u -d $T/zcompdump >/dev/null 2>&1
    source $ROOT/zsh/archaic.zsh >/dev/null 2>&1
    _archaic_helper_listen=$T/zsh.sock
    cd $FIX
    LBUFFER='cd al'
    _archaic_zle_update
    printf '%s' \"\$POSTDISPLAY\"
" 2>/dev/null)"
[ "$ZGOT2" = "pha" ] && ok "zsh ghost for bare token" || bad "zsh ghost for bare token" "got '$ZGOT2'"

# ── 9. Ghost: zsh accept inserts the remainder ───────────────────────────────
ZGOT3="$(env $ENV zsh -f -c "
    autoload -Uz compinit && compinit -u -d $T/zcompdump >/dev/null 2>&1
    source $ROOT/zsh/archaic.zsh >/dev/null 2>&1
    _archaic_helper_listen=$T/zsh.sock
    cd $FIX
    LBUFFER='vim gam'
    _archaic_zle_update
    _archaic_accept_suggestion >/dev/null 2>&1
    printf '%s|%s' \"\$LBUFFER\" \"\$POSTDISPLAY\"
" 2>/dev/null)"
[ "$ZGOT3" = "vim gamma.txt|" ] && ok "zsh accept inserts ghost remainder" \
    || bad "zsh accept inserts ghost remainder" "got '$ZGOT3'"

# ── 10. Discoverability: status + doctor document the accept key ─────────────
env $ENV bash -c "source $ROOT/bash/archaic.bash; archaic-status" 2>/dev/null | grep -q "Ctrl+Space" \
    && ok "bash status documents accept key" || bad "bash status documents accept key"
env $ENV fish -c "source $ROOT/fish/archaic.fish; __archaic_status" 2>/dev/null | grep -q "Ctrl+Space" \
    && ok "fish status documents accept key" || bad "fish status documents accept key"
env $ENV zsh -f -c "source $ROOT/zsh/archaic.zsh >/dev/null 2>&1; archaic-status" 2>/dev/null | grep -q "Ctrl+Space" \
    && ok "zsh status documents accept key" || bad "zsh status documents accept key"
cli doctor 2>/dev/null | grep -q "Ctrl+Space" \
    && ok "doctor documents accept key" || bad "doctor documents accept key"

# ── 10b. Ghost: fish suggestion via stubbed commandline ──────────────────────
# commandline is interactive-only, so stub it to drive the real function.
fish_ghost() { # $1 = space-separated tokens, first line = current token
    local toks="$1"
    env $ENV fish -c "
        function commandline
            if test \"\$argv[1]\" = -t
                echo '$toks' | tr ' ' '\n' | tail -1
            else
                echo '$toks' | tr ' ' '\n'
            end
        end
        source $ROOT/fish/archaic.fish
        set -g archaic_helper_listen $T/fish-ghost.sock
        cd $FIX
        __archaic_get_suggestion
        printf '%s' \"\$__archaic_suggestion\"
    " 2>/dev/null
}
[ "$(fish_ghost "cd al")" = "pha" ] && ok "fish ghost for bare token" \
    || bad "fish ghost for bare token" "got '$(fish_ghost "cd al")'"
[ "$(fish_ghost "cd $FIX/al")" = "pha" ] && ok "fish ghost for slash token" \
    || bad "fish ghost for slash token" "got '$(fish_ghost "cd $FIX/al")'"
[ -z "$(fish_ghost "vim")" ] && ok "fish ghost stays off in command position" \
    || bad "fish ghost stays off in command position" "got '$(fish_ghost "vim")'"
[ -z "$(fish_ghost "git al")" ] && ok "fish ghost stays off for non-file commands" \
    || bad "fish ghost stays off for non-file commands" "got '$(fish_ghost "git al")'"

# ── 11. Regression: shell queries never harm the daemon socket ───────────────# (fish once deleted the live socket on first use via a broken ping.)
if env $ENV fish -c "source $ROOT/fish/archaic.fish; __archaic_ping" >/dev/null 2>&1; then
    ok "fish ping succeeds"
else
    bad "fish ping succeeds"
fi
if [ -S "$DSOCK" ] && cli ping >/dev/null 2>&1; then
    ok "daemon socket survives all shell queries"
else
    bad "daemon socket survives all shell queries"
fi

echo ""
echo "shell-contract: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
