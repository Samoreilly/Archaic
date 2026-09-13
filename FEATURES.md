# Archaic PE backlog

Status: done. Boxes checked only after tests passed and the change was pushed.

## 1. doctor --fix + empty-Tab hint
- [x] `archaic-cli doctor --fix` repairs stale socket, missing user unit, missing plugins, missing config
- [x] Empty Tab prints one dim hint (outside roots / ignored / daemon down / indexing)
- [x] Tests + push

## 2. Persistent helper (one process per shell)
- [x] Helper stays connected to the daemon for the shell lifetime
- [x] Shells do not fork `timeout` + helper + cli on every Tab
- [x] Fallback to one-shot if persist fails
- [x] Tests + push (`test/integration/test_persistent_helper.sh`: 15/15)

## 3. Recent-first ranking + accept learning
- [x] Last-used / cwd-proximate paths beat alphabetical siblings
- [x] Accept (Tab/ghost/cd) records selection and affects next Tab
- [x] Tests + push (`test/integration/test_recent_first.sh`: 5/5)

## 4. Copy-install + generated config + honest README
- [x] `install.sh` copies plugins (no clone symlink requirement)
- [x] Writes `~/.config/archaic/config.toml` if missing
- [x] README matches actual roots / install / commands
- [x] Tests + push (`test/integration/test_copy_install.sh`: 33/33)

## 5. Memory budget in doctor + enforcement
- [x] `doctor`/`health` report RSS vs `max_memory_mb`
- [x] Scanner/index sheds or refuses when over cap
- [x] Tests + push (unit `memory_budget_*` + `test/integration/test_memory_budget.sh`: 4/4)

## 6. Per-root policy + live unwatch
- [x] Per-root depth / ignore / watch in config
- [x] `unwatch` drops the live watcher + scan list without restart
- [x] Tests + push (unit `root_*` + `test/integration/test_per_root_unwatch.sh`: 14/14)

## 7. Ghost-text
- [x] Hints work without requiring `/` in the token
- [x] Bash actually renders the ghost
- [x] Accept key documented in status / doctor
- [x] Tests + push (covered by `test/integration/test_shell_contract.sh`: 21/21)

## 8. Shell contract tests
- [x] Fixture tree test: fish/bash/zsh/`archaic-cli complete` agree
- [x] Tests + push (`test/integration/test_shell_contract.sh`: 21/21)
