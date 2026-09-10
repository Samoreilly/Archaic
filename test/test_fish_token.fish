#!/usr/bin/env fish
# Token rewrite checks (no daemon).

set -l root (dirname (dirname (status filename)))
source "$root/fish/archaic.fish"

set -g fail 0
function expect -a got want label
    if test "$got" != "$want"
        echo "FAIL $label: got '$got' want '$want'"
        set -g fail 1
    else
        echo "PASS $label"
    end
end

set -l home $HOME
set -l base $home/samdev
set -l child $base/archaic

expect (__archaic_token_path $child samdev $base samdev) samdev/archaic relative
expect (__archaic_token_path $child / $home /) $child absolute-root
expect (__archaic_token_path $child $base $base $base) $child absolute-prefix
expect (__archaic_token_path $child ~/samdev $base ~/samdev) ~/samdev/archaic tilde
expect (__archaic_token_path $child "" $base "") archaic empty-token-basename
expect (__archaic_canon_path ~/samdev/archaic) $child canon-tilde

if test $fail -eq 0
    echo "All token tests passed"
    exit 0
end
exit 1
