#!/bin/bash
# Helpers shared by the start-test.sh scripts: results directory handling,
# container log collection and PDF report generation.
# Meant to be sourced, not executed.

# rl_init_results <project_root> [test_name]
# Sets TEST_NAME, RESULTS_DIR and LOG_DIR and creates the directories.
rl_init_results() {
    local root="$1" name="${2:-}"
    if [ -z "$name" ]; then
        name="test_$(head -c4 /dev/urandom | od -An -tx1 | tr -d ' \n')"
    fi
    if [[ ! "$name" =~ ^[A-Za-z0-9._-]+$ || "$name" == "." || "$name" == ".." ]]; then
        echo "[ERROR] Invalid test name '$name' (allowed: letters, digits, '.', '_', '-')." >&2
        return 1
    fi
    TEST_NAME="$name"
    RESULTS_DIR="$root/results/$name"
    LOG_DIR="$RESULTS_DIR/logs"
    if [ -e "$RESULTS_DIR" ]; then
        if [ "${FORCE:-0}" != "1" ]; then
            echo "[ERROR] $RESULTS_DIR already exists. Use another test name, or run with FORCE=1 to overwrite it." >&2
            return 1
        fi
        rm -rf "$RESULTS_DIR"
    fi
    mkdir -p "$LOG_DIR"
    echo "[INFO] Test name: $TEST_NAME  (results: $RESULTS_DIR)"
}

# rl_extract_aliases <manifest>
# Prints the task-wrapper container names (the `alias:` entries of `images:`).
rl_extract_aliases() {
    grep -E '^[[:space:]]*-[[:space:]]*alias:' "$1" \
        | sed -E 's/.*alias:[[:space:]]*"?([^"[:space:]]+)"?.*/\1/'
}

# rl_follow_container <name>
# Waits for the container to exist, streams its logs to the terminal and to
# $LOG_DIR/<name>.log, and returns when the container stops.
rl_follow_container() {
    local name="$1" waited=0 timeout="${START_TIMEOUT:-180}"
    until docker inspect --type container "$name" >/dev/null 2>&1; do
        if [ "$waited" -ge "$timeout" ]; then
            echo "[ERROR] Container '$name' did not appear within ${timeout}s." >&2
            return 1
        fi
        if [ "$(docker inspect -f '{{.State.Status}} {{.State.ExitCode}}' deploy-manager 2>/dev/null | awk '{print ($1=="exited" && $2!=0)}')" = "1" ]; then
            echo "[ERROR] deploy-manager failed before starting '$name' (see $LOG_DIR/deploy-manager.log)." >&2
            return 1
        fi
        sleep 1
        waited=$((waited + 1))
    done
    docker logs -f "$name" 2>&1 | tee "$LOG_DIR/$name.log"
}

# rl_save_container_logs <name>...
# One-shot dump of the logs of the given (existing) containers.
rl_save_container_logs() {
    local n
    for n in "$@"; do
        docker logs "$n" >"$LOG_DIR/$n.log" 2>&1 || true
    done
}

# rl_generate_report <project_root> <project_name> [manifest]
# Runs tools/perf_report.py on $LOG_DIR/*.log and writes the results into $RESULTS_DIR.
rl_generate_report() {
    local root="$1" project="$2" manifest="${3:-}" py="python3" rc=0
    if [ -n "$manifest" ] && [ -f "$manifest" ]; then
        cp "$manifest" "$RESULTS_DIR/manifest.yaml"
    else
        manifest=""
    fi

    if ! "$py" -c 'import matplotlib, numpy, yaml' 2>/dev/null; then
        local venv="$root/tools/.venv"
        echo "[INFO] Installing report dependencies (matplotlib, numpy, PyYAML) in $venv ..."
        if python3 -m venv "$venv" && "$venv/bin/pip" install -q matplotlib numpy PyYAML; then
            py="$venv/bin/python"
        else
            echo "[WARN] Could not install the report dependencies." >&2
            echo "       Run 'pip install matplotlib numpy PyYAML' and then:" >&2
            echo "       python3 $root/tools/perf_report.py --logs $LOG_DIR/*.log --out $RESULTS_DIR" >&2
            rl_fix_owner "$root"
            return 1
        fi
    fi

    local logs=()
    shopt -s nullglob
    logs=("$LOG_DIR"/*.log)
    shopt -u nullglob

    local args=(--logs "${logs[@]}" --out "$RESULTS_DIR" --project "$project" --test-name "$TEST_NAME")
    [ -n "$manifest" ] && args+=(--manifest "$manifest")

    echo "[INFO] Generating performance report ..."
    "$py" "$root/tools/perf_report.py" "${args[@]}" || rc=$?
    rl_fix_owner "$root"
    return $rc
}

# When the script runs under sudo, hand the generated files back to the invoking user.
rl_fix_owner() {
    local root="$1"
    if [ "$(id -u)" = "0" ] && [ -n "${SUDO_USER:-}" ]; then
        local grp
        grp="$(id -gn "$SUDO_USER")"
        chown "$SUDO_USER:$grp" "$root/results" 2>/dev/null || true
        chown -R "$SUDO_USER:$grp" "$RESULTS_DIR" 2>/dev/null || true
        if [ -d "$root/tools/.venv" ]; then
            chown -R "$SUDO_USER:$grp" "$root/tools/.venv" 2>/dev/null || true
        fi
    fi
}
