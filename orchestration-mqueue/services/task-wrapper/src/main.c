#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <glib.h>
#include <signal.h>
#include "task_wrapper.h"
#include "logger.h"


/* Global reference to the main loop to allow the signal handler to quit it. */
static GMainLoop *g_main_loop = NULL;


/**
 * @brief Signal handler for SIGINT (Ctrl+C).
 * 
 * This function is called when the user presses Ctrl+C. It gracefully
 * requests the GMainLoop to terminate, allowing for a clean shutdown.
 */
static void sigint_handler(int signum) {
    (void)signum; // Unused parameter
    log_message(LOG_INFO, "task-wrapper", "SIGINT received, initiating shutdown...");
    if (g_main_loop) g_main_loop_quit(g_main_loop);
}

int main(int argc, char *argv[]) {
    int exit_code = 0;



    /* Memory Locking per Real-Time */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
        perror("mlockall failed");
        return -1;
    }

     /* Set Core 4 */
    cpu_set_t set;
    CPU_ZERO(&set);                                     // clear cpu mask
    CPU_SET(4, &set);             // set cpu 4
    sched_setaffinity(0, sizeof(cpu_set_t), &set);      // 0 is the calling process

    /* Setting scheduler policy and priority*/
    struct sched_param param;
    param.sched_priority = 90; 
    sched_setscheduler(0, SCHED_FIFO, &param);


    /* Register the signal handler for a clean closure */
    signal(SIGINT, sigint_handler);

    /* Init Task Wrapper */
    char *env_task = getenv("TASK_NAME");
    char *env_queue = getenv("TASK_QUEUE_NAME");
    
    
    gchar *q_path = g_strdup_printf("/%s_q", env_queue ? env_queue : DEFAULT_TASK_QUEUE);

    log_message(LOG_INFO, "task-wrapper", "======== Task Wrapper Init ========");
    task_wrapper_t *tw = task_wrapper_new(
        env_task ? env_task : DEFAULT_TASK_NAME, 
        q_path, 
        "/execution_manager_q"
    );
    g_free(q_path);

    

    /* 3. Setup GMainLoop e Sorgenti eventi */
    g_main_loop = g_main_loop_new(NULL, FALSE);

    // Trasformiamo il descrittore mqd_t in un canale monitorabile da GLib
    GIOChannel *channel = g_io_channel_unix_new((gint)tw->task_queue);
    
    // Molto importante: mq_receive vuole un canale binario senza codifiche strane
    g_io_channel_set_encoding(channel, NULL, NULL);
    g_io_channel_set_buffered(channel, FALSE);

    // Colleghiamo la coda al loop. Passiamo 'tw' per averlo nella callback.
    g_io_add_watch(channel, G_IO_IN, (GIOFunc)handle_input_message, tw);

    log_message(LOG_INFO, "task-wrapper", "=== Task Wrapper (%s) Initialized ===", tw->task_name->str);
    log_message(LOG_INFO, "task-wrapper", "Click Ctrl+C for a clean exit.");
    log_message(LOG_INFO, "task-wrapper", "=== Task Wrapper (%s) Start Main Loop ===", tw->task_name->str);

    log_message(LOG_INFO, "task-wrapper", "(%s): It's ready. Waiting for messages ...", tw->task_name->str);

    /* Start Loop */
    g_main_loop_run(g_main_loop);

    /* Cleanup */
    log_message(LOG_INFO, "task-wrapper", "=== Task Wrapper (%s) End Main Loop ===", tw->task_name->str);
    log_message(LOG_INFO, "task-wrapper", "(%s): Main loop terminated. Cleaning up...", tw->task_name->str);
    g_io_channel_unref(channel);
    g_main_loop_unref(g_main_loop);
    task_wrapper_free(tw);
    log_message(LOG_INFO, "task-wrapper", "Cleanup complete. Exiting.");
    log_message(LOG_INFO, "task-wrapper", "=== Task Wrapper Finish ===");

    return exit_code;
}