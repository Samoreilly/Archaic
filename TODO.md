# Archaic — Feature & Improvement TODO

Generated: 2026-05-17 | Status: Planning phase

## Core Engine

### Performance
- [ ] **Memory-mapped state persistence** — Replace fread/fwrite with mmap for instant state load (zero-copy)
- [ ] **Trie node pooling** — Pre-allocate nodes in chunks to reduce malloc overhead during scanning
- [ ] **Lock-free bucket access** — Replace per-bucket mutex with atomic CAS for read-heavy workloads
- [ ] **SIMD-optimized string comparison** — Use SSE/AVX for path prefix matching in trie traversal
- [ ] **Adaptive thread pool** — Scale scanner threads based on CPU count and I/O wait
- [ ] **Completion result streaming** — Stream results over IPC instead of buffering entire response
- [ ] **Hot path optimization** — Inline critical trie traversal functions with `__attribute__((always_inline))`

### Scoring & Ranking
- [ ] **TF-IDF scoring** — Weight rare paths higher than common ones (e.g., `src/` vs `.git/hooks/`)
- [ ] **Contextual awareness** — Boost completions based on current working directory proximity
- [ ] **Usage decay function** — Exponential decay for recency instead of linear timestamp comparison
- [ ] **File type awareness** — Prefer executables/scripts when completing after commands like `./`
- [ ] **Learned preferences** — Track which completions users actually select and adjust weights

### Fuzzy Matching
- [ ] **Levenshtein distance fallback** — When subsequence matching returns too many results, rank by edit distance
- [ ] **Typo correction** — Suggest "Did you mean?" for common misspellings of known paths
- [ ] **CamelCase/snake_case awareness** — Match `myF` against `my_file.c` and `MyFile.c`
- [ ] **Fuzzy scoring integration** — Blend fuzzy match quality with frequency/recency scores

## Shell Integration

### Fish
- [ ] **ZSH completion support** — Port Fish plugin to ZSH with equivalent tab completion + suggestions
- [ ] **Bash suggestion display** — Implement inline ghost text suggestions for Bash (currently tab-only)
- [ ] **Command-specific scoring** — Different completion behavior for `cd` vs `vim` vs `cat`
- [ ] **Multi-command chaining** — Complete paths in `cd dir && vim file` scenarios
- [ ] **Git branch completion** — Integrate with git to complete branch names after `git checkout`
- [ ] **Environment variable expansion** — Complete `$HOME/` and other env vars in paths

### UX
- [ ] **Color-coded completions** — Directories in blue, executables in green, symlinks in cyan
- [ ] **File size/type hints** — Show file size and type alongside completion entries
- [ ] **Recent files list** — Quick access to recently accessed paths
- [ ] **Bookmark system** — User-defined shortcuts for frequently accessed directories

## Daemon & IPC

### Reliability
- [ ] **Daemon auto-restart** — systemd watchdog integration for automatic crash recovery
- [ ] **Graceful degradation** — Serve cached results when scanner is busy or crashed
- [ ] **Health check endpoint** — HTTP endpoint for monitoring tools (Prometheus, Nagios)
- [ ] **Socket activation** — systemd socket activation for on-demand daemon startup
- [ ] **Multiple daemon instances** — Support scanning multiple root paths with separate daemons

### IPC Protocol
- [ ] **Protocol v2** — Add compression (zstd) for large completion responses
- [ ] **Bidirectional streaming** — Allow daemon to push updates to clients (scan progress, new paths)
- [ ] **Authentication** — Optional socket credentials check for multi-user systems
- [ ] **Batch queries** — Send multiple completion requests in single IPC call

## Configuration & Setup

### Config
- [ ] **`.archaicignore` file** — Per-project ignore file (like `.gitignore`) that merges with global config
- [ ] **Config validation** — Validate config file on daemon startup with helpful error messages
- [ ] **Hot config reload** — SIGHUP triggers config reload without daemon restart
- [ ] **Config schema documentation** — JSON schema for IDE autocomplete on config files

### Installation
- [ ] **Homebrew formula** — `brew install archaic` for macOS users
- [ ] **APT/YUM packages** — Debian and RPM package builds via GitHub Actions
- [ ] **NixOS module** — Declarative configuration for NixOS users
- [ ] **Docker image** — Containerized daemon for CI/CD environments
- [ ] **Windows support** — WSL2 native or native Windows with Win32 APIs

## Developer Experience

### Tooling
- [ ] **CLI dashboard** — `archaic-cli dashboard` showing real-time metrics, top paths, cache hit rate
- [ ] **Query profiler** — `archaic-cli profile <prefix>` showing which trie nodes were traversed
- [ ] **Scan progress bar** — Visual progress indicator during initial scan
- [ ] **Path index browser** — `archaic-cli browse` to explore indexed paths interactively

### Testing
- [ ] **Property-based tests** — QuickCheck-style tests for trie invariants
- [ ] **Fuzz testing** — AFL/libFuzzer for IPC protocol parser
- [ ] **Benchmark regression detection** — CI fails if perf benchmarks degrade >10%
- [ ] **Memory leak detection** — Valgrind/ASan integration in CI

### Documentation
- [ ] **Architecture diagram** — Visual diagram of daemon, scanner, trie, cache, IPC flow
- [ ] **Contributing guide** — Code style, testing requirements, PR template
- [ ] **API reference** — Complete IPC protocol documentation
- [ ] **Performance tuning guide** — How to optimize for specific workloads (large monorepos, etc.)

## Advanced Features

### Intelligence
- [ ] **Project detection** — Auto-detect project roots (package.json, Cargo.toml, CMakeLists.txt) and prioritize them
- [ ] **Language-aware filtering** — Show `.py` files more prominently in Python projects, `.rs` in Rust, etc.
- [ ] **Import path completion** — Complete module imports (`import numpy as np` → suggest `numpy/` paths)
- [ ] **Alias system** — User-defined shortcuts (`~proj` → `/home/user/projects`)

### Integration
- [ ] **VS Code extension** — Native autocomplete provider using archaic daemon
- [ ] **Neovim plugin** — LSP-compatible completion source
- [ ] **Emacs package** — Company-mode integration
- [ ] **tmux integration** — Completion pane for tmux sessions

### Security
- [ ] **Path traversal protection** — Validate that completions don't escape scan root
- [ ] **Symlink following control** — Configurable symlink depth limit
- [ ] **Permission-aware scanning** — Skip directories without read permission gracefully
- [ ] **Audit logging** — Log all completion queries for compliance environments

## Infrastructure

### CI/CD
- [ ] **Release automation** — Automated version tagging, changelog generation, binary releases
- [ ] **Cross-compilation** — Build for ARM64 (Raspberry Pi, Apple Silicon) in CI
- [ ] **Coverage reporting** — Codecov integration with PR comments
- [ ] **Nightly builds** — Automated nightly builds with latest features

### Monitoring
- [ ] **OpenTelemetry export** — Export metrics to Jaeger, Grafana, Datadog
- [ ] **Alert rules** — Pre-configured alerts for high memory, low cache hit rate, scan failures
- [ ] **Usage analytics** — Anonymous opt-in telemetry for feature prioritization
