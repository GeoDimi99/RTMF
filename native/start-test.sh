#!/bin/bash
# =============================================================================
# start-test.sh
#
# Builds the Execution Manager service, launches it and measures it.
# This script starts the whole system: it generates the schedule plan from the
# manifest (tests/test_0_code/manifest.yaml, same format as the other
# implementations), compiles the C sources via CMake, runs the resulting
# binary, saves its output in results/<test-name>/ and generates the
# performance report there (report.pdf, perf.csv, stats.csv).
#
# Usage:
#   ./start-test.sh [test-name]        (the binary is run through `sudo -n`)
#   sudo ./start-test.sh [test-name]
#
#   test-name   optional; results go to results/<test-name>/. When omitted a
#               random results/test_<id>/ directory is created.
#
# Environment:
#   MANIFEST  Manifest to run (default: tests/test_0_code/manifest.yaml).
#   CMAKE     Override the cmake binary to use (default: auto-detected).
#   FORCE=1   Overwrite results/<test-name>/ if it already exists.
#
# Notes:
#   - Real-time scheduling (SCHED_FIFO) and `mlockall` require root. When the
#     script is not run as root, only the binary is run as root via `sudo -n`
#     (no password prompt), so unattended runs (run-all-tests.sh) need a
#     NOPASSWD sudoers rule for the binary; the script prints it if missing.
# =============================================================================

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EM_DIR="$ROOT_DIR/services/execution-manager"
BUILD_DIR="$EM_DIR/build"
BIN="$BUILD_DIR/execution-manager"
MANIFEST_PATH="$(realpath "${MANIFEST:-$ROOT_DIR/tests/test_0_code/manifest.yaml}")"

source "$ROOT_DIR/tools/report-lib.sh"

[ -f "$MANIFEST_PATH" ] || { echo "[ERROR] Manifest not found: $MANIFEST_PATH" >&2; exit 1; }

# The binary needs root: run it directly when we are root, otherwise via `sudo -n`
if [ "$(id -u)" = "0" ]; then
    RUN_AS_ROOT=()
elif sudo -n -l "$BIN" >/dev/null 2>&1; then
    RUN_AS_ROOT=(sudo -n)
else
    echo "[ERROR] The Execution Manager needs root (SCHED_FIFO, mlockall) and 'sudo -n' is not allowed for it." >&2
    echo "        Run this script with sudo, or allow the binary once without password:" >&2
    echo "          echo '$(id -un) ALL=(root) NOPASSWD: $BIN' | sudo tee /etc/sudoers.d/rtmf-native" >&2
    echo "          sudo chmod 440 /etc/sudoers.d/rtmf-native && sudo visudo -cf /etc/sudoers.d/rtmf-native" >&2
    exit 1
fi

# A build/ left by a previous `sudo` run is owned by root and cannot be rebuilt by this user
if [ "$(id -u)" != "0" ] && [ -e "$BUILD_DIR" ] && [ "$(find "$BUILD_DIR" ! -user "$(id -u)" -print -quit)" ]; then
    echo "[ERROR] $BUILD_DIR contains files owned by another user (left by a previous 'sudo' run)." >&2
    echo "        Fix it once with:  sudo chown -R $(id -un): $BUILD_DIR" >&2
    exit 1
fi

# Python used by the plan generator: it needs PyYAML (the report venv is used as a fallback)
PYTHON="$(command -v python3)"
if ! "$PYTHON" -c 'import yaml' 2>/dev/null; then
    if [ -x "$ROOT_DIR/tools/.venv/bin/python" ] && "$ROOT_DIR/tools/.venv/bin/python" -c 'import yaml' 2>/dev/null; then
        PYTHON="$ROOT_DIR/tools/.venv/bin/python"
    else
        echo "[ERROR] PyYAML is required to generate the schedule plan: pip install PyYAML" >&2
        exit 1
    fi
fi

rl_init_results "$ROOT_DIR" "${1:-}"
echo "[*] Manifest: $MANIFEST_PATH"

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
"$CMAKE_BIN" -S "$EM_DIR" -B "$BUILD_DIR" -DPLAN_MANIFEST="$MANIFEST_PATH" -DPython3_EXECUTABLE="$PYTHON"

echo "[*] Building Execution Manager (schedule plan generated from the manifest)..."
"$CMAKE_BIN" --build "$BUILD_DIR"

# When run with sudo, give build/ back to the invoking user so later non-root runs can rebuild it
if [ "$(id -u)" = "0" ] && [ -n "${SUDO_USER:-}" ]; then
    chown -R "$SUDO_USER:$(id -gn "$SUDO_USER")" "$BUILD_DIR" 2>/dev/null || true
fi

echo "[*] Launching Execution Manager... (Ctrl+C stops it early; the report uses the data collected so far)"
echo "------------------------------------------------------------"
trap ':' INT   # the binary handles SIGINT itself and exits cleanly
set +e
"${RUN_AS_ROOT[@]}" "$BIN" 2>&1 | tee -i "$LOG_DIR/execution-manager.log"
EM_RC=${PIPESTATUS[0]}
set -e
trap - INT
echo "------------------------------------------------------------"
echo "[*] Execution Manager terminated (exit code $EM_RC)."

REPORT_RC=0
rl_generate_report "$ROOT_DIR" "native" "$MANIFEST_PATH" || REPORT_RC=$?

echo "[*] Done. Results: $RESULTS_DIR"
exit $REPORT_RC
