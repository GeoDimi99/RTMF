#include "task_ipc.h"


/* * Create/Open a queue with O_CREAT.
 * Note: If the queue creation fails, this function terminates the program.
 */
mqd_t create_queue(const char *name, int flags)
{
    struct mq_attr attr = {
        .mq_maxmsg  = 10,               /* Max messages in queue */
        .mq_msgsize = sizeof(ipc_msg_t), /* Fixed message size */
    };

    /* flags | O_CREAT ensures the queue is created if it doesn't exist */
    mqd_t qd = mq_open(name, flags | O_CREAT, 0666, &attr);
    
    if (qd == (mqd_t)-1) {
        perror("mq_open (create) failed");
        exit(EXIT_FAILURE);
    }
    return qd;
}

/* * Open an existing queue WITHOUT O_CREAT.
 * Returns -1 on failure, allowing the caller to handle the error (e.g., polling).
 */
mqd_t open_queue(const char *name, int flags)
{
    /* Explicitly remove O_CREAT to ensure we only open existing queues */
    flags &= ~O_CREAT; 

    mqd_t qd = mq_open(name, flags);
    
    /* We do NOT exit here. We return the error to the caller. */
    return qd;
}

/* * Send a message to the queue. 
 * Returns 0 on success, -1 on failure.
 */
int send_message(mqd_t qd, const ipc_msg_t *msg)
{
    return mq_send(qd, (const char *)msg, sizeof(*msg), 0);
}

/* * Receive a message from the queue.
 * Returns 1 on success, 0 if empty (non-blocking), -1 on error.
 */
ssize_t receive_message(mqd_t qd, ipc_msg_t *msg)
{
    return mq_receive(qd, (char *)msg, sizeof(*msg), NULL);
}

/* Close the descriptor. Does not remove the queue from the system. */
void close_queue(mqd_t qd)
{
    if (mq_close(qd) == -1) {
        perror("Warning: mq_close failed");
    }
}

/* Remove the queue from the kernel. Critical for cleanup. */
void destroy_queue(const char *name)
{
    /* mq_unlink returns -1 if the queue doesn't exist (ENOENT), which is fine. */
    if (mq_unlink(name) == -1) {
        if (errno != ENOENT) {
            perror("Warning: mq_unlink failed");
        }
    }
}