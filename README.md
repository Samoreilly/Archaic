# Archaic

**Blazing-fast, intelligent terminal autocomplete daemon for Fish and Bash.**

Archaic is a C-based autocomplete daemon that indexes your filesystem and serves
context-aware, frequency-weighted path completions in sub-milliseconds. It runs
as a background process, learns from your usage patterns, and integrates
seamlessly with Fish and Bash shells.

## Features

- **Sub-millisecond query latency** — radix tree + binary search + LRU cache
- **Intelligent scoring** — frequency, recency, depth, and type weighted ranking
- **Enhanced fuzzy matching** — 5-strategy scoring: exact prefix, segment tokenization (acronym matching), path-boundary bonuses, Levenshtein typo tolerance, and full-path fallback
- **Background filesystem scanning** — multi-threaded, configurable depth
- **Real-time filesystem watching** — inotify (Linux) / kqueue (macOS) with polling fallback
- **Periodic auto-rescan** — stays in sync with filesystem changes
- **State persistence** — survives daemon restarts via binary save/load (atomic write)
- **TOML configuration** — sensible defaults, fully customizable
- **Fish, Bash, and Zsh integration** — tab completion and inline suggestions
- **Neovim integration** — nvim-cmp source, Telescope picker, user commands
- **Unix domain socket IPC** — versioned binary protocol for client communication
- **Memory safety** — configurable limits, per-bucket refcounting, LRU eviction
- **Zero external dependencies** — pure C23, POSIX, pthreads (except libfmt for logging)

## Architecture

```
Fish/Bash/Zsh → archaic-helper (persistent) → Unix Socket → archaic-daemon
                                                        ├── Radix Tree (sorted, bsearch)
                                                        ├── Bucket Store (65536, LRU)
                                                        ├── Query Cache (LRU + TTL)
                                                        ├── Enhanced Fuzzy (5-strategy)
                                                        ├── Filesystem Watcher (inotify/kqueue)
                                                        └── Parallel Scanner (4 threads)
```

The daemon runs as a persistent background process. Fish communicates through
`archaic-helper`, which maintains a persistent Unix domain socket connection to
avoid per-invocation overhead. The daemon holds a radix tree of indexed paths,
a bucket store for frequency tracking, and an LRU query cache for repeated
lookups.

## Quick Start

### One-command install (recommended)

```bash
git clone https://github.com/Samoreilly/Archaic.git
cd Archaic
./install.sh ~/projects
```

This builds the project, starts the daemon, and installs shell integration for your
current shell (Fish, Bash, or Zsh) — no further steps required.

### Homebrew (macOS)

```bash
brew install --formula Formula/archaic.rb
archaic --daemon ~/projects
```

### Nix

```bash
nix build
./result/bin/archaic --daemon ~/projects
```

### Manual install

```bash
# 1. Build
git clone https://github.com/Samoreilly/Archaic.git
cd Archaic
mkdir build && cd build
cmake .. && make

# 2. Start the daemon
./run.sh start ~/projects

# 3. Install shell integration
./run.sh install-fish   # Fish shell
./run.sh install-bash   # Bash
./run.sh install-zsh    # Zsh

# 4. Restart your shell or run:
#    Fish:  source ~/.config/fish/conf.d/archaic.fish
#    Bash:  source ~/.local/share/bash-completion/completions/archaic.bash
#    Zsh:   source ~/.config/zsh/archaic.zsh
```

## Configuration

Archaic reads configuration from the first path found in this order:

1. `$ARCHAIC_CONFIG` (environment variable)
2. `~/.config/archaic/config.toml`
3. `/etc/archaic/config.toml`

### Full Configuration

```toml
[daemon]
scan_path = "/home/user/projects"
socket_path = "/tmp/archaic-daemon.sock"
scan_threads = 4
max_depth = 10
rescan_interval_seconds = 300

[storage]
max_buckets = 65536
max_nodes_per_bucket = 100000
cache_max_entries = 1024
cache_ttl_seconds = 2

[scoring]
weight_frequency = 0.40
weight_recency = 0.30
weight_depth = 0.20
weight_type = 0.10

[fish]
commands = ["cd", "ls", "cat", "vim", "nvim", "less", "bat", "rm", "mv", "cp", "mkdir", "touch"]

[scanner]
ignore_dirs = [".git", "node_modules", "build", "dist", "__pycache__", ".venv"]
ignore_files = ["*.pyc", "*.o", "*.log", ".DS_Store"]
```

