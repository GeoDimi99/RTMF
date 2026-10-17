#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <hiredis/hiredis.h>
#include "grpc_client.h"
#include "logger.h"

#define MAX_TASKS 100
#define MAX_EPOLL_EVENTS 20

// Task status
typedef enum {
    TASK_STATUS_PENDING,
    TASK_STATUS_RUNNING,
    TASK_STATUS_COMPLETED,
    TASK_STATUS_TIMEOUT,
    TASK_STATUS_ERROR
} task_status_t;

// Event types for epoll
typedef enum {
    EVENT_TYPE_START_TIMER,
    EVENT_TYPE_TIMEOUT_TIMER,
    EVENT_TYPE_TASK_COMPLETION,
    EVENT_TYPE_WARMUP_TIMER
} event_type_t;

// Scheduled task structure
typedef struct {
    int task_id;
    char task_name[128];
    char service_address[256];
    char inputs_json[4096];
    int priority;
    char policy[32];
    int cpu_affinity;          // CPU core for the task thread (-1 = unset)

    // Scheduling (in milliseconds, relative to schedule start)
    uint64_t start_time_ms;    // Relative to schedule start
    uint64_t deadline_ms;      // Absolute from schedule start

    // Timers
    int start_timer_fd;
    int timeout_timer_fd;

    // State
    task_status_t status;      // Written by the gRPC call thread and the event loop (under lock)
    int finished;              // Counted as finished in this iteration (event loop only)
    pthread_t grpc_thread;
    int grpc_thread_running;   // grpc_thread must be joined at the end of the iteration
    pthread_mutex_t lock;

    // gRPC handle for cancellation (never set: grpc_execute_task_async is not cancellable,
    // a timed-out call runs to completion and is joined at the end of the iteration)
    grpc_call_handle_t grpc_handle;

    // Per-iteration epoll event data pointers (freed after each iteration)
    void *start_event_data;
    void *timeout_event_data;
    int warmup_timer_fd;
    void *warmup_event_data;

    long start_time_request;   // Dispatch timestamp (ns, CLOCK_MONOTONIC), for the performance

} scheduled_task_t;

static int iteration = 0; // Counter for measure the performance

// Event data for epoll
typedef struct {
    event_type_t type;
    scheduled_task_t *task;
} epoll_event_data_t;

// Global state
static struct timespec g_schedule_start;
// Completion notifications (task ids) from the gRPC call threads to the event loop.
// A pipe and not an eventfd: an eventfd adds up the written values, so two tasks completing
// together (e.g. 1 and 7) were read as one completion of task 8.
static int g_completion_pipe[2] = {-1, -1};
static scheduled_task_t g_tasks[MAX_TASKS];

// Helper: Get elapsed time in milliseconds since schedule start
static uint64_t get_elapsed_ms() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    
    uint64_t start_ms = g_schedule_start.tv_sec * 1000ULL + g_schedule_start.tv_nsec / 1000000ULL;
    uint64_t now_ms = now.tv_sec * 1000ULL + now.tv_nsec / 1000000ULL;
    
    return now_ms - start_ms;
}

// Helper: Convert milliseconds to timespec
static void ms_to_timespec(uint64_t ms, struct timespec *ts) {
    ts->tv_sec = ms / 1000;
    ts->tv_nsec = (ms % 1000) * 1000000UL;
}

// ============================================
// RT TIMERS
// ============================================
// On the PREEMPT_RT kernel a timerfd expiration is handled by ksoftirqd (SCHED_OTHER):
// while a SCHED_FIFO task computes on the same core it cannot run, so the start timers
// fired only when that task ended (e.g. task 3 launched at ~390 ms instead of 350 ms).
// These timers are eventfds (read and polled exactly like a timerfd) signalled by a
// thread that inherits the middleware SCHED_FIFO 90 and sleeps with pthread_cond_timedwait:
// for RT threads the kernel expires that sleep in hard interrupt context, so it is not
// delayed by the tasks (same as the poll() timeout used by the GLib main loop elsewhere).

#define MAX_RT_TIMERS (MAX_TASKS * 3)   // start + timeout + warmup per task

typedef struct {
    int fd;                  // eventfd, -1 = free slot
    int armed;
    struct timespec expiry;  // CLOCK_MONOTONIC, absolute
} rt_timer_t;

