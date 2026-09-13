# Archaic PE backlog

Status: in progress. Check boxes only after tests pass and the change is pushed.

## 1. doctor --fix + empty-Tab hint
- [x] `archaic-cli doctor --fix` repairs stale socket, missing user unit, missing plugins, missing config
- [x] Empty Tab prints one dim hint (outside roots / ignored / daemon down / indexing)
- [x] Tests + push

## 2. Persistent helper (one process per shell)
- [x] Helper stays connected to the daemon for the shell lifetime
- [x] Shells do not fork `timeout` + helper + cli on every Tab
- [x] Fallback to one-shot if persist fails
- [x] Tests + push

## 3. Recent-first ranking + accept learning
- [x] Last-used / cwd-proximate paths beat alphabetical siblings
- [x] Accept (Tab/ghost/cd) records selection and affects next Tab
- [x] Tests + push

## 4. Copy-install + generated config + honest README
- [x] `install.sh` copies plugins (no clone symlink requirement)
- [x] Writes `~/.config/archaic/config.toml` if missing
- [x] README matches actual roots / install / commands
- [x] Tests + push

## 5. Memory budget in doctor + enforcement
- [x] `doctor`/`health` report RSS vs `max_memory_mb`
- [x] Scanner/index sheds or refuses when over cap
- [x] Tests + push

## 6. Per-root policy + live unwatch
- [ ] Per-root depth / ignore / watch in config
- [ ] `unwatch` drops the live watcher + scan list without restart
- [ ] Tests + push

## 7. Ghost-text
- [ ] Hints work without requiring `/` in the token
- [ ] Bash actually renders the ghost
- [ ] Accept key documented in status / doctor
- [ ] Tests + push

## 8. Shell contract tests
- [ ] Fixture tree test: fish/bash/zsh/`archaic-cli complete` agree
- [ ] Tests + push
