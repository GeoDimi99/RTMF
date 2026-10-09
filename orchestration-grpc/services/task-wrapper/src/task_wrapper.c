#include "task_wrapper.h"



/**
 * Initializes the task wrapper context and queues.
 * 1. Sets up the wrapper name and queue names.
 * 2. Creates/Resets the RX queue (Input).
 * 3. Polls/Waits for the TX queue (Output/Manager) to become available.
 */
int init_task_wrapper(task_wrapper_t* svc, const char* name, const char* rx_q, const char* tx_q) {
    if (!svc) return -1;

    /* Initialize context strings and state */
    snprintf(svc->task_name, MAX_TASK_NAME, "%s", name);
    snprintf(svc->rx_queue_name, MAX_QUEUE_NAME, "%s", rx_q);
    snprintf(svc->tx_queue_name, MAX_QUEUE_NAME, "%s", tx_q);

    /* Setup RX Queue (Wrapper Input) */
    /* Destroy the queue if it was left over from a previous crash. This ensures we start with an empty queue. */
    destroy_queue(svc->rx_queue_name);
    svc->rx_fd = create_queue(svc->rx_queue_name, O_RDONLY); 

    log_message(LOG_INFO, svc->task_name , "RX Queue initialized: %s", svc->rx_queue_name);


    /* Setup TX Queue (Connection to Execution Manager) */
    /* Polling Loop: Wait until the Manager creates its queue */
    while (1) {
        /* Attempt to open the queue using the new helper (No O_CREAT) */
        svc->tx_fd = open_queue(svc->tx_queue_name, O_WRONLY);

        /* Case A: Success - Connection established */
        if (svc->tx_fd != (mqd_t)-1) {
            log_message(LOG_INFO, svc->task_name , "TX Queue initialized: %s", svc->tx_queue_name);
            break; /* Exit the loop */
        }

        /* Case B: Queue not found yet (Manager is not ready) */
        if (errno == ENOENT) {
            /* Wait 1 second before retrying */
            sleep(1); 
        } 
        /* Case C: Critical failure (Permission denied, limit reached, etc.) */
        else {
            log_message(LOG_ERROR, svc->task_name, "Fatal error connecting to Execution Manager: %s", strerror(errno));
            return -1; /* Return failure code */
        }
    }

    return 0; /* Initialization successful */
}



/**
 * Cleanups resources.
 */
void close_task_wrapper(task_wrapper_t* svc) {
    if (svc) {
        log_message(LOG_INFO, svc->task_name , "Shutting down...");
        close_queue(svc->rx_fd);
        close_queue(svc->tx_fd);
        destroy_queue(svc->rx_queue_name);
    }
}