static rt_timer_t g_rt_timers[MAX_RT_TIMERS];
static pthread_mutex_t g_rt_timers_lock;
static pthread_cond_t g_rt_timers_cond;
static pthread_once_t g_rt_timers_once = PTHREAD_ONCE_INIT;

static int timespec_cmp(const struct timespec *a, const struct timespec *b) {
    if (a->tv_sec != b->tv_sec) return a->tv_sec < b->tv_sec ? -1 : 1;
    if (a->tv_nsec != b->tv_nsec) return a->tv_nsec < b->tv_nsec ? -1 : 1;
    return 0;
}

static void *rt_timer_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_rt_timers_lock);
    while (1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        // Fire the expired timers and find the next one
        rt_timer_t *next = NULL;
        for (int i = 0; i < MAX_RT_TIMERS; i++) {
            rt_timer_t *t = &g_rt_timers[i];
            if (t->fd < 0 || !t->armed) continue;
            if (timespec_cmp(&t->expiry, &now) <= 0) {
                uint64_t one = 1;
                if (write(t->fd, &one, sizeof(one)) != sizeof(one)) {
                    log_message(LOG_ERROR, "execution-manager", "RT timer: eventfd write failed: %s", strerror(errno));
                }
                t->armed = 0;
            } else if (!next || timespec_cmp(&t->expiry, &next->expiry) < 0) {
                next = t;
            }
        }

        // Sleep until the next expiry, or until a timer is (re)armed
        if (next) {
            struct timespec until = next->expiry;
            pthread_cond_timedwait(&g_rt_timers_cond, &g_rt_timers_lock, &until);
        } else {
            pthread_cond_wait(&g_rt_timers_cond, &g_rt_timers_lock);
        }
    }
    return NULL;
}

static void rt_timers_init(void) {
    for (int i = 0; i < MAX_RT_TIMERS; i++) {
        g_rt_timers[i].fd = -1;
        g_rt_timers[i].armed = 0;
    }

    // Priority inheritance: the scheduler main thread also takes this lock
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setprotocol(&mattr, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&g_rt_timers_lock, &mattr);
    pthread_mutexattr_destroy(&mattr);

    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_rt_timers_cond, &cattr);
    pthread_condattr_destroy(&cattr);

    // The thread inherits the middleware SCHED_FIFO 90 of the scheduler (see main.c)
    pthread_t thread;
    int rc = pthread_create(&thread, NULL, rt_timer_thread, NULL);
    if (rc != 0) {
        log_message(LOG_ERROR, "execution-manager", "RT timer thread creation failed (rc=%d)", rc);
        return;
    }
    pthread_detach(thread);
    log_message(LOG_INFO, "execution-manager", "RT timer thread started");
}

static rt_timer_t *rt_timer_find(int fd) {
    for (int i = 0; i < MAX_RT_TIMERS; i++) {
        if (g_rt_timers[i].fd == fd) return &g_rt_timers[i];
    }
    return NULL;
}

// Create a disarmed timer. Returns a non-blocking fd usable with epoll, or -1.
static int rt_timer_create(void) {
    pthread_once(&g_rt_timers_once, rt_timers_init);

    int fd = eventfd(0, EFD_NONBLOCK);
    if (fd < 0) return -1;

    pthread_mutex_lock(&g_rt_timers_lock);
    rt_timer_t *t = rt_timer_find(-1);
    if (t) {
        t->fd = fd;
        t->armed = 0;
    }
    pthread_mutex_unlock(&g_rt_timers_lock);

    if (!t) {
        close(fd);
        errno = ENOSPC;
        return -1;
    }
    return fd;
}

// Arm the timer to expire at schedule start + at_ms (absolute, like the manifest times)
static void rt_timer_arm(int fd, uint64_t at_ms) {
    struct timespec offset;
    ms_to_timespec(at_ms, &offset);

    pthread_mutex_lock(&g_rt_timers_lock);
    rt_timer_t *t = rt_timer_find(fd);
    if (t) {
        t->expiry.tv_sec = g_schedule_start.tv_sec + offset.tv_sec;
        t->expiry.tv_nsec = g_schedule_start.tv_nsec + offset.tv_nsec;
        if (t->expiry.tv_nsec >= 1000000000L) {
            t->expiry.tv_sec++;
            t->expiry.tv_nsec -= 1000000000L;
        }
        t->armed = 1;
        pthread_cond_signal(&g_rt_timers_cond);
    }
    pthread_mutex_unlock(&g_rt_timers_lock);
}

