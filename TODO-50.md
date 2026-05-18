# Archaic — 50 High-Value TODOs

Generated: 2026-05-18 | Status: Implementation phase

## Wave 1: Core Performance & Reliability (1-10)
- [ ] **1. IPC payload compression** — zstd compress large completion responses (>1KB)
- [ ] **2. Connection pooling for helper** — Reuse helper-daemon socket across multiple queries
- [ ] **3. Async scan with inotify/fsevents** — Real-time filesystem watching instead of periodic rescan
- [ ] **4. Trie compaction** — Merge single-child nodes to reduce memory footprint
- [ ] **5. LRU cache persistence** — Save/restore cache entries across daemon restarts
- [ ] **6. Query result deduplication** — Prevent duplicate paths in completion results
- [ ] **7. Symlink resolution cache** — Cache resolved symlink targets to avoid repeated stat() calls
- [ ] **8. Scanner checkpoint/resume** — Save scan progress and resume after interruption
- [ ] **9. Memory budget enforcement** — Hard limit on total heap usage with graceful eviction
- [ ] **10. Crash-safe state file** — Atomic write with rename() to prevent corruption on crash

## Wave 2: Shell Integration & UX (11-20)
- [ ] **11. ZSH completion plugin** — Port Fish plugin to ZSH with equivalent features
- [ ] **12. Bash inline suggestions** — Ghost text via READLINE_LINE/READLINE_POINT
- [ ] **13. Fish completion metadata** — Show file size, modification time in completion descriptions
- [ ] **14. Command context awareness** — Different scoring for `vim` (prefer source files) vs `cd` (prefer dirs)
- [ ] **15. Environment variable expansion** — Complete `$HOME/`, `$PROJECT/` in paths
- [ ] **16. Tilde expansion in helper** — Resolve `~/` to home directory before querying
- [ ] **17. Fish completion grouping** — Group results by directory with separators
- [ ] **18. Bash fallback to native completion** — Merge archaic results with bash-builtins
- [ ] **19. Completion debounce tuning** — Adaptive debounce based on typing speed
- [ ] **20. Fish right-prompt performance** — Cache suggestions to avoid blocking prompt rendering

## Wave 3: Scoring & Intelligence (21-30)
- [ ] **21. Git-aware scoring** — Boost files in git-tracked directories, ignore untracked
- [ ] **22. File extension relevance** — Prefer `.c`/`.h` when in C projects, `.py` in Python
- [ ] **23. Directory depth penalty** — Penalize deeply nested paths unless frequently used
- [ ] **24. Time-of-day awareness** — Boost work-related paths during work hours
- [ ] **25. Session-based learning** — Track completions accepted during current session
- [ ] **26. Cross-project frequency** — Share frequency data across multiple scan roots
- [ ] **27. Executable bit awareness** — Prefer executable files when completing after `./`
- [ ] **28. Hidden file demotion** — Push dotfiles/dotdirs to end unless explicitly typed
- [ ] **29. Path segment tokenization** — Match `srcio` against `src/io/` (skip separators)
- [ ] **30. Recent-first mode** — Option to sort by recency instead of composite score

## Wave 4: Security & Robustness (31-40)
- [ ] **31. Path traversal guard** — Reject completions that escape configured scan roots
- [ ] **32. Socket permission hardening** — Restrict Unix socket to owner-only (0700)
- [ ] **33. IPC message size limits** — Enforce max payload size to prevent DoS
- [ ] **34. Graceful OOM handling** — Detect allocation failures and shed load gracefully
- [ ] **35. Config file sandbox** — Validate all config values against safe ranges
- [ ] **36. Signal-safe shutdown** — Ensure trie state is saved before SIGTERM exit
- [ ] **37. Duplicate daemon detection** — Prevent multiple daemons on same socket
- [ ] **38. Stale socket cleanup** — Auto-remove orphaned socket files on startup
- [ ] **39. Scanner permission errors** — Log and skip unreadable directories without crashing
- [ ] **40. IPC protocol fuzzing** — Add fuzz test for malformed IPC messages

## Wave 5: Developer Experience & Infrastructure (41-50)
- [ ] **41. `archaic-cli stats` command** — Show top-10 most-completed paths, cache hit rate
- [ ] **42. `archaic-cli clear-cache` command** — Flush query cache without restart
- [ ] **43. `archaic-cli reindex` command** — Force full re-scan with progress output
- [ ] **44. Integration test framework** — Shell-based tests that verify Fish/Bash completions
- [ ] **45. AddressSanitizer CI job** — Run tests with ASan to catch memory bugs
- [ ] **46. Benchmark suite** — Microbenchmarks for trie insert, query, fuzzy match
- [ ] **47. Man page for archaic-helper** — Document helper commands and usage
- [ ] **48. Shellcheck for Fish plugin** — Add fish -n linting to CI
- [ ] **49. Release versioning** --version flag on all binaries, git tag integration
- [ ] **50. Uninstall target verification** — `make uninstall` removes all installed files
