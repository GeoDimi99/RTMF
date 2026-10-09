#!/bin/bash
# ============================================================================
#  start-test.sh  (choreography-mqueue)
#
#  Bring up the whole choreography-mqueue system and measure it:
#    1. Read the test manifest (DSL)
#    2. Build the task-wrapper image and one Docker image per task defined in
#       the manifest (SDK image-builder)
#    3. Build the infrastructure images via docker compose
#    4. Clean stale POSIX message queues
#    5. Start the stack (redis + deploy-manager; deploy-manager then starts
#       the task containers)
#    6. Follow the task containers until all of them finish their iterations
#    7. Save the logs in results/<test-name>/ and generate the performance
#       report (report.pdf, perf.csv, stats.csv) there
#
#  Usage:
#       ./start-test.sh [test-name]
#
#    test-name   optional; results go to results/<test-name>/. When omitted a
#                random results/test_<id>/ directory is created.
#
#  Environment:
#    MANIFEST    manifest to run (default: tests/test_0_code/manifest.yaml)
#    FORCE=1     overwrite results/<test-name>/ if it already exists
#
#  Ctrl+C stops the test early; the report is built from the data collected
#  so far.
# ============================================================================

set -euo pipefail

# --- Resolve script directory so the script can be launched from anywhere ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
source "$SCRIPT_DIR/tools/report-lib.sh"

# --- Resolve the manifest path ---------------------------------------------
MANIFEST_PATH="${MANIFEST:-tests/test_0_code/manifest.yaml}"
CONTEXT="$(dirname "$MANIFEST_PATH")/"

rl_init_results "$SCRIPT_DIR" "${1:-}"

if [ ! -f "$MANIFEST_PATH" ]; then
    echo "Error: Manifest file not found at $MANIFEST_PATH"
    exit 1
fi

echo "[INFO] Using manifest: $MANIFEST_PATH"

# --- Task container names (= image aliases) declared in the manifest -------
TASK_NAMES=$(rl_extract_aliases "$MANIFEST_PATH" | sort -u)

if [ -z "$TASK_NAMES" ]; then
    echo "[WARN] No 'alias:' entries found in $MANIFEST_PATH"
fi

# --- Cleanup any previous run ----------------------------------------------
echo "[INFO] Stopping previous stack (if any)..."
docker compose down --remove-orphans || true

# Force-remove the fixed-name infrastructure containers in case a previous
# run left them behind under a different compose project.
for c in redis deploy-manager; do
    docker rm -f "$c" 2>/dev/null || true
done

# Force-remove any leftover task containers named after the manifest images.
for img in $TASK_NAMES; do
    docker rm -f "$img" 2>/dev/null || true
done

# --- Build one Docker image per task using the task-wrapper Dockerfile ----
docker build -t rtmf/task-wrapper:choreography-mqueue -f services/task-wrapper/Dockerfile .
python3 sdk/image-builder/src/main.py -f "$MANIFEST_PATH" -c "$CONTEXT"

# --- Build infrastructure services (deploy-manager, etc.) -----------------
echo "[INFO] Building infrastructure services..."
docker compose build

# --- Remove stale POSIX message queues ------------------------------------
if [ -d /dev/mqueue ]; then
    echo "[INFO] Cleaning /dev/mqueue/*"
    rm -f /dev/mqueue/* 2>/dev/null || sudo rm -f /dev/mqueue/* 2>/dev/null || true
fi

# --- Start the whole stack (detached) --------------------------------------
echo "[INFO] Starting the choreography stack..."
docker compose up --build -d

# --- Follow every task container until it finishes ------------------------
echo ""
echo "[INFO] Following task container logs. Press Ctrl+C to stop the test early."
echo "----------------------------------------------------------------"

stop_all() {
    echo; echo "[INFO] Interrupted: stopping the stack, reporting on the data collected so far..."
    docker compose stop >/dev/null 2>&1 || true
    for n in $TASK_NAMES; do docker stop "$n" >/dev/null 2>&1 || true; done
}
trap stop_all INT TERM

pids=()
for n in $TASK_NAMES; do
    rl_follow_container "$n" &
    pids+=($!)
done
wait "${pids[@]}" || true
wait || true   # after an interrupt: let the log followers drain

echo "----------------------------------------------------------------"
for n in $TASK_NAMES; do
    code=$(docker inspect --type container -f '{{.State.ExitCode}}' "$n" 2>/dev/null || echo "missing")
    [ "$code" = "0" ] || echo "[WARN] Task container '$n' exited with code $code"
done

# --- Save the remaining logs and stop the stack ----------------------------
rl_save_container_logs deploy-manager
docker compose stop >/dev/null 2>&1 || true

# --- Performance report ----------------------------------------------------
REPORT_RC=0
rl_generate_report "$SCRIPT_DIR" "choreography-mqueue" "$MANIFEST_PATH" || REPORT_RC=$?

echo "[INFO] Done. Results: $RESULTS_DIR"
exit $REPORT_RC
