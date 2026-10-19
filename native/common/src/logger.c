#include "logger.h"
#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>

#define LOG_LINE_MAX 2048   // Longer lines are truncated

/* Get timestamp in "YYYY-MM-DD HH:MM:SS,mmm" format */
static void get_timestamp(char *buffer, size_t len) {
    struct timespec ts;
    struct tm tm_info;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm_info);

    int ms = ts.tv_nsec / 1000000;

    snprintf(buffer, len, "%04d-%02d-%02d %02d:%02d:%02d,%03d",
             tm_info.tm_year + 1900,
             tm_info.tm_mon + 1,
             tm_info.tm_mday,
             tm_info.tm_hour,
             tm_info.tm_min,
             tm_info.tm_sec,
             ms);
}

void log_message(log_level_t level, const char *component, const char *fmt, ...) {
    static const char *level_str[] = {"DEBUG", "INFO", "WARN", "ERROR", "PERF"};

    char timestamp[32];
    get_timestamp(timestamp, sizeof(timestamp));

    /* Build the whole line in a local buffer and emit it with a single write(2):
     *  - the line cannot interleave with the lines of the other threads (corrupted PERF lines);
     *  - no user-space lock is held: with the stdio lock, a task thread requeued behind a
     *    same-priority task while holding it (SCHED_FIFO) blocked the scheduler thread for a
     *    whole task. The kernel pipe/tty locks taken by write(2) have priority inheritance
     *    on PREEMPT_RT. A write of up to PIPE_BUF bytes to a pipe is atomic. */
    char line[LOG_LINE_MAX];
    int n = snprintf(line, sizeof(line), "%s [%s] %s: ", timestamp, level_str[level], component);
    if (n < 0) return;

    va_list args;
    va_start(args, fmt);
    if (n < (int)sizeof(line)) {
        int m = vsnprintf(line + n, sizeof(line) - n, fmt, args);
        if (m > 0) n += m;
    }
    va_end(args);

    if (n > (int)sizeof(line) - 1) n = sizeof(line) - 1;   // truncated: keep room for '\n'
    line[n++] = '\n';

    ssize_t off = 0;
    while (off < n) {
        ssize_t w = write(STDOUT_FILENO, line + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        off += w;
    }
}

void log_perf(const char *component, int iteration, unsigned int task_id,
              long start_req_ns, long end_req_ns, long start_res_ns, long end_res_ns,
              int core_id) {
    double q_em_tw_ms = (end_req_ns - start_req_ns) / 1e6;
    double t_in_tw_ms  = (start_res_ns - end_req_ns) / 1e6;
    double q_tw_em_ms  = (end_res_ns - start_res_ns) / 1e6;
    double total_ms    = (end_res_ns - start_req_ns) / 1e6;

    log_message(LOG_PERF, component,
        "iteration=%d task_id=%u start_req_ns=%ld end_req_ns=%ld start_res_ns=%ld end_res_ns=%ld "
        "q_em_tw_ms=%.3f t_in_tw_ms=%.3f q_tw_em_ms=%.3f total_ms=%.3f core_id=%d",
        iteration, task_id, start_req_ns, end_req_ns, start_res_ns, end_res_ns,
        q_em_tw_ms, t_in_tw_ms, q_tw_em_ms, total_ms, core_id);
}
