#!/usr/bin/env bash
#
# start-test.sh  (orchestration-grpc)
#
# End-to-end build, run and benchmark for the RealTime Microservices stack.
#
#   1. Build the task-wrapper base image
#   2. Build the per-task images (via the SDK image-builder)
#   3. Build deploy-manager and execution-manager
#   4. Start Redis
#   5. Run deploy-manager (provisions task-services and populates Redis)
#   6. Run execution-manager (executes the schedule via gRPC)
#   7. Save the logs and cleanup
#   8. Generate the performance report (report.pdf, perf.csv, stats.csv)
#
# Usage:
#   ./start-test.sh [test-name]
#
#   test-name   optional; results go to results/<test-name>/. When omitted a
#               random results/test_<id>/ directory is created.
#
# Environment:
#   MANIFEST    manifest to run (default: task/manifest.yaml; a custom one is
#               copied over task/manifest.yaml)
#   FORCE=1     overwrite results/<test-name>/ if it already exists
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
source "$SCRIPT_DIR/tools/report-lib.sh"

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

TOTAL=9
step() { echo -e "\n${GREEN}▶ [$1/$TOTAL] $2${NC}\n"; }
fail() { echo -e "\n${RED}✖ Error at step $1: $2${NC}\n" >&2; exit 1; }

MANIFEST_FILE="task/manifest.yaml"
SOURCE_MANIFEST="${MANIFEST:-$MANIFEST_FILE}"

rl_init_results "$SCRIPT_DIR" "${1:-}"

if [ "$SOURCE_MANIFEST" != "$MANIFEST_FILE" ]; then
  if [ ! -f "$SOURCE_MANIFEST" ]; then
    fail 0 "Source manifest not found: $SOURCE_MANIFEST"
  fi
  cp "$SOURCE_MANIFEST" "$MANIFEST_FILE"
fi

echo -e "\n=============================================="
echo -e "${GREEN}    RealTime Microservices  -  $SOURCE_MANIFEST${NC}"
echo -e "==============================================\n"

# 0 — Pre-clean any stale containers
docker compose down >/dev/null 2>&1 || true
# Remove any stray fixed-name containers from previous runs (different compose project, etc.)
for c in redis deploy-manager execution-manager-grpc; do
  docker rm -f "$c" >/dev/null 2>&1 || true
done
ACTIVE_CONTAINERS=$(docker ps -aq --filter "name=task-service-" || true)
if [ -n "$ACTIVE_CONTAINERS" ]; then
  docker rm -f $ACTIVE_CONTAINERS >/dev/null 2>&1 || true
fi

# 1 — Build task-wrapper base image
step 1 "docker build task-wrapper"
docker build --no-cache -t jeffadac/realtime-microservices:task-wrapper \
  -f services/task-wrapper/Dockerfile . \
  || fail 1 "docker build task-wrapper"

# 2 — Build per-task images via SDK image-builder
step 2 "python3 image-builder (task manifest)"
python3 sdk/image-builder/src/main.py -f "$MANIFEST_FILE" -c task --no-cache \
  || fail 2 "python3 image-builder"

# 3 — Build deploy-manager
step 3 "docker compose build deploy-manager"
docker compose build deploy-manager \
  || fail 3 "docker compose build deploy-manager"

# 4 — Build execution-manager
step 4 "docker compose build execution-manager"
docker compose build execution-manager \
  || fail 4 "docker compose build execution-manager"

# 5 — Start Redis (background)
step 5 "docker compose up -d redis"
docker compose up -d redis \
  || fail 5 "docker compose up redis"

# 6 — Run deploy-manager (provisions task-services and populates Redis)
step 6 "docker compose up deploy-manager"
docker compose up deploy-manager \
  || fail 6 "docker compose up deploy-manager"

DEPLOY_EXIT=$(docker inspect deploy-manager --format '{{.State.ExitCode}}' 2>/dev/null || echo "missing")
if [ "$DEPLOY_EXIT" != "0" ]; then
  fail 6 "deploy-manager exited with code $DEPLOY_EXIT"
fi

# 7 — Run execution-manager (executes the schedule via gRPC)
step 7 "docker compose up execution-manager"
docker compose up execution-manager \
  || fail 7 "docker compose up execution-manager"

# Save the logs before the containers are removed
rl_save_container_logs execution-manager-grpc deploy-manager redis
mv "$LOG_DIR/execution-manager-grpc.log" "$LOG_DIR/execution-manager.log"
for c in $(docker ps -a --filter "name=task-service-" --format '{{.Names}}' 2>/dev/null || true); do
  rl_save_container_logs "$c"
done

# 8 — Cleanup
step 8 "docker compose down + remove task-service-* containers"
docker compose down
ACTIVE_CONTAINERS=$(docker ps -aq --filter "name=task-service-" || true)
if [ -n "$ACTIVE_CONTAINERS" ]; then
  docker rm -f $ACTIVE_CONTAINERS
fi

# 9 — Performance report
step 9 "performance report"
REPORT_RC=0
rl_generate_report "$SCRIPT_DIR" "orchestration-grpc" "$SOURCE_MANIFEST" || REPORT_RC=$?

echo -e "\n${GREEN}✔ All steps completed. Results: $RESULTS_DIR${NC}\n"
exit $REPORT_RC
