#!/bin/bash
# =============================================================================
# start-test.sh
#
# Builds the Execution Manager service, launches it and measures it.
# This script starts the whole system: it compiles the C sources via CMake,
# runs the resulting binary (which executes the schedule defined in
# `services/execution-manager/src/main.c`), saves its output in
# results/<test-name>/ and generates the performance report there
# (report.pdf, perf.csv, stats.csv).
#
# Usage:
#   sudo ./start-test.sh [test-name]
#
#   test-name   optional; results go to results/<test-name>/. When omitted a
#               random results/test_<id>/ directory is created.
#
# Environment:
#   CMAKE   Override the cmake binary to use (default: auto-detected).
#   FORCE=1 Overwrite results/<test-name>/ if it already exists.
#
# Notes:
#   - Real-time scheduling (SCHED_FIFO) and `mlockall` require elevated
#     privileges. Run with `sudo` if you hit EPERM errors, or grant the
#     appropriate capabilities (e.g. CAP_SYS_NICE, CAP_IPC_LOCK).
# =============================================================================

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EM_DIR="$ROOT_DIR/services/execution-manager"
BUILD_DIR="$EM_DIR/build"
BIN="$BUILD_DIR/execution-manager"

source "$ROOT_DIR/tools/report-lib.sh"
rl_init_results "$ROOT_DIR" "${1:-}"

# Resolve a working cmake: try $CMAKE, then common system locations, then PATH.
# This avoids picking up broken/wrong-arch binaries (e.g. ~/.local/bin/cmake).
find_cmake() {
    local candidates=("${CMAKE:-}" /usr/bin/cmake /usr/local/bin/cmake /bin/cmake)
    for c in "${candidates[@]}"; do
        if [[ -n "$c" && -x "$c" ]] && "$c" --version >/dev/null 2>&1; then
            echo "$c"; return 0
        fi
    done
    while IFS= read -r c; do
        if "$c" --version >/dev/null 2>&1; then echo "$c"; return 0; fi
    done < <(command -v -a cmake 2>/dev/null || true)
    return 1
}

CMAKE_BIN="$(find_cmake)" || {
    echo "[ERROR] No working 'cmake' found. Install cmake (>= 3.20) or set CMAKE=/path/to/cmake." >&2
    exit 1
}
echo "[*] Using cmake: $CMAKE_BIN ($("$CMAKE_BIN" --version | head -n1))"

# If build/ was generated from a different absolute path (e.g. this repo was
# copied, cloned elsewhere, or moved), CMake's cached paths in CMakeCache.txt
# no longer match and configuration fails with a "different directory" error.
# Detect that and discard the stale cache so this script works regardless of
# where the repo currently lives.
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    cached_source="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt")"
    if [[ -n "$cached_source" && "$cached_source" != "$EM_DIR" ]]; then
        echo "[*] build/ was configured from a different path ($cached_source); resetting it..."
        rm -rf "$BUILD_DIR"
    fi
fi

echo "[*] Configuring Execution Manager (CMake)..."
"$CMAKE_BIN" -S "$EM_DIR" -B "$BUILD_DIR"

echo "[*] Building Execution Manager..."
"$CMAKE_BIN" --build "$BUILD_DIR"

echo "[*] Launching Execution Manager... (Ctrl+C stops it early; the report uses the data collected so far)"
echo "------------------------------------------------------------"
trap ':' INT   # the binary handles SIGINT itself and exits cleanly
set +e
"$BIN" 2>&1 | tee -i "$LOG_DIR/execution-manager.log"
EM_RC=${PIPESTATUS[0]}
set -e
trap - INT
echo "------------------------------------------------------------"
echo "[*] Execution Manager terminated (exit code $EM_RC)."

REPORT_RC=0
rl_generate_report "$ROOT_DIR" "native" || REPORT_RC=$?

echo "[*] Done. Results: $RESULTS_DIR"
exit $REPORT_RC