// Disarm the timer and discard an expiration not yet read (as timerfd_settime does)
static void rt_timer_disarm(int fd) {
    pthread_mutex_lock(&g_rt_timers_lock);
    rt_timer_t *t = rt_timer_find(fd);
    if (t) t->armed = 0;
    pthread_mutex_unlock(&g_rt_timers_lock);

    uint64_t dummy;
    while (read(fd, &dummy, sizeof(dummy)) > 0) {}
}

// Release the timer slot and close its fd
static void rt_timer_close(int fd) {
    pthread_mutex_lock(&g_rt_timers_lock);
    rt_timer_t *t = rt_timer_find(fd);
    if (t) {
        t->fd = -1;
        t->armed = 0;
    }
    pthread_mutex_unlock(&g_rt_timers_lock);
    close(fd);
}

// Notify the event loop that a task finished (called with task->lock held)
static void notify_task_finished(scheduled_task_t *task) {
    uint64_t task_id = (uint64_t)task->task_id;
    if (write(g_completion_pipe[1], &task_id, sizeof(task_id)) != sizeof(task_id)) {
        log_message(LOG_ERROR, "execution-manager", "Completion notification for task %d failed: %s",
               task->task_id, strerror(errno));
    }
}

// Callback wrapper that notifies event loop when task completes
static void scheduler_task_callback(unsigned int task_id,
                                   const char* status,
                                   const char* result_json,
                                   const char* error_message,
                                   long end_time_request,
                                   long start_time_result,
                                   int core_id,
                                   void* user_data) {
    (void)task_id;
    scheduled_task_t *task = (scheduled_task_t *)user_data;

    if (strcmp(status, "STARTED") == 0) {
        log_message(LOG_INFO, "execution-manager", "T=%lu ms: task %d '%s' STARTED",
               get_elapsed_ms(), task->task_id, task->task_name);
    } else if (strcmp(status, "COMPLETED") == 0) {
        /* Store the result: equivalent of schedule_set_result() in the other versions */
        pthread_mutex_lock(&task->lock);
        if (task->status == TASK_STATUS_RUNNING) {
            task->status = TASK_STATUS_COMPLETED;
            // Notify inside the lock: a task that already timed out is not notified again
            notify_task_finished(task);
        }
        pthread_mutex_unlock(&task->lock);

        /* Measure the end_time_result (for the performance): after the result is stored */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long end_time_result = ts.tv_sec * 1000000000L + ts.tv_nsec;
        /* -------------------------------------------------- */

        log_message(LOG_INFO, "execution-manager", "T=%lu ms: task %d '%s' COMPLETED",
               get_elapsed_ms(), task->task_id, task->task_name);
        log_message(LOG_INFO, "execution-manager", "Result: %s", result_json);

        /* Log the performance metrics to be analysed */
        log_perf("execution-manager", iteration, task->task_id, task->start_time_request,
                  end_time_request, start_time_result, end_time_result, core_id);
        /* --------------------------------------------------- */

    } else if (strcmp(status, "ERROR") == 0) {
        log_message(LOG_ERROR, "execution-manager", "T=%lu ms: task %d '%s' ERROR: %s",
               get_elapsed_ms(), task->task_id, task->task_name, error_message);

        pthread_mutex_lock(&task->lock);
        if (task->status == TASK_STATUS_RUNNING) {
            task->status = TASK_STATUS_ERROR;
            // Notify inside the lock: a task that already timed out is not notified again
            notify_task_finished(task);
        }
        pthread_mutex_unlock(&task->lock);

    } else if (strcmp(status, "CANCELLED") == 0) {
        log_message(LOG_WARN, "execution-manager", "T=%lu ms: task %d '%s' CANCELLED",
               get_elapsed_ms(), task->task_id, task->task_name);
    }
}

// Thread function for gRPC call
typedef struct {
    scheduled_task_t *task;
} grpc_thread_args_t;

