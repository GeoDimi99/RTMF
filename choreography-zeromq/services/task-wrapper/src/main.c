#define _GNU_SOURCE
#include <glib.h>
#include <stdlib.h>
#include <signal.h> // For signals
#include <unistd.h> // For sleep()
#include <sched.h>
#include <sys/mman.h>

#include "schedule.h"
#include "execution_manager.h"
#include "app_task.h"
#include "logger.h"




/* Gloabal flag for check the main loop */
volatile gboolean keep_running = TRUE;


/* Signal Handler for SIGINT (Ctrl+C) */
void int_handler(int dummy) {
    (void)dummy;
    log_message(LOG_INFO, "execution-manager", "SIGINT received.");
    keep_running = FALSE;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;


    /* Lock memory */
    if(mlockall(MCL_CURRENT|MCL_FUTURE) == -1) {
        log_message(LOG_ERROR, "execution-manager", "mlockall failed: %m");
        exit(EXIT_FAILURE);
    }

    /* Registre the signal handler for a clean clousure */
    signal(SIGINT, int_handler);

    /* Set Core 4 */
    cpu_set_t set;
    CPU_ZERO(&set);                                     // clear cpu mask
    CPU_SET(4, &set);                                   // set cpu 4
    sched_setaffinity(0, sizeof(cpu_set_t), &set);      // 0 is the calling process

    /* ------ Init Execution Manager ------ */
    int exit_code = 0;
    char *env_task = getenv("TASK_NAME");
    char *env_queue = getenv("TASK_QUEUE_NAME");
    
    gchar *em_name = g_strdup_printf("%s", env_task ? env_task : DEFAULT_EXECUTION_MANAGER_NAME);

    execution_manager_t *em = em_new(em_name);
    if (!em) {
        log_message(LOG_ERROR, "execution-manager", "em_new failed.");
        exit(EXIT_FAILURE);
    }

    schedule_t *sched = NULL; // Init to NULL

    log_message(LOG_INFO, "execution-manager", "Initialized (Click Ctrl+C for a clean exit)");

    /* Control for new schedule */
    em_wait_for_schedule(em);
    sched = em_read_schedule(em);
    schedule_print(sched);


    /* -------------- Main Loop Execution -------------- */
    for(int i=0; i < sched->schedule_iterations && keep_running; i++){
        iteration = i; // Counter for measure the performance

        if (i > 0) {
            schedule_reset(sched);
        }

        /* Run the schedule */
        log_message(LOG_INFO, "execution-manager", "Start iteration %d/%d, progress percentage %d%%", i, sched->schedule_iterations, (i*100)/sched->schedule_iterations);
        em_run_schedule(em, sched);
        

        //if (keep_running) {
            //g_print("\n[INFO] Execution Manager: Schedule Completed. Reboot in 5 seconds... (or push Ctrl+C for exit)...\n\n");
            
            /* Slee for 5 second, but check flag each second */
            //for (int i = 0; i < 5 && keep_running; i++) {
                //sleep(1);
            //}
        //}
    }

    log_message(LOG_INFO, "execution-manager", "Exit from the main loop. Cleanup ...");

    if (em) em_free(em);
    if (sched) schedule_free(sched);

    log_message(LOG_INFO, "execution-manager", "Cleanup completed.");
    return exit_code;
}