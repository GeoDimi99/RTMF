#ifndef TASK_IPC_H
#define TASK_IPC_H

#include <mqueue.h>
#include <glib.h>
#include "task.h"

/* Max Queue names */
#define MAX_QUEUE_NAME   64

#define MAX_TASK_JSON_IN  1024
#define MAX_TASK_JSON_OUT 1024


/* Queue names */
#define DEFAULT_EM_QUEUE    "/execution_manager_q"
#define DEFAULT_TASK_QUEUE  "/task_service_queue"

/* --- Data Structures & Enums --- */

/* Message types identifier */
typedef enum {
    MSG_TASK_REQUEST = 0,
    MSG_TASK_RESULT,
    MSG_TASK_ABORT
} msg_type_t;

typedef struct {
    sched_policy_t policy;
    gint8 priority;
    gint cpu_affinity;
    guint8 repetition;
    gchar input_data[MAX_TASK_JSON_IN];
} task_request_t;

typedef gchar task_outcome_t;



/* Main IPC Message Structure */
typedef struct {
    msg_type_t type;
    guint16  task_id;
    /* Timestamps for measures the performance */
    glong start_time_request;
    glong end_time_request;
    glong start_time_result;
    gint  core_id;            /* CPU core the task thread actually ran on */
    /*-----------------------------------------*/
    union {
        task_request_t task_request;                /* Payload for TASK_REQUEST */
        task_outcome_t result[MAX_TASK_JSON_OUT];    /* Payload for TASK_RESULT */
    } data;
} ipc_msg_t;



#endif /* TASK_IPC_H */
