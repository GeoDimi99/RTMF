#ifndef TASK_WRAPPER_H
#define TASK_WRAPPER_H

#include <stdint.h>
#include <unistd.h>       /* For sleep() */
#include <errno.h>        /* For errno, ENOENT */
#include <stdio.h>
#include <string.h>

#include "task.h"
#include "logger.h"
#include "task_ipc.h"

#define DEFAULT_TASK_NAME "task-wrapper"



/* Task descriptor */
typedef struct {
    /* Static configuration */
    char task_name[MAX_TASK_NAME];
    char rx_queue_name[MAX_QUEUE_NAME];     // Listening queue (Task Queue)
    char tx_queue_name[MAX_QUEUE_NAME];     // Response queue (EM Queue) 

    /* Runtime state */
    mqd_t rx_fd;                            // Input queue descriptor
    mqd_t tx_fd;                            // Output queue descriptor

} task_wrapper_t;



int init_task_wrapper(task_wrapper_t* svc, const char* name, const char* rx_q, const char* tx_q);
void run_task_wrapper(task_wrapper_t* svc);
void close_task_wrapper(task_wrapper_t* svc);



#endif /* TASK_WRAPPER_H */