static void *grpc_task_thread(void *arg) {
    grpc_thread_args_t *args = (grpc_thread_args_t *)arg;
    scheduled_task_t *task = args->task;

    log_message(LOG_DEBUG, "execution-manager", "Thread for task %d starting...", task->task_id);

    // grpc_execute_task_async blocks until the result arrives, so it runs in its own thread
    // and the scheduler thread stays free. It is not cancelled at the task deadline: the
    // thread is joined at the end of the iteration (see execute_schedule_with_event_loop).
    grpc_execute_task_async(
        task->service_address,
        task->task_id,
        task->task_name,
        task->inputs_json,
        task->priority,
        task->policy,
        task->cpu_affinity,
        scheduler_task_callback,
        (void*)task
    );
    
    log_message(LOG_DEBUG, "execution-manager", "Thread for task %d completed", task->task_id);
    
    free(args);
    return NULL;
}

// Launch a task at its scheduled time
static void launch_task(scheduled_task_t *task) {
    log_message(LOG_INFO, "execution-manager", "T=%lu ms: launching task %d '%s' (deadline: %lu ms)",
           get_elapsed_ms(), task->task_id, task->task_name, task->deadline_ms);
    
    pthread_mutex_lock(&task->lock);
    task->status = TASK_STATUS_RUNNING;
    pthread_mutex_unlock(&task->lock);
    
    /* Measure the start_time_request (for the performance): at the dispatch point, before
     * the thread that issues the gRPC call is created */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    task->start_time_request = ts.tv_sec * 1000000000L + ts.tv_nsec;
    /* --------------------------------------------------- */

    // Create thread for gRPC call
    grpc_thread_args_t *args = malloc(sizeof(grpc_thread_args_t));
    args->task = task;
    
    // The thread inherits the middleware SCHED_FIFO 90 of the scheduler (see main.c).
    // It is joinable: the event loop joins it at the end of the iteration.
    int rc = pthread_create(&task->grpc_thread, NULL, grpc_task_thread, args);
    if (rc != 0) {
        // The timeout timer below still fires, so the iteration does not hang
        log_message(LOG_ERROR, "execution-manager", "pthread_create for task %d failed (rc=%d)", task->task_id, rc);
        free(args);
    } else {
        task->grpc_thread_running = 1;
    }
    
    // Arm the timeout timer (a deadline already passed fires immediately)
    if (task->deadline_ms > 0 && task->timeout_timer_fd >= 0) {
        rt_timer_arm(task->timeout_timer_fd, task->deadline_ms);
        log_message(LOG_DEBUG, "execution-manager", "Timeout armed for task %d at T=%lu ms",
               task->task_id, task->deadline_ms);
    }
}

static const char *task_status_name(task_status_t status) {
    switch (status) {
        case TASK_STATUS_PENDING:   return "PENDING";
        case TASK_STATUS_RUNNING:   return "RUNNING";
        case TASK_STATUS_COMPLETED: return "COMPLETED";
        case TASK_STATUS_TIMEOUT:   return "TIMEOUT";
        case TASK_STATUS_ERROR:     return "ERROR";
    }
    return "UNKNOWN";
}

// Handle the deadline of a task. Returns 1 if the task was still running (it is now
// TIMEOUT and finished), 0 if it had already completed: its completion notification
// is processed by the event loop and counts it as finished.
static int handle_task_timeout(scheduled_task_t *task) {
    int timed_out = 0;

    pthread_mutex_lock(&task->lock);
    if (task->status == TASK_STATUS_RUNNING) {
        task->status = TASK_STATUS_TIMEOUT;
        timed_out = 1;

        // Cancel gRPC call (if cancellation was implemented)
        if (task->grpc_handle) {
            log_message(LOG_INFO, "execution-manager", "Cancelling gRPC call for task %d", task->task_id);
            grpc_cancel_task(task->grpc_handle);
        }
    }
    pthread_mutex_unlock(&task->lock);

    if (timed_out) {
        log_message(LOG_WARN, "execution-manager", "T=%lu ms: TIMEOUT! Task %d '%s' missed its deadline",
               get_elapsed_ms(), task->task_id, task->task_name);
    } else {
        log_message(LOG_DEBUG, "execution-manager", "T=%lu ms: task %d already finished, timeout ignored",
               get_elapsed_ms(), task->task_id);
    }
    return timed_out;
}

// Handle the completion notification of a task (COMPLETED or ERROR)
static void handle_task_completion(scheduled_task_t *task) {
    pthread_mutex_lock(&task->lock);
    task_status_t status = task->status;
    pthread_mutex_unlock(&task->lock);

    log_message(LOG_INFO, "execution-manager", "Task %d '%s' finished with status %s",
           task->task_id, task->task_name, task_status_name(status));

    // Disarm the timeout timer, whatever the outcome, so it cannot fire later
    if (task->timeout_timer_fd >= 0) {
        rt_timer_disarm(task->timeout_timer_fd);
        log_message(LOG_DEBUG, "execution-manager", "Timeout disarmed for task %d", task->task_id);
    }
}

