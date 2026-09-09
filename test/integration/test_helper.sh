#!/usr/bin/env bash
# test_helper.sh - Common functions for archaic integration tests
#
# Usage: source this file from test scripts
#   source "$(dirname "$0")/test_helper.sh"

# ── Configuration ─────────────────────────────────────────────────────────────
ARCHAIC_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCHAIC_BIN="${ARCHAIC_ROOT}/build/archaic"
ARCHAIC_CLI="${ARCHAIC_ROOT}/build/archaic-cli"
TEST_SOCK="/tmp/archaic-test-daemon.sock"
TEST_SCAN_PATH="${ARCHAIC_ROOT}"
TEST_TIMEOUT=10
TEST_PING_RETRIES=20
TEST_PING_INTERVAL=0.2

# ── Counters ──────────────────────────────────────────────────────────────────
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0
CURRENT_TEST=""

# ── Colors ────────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m' # No Color

# ── Daemon lifecycle ─────────────────────────────────────────────────────────

# Start the daemon for testing.
# Uses a unique socket path to avoid conflicts with production daemon.
start_daemon() {
    local scan_path="${1:-$TEST_SCAN_PATH}"

    # Clean up any previous test daemon
    stop_daemon 2>/dev/null
    rm -f "$TEST_SOCK" "${TEST_SOCK}.pid"

    # Start daemon in background
    "$ARCHAIC_BIN" --daemon "$scan_path" "$TEST_SOCK" &>/dev/null &
    local daemon_pid=$!

    # Wait for daemon to become responsive
    local retries=0
    while [ $retries -lt $TEST_PING_RETRIES ]; do
        if [ -S "$TEST_SOCK" ]; then
            if "$ARCHAIC_CLI" --sock "$TEST_SOCK" ping &>/dev/null; then
                return 0
            fi
        fi
        sleep "$TEST_PING_INTERVAL"
        retries=$((retries + 1))
    done

    # Daemon didn't start in time
    kill "$daemon_pid" 2>/dev/null
    wait "$daemon_pid" 2>/dev/null
    return 1
}

# Stop the test daemon.
stop_daemon() {
    if [ -S "$TEST_SOCK" ]; then
        # Try graceful shutdown via IPC
        "$ARCHAIC_CLI" --sock "$TEST_SOCK" shutdown &>/dev/null 2>&1 || true
        sleep 0.5
    fi

    # Kill by PID file if it exists
    if [ -f "${TEST_SOCK}.pid" ]; then
        local pid
        pid=$(cat "${TEST_SOCK}.pid" 2>/dev/null)
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null || true
            sleep 0.2
            kill -9 "$pid" 2>/dev/null || true
        fi
        rm -f "${TEST_SOCK}.pid"
    fi

    # Clean up socket
    rm -f "$TEST_SOCK"
}

# ── Query helpers ─────────────────────────────────────────────────────────────

# Send a complete query to the daemon.
# Usage: query_complete <prefix> [limit] [cwd]
# Returns completions on stdout (format: "D /path" or "F /path")
query_complete() {
    local prefix="${1:-}"
    local limit="${2:-20}"
    local cwd="${3:-}"
    "$ARCHAIC_CLI" --sock "$TEST_SOCK" complete "$prefix" "$limit" "$cwd" 2>/dev/null
}

# Send a fuzzy query to the daemon.
# Usage: query_fuzzy <prefix> [limit]
# Returns fuzzy completions on stdout (format: "D /path" or "F /path")
query_fuzzy() {
    local prefix="${1:-}"
    local limit="${2:-20}"
    "$ARCHAIC_CLI" --sock "$TEST_SOCK" fuzzy "$prefix" "$limit" 2>/dev/null
}

# Send a ping to the daemon.
# Returns 0 if daemon is alive.
query_ping() {
    "$ARCHAIC_CLI" --sock "$TEST_SOCK" ping &>/dev/null
}

# Get daemon metrics.
# Returns metrics output on stdout.
query_metrics() {
    "$ARCHAIC_CLI" --sock "$TEST_SOCK" metrics 2>/dev/null
}

# Get daemon stats (formatted).
query_stats() {
    "$ARCHAIC_CLI" --sock "$TEST_SOCK" stats 2>/dev/null
}

# ── Assertion helpers ─────────────────────────────────────────────────────────

# Assert that output contains the expected string.
# Usage: assert_contains <output> <expected>
assert_contains() {
    local output="$1"
    local expected="$2"
    if echo "$output" | grep -qF "$expected"; then
        return 0
    else
        return 1
    fi
}

# Assert that output contains a regex pattern.
# Usage: assert_matches <output> <regex>
assert_matches() {
    local output="$1"
    local regex="$2"
    if echo "$output" | grep -qE "$regex"; then
        return 0
    else
        return 1
    fi
}

# Assert that the number of non-empty lines in output is at least min_count.
# Usage: assert_count <output> <min_count>
assert_count() {
    local output="$1"
    local min_count="$2"
    local actual
    actual=$(echo "$output" | grep -c '.' 2>/dev/null || echo 0)
    if [ "$actual" -ge "$min_count" ]; then
        return 0
    else
        return 1
    fi
}

# Assert that a command exits with code 0.
# Usage: assert_success <command...>
assert_success() {
    "$@" &>/dev/null
    return $?
}

# Assert that output is non-empty.
# Usage: assert_non_empty <output>
assert_non_empty() {
    local output="$1"
    if [ -n "$output" ]; then
        return 0
    else
        return 1
    fi
}

# Assert that output starts with a given prefix.
# Usage: assert_starts_with <output> <prefix>
assert_starts_with() {
    local output="$1"
    local prefix="$2"
    case "$output" in
        "${prefix}"*) return 0 ;;
        *) return 1 ;;
    esac
}

# ── Test framework ────────────────────────────────────────────────────────────

# Begin a test case.
# Usage: test_begin "description"
test_begin() {
    CURRENT_TEST="$1"
    TESTS_RUN=$((TESTS_RUN + 1))
}

# Mark the current test as passed.
test_pass() {
    TESTS_PASSED=$((TESTS_PASSED + 1))
    printf "  ${GREEN}PASS${NC}  %s\n" "$CURRENT_TEST"
    CURRENT_TEST=""
}

# Mark the current test as failed with an optional message.
test_fail() {
    local msg="${1:-}"
    TESTS_FAILED=$((TESTS_FAILED + 1))
    if [ -n "$msg" ]; then
        printf "  ${RED}FAIL${NC}  %s (%s)\n" "$CURRENT_TEST" "$msg"
    else
        printf "  ${RED}FAIL${NC}  %s\n" "$CURRENT_TEST"
    fi
    CURRENT_TEST=""
}

# Assert helper that auto-reports pass/fail.
# Usage: assert <condition_command>
assert() {
    if "$@" &>/dev/null; then
        test_pass
    else
        test_fail "assertion failed: $*"
    fi
}

# Print test summary.
test_summary() {
    echo ""
    echo "─────────────────────────────────────────"
    printf "Tests: %d  " "$TESTS_RUN"
    printf "${GREEN}Passed: %d${NC}  " "$TESTS_PASSED"
    printf "${RED}Failed: %d${NC}\n" "$TESTS_FAILED"
    echo "─────────────────────────────────────────"

    if [ "$TESTS_FAILED" -gt 0 ]; then
        return 1
    fi
    return 0
}

# ── Cleanup on exit ───────────────────────────────────────────────────────────
# Register cleanup to stop daemon when test script exits.
register_cleanup() {
    trap 'stop_daemon 2>/dev/null' EXIT INT TERM
}
