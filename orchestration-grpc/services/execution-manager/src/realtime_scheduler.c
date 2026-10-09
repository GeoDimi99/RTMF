#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
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
    task_status_t status;
    pthread_t grpc_thread;
    pthread_mutex_t lock;

    // gRPC handle for cancellation
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
static int g_completion_eventfd = -1;
static scheduled_task_t g_tasks[MAX_TASKS];
static int g_num_tasks = 0;

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
        log_message(LOG_INFO, "execution-manager", "T=%lu ms: task %d '%s' COMPLETED",
               get_elapsed_ms(), task->task_id, task->task_name);
        log_message(LOG_INFO, "execution-manager", "Result: %s", result_json);

        /* Measure the end_time_result (for the performance) */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long end_time_result = ts.tv_sec * 1000000000L + ts.tv_nsec;
        /* -------------------------------------------------- */

        /* Log the performance metrics to be analysed */
        log_perf("execution-manager", iteration, task->task_id, task->start_time_request,
                  end_time_request, start_time_result, end_time_result, core_id);
        /* --------------------------------------------------- */

        pthread_mutex_lock(&task->lock);
        if (task->status == TASK_STATUS_RUNNING) {
            task->status = TASK_STATUS_COMPLETED;
            // Notify event loop inside lock to suppress stale notifications from timed-out tasks
            uint64_t notify = task->task_id;
            write(g_completion_eventfd, &notify, sizeof(notify));
        }
        pthread_mutex_unlock(&task->lock);

    } else if (strcmp(status, "ERROR") == 0) {
        log_message(LOG_ERROR, "execution-manager", "T=%lu ms: task %d '%s' ERROR: %s",
               get_elapsed_ms(), task->task_id, task->task_name, error_message);

        pthread_mutex_lock(&task->lock);
        if (task->status == TASK_STATUS_RUNNING) {
            task->status = TASK_STATUS_ERROR;
            // Notify event loop inside lock to suppress stale notifications from timed-out tasks
            uint64_t notify = task->task_id;
            write(g_completion_eventfd, &notify, sizeof(notify));
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

    /* Measure the start_time_request (for the performance) */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    task->start_time_request = ts.tv_sec * 1000000000L + ts.tv_nsec;
    /* --------------------------------------------------- */

    // Note: grpc_execute_task_async is blocking, but runs in separate thread
    // so main scheduler thread remains free
    // Cancellation is handled by gRPC server checking context->IsCancelled()
    // which happens when deadline expires
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
    
    // Create thread for gRPC call
    grpc_thread_args_t *args = malloc(sizeof(grpc_thread_args_t));
    args->task = task;
    
    // Use SCHED_FIFO prio 85 (below scheduler prio 95) so the scheduler can preempt
    // this thread when start timers fire, avoiding starvation of the epoll event loop.
    pthread_attr_t grpc_attr;
    struct sched_param grpc_param = {.sched_priority = 85};
    pthread_attr_init(&grpc_attr);
    pthread_attr_setinheritsched(&grpc_attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&grpc_attr, SCHED_FIFO);
    pthread_attr_setschedparam(&grpc_attr, &grpc_param);
    int rc = pthread_create(&task->grpc_thread, &grpc_attr, grpc_task_thread, args);
    pthread_attr_destroy(&grpc_attr);
    if (rc != 0) {
        log_message(LOG_WARN, "execution-manager", "pthread_create with SCHED_FIFO 85 failed (rc=%d), retrying with inherited attrs", rc);
        pthread_create(&task->grpc_thread, NULL, grpc_task_thread, args);
    }
    pthread_detach(task->grpc_thread);
    
    // Arm timeout timer if deadline is set
    if (task->deadline_ms > 0 && task->timeout_timer_fd >= 0) {
        uint64_t now_ms = get_elapsed_ms();
        
        if (task->deadline_ms > now_ms) {
            uint64_t timeout_ms = task->deadline_ms - now_ms;
            
            struct itimerspec timeout_spec = {0};
            ms_to_timespec(timeout_ms, &timeout_spec.it_value);
            
            timerfd_settime(task->timeout_timer_fd, 0, &timeout_spec, NULL);
            
            log_message(LOG_DEBUG, "execution-manager", "Timeout armed for task %d at T=%lu ms (in %lu ms)",
                   task->task_id, task->deadline_ms, timeout_ms);
        }
    }
}

// Abort a task that exceeded its deadline
static void abort_task_timeout(scheduled_task_t *task) {
    log_message(LOG_WARN, "execution-manager", "T=%lu ms: TIMEOUT! Aborting task %d '%s'",
           get_elapsed_ms(), task->task_id, task->task_name);

    pthread_mutex_lock(&task->lock);

    if (task->status == TASK_STATUS_RUNNING) {
        task->status = TASK_STATUS_TIMEOUT;

        // Cancel gRPC call (if cancellation was implemented)
        if (task->grpc_handle) {
            log_message(LOG_INFO, "execution-manager", "Cancelling gRPC call for task %d", task->task_id);
            grpc_cancel_task(task->grpc_handle);
        }

        log_message(LOG_ERROR, "execution-manager", "Task %d aborted due to timeout", task->task_id);
    } else {
        log_message(LOG_DEBUG, "execution-manager", "Task %d already completed, timeout ignored", task->task_id);
    }

    pthread_mutex_unlock(&task->lock);
}

// Handle task completion
static void handle_task_completion(scheduled_task_t *task) {
    log_message(LOG_INFO, "execution-manager", "Task %d '%s' completed with status %d",
           task->task_id, task->task_name, task->status);

    pthread_mutex_lock(&task->lock);

    if (task->status == TASK_STATUS_COMPLETED) {
        // Disarm timeout timer
        if (task->timeout_timer_fd >= 0) {
            struct itimerspec disarm = {0};
            timerfd_settime(task->timeout_timer_fd, 0, &disarm, NULL);
            log_message(LOG_DEBUG, "execution-manager", "Timeout disarmed for task %d", task->task_id);
        }
    }

    pthread_mutex_unlock(&task->lock);
}

// Main function to execute schedule with event loop, repeated for the given number of iterations
void execute_schedule_with_event_loop(redisContext *redis, int num_tasks, int iterations) {
    log_message(LOG_INFO, "execution-manager", "Real-time event-driven scheduler starting");

    g_num_tasks = num_tasks;

    // Create epoll instance (shared across all iterations)
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        log_message(LOG_ERROR, "execution-manager", "epoll_create1 failed: %s", strerror(errno));
        return;
    }
    
    // Create eventfd for task completion notifications (shared across all iterations)
    g_completion_eventfd = eventfd(0, EFD_NONBLOCK);
    
    epoll_event_data_t *completion_event_data = malloc(sizeof(epoll_event_data_t));
    completion_event_data->type = EVENT_TYPE_TASK_COMPLETION;
    completion_event_data->task = NULL;
    
    struct epoll_event completion_ev = {
        .events = EPOLLIN,
        .data.ptr = completion_event_data
    };
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_completion_eventfd, &completion_ev);
    
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
        snprintf(task->service_address, sizeof(task->service_address), "localhost:%d", service_port);
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
        
        // Drain any stale eventfd notifications left from the previous iteration
        {
            uint64_t dummy;
            while (read(g_completion_eventfd, &dummy, sizeof(dummy)) > 0) {}
        }
        
        // Create and arm timers for each task
        for (int i = 0; i < num_tasks; i++) {
            scheduled_task_t *task = &g_tasks[i];
            
            task->status = TASK_STATUS_PENDING;
            task->grpc_handle = NULL;
            task->warmup_timer_fd = -1;
            task->warmup_event_data = NULL;
            
            // Create pre-warmup timer: fire 2 s before task start to ensure the
            // channel is READY at launch time, absorbing any IDLE re-connect cost.
            if (task->start_time_ms > 2000) {
                task->warmup_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
                if (task->warmup_timer_fd >= 0) {
                    struct itimerspec warmup_spec = {0};
                    ms_to_timespec(task->start_time_ms - 2000, &warmup_spec.it_value);
                    timerfd_settime(task->warmup_timer_fd, 0, &warmup_spec, NULL);
                    
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
            task->start_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
            if (task->start_timer_fd < 0) {
                log_message(LOG_ERROR, "execution-manager", "timerfd_create (start) failed: %s", strerror(errno));
                continue;
            }
            
            struct itimerspec start_spec = {0};
            ms_to_timespec(task->start_time_ms, &start_spec.it_value);
            timerfd_settime(task->start_timer_fd, 0, &start_spec, NULL);
            
            epoll_event_data_t *start_data = malloc(sizeof(epoll_event_data_t));
            start_data->type = EVENT_TYPE_START_TIMER;
            start_data->task = task;
            task->start_event_data = start_data;
            
            struct epoll_event start_ev = {
                .events = EPOLLIN,
                .data.ptr = start_data
            };
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, task->start_timer_fd, &start_ev);
            
            // Create timeout timer (disarmed initially)
            task->timeout_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
            if (task->timeout_timer_fd < 0) {
                log_message(LOG_ERROR, "execution-manager", "timerfd_create (timeout) failed: %s", strerror(errno));
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

        // Event loop for this iteration
        int tasks_pending = num_tasks;

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
                        close(data->task->start_timer_fd);
                        data->task->start_timer_fd = -1;
                        
                        break;
                    }
                    
                    case EVENT_TYPE_TIMEOUT_TIMER: {
                        uint64_t expirations;
                        read(data->task->timeout_timer_fd, &expirations, sizeof(expirations));
                        
                        abort_task_timeout(data->task);

                        tasks_pending--;
                        log_message(LOG_DEBUG, "execution-manager", "%d tasks remaining", tasks_pending);

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
                        close(data->task->warmup_timer_fd);
                        data->task->warmup_timer_fd = -1;
                        
                        break;
                    }
                    
                    case EVENT_TYPE_TASK_COMPLETION: {
                        uint64_t task_id;
                        read(g_completion_eventfd, &task_id, sizeof(task_id));
                        
                        if (task_id > 0 && task_id <= (uint64_t)num_tasks) {
                            scheduled_task_t *task = &g_tasks[task_id - 1];
                            handle_task_completion(task);

                            tasks_pending--;
                            log_message(LOG_DEBUG, "execution-manager", "%d tasks remaining", tasks_pending);
                        }
                        
                        break;
                    }
                }
            }
        }
        
        log_message(LOG_INFO, "execution-manager", "Iteration %d/%d complete", iter + 1, iterations);
        
        // Cleanup timers for this iteration (close fds and free event data)
        for (int i = 0; i < num_tasks; i++) {
            scheduled_task_t *task = &g_tasks[i];
            
            if (task->start_timer_fd >= 0) {
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, task->start_timer_fd, NULL);
                close(task->start_timer_fd);
                task->start_timer_fd = -1;
            }
            if (task->timeout_timer_fd >= 0) {
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, task->timeout_timer_fd, NULL);
                close(task->timeout_timer_fd);
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
                close(task->warmup_timer_fd);
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
    close(g_completion_eventfd);
    close(epoll_fd);
}
