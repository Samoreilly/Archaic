# Archaic — 50 High-Value TODOs

Generated: 2026-05-18 | Status: Implementation phase
Reconciled: 2026-09-13 — items verified shipped are checked (many via
test/integration/ contract tests); the PE backlog tracker is
FEATURES.md (all 8 items done+pushed).

## Wave 1: Core Performance & Reliability (1-10)
- [ ] **1. IPC payload compression** — zstd compress large completion responses (>1KB)
- [x] **2. Connection pooling for helper** — Reuse helper-daemon socket across multiple queries (shipped: persistent --serve/--ask, tested)
- [x] **3. Async scan with inotify/fsevents** — Real-time filesystem watching instead of periodic rescan (shipped: watcher triggers prompt rescans)
- [ ] **4. Trie compaction** — Merge single-child nodes to reduce memory footprint
- [x] **5. LRU cache persistence** — Save/restore cache entries across daemon restarts (shipped: hot cache save/restore)
- [x] **6. Query result deduplication** — Prevent duplicate paths in completion results (shipped: server-side dedup set)
- [ ] **7. Symlink resolution cache** — Cache resolved symlink targets to avoid repeated stat() calls
- [ ] **8. Scanner checkpoint/resume** — Save scan progress and resume after interruption
- [x] **9. Memory budget enforcement** — Hard limit on total heap usage with graceful eviction (shipped + tested)
- [x] **10. Crash-safe state file** — Atomic write with rename() to prevent corruption on crash (shipped)

## Wave 2: Shell Integration & UX (11-20)
- [x] **11. ZSH completion plugin** — Port Fish plugin to ZSH with equivalent features (shipped, contract-tested)
- [x] **12. Bash inline suggestions** — Ghost text via READLINE_LINE/READLINE_POINT (shipped: Tab render + live accept, tested)
- [ ] **13. Fish completion metadata** — Show file size, modification time in completion descriptions
- [x] **14. Command context awareness** — Different scoring for `vim` (prefer source files) vs `cd` (prefer dirs) (shipped: dirs-only + per-command scoring)
- [x] **15. Environment variable expansion** — Complete `$HOME/`, `$PROJECT/` in paths (shipped in all shells)
- [x] **16. Tilde expansion in helper** — Resolve `~/` to home directory before querying (shipped)
- [ ] **17. Fish completion grouping** — Group results by directory with separators
- [x] **18. Bash fallback to native completion** — Merge archaic results with bash-builtins (shipped: `complete -o default`)
- [ ] **19. Completion debounce tuning** — Adaptive debounce based on typing speed
- [ ] **20. Fish right-prompt performance** — Cache suggestions to avoid blocking prompt rendering

## Wave 3: Scoring & Intelligence (21-30)
- [ ] **21. Git-aware scoring** — Boost files in git-tracked directories, ignore untracked
- [ ] **22. File extension relevance** — Prefer `.c`/`.h` when in C projects, `.py` in Python
- [ ] **23. Directory depth penalty** — Penalize deeply nested paths unless frequently used
- [ ] **24. Time-of-day awareness** — Boost work-related paths during work hours
- [x] **25. Session-based learning** — Track completions accepted during current session (shipped + tested)
- [ ] **26. Cross-project frequency** — Share frequency data across multiple scan roots
- [ ] **27. Executable bit awareness** — Prefer executable files when completing after `./`
- [x] **28. Hidden file demotion** — Push dotfiles/dotdirs to end unless explicitly typed (shipped: hidden_file_penalty)
- [ ] **29. Path segment tokenization** — Match `srcio` against `src/io/` (skip separators)
- [ ] **30. Recent-first mode** — Option to sort by recency instead of composite score

## Wave 4: Security & Robustness (31-40)
- [x] **31. Path traversal guard** — Reject completions that escape configured scan roots (shipped: dotdot rejection + normalization)
- [x] **32. Socket permission hardening** — Restrict Unix socket to owner-only (0700) (shipped, plus SO_PEERCRED)
- [x] **33. IPC message size limits** — Enforce max payload size to prevent DoS (shipped: payload/want caps)
- [ ] **34. Graceful OOM handling** — Detect allocation failures and shed load gracefully
- [x] **35. Config file sandbox** — Validate all config values against safe ranges (shipped: sandbox validate + clamps)
- [x] **36. Signal-safe shutdown** — Ensure trie state is saved before SIGTERM exit (shipped: TERM handler saves state)
- [x] **37. Duplicate daemon detection** — Prevent multiple daemons on same socket (shipped)
- [x] **38. Stale socket cleanup** — Auto-remove orphaned socket files on startup (shipped: daemon + doctor + shells)
- [x] **39. Scanner permission errors** — Log and skip unreadable directories without crashing (shipped)
- [ ] **40. IPC protocol fuzzing** — Add fuzz test for malformed IPC messages

## Wave 5: Developer Experience & Infrastructure (41-50)
- [x] **41. `archaic-cli stats` command** — Show top-10 most-completed paths, cache hit rate (shipped: stats command)
- [x] **42. `archaic-cli clear-cache` command** — Flush query cache without restart (shipped)
- [x] **43. `archaic-cli reindex` command** — Force full re-scan with progress output (shipped)
- [x] **44. Integration test framework** — Shell-based tests that verify Fish/Bash completions (shipped: test/integration/, 110 checks)
- [ ] **45. AddressSanitizer CI job** — Run tests with ASan to catch memory bugs
- [x] **46. Benchmark suite** — Microbenchmarks for trie insert, query, fuzzy match (shipped: archaic-bench + baselines)
- [x] **47. Man page for archaic-helper** — Document helper commands and usage (shipped: man/archaic-helper.1)
- [ ] **48. Shellcheck for Fish plugin** — Add fish -n linting to CI
- [x] **49. Release versioning** --version flag on all binaries, git tag integration (shipped: --version on all binaries)
- [ ] **50. Uninstall target verification** — `make uninstall` removes all installed files