### Configuration Options

| Section | Key | Default | Description |
|---|---|---|---|
| `daemon` | `scan_path` | *(required)* | Root directory to index |
| `daemon` | `socket_path` | `/tmp/archaic-daemon.sock` | Unix socket location |
| `daemon` | `scan_threads` | `4` | Parallel scanner thread count |
| `daemon` | `max_depth` | `10` | Maximum directory traversal depth |
| `daemon` | `rescan_interval_seconds` | `300` | Auto-rescan interval (0 to disable) |
| `daemon` | `watcher_fallback_interval` | `30` | Polling interval when inotify/kqueue unavailable |
| `storage` | `max_buckets` | `65536` | Hash bucket count for path store |
| `storage` | `max_nodes_per_bucket` | `100000` | Max entries per bucket |
| `storage` | `cache_max_entries` | `1024` | Query cache size |
| `storage` | `cache_ttl_seconds` | `2` | Query cache time-to-live |
| `scoring` | `weight_frequency` | `0.40` | Usage frequency weight |
| `scoring` | `weight_recency` | `0.30` | Last-access recency weight |
| `scoring` | `weight_depth` | `0.20` | Path depth preference weight |
| `scoring` | `weight_type` | `0.10` | File type preference weight |
| `fish` | `commands` | *(see above)* | Commands that trigger completions (Fish) |
| `bash` | `commands` | *(see above)* | Commands that trigger completions (Bash) |
| `scanner` | `ignore_dirs` | *(see above)* | Directories to skip during scan |
| `scanner` | `ignore_files` | *(see above)* | File patterns to skip during scan |

## CLI Commands

### Daemon Management (`run.sh`)

```bash
./run.sh start [path]     # Start daemon scanning path
./run.sh stop             # Stop daemon
./run.sh restart [path]   # Restart daemon
./run.sh status           # Check daemon status
./run.sh rescan           # Trigger manual rescan
./run.sh install-fish     # Install Fish shell plugin
./run.sh uninstall-fish   # Remove Fish shell plugin
./run.sh install-bash     # Install Bash completion script
./run.sh uninstall-bash   # Remove Bash completion script
./run.sh install-zsh      # Install Zsh completion script
./run.sh uninstall-zsh    # Remove Zsh completion script
```

### Client (`archaic-cli`)

```bash
archaic-cli scan <path>           # Trigger filesystem scan
archaic-cli query <cwd> <input>   # Validate path
archaic-cli complete <prefix> [n] # Get completions
archaic-cli suggest <prefix>      # Get best suggestion
archaic-cli ping                  # Check daemon health
archaic-cli metrics               # View daemon metrics
archaic-cli scan-status           # Check scan progress
archaic-cli save <path>           # Save state to file
archaic-cli shutdown              # Stop daemon
```

### Helper (Persistent Connection)

```bash
archaic-helper [socket_path]      # Start persistent helper
# Then pipe commands: echo "complete /path 20" | archaic-helper
```

The helper maintains a persistent connection to the daemon, eliminating the
overhead of socket setup for each query. This is what the Fish plugin uses
internally.

## Shell Integration

### Fish

Archaic provides two completion modes in Fish:

- **Tab completion** — press `Tab` to cycle through ranked path suggestions
- **Inline suggestions** — ghost text appears as a right-prompt suggestion

#### Accepting Suggestions

| Key | Action |
|---|---|
| `Tab` | Cycle through completions |
| `Alt+Right` | Accept inline suggestion |
| `Ctrl+R` | Accept inline suggestion |

### Bash

Archaic provides tab completion for Bash:

- **Tab completion** — press `Tab` to cycle through ranked path suggestions
- **Directory-only filtering** for `cd`, `mkdir`, `pushd`, `popd`, `rmdir`
- **Fuzzy fallback** when exact prefix match returns no results

#### Installation

```bash
./run.sh install-bash
# Or manually:
source ~/.local/share/bash-completion/completions/archaic.bash
```

### Zsh

Archaic provides tab completion for Zsh with the same features as Bash.

#### Installation

```zsh
# Add to ~/.zshrc:
source /path/to/archaic/zsh/archaic.zsh
```

### Neovim

Archaic provides integration with Neovim through two modes:

- **nvim-cmp source** — autocomplete paths while typing
- **Telescope picker** — fuzzy-find files with `:ArchaicFind`

