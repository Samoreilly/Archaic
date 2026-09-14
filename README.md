# Archaic

Path completion for Fish, Bash, and Zsh. A background daemon indexes your
tree once and answers Tab over a Unix socket.

## Install

### Arch / CachyOS

```bash
git clone https://github.com/Samoreilly/Archaic.git
cd Archaic/packaging/arch/archaic-git
makepkg -si
systemctl --user enable --now archaic.service
```

If you previously used `./install.sh`, run `./run.sh disable-service` first so the pacman unit is not shadowed.

Then open a **new** terminal, type `cd ` and press Tab.

To publish on the AUR later: push `packaging/arch/archaic-git` to `ssh://aur@aur.archlinux.org/archaic-git.git`. After that: `paru -S archaic-git`.

### Anywhere else

Needs CMake 3.22+, a C23 compiler, and [libfmt](https://fmt.dev).

```bash
# Debian/Ubuntu: sudo apt install cmake g++ libfmt-dev
# Fedora:        sudo dnf install cmake gcc fmt-devel
# macOS:         brew install cmake fmt

git clone https://github.com/Samoreilly/Archaic.git
cd Archaic
./install.sh              # copies binaries + plugins; indexes workspace dirs
# ./install.sh ~/projects # also watch this tree
```

`install.sh` copies files into `~/.local/bin` and your shell config. You do not need to keep the clone.

Default index is existing dirs among `~/src`, `~/projects`, `~/project`, `~/dev`, `~/code`, `~/git`, `~/repos`, `~/samdev`, `~/work` — not all of `$HOME`. (If none of those exist, it falls back to `$HOME`.)

```bash
archaic-cli doctor
# archaic-cli doctor --fix
```

## After install

| Command | What it does |
|---|---|
| `archaic-cli doctor` | Is the daemon up? |
| `archaic-cli doctor --fix` | Write config, install plugins, start daemon |
| `./run.sh status` | Socket / running |
| `./run.sh rescan` | Re-index |
| `./run.sh restart` | Restart |
| `./run.sh disable-service` | Stop login autostart |

On Linux the user systemd unit starts the daemon at login.

## Config

`install.sh` writes `~/.config/archaic/config.toml` if missing. See
[config.example.toml](config.example.toml) for every key.

Add more trees without reinstalling:

```bash
archaic-cli watch ~/whatever
archaic-cli unwatch ~/whatever   # drops it live: scan list, watcher, index
archaic-cli roots
```

Per-root policy goes on `~/.config/archaic/roots` lines:
`/path depth=3 watch=0 ignore_dirs=target,dist` (see `config.example.toml`).

File changes are picked up by rescanning, not patched into the index:
the watcher triggers a prompt full rescan of the roots (per-file
incremental updates don't exist; `unwatch` drops whole roots only).

- Socket: `$XDG_RUNTIME_DIR/archaic.sock` (else `/tmp/archaic-$UID.sock`)
- State: `$XDG_CACHE_HOME/archaic/state.bin` (else `~/.cache/archaic/state.bin`)
- `cd` / `mkdir` complete directories only
- Hidden files only if you typed `.`
- `sudo` / `make` / `python` complete paths only when the token looks like one

## Performance

Measured with `./build/archaic-bench-e2e`: a real daemon (fork/exec'd) serving
real IPC completions over a Unix socket against real directory trees on disk.
Cold numbers are the first Tab press after startup; warm numbers are
subsequent presses served from the in-memory cache.

| tree  | files  | first scan | save  | cold 1st Tab | warm Tab (p50) | warm Tab (p95) |
|-------|--------|------------|-------|-------------|----------------|----------------|
| small |  5 000 |     50 ms  |  2 ms |     1.8 ms  |        0.05 ms |        0.07 ms |
| med   | 20 000 |   1 100 ms |  8 ms |     8.2 ms  |        0.03 ms |        0.13 ms |
| large | 50 000 |   7 500 ms | 24 ms |    24.2 ms  |        0.04 ms |        0.15 ms |

**What's happening at each stage:**

| Stage | Hot / Cold | What it does |
|---|---|---|
| Warm Tab | hot | `cache_get` → 16-shard hash lookup + LRU move → response in **~30 µs** |
| Cold 1st Tab | cold | `find_prefix_node` → radix DFS → `scored_completions` sort → `cache_put` → response in **2–24 ms** |
| First scan | cold | `scanner_worker` threads × `opendir/readdir` + bucket insert + trie insert → **50–7 500 ms** |
| Save | cold | Snapshot bucket refs → DFS serialize → `rename()` → **2–24 ms** for 0.9–9 MB |

**Hot path (cached completion) — what happens on every Tab press:**

1. Client sends `IPC_MSG_COMPLETE` (8 KB packed payload) over the Unix socket
2. Server `accept()` → `SO_PEERCRED` uid check → thread pool dispatch
3. `cache_get()`: djb2 hash → shard lock (16 shards) → linear probe → LRU move-to-front → return borrowed pointer
4. Response packed into a 256 KB thread-local buffer (zero-alloc), sent back over the socket

**Cold path (cache miss / startup):**

1. `find_prefix_node`: radix descent, memcmp on edge keys, binary search for nodes with >8 children
2. `scored_completions_collect`: DFS, `compute_score` per node (weighted: freq 40%, recency 30%, depth 15%, type 10%, cwd 5%)
3. `qsort` by score → top 50 returned → `cache_put` (deep-copy outside shard lock)
4. Subsequent queries for the same prefix skip all of the above

Full benchmark source: [`test/bench-e2e.c`](test/bench-e2e.c).
Run with: `cmake --build build -j$(nproc) --target archaic-bench-e2e && ./build/archaic-bench-e2e`

## Keybindings

When a dimmed ghost hint appears after your path, accept it without retyping:

| Keys | What it does |
|---|---|
| `Ctrl+Space` | Accept the hinted path (works in every terminal) |
| `Alt+Right` | Accept the hinted path (where your terminal passes it through) |
| `Alt+Down` / `Alt+Up` | Cycle through alternative completions |
| `Alt+Shift+Right` | Accept and keep completing (fish) |

Set `ARCHAIC_SUGGEST_ON_PROMPT=0` to turn ghost hints off (Tab completion keeps working).

## Troubleshooting

```bash
archaic-cli doctor
systemctl --user status archaic   # Linux
```

No completions: new shell, and the scan path must contain the files you want.
Daemon down: `./run.sh enable-service` or `./run.sh start`.

## License

MIT — see [LICENSE](LICENSE).
