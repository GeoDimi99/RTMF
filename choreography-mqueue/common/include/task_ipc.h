#ifndef TASK_IPC_H
#define TASK_IPC_H

#include <mqueue.h>
#include <glib.h>
#include "task.h"

#define PRIO_NORMAL 0
#define PRIO_HIGH   10
#define PRIO_SYNC   20  /* Highest priority for synchronization */

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
    MSG_TASK_READY,
    MSG_TASK_SYNC,
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
    union {
        task_request_t task_request;                /* Payload for TASK_REQUEST */
        task_outcome_t result[MAX_TASK_JSON_OUT];    /* Payload for TASK_RESULT */
        gint64 sync_time_us;
    } data;
} ipc_msg_t;



#endif /* TASK_IPC_H */