// Main function to execute schedule with event loop, repeated for the given number of iterations
void execute_schedule_with_event_loop(redisContext *redis, int num_tasks, int iterations) {
    log_message(LOG_INFO, "execution-manager", "Real-time event-driven scheduler starting");

    // Create epoll instance (shared across all iterations)
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        log_message(LOG_ERROR, "execution-manager", "epoll_create1 failed: %s", strerror(errno));
        return;
    }
    
    // Create the pipe for task completion notifications (shared across all iterations)
    if (pipe2(g_completion_pipe, O_NONBLOCK | O_CLOEXEC) != 0) {
        log_message(LOG_ERROR, "execution-manager", "pipe2 failed: %s", strerror(errno));
        close(epoll_fd);
        return;
    }
    
    epoll_event_data_t *completion_event_data = malloc(sizeof(epoll_event_data_t));
    completion_event_data->type = EVENT_TYPE_TASK_COMPLETION;
    completion_event_data->task = NULL;
    
    struct epoll_event completion_ev = {
        .events = EPOLLIN,
        .data.ptr = completion_event_data
    };
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_completion_pipe[0], &completion_ev);
    
    log_message(LOG_INFO, "execution-manager", "Loading schedule: %d task(s), %d iteration(s)", num_tasks, iterations);
    
    // ---- Load task data from Redis ONCE (shared across all iterations) ----
    for (int i = 0; i < num_tasks; i++) {
        scheduled_task_t *task = &g_tasks[i];
        task->task_id = i + 1;
        task->start_timer_fd = -1;
        task->timeout_timer_fd = -1;
        task->start_event_data = NULL;
        task->timeout_event_data = NULL;
        task->warmup_timer_fd = -1;
        task->warmup_event_data = NULL;
        
        // Read task from Redis
        char key[64];
        snprintf(key, sizeof(key), "scheduletask:%d", task->task_id);
        
        redisReply *reply = redisCommand(redis, "HGETALL %s", key);
        if (!reply || reply->type != REDIS_REPLY_ARRAY) {
            log_message(LOG_ERROR, "execution-manager", "Failed to read task %d from Redis", task->task_id);
            if (reply) freeReplyObject(reply);
            continue;
        }
        
        // Parse task data
        char *task_name = NULL;
        char *task_policy = NULL;
        int task_priority = 50;
        int task_cpu_affinity = -1;
        int task_start = 0;
        int task_deadline = 0;
        char *task_inputs = NULL;
        int service_port = 50051;
        
        for (size_t j = 0; j < reply->elements; j += 2) {
            char *field = reply->element[j]->str;
            char *value = reply->element[j+1]->str;
            
            if (strcmp(field, "name") == 0) {
                task_name = value;
            } else if (strcmp(field, "policy") == 0) {
                task_policy = value;
            } else if (strcmp(field, "priority") == 0) {
                task_priority = atoi(value);
            } else if (strcmp(field, "cpu_affinity") == 0) {
                task_cpu_affinity = atoi(value);
            } else if (strcmp(field, "start") == 0) {
                task_start = atoi(value);
            } else if (strcmp(field, "deadline") == 0) {
                task_deadline = atoi(value);
            } else if (strcmp(field, "inputs") == 0) {
                task_inputs = value;
            } else if (strcmp(field, "service_port") == 0) {
                service_port = atoi(value);
            }
        }
        
        if (!task_name || !task_inputs) {
            log_message(LOG_ERROR, "execution-manager", "Missing data for task %d", task->task_id);
            freeReplyObject(reply);
            continue;
        }
        
        // Convert input JSON from nested to simple format
        // From: {"a": {"type": "int", "value": 10}} 
        // To: {"a": 10}
        char simple_inputs[4096] = "{";
        const char* search_ptr = task_inputs;
        int first_field = 1;
        
        while (1) {
            const char* quote1 = strchr(search_ptr, '\"');
            if (!quote1) break;
            quote1++;
            
            const char* quote2 = strchr(quote1, '\"');
            if (!quote2) break;
            
            char key[64];
            size_t key_len = quote2 - quote1;
            if (key_len >= sizeof(key)) key_len = sizeof(key) - 1;
            memcpy(key, quote1, key_len);
            key[key_len] = '\0';
            
            if (strcmp(key, "type") == 0 || strcmp(key, "value") == 0) {
                search_ptr = quote2 + 1;
                continue;
            }
            
            const char* value_str = strstr(quote2, "\"value\"");
            if (!value_str || (value_str - quote2) > 100) {
                search_ptr = quote2 + 1;
                continue;
            }
            
            const char* num_start = value_str + 7;
            while (*num_start && (*num_start == ' ' || *num_start == ':')) num_start++;
            
            char num[32];
            int ni = 0;
            while (*num_start && ((*num_start >= '0' && *num_start <= '9') || *num_start == '-' || *num_start == '.')) {
                if (ni < sizeof(num) - 1) num[ni++] = *num_start;
                num_start++;
            }
            num[ni] = '\0';
            
            if (ni > 0) {
                if (!first_field) strcat(simple_inputs, ", ");
                strcat(simple_inputs, "\"");
                strcat(simple_inputs, key);
                strcat(simple_inputs, "\": ");
                strcat(simple_inputs, num);
                first_field = 0;
            }
            
            search_ptr = num_start;
        }
        strcat(simple_inputs, "}");
        
        // Fill task structure
        strncpy(task->task_name, task_name, sizeof(task->task_name) - 1);
        // gRPC over the Unix domain socket of the task service (shared /ipc, see grpc_server.cpp)
        snprintf(task->service_address, sizeof(task->service_address), "unix:///ipc/task_%d.sock", service_port);
        strncpy(task->inputs_json, simple_inputs, sizeof(task->inputs_json) - 1);
        task->priority = task_priority;
        task->cpu_affinity = task_cpu_affinity;
        strncpy(task->policy, task_policy ? task_policy : "fifo", sizeof(task->policy) - 1);
        // Manifest specifies start/deadline directly in milliseconds.
        task->start_time_ms = (uint64_t)task_start;
        task->deadline_ms = (uint64_t)task_deadline;
        task->status = TASK_STATUS_PENDING;
        task->grpc_handle = NULL;
        pthread_mutex_init(&task->lock, NULL);
        
        log_message(LOG_DEBUG, "execution-manager", "Task %d: start_time=%d ms, deadline=%d ms (parsed from Redis)",
               task->task_id, task_start, task_deadline);

        freeReplyObject(reply);

        log_message(LOG_INFO, "execution-manager", "Task %d '%s': start=%lu ms, deadline=%lu ms",
               task->task_id, task->task_name, task->start_time_ms, task->deadline_ms);
    }
    
    // ---- Iteration loop ----
    for (int iter = 0; iter < iterations; iter++) {
        iteration = iter; // Counter for measure the performance

        log_message(LOG_INFO, "execution-manager", "===== ITERATION %d / %d =====", iter + 1, iterations);

        // Pre-start grace period (only before the first iteration): give task
        // service containers/gRPC servers extra time to reach a fully-ready
        // state before T=0 is latched and timers are armed.
        if (iter == 0) {
            log_message(LOG_INFO, "execution-manager", "Pre-start grace period: sleeping 5 s before T=0...");
            sleep(5);
            log_message(LOG_INFO, "execution-manager", "Grace period elapsed, latching T=0");
        }
        
        // Reset schedule start time for this iteration
        clock_gettime(CLOCK_MONOTONIC, &g_schedule_start);
        log_message(LOG_INFO, "execution-manager", "Schedule started at T=0");
        
        // Drain any stale completion notifications left from the previous iteration
        {
            uint64_t dummy;
            while (read(g_completion_pipe[0], &dummy, sizeof(dummy)) > 0) {}
        }
        
        // Create and arm timers for each task. Only the tasks with a start timer are
        // launched, so only they are counted as pending in this iteration.
        int tasks_pending = 0;
        for (int i = 0; i < num_tasks; i++) {
            scheduled_task_t *task = &g_tasks[i];
            
            task->status = TASK_STATUS_PENDING;
            task->finished = 0;
            task->grpc_thread_running = 0;
            task->grpc_handle = NULL;
            task->warmup_timer_fd = -1;
            task->warmup_event_data = NULL;
            
            // Create pre-warmup timer: fire 2 s before task start to ensure the
            // channel is READY at launch time, absorbing any IDLE re-connect cost.
            if (task->start_time_ms > 2000) {
                task->warmup_timer_fd = rt_timer_create();
                if (task->warmup_timer_fd >= 0) {
                    rt_timer_arm(task->warmup_timer_fd, task->start_time_ms - 2000);
                    
                    epoll_event_data_t *warmup_data = malloc(sizeof(epoll_event_data_t));
                    warmup_data->type = EVENT_TYPE_WARMUP_TIMER;
                    warmup_data->task = task;
                    task->warmup_event_data = warmup_data;
                    
                    struct epoll_event warmup_ev = {
                        .events = EPOLLIN,
                        .data.ptr = warmup_data
                    };
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, task->warmup_timer_fd, &warmup_ev);
                }
            }
            
            // Create start timer
            task->start_timer_fd = rt_timer_create();
            if (task->start_timer_fd < 0) {
                log_message(LOG_ERROR, "execution-manager", "rt_timer_create (start) failed: %s", strerror(errno));
                continue;
            }
            
            rt_timer_arm(task->start_timer_fd, task->start_time_ms);
            
            epoll_event_data_t *start_data = malloc(sizeof(epoll_event_data_t));
            start_data->type = EVENT_TYPE_START_TIMER;
            start_data->task = task;
            task->start_event_data = start_data;
            
            struct epoll_event start_ev = {
                .events = EPOLLIN,
                .data.ptr = start_data
            };
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, task->start_timer_fd, &start_ev);
            tasks_pending++;
            
            // Create timeout timer (disarmed initially)
            task->timeout_timer_fd = rt_timer_create();
            if (task->timeout_timer_fd < 0) {
                log_message(LOG_ERROR, "execution-manager", "rt_timer_create (timeout) failed: %s", strerror(errno));
                continue;
            }
            
            epoll_event_data_t *timeout_data = malloc(sizeof(epoll_event_data_t));
            timeout_data->type = EVENT_TYPE_TIMEOUT_TIMER;
            timeout_data->task = task;
            task->timeout_event_data = timeout_data;
            
            struct epoll_event timeout_ev = {
                .events = EPOLLIN,
                .data.ptr = timeout_data
            };
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, task->timeout_timer_fd, &timeout_ev);
        }
        
        log_message(LOG_INFO, "execution-manager", "Entering event loop (no sleep - event-driven)");

        // Event loop for this iteration: it ends when every launched task has finished
        // (completed, failed or missed its deadline). Each task is counted exactly once.
        while (tasks_pending > 0) {
            struct epoll_event events[MAX_EPOLL_EVENTS];

            int n = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, -1);

            if (n < 0) {
                log_message(LOG_ERROR, "execution-manager", "epoll_wait failed: %s", strerror(errno));
                break;
            }

            log_message(LOG_DEBUG, "execution-manager", "T=%lu ms: epoll_wait returned %d event(s)", get_elapsed_ms(), n);
            for (int i = 0; i < n; i++) {
                epoll_event_data_t *d = (epoll_event_data_t *)events[i].data.ptr;
                log_message(LOG_DEBUG, "execution-manager", "event[%d]: type=%d task=%s", i, d->type, d->task ? d->task->task_name : "NULL");
            }
            for (int i = 0; i < n; i++) {
                epoll_event_data_t *data = (epoll_event_data_t *)events[i].data.ptr;
                
                switch (data->type) {
                    case EVENT_TYPE_START_TIMER: {
                        uint64_t expirations;
                        read(data->task->start_timer_fd, &expirations, sizeof(expirations));
                        
                        launch_task(data->task);
                        
                        // Remove start timer from epoll (one-shot)
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, data->task->start_timer_fd, NULL);
                        rt_timer_close(data->task->start_timer_fd);
                        data->task->start_timer_fd = -1;
                        
                        break;
                    }
                    
                    case EVENT_TYPE_TIMEOUT_TIMER: {
                        uint64_t expirations;
                        read(data->task->timeout_timer_fd, &expirations, sizeof(expirations));
                        
                        if (handle_task_timeout(data->task) && !data->task->finished) {
                            data->task->finished = 1;
                            tasks_pending--;
                            log_message(LOG_DEBUG, "execution-manager", "%d tasks remaining", tasks_pending);
                        }

                        break;
                    }

                    case EVENT_TYPE_WARMUP_TIMER: {
                        uint64_t expirations;
                        read(data->task->warmup_timer_fd, &expirations, sizeof(expirations));

                        log_message(LOG_DEBUG, "execution-manager", "T=%lu ms: pre-warmup for task %d '%s'",
                               get_elapsed_ms(), data->task->task_id, data->task->task_name);
                        grpc_warmup_channel(data->task->service_address);
                        
                        // One-shot: remove and close warmup timer
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, data->task->warmup_timer_fd, NULL);
                        rt_timer_close(data->task->warmup_timer_fd);
                        data->task->warmup_timer_fd = -1;
                        
                        break;
                    }
                    
                    case EVENT_TYPE_TASK_COMPLETION: {
                        // Several tasks may have completed together: read all their ids
                        uint64_t task_id;
                        while (read(g_completion_pipe[0], &task_id, sizeof(task_id)) == sizeof(task_id)) {
                            if (task_id == 0 || task_id > (uint64_t)num_tasks) {
                                log_message(LOG_ERROR, "execution-manager", "Invalid completion notification: task %lu", task_id);
                                continue;
                            }
                            scheduled_task_t *task = &g_tasks[task_id - 1];
                            handle_task_completion(task);

                            if (!task->finished) {
                                task->finished = 1;
                                tasks_pending--;
                                log_message(LOG_DEBUG, "execution-manager", "%d tasks remaining", tasks_pending);
                            }
                        }
                        break;
                    }
                }
            }
        }
        
        // Wait for every gRPC call of this iteration: a call that missed its deadline is still
        // running, and it must not overlap the next iteration (its result and its perf record
        // would be attributed to the wrong iteration)
        for (int i = 0; i < num_tasks; i++) {
            scheduled_task_t *task = &g_tasks[i];
            if (!task->grpc_thread_running) continue;
            pthread_mutex_lock(&task->lock);
            int timed_out = (task->status == TASK_STATUS_TIMEOUT);
            pthread_mutex_unlock(&task->lock);
            if (timed_out) {
                log_message(LOG_WARN, "execution-manager", "Waiting for task %d '%s' (missed its deadline) before the next iteration",
                       task->task_id, task->task_name);
            }
            pthread_join(task->grpc_thread, NULL);
            task->grpc_thread_running = 0;
        }

        log_message(LOG_INFO, "execution-manager", "Iteration %d/%d complete", iter + 1, iterations);
        
        // Cleanup timers for this iteration (close fds and free event data)
        for (int i = 0; i < num_tasks; i++) {
            scheduled_task_t *task = &g_tasks[i];
            
            if (task->start_timer_fd >= 0) {
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, task->start_timer_fd, NULL);
                rt_timer_close(task->start_timer_fd);
                task->start_timer_fd = -1;
            }
            if (task->timeout_timer_fd >= 0) {
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, task->timeout_timer_fd, NULL);
                rt_timer_close(task->timeout_timer_fd);
                task->timeout_timer_fd = -1;
            }
            if (task->start_event_data) {
                free(task->start_event_data);
                task->start_event_data = NULL;
            }
            if (task->timeout_event_data) {
                free(task->timeout_event_data);
                task->timeout_event_data = NULL;
            }
            if (task->warmup_timer_fd >= 0) {
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, task->warmup_timer_fd, NULL);
                rt_timer_close(task->warmup_timer_fd);
                task->warmup_timer_fd = -1;
            }
            if (task->warmup_event_data) {
                free(task->warmup_event_data);
                task->warmup_event_data = NULL;
            }
        }
        
        // Warm up all channels during inter-iteration idle time so they are
        // READY (state=2) when the next iteration's tasks fire, eliminating
        // the IDLE→CONNECTING→READY transitions paid inline when the RPC is sent.
        if (iter + 1 < iterations) {
            for (int i = 0; i < num_tasks; i++) {
                grpc_warmup_channel(g_tasks[i].service_address);
            }
        }
    }
    
    log_message(LOG_INFO, "execution-manager", "All %d iteration(s) completed", iterations);
    
    // Final cleanup
    for (int i = 0; i < num_tasks; i++) {
        scheduled_task_t *task = &g_tasks[i];
        pthread_mutex_destroy(&task->lock);
    }
    
    free(completion_event_data);
    close(g_completion_pipe[0]);
    close(g_completion_pipe[1]);
    close(epoll_fd);
}
