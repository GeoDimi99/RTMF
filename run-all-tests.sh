#!/bin/bash
# ============================================================================
#  run-all-tests.sh
#
#  Run the whole benchmark campaign (every test scenario on every
#  implementation) with a single command.
#
#  For each implementation and each scenario in tests/manifests/<scenario>.yaml:
#    1. write the scenario manifest over <impl>/tests/test_0_code/manifest.yaml
#       (the deploy-manager image bakes that file in, so it must be replaced
#       in place), with the implementation's own `base:`/`repo:` image lines
#       and `iterations:` set to $ITERATIONS
#    2. run <impl>/start-test.sh test_<scenario>_it<ITERATIONS>_<RUN_ID>
#  The original manifest of every implementation is restored at the end.
#
#  Usage:
#       ./run-all-tests.sh
#
#  Environment:
#    ITERATIONS   iterations per test                  (default: 301;
#                 the reports drop iteration #0 as warm-up, so the
#                 statistics cover 300 iterations)
#    RUN_ID       suffix of the result folder names    (default: 1)
#    IMPLS        implementations to run (space separated)
#                 (default: choreography-mqueue choreography-zeromq
#                           orchestration-grpc orchestration-mqueue)
#    TESTS        scenarios to run (space separated, names of tests/manifests/*.yaml)
#                 (default: all of them)
#    FORCE=1      re-run and overwrite tests whose results folder already exists
#                 (otherwise they are skipped)
#
#  Ctrl+C stops the current test (its report is still generated) and then
#  aborts the campaign.
# ============================================================================

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

ITERATIONS="${ITERATIONS:-301}"
RUN_ID="${RUN_ID:-1}"
IMPLS="${IMPLS:-choreography-mqueue choreography-zeromq orchestration-grpc orchestration-mqueue}"
TESTS="${TESTS:-single_core single_core_nort dual_core_sep dual_core_mix three_core_sep three_core_mix quad_core_sep quad_core_mix}"
MANIFESTS_DIR="$ROOT/tests/manifests"

if ! [[ "$ITERATIONS" =~ ^[0-9]+$ ]] || [ "$ITERATIONS" -le 0 ]; then
    echo "[ERROR] ITERATIONS must be a positive integer (got '$ITERATIONS')." >&2
    exit 1
fi

# --- Validate the inputs before starting anything --------------------------
for impl in $IMPLS; do
    for f in "$impl/start-test.sh" "$impl/tests/test_0_code/manifest.yaml"; do
        [ -f "$ROOT/$f" ] || { echo "[ERROR] Missing $ROOT/$f" >&2; exit 1; }
    done
done
for t in $TESTS; do
    [ -f "$MANIFESTS_DIR/$t.yaml" ] || { echo "[ERROR] Missing $MANIFESTS_DIR/$t.yaml" >&2; exit 1; }
done

CAMPAIGN_LOG="$ROOT/run-all-tests_$(date +%Y%m%d_%H%M%S).log"

# --- Back up the default manifests and restore them on exit ----------------
BACKUP_DIR="$(mktemp -d)"
for impl in $IMPLS; do
    cp "$ROOT/$impl/tests/test_0_code/manifest.yaml" "$BACKUP_DIR/$impl.yaml"
done
restore_manifests() {
    for impl in $IMPLS; do
        [ -f "$BACKUP_DIR/$impl.yaml" ] && cp "$BACKUP_DIR/$impl.yaml" "$ROOT/$impl/tests/test_0_code/manifest.yaml"
    done
    rm -rf "$BACKUP_DIR"
}
trap restore_manifests EXIT

ABORT=0
trap 'ABORT=1' INT TERM

# make_manifest <scenario> <impl>: write the scenario manifest for <impl> to stdout.
make_manifest() {
    local src="$MANIFESTS_DIR/$1.yaml" orig="$BACKUP_DIR/$2.yaml" base repo
    base="$(grep -m1 -E '^[[:space:]]*base:' "$orig")"
    repo="$(grep -m1 -E '^[[:space:]]*repo:' "$orig")"
    awk -v base="$base" -v repo="$repo" -v it="$ITERATIONS" '
        /^[[:space:]]*base:/       && !b { print base; b = 1; next }
        /^[[:space:]]*repo:/       && !r { print repo; r = 1; next }
        /^[[:space:]]*iterations:/ && !i { sub(/iterations:.*/, "iterations: " it); i = 1 }
        { print }
    ' "$src"
}

log() { echo "$*" | tee -a "$CAMPAIGN_LOG"; }

declare -a SUMMARY=()
total=$(( $(wc -w <<<"$IMPLS") * $(wc -w <<<"$TESTS") ))
n=0
START_ALL=$(date +%s)

log "[INFO] Campaign: iterations=$ITERATIONS run_id=$RUN_ID"
log "[INFO] Implementations: $IMPLS"
log "[INFO] Tests: $TESTS"
log "[INFO] Campaign log: $CAMPAIGN_LOG"

for impl in $IMPLS; do
    for t in $TESTS; do
        [ "$ABORT" = "1" ] && break 2
        n=$((n + 1))
        name="test_${t}_it${ITERATIONS}_${RUN_ID}"
        results="$ROOT/$impl/results/$name"

        if [ -e "$results" ] && [ "${FORCE:-0}" != "1" ]; then
            log "[SKIP] ($n/$total) $impl / $name already exists (FORCE=1 to overwrite)"
            SUMMARY+=("SKIP  $impl/$name")
            continue
        fi

        log ""
        log "================================================================"
        log "[RUN ] ($n/$total) $impl / $name   ($(date '+%F %T'))"
        log "================================================================"

        make_manifest "$t" "$impl" > "$ROOT/$impl/tests/test_0_code/manifest.yaml"

        t0=$(date +%s)
        # MANIFEST/MISSION_DIR are unset so every implementation uses the
        # (just rewritten) default manifest.
        ( cd "$ROOT/$impl" && env -u MANIFEST -u MISSION_DIR FORCE="${FORCE:-0}" ./start-test.sh "$name" )
        rc=$?
        dt=$(( $(date +%s) - t0 ))

        if [ "$rc" = "0" ] && [ -f "$results/report.pdf" ]; then
            status="OK  "
        else
            status="FAIL"
        fi
        log "[$status] $impl / $name  (exit $rc, ${dt}s)"
        SUMMARY+=("$status  $impl/$name  (${dt}s)")
    done
done

log ""
log "======================== SUMMARY ========================"
for s in "${SUMMARY[@]}"; do log "  $s"; done
[ "$ABORT" = "1" ] && log "  (campaign interrupted by the user)"
log "Total time: $(( ($(date +%s) - START_ALL) / 60 )) min"

if printf '%s\n' "${SUMMARY[@]}" | grep -q '^FAIL' || [ "$ABORT" = "1" ]; then
    exit 1
fi
exit 0
