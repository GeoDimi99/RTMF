#ifndef TASK_IPC_H
#define TASK_IPC_H

#include <zmq.h>
#include <glib.h>
#include "task.h"

/* ZeroMQ IPC endpoints for synchronization (shared volume at /ipc) */
#define ZMQ_SYNC_PULL_ENDPOINT  "ipc:///ipc/sync_ready.sock"   /* Leader PULL: receives READY from workers */
#define ZMQ_SYNC_PUB_ENDPOINT   "ipc:///ipc/sync_pub.sock"     /* Leader XPUB: broadcasts SYNC to all workers */

#define MAX_TASK_JSON_IN  1024
#define MAX_TASK_JSON_OUT 1024


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


/* Main IPC Message Structure (sent over ZeroMQ) */
typedef struct {
    msg_type_t type;
    guint16  task_id;
    union {
        task_request_t task_request;                /* Payload for TASK_REQUEST */
        task_outcome_t result[MAX_TASK_JSON_OUT];   /* Payload for TASK_RESULT */
        gint64 sync_time_us;                        /* T-Zero timestamp (microseconds, monotonic) */
    } data;
} ipc_msg_t;



#endif /* TASK_IPC_H */
