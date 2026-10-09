#ifndef LOGGER_H
#define LOGGER_H

/*
 * Unified log schema shared by every project under projects/jeff/:
 *
 *   YYYY-MM-DD HH:MM:SS,mmm [LEVEL] component: message
 *
 * LEVEL is one of DEBUG, INFO, WARN, ERROR, PERF.
 *
 * PERF lines are machine-readable: "message" is always the same
 * space-separated set of key=value fields, in the same order, emitted by
 * log_perf() below. Downstream analysis scripts key off "[PERF]" plus that
 * fixed field layout, so it must not vary between projects.
 */

typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_PERF
} log_level_t;

/*
 * Log a formatted message using the unified schema. `component` identifies
 * the emitting service/subsystem (e.g. "deploy-manager", "execution-manager",
 * "execution-manager:zmq", "task-wrapper"). `fmt` must NOT include a
 * trailing newline; one is appended automatically.
 */
void log_message(log_level_t level, const char *component, const char *fmt, ...);

/*
 * Log one task's performance measurement as a machine-readable [PERF] line.
 * All four raw timestamps are CLOCK_MONOTONIC nanoseconds:
 *   start_req_ns  - before the request to execute the task was issued
 *   end_req_ns    - beginning of the task thread's execution
 *   start_res_ns  - end of the task thread's execution
 *   end_res_ns    - when the result was received back
 * core_id is the CPU core the task thread actually ran on (-1 if unknown).
 *
 * Emits exactly:
 *   iteration=<i> task_id=<id> start_req_ns=<ns> end_req_ns=<ns>
 *   start_res_ns=<ns> end_res_ns=<ns> q_em_tw_ms=<f> t_in_tw_ms=<f>
 *   q_tw_em_ms=<f> total_ms=<f> core_id=<n>
 */
void log_perf(const char *component, int iteration, unsigned int task_id,
              long start_req_ns, long end_req_ns, long start_res_ns, long end_res_ns,
              int core_id);

#endif /* LOGGER_H */
