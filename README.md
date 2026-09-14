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

**Tab feels instant.** On any project under 50k files, the cached lookup (every
Tab after the first) takes **~0.03 ms** — thousands of times below the 100 ms
human perception threshold. The first Tab after login walks the index in
**2–23 ms**, also imperceptible.

### What you actually care about

| | small project | medium project | large project |
|---|---|---|---|
| files indexed | 5 000 | 20 000 | 50 000 |
| time to first Tab | **1.8 ms** | **7.8 ms** | **23 ms** |
| cached Tab (median) | **0.04 ms** | **0.04 ms** | **0.03 ms** |
| idle memory (RSS) | **10 MB** | **17 MB** | **33 MB** |
| results per query | 9 | 11 | 14 |
| first scan (one-time) | 50 ms | 1.0 s | 8.8 s |

Cached Tab latency is constant regardless of project size — the index is in
memory and lookups are a hash probe. First-scan time and memory scale linearly
with file count.

### Scaling

The daemon uses ~0.7 KB of RSS per indexed file. Extrapolate for your project:

| your project | estimated memory | estimated first Tab |
|---|---|---|
| 5k files (small lib) | 10 MB | < 2 ms |
| 20k files (typical app) | 17 MB | < 8 ms |
| 50k files (large monorepo) | 33 MB | < 25 ms |
| 200k files (huge repo) | ~140 MB | ~100 ms |

First Tab times are linear — double the files, double the latency. Cached Tab
stays at ~0.04 ms no matter what.

### How many results does Tab show?

Each Tab press returns up to **50 completions**, ranked by a weighted score
(frequency 40%, recency 30%, depth 15%, type 10%, cwd proximity 5%). The
shell plugin picks the best match and shows alternatives you can cycle through
with `Alt+Down` / `Alt+Up`.

### Benchmark details

All numbers from `test/bench-e2e.c`: a real daemon (fork/exec'd) serving
completions over a Unix socket against real directory trees on disk.
Times are `CLOCK_MONOTONIC` wall-clock. Memory is the daemon's RSS reported
by its own health endpoint after the scan completes.

<details>
<summary>Implementation details (for the curious)</summary>

**Warm path** (cached completion — every Tab after the first):
1. Client packs `IPC_MSG_COMPLETE` (8 KB) → Unix socket
2. Server `accept()` → `SO_PEERCRED` uid check → thread pool
3. `cache_get()`: djb2 hash → 16-shard lock → linear probe → LRU move → return pointer
4. Response packed in 256 KB thread-local buffer (zero-alloc) → socket → done

**Cold path** (cache miss — first Tab after login):
1. `find_prefix_node`: radix tree descent, memcmp on edge keys, binary search for wide nodes
2. `scored_completions_collect`: DFS, `compute_score` per node
3. `qsort` by score → top 50 returned → `cache_put` (deep-copy outside shard lock)
4. Subsequent queries for the same prefix skip this entirely

Run the benchmark yourself:
```
cmake --build build -j$(nproc) --target archaic-bench-e2e
./build/archaic-bench-e2e
```
Source: [`test/bench-e2e.c`](test/bench-e2e.c)
</details>

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
