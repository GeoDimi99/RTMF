#ifndef TASK_IPC_H
#define TASK_IPC_H

#include <stdint.h>
#include <mqueue.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include "task.h"

/* Max sizes */
#define MAX_QUEUE_NAME      64
#define MAX_TASK_NAME       64
#define MAX_TASK_JSON_OUT   4096

/* Queue names */
#define DEFAULT_EM_QUEUE    "/execution_manager"
#define DEFAULT_TASK_QUEUE  "/task_service"

/* --- Data Structures & Enums --- */

/* Message types identifier */
typedef enum {
    MSG_TASK_REQUEST = 1,
    MSG_TASK_ACK,
    MSG_GET_STATUS,
    MSG_TASK_STATUS,
    MSG_GET_RESULTS,
    MSG_TASK_RESULT
} msg_type_t;

/* ACK status codes */
typedef enum {
    ACK_OK = 0,
    ACK_ERROR = 1
} ack_status_t;

/* Service operational states */
typedef enum {
    IDLE = 0,
    RUNNING,
    COMPLETED
} task_service_state_t;

/* Main IPC Message Structure */
typedef struct {
    msg_type_t type;
    uint32_t   task_id;

    union {
        task_t task;                    /* Payload for TASK_REQUEST */
        ack_status_t ack;               /* Payload for TASK_ACK */
        task_service_state_t status;    /* Payload for STATUS */
        char result[MAX_TASK_JSON_OUT]; /* Payload for TASK_RESULT */
    } data;
} ipc_msg_t;

/* --- POSIX MQ Helper Prototypes --- */

/* Create a new queue (uses O_CREAT). Exits on failure. */
mqd_t create_queue(const char *name, int flags);

/* Open an existing queue (no O_CREAT). Returns -1 on failure. */
mqd_t open_queue(const char *name, int flags);

/* Send a typed message */
int send_message(mqd_t qd, const ipc_msg_t *msg);

/* Receive a typed message */
ssize_t receive_message(mqd_t qd, ipc_msg_t *msg);

/* Close the queue descriptor (Process level) */
void close_queue(mqd_t qd);

/* Unlink/Destroy the queue (System level) */
void destroy_queue(const char *name);

#endif /* TASK_IPC_H */