#### Installation (lazy.nvim)

```lua
{
  "Samoreilly/Archaic",
  lazy = true,
  dir = "/path/to/archaic/nvim",
  config = function()
    require("archaic").setup({
      socket_path = "/tmp/archaic-daemon.sock",
      timeout_ms = 100,
      enable_cmp = true,
      enable_telescope = true,
    })
  end,
}
```

#### Manual Setup

```lua
-- Add to your init.lua or a plugin spec:
local archaic = require("archaic")
archaic.setup({})

-- User commands:
-- :ArchaicStatus   — check daemon status
-- :ArchaicReindex   — trigger rescan
-- :ArchaicToggle    — toggle completions
-- :ArchaicCheckHealth — health check

-- Telescope extension:
require("telescope").load_extension("archaic")
:Telescope archaic find_files
```

### Behavior (Both Shells)

- Commands listed in `[fish].commands` or `[bash].commands` trigger completions
- `cd` and `mkdir` filter to directories only
- The plugin monitors daemon health and falls back gracefully if unavailable
- Suggestions are ranked by the daemon's scoring algorithm

## Performance

Archaic's speed comes from several layered optimizations:

- **Path-compressed trie (radix tree)** — common path prefixes are stored once,
  reducing memory and traversal time
- **Binary search on sorted children** — child node lookups use `bsearch` for
  O(log n) instead of linear scan
- **O(log n) duplicate detection** — scored completions use binary search to
  avoid duplicates without hashing overhead
- **LRU cache with TTL** — repeated queries hit the cache instead of traversing
  the tree
- **Per-bucket refcounting** — lock-free concurrent reads via atomic reference
  counts on each hash bucket
- **4-thread parallel scanner** — filesystem indexing runs across multiple
  threads with work-stealing
- **Real-time filesystem watching** — inotify/kqueue push updates instead of
  periodic full rescans

## Fuzzy Matching

Archaic uses a 5-strategy scoring system for fuzzy path matching:

| Strategy | Priority | Example |
|----------|----------|---------|
| Exact prefix | 1000+ | `src/ma` → `src/main.c` |
| Segment tokenization | 100-500 | `srma` → `src/main.c`, `sio` → `src/io/` |
| Subsequence + path bonuses | 50-200 | `srcmai` → `src/main.c` |
| Levenshtein basename | 10-49 | `maim` → `main.c` (1 edit) |
| Levenshtein full path | 1-9 | `src/maim` → `src/main.c` |

Segment tokenization breaks paths on `/`, `.`, `-`, and `_` to match
acronyms: typing `sio` finds `src/io/`, typing `mvcm` finds `models/view_controller.cr`.

## Filesystem Watcher

Archaic watches for filesystem changes in real time:

- **Linux**: Uses `inotify` for immediate event notification
- **macOS**: Uses `kqueue` with `EVFILT_VNODE` for directory monitoring
- **Fallback**: Polling-based timer (configurable interval)

Watched directories are added recursively, skipping `.git`, `node_modules`,
`.venv`, `__pycache__`, `build`, and `dist`. New directories are automatically
added when created. Removed directories are cleaned up.

To force a full rescan regardless of the watcher, send SIGHUP or use
`archaic-cli scan <path>`.

## Troubleshooting

### Daemon won't start

- Check that the socket path is writable: `ls -la /tmp/archaic-daemon.sock`
- Remove stale socket files: `rm -f /tmp/archaic-daemon.sock`
- Verify the scan path exists and is readable

### No completions appearing

- Confirm the daemon is running: `./run.sh status`
- Verify the scan path is correct: `archaic-cli scan-status`
- Trigger a manual rescan: `./run.sh rescan`
- Check that paths exist under the configured `scan_path`

### Fish or Bash plugin not working

- Fish: source the plugin manually: `source ~/.config/fish/conf.d/archaic.fish`
- Bash: source the script manually: `source ~/.local/share/bash-completion/completions/archaic.bash`
- Verify the helper is installed and in your `PATH`
- Check that commands in `[fish].commands` or `[bash].commands` match your usage

### High memory usage

- Reduce `max_buckets` in the `[storage]` section
- Lower `max_nodes_per_bucket` to cap per-bucket growth
- Decrease `max_depth` in `[daemon]` to limit indexed directories
- Restart the daemon to apply configuration changes

## License

MIT License — see [LICENSE](LICENSE) for details.
