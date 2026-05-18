# Contributing to Archaic

Thank you for your interest in contributing! This guide covers how to set up, develop, and submit changes.

## Development Setup

### Prerequisites

- C23 compiler (GCC 14+, Clang 18+, or Apple Clang 15+)
- CMake 3.22+
- libfmt-dev (or fmt via Homebrew)
- Fish, Bash, or Zsh (for shell plugin testing)

### Build

```bash
git clone https://github.com/Samoreilly/Archaic.git
cd Archaic
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

### Run Tests

```bash
# Unit tests (47 tests)
./build/archaic-unit

# Integration tests (15 tests)
./build/archaic "$HOME"

# Benchmarks
./build/archaic-bench

# Format check
clang-format --dry-run --Werror src/*.c src/*.h ipc/*.c ipc/*.h main.c test/*.c test/unit_tests.c test/bench.c
```

### ASan Build

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
make -j$(nproc)
./archaic "$HOME"
```

## Code Style

- **C23** standard with POSIX and pthreads
- **clang-format** enforced via CI (`.clang-format` config)
- Snake_case for functions and variables
- UPPERCASE for constants and macros
- Each module in `src/modulename.c` with `src/modulename.h`
- No external dependencies (except libfmt for logging)
- All new code must pass: build, lint, unit tests, integration tests

## Commit Messages

Use conventional commit format:

```
feat: add Levenshtein distance for fuzzy matching
fix: resolve ANSI color bug in Fish completions
docs: update README with Neovim plugin instructions
test: add cache eviction unit tests
refactor: extract scoring into separate functions
```

## Pull Request Process

1. Fork the repository
2. Create a feature branch (`git checkout -b feat/my-feature`)
3. Make changes with tests
4. Run all tests locally
5. Push and create PR against `main`

## Architecture

```
Fish/Bash/Zsh → archaic-helper (persistent) → Unix Socket → archaic-daemon
                                                        ├── Radix Tree (sorted, bsearch)
                                                        ├── Bucket Store (65536, LRU)
                                                        ├── Query Cache (LRU + TTL)
                                                        └── Parallel Scanner (4 threads)
```

Key modules:
- **src/trie.c** — Radix tree, scoring, fuzzy matching
- **src/scanner.c** — Multi-threaded filesystem scanner
- **src/cache.c** — Sharded query cache with TTL
- **src/helper.c** — Persistent connection helper for shells
- **ipc/protocol.h** — Binary IPC protocol (versioned)
- **src/config.c** — TOML configuration parser

## Reporting Issues

- Bug reports: GitHub Issues with reproduction steps
- Feature requests: GitHub Issues with use case description
- Security: Email sam@archaic.dev (if sensitive)