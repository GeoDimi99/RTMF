#define _GNU_SOURCE
#include <glib.h>
#include <signal.h> // For signals
#include <unistd.h> // For sleep()
#include <sched.h>
#include <sys/mman.h>
#include <errno.h>

#include "schedule.h"
#include "execution_manager.h"
#include "app_task.h"
#include "logger.h"
#include "schedule_plan.h"     // Generated from the manifest
#include <stdlib.h>


#define DEFAULT_EM_CPU_AFFINITY 4
#define DEFAULT_TASK_CPU_AFFINITY 5


/* Gloabal flag for check the main loop */
volatile gboolean keep_running = TRUE;


/* Signal Handler for SIGINT (Ctrl+C) */
void int_handler(int dummy) {
    (void)dummy;
    log_message(LOG_INFO, "execution-manager", "SIGINT received");
    keep_running = FALSE;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;


    /* Set Core DEFAULT_EM_CPU_AFFINITY */
    cpu_set_t set;
    CPU_ZERO(&set);                                     // clear cpu mask
    CPU_SET(DEFAULT_EM_CPU_AFFINITY, &set);             // set cpu DEFAULT_EM_CPU_AFFINITY
    sched_setaffinity(0, sizeof(cpu_set_t), &set);      // 0 is the calling process


    /* Lock memory */
    if(mlockall(MCL_CURRENT|MCL_FUTURE) == -1) {
        log_message(LOG_ERROR, "execution-manager", "mlockall failed: %s", g_strerror(errno));
        exit(EXIT_FAILURE);
    }
    
    /* Setting scheduler policy and priority*/
    struct sched_param param;
    param.sched_priority = 95;
    if (sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
        log_message(LOG_WARN, "execution-manager", "sched_setscheduler failed: %s. Try running with root privileges", g_strerror(errno));
    }


    /* Registre the signal handler for a clean clousure */
    signal(SIGINT, int_handler);

    /* ------ Init Execution Manager ------ */
    int exit_code = 0;
    execution_manager_t *em = em_new(DEFAULT_EXECUTION_MANAGER_NAME);
    if (!em) {
        log_message(LOG_ERROR, "execution-manager", "em_new failed");
        exit(EXIT_FAILURE);
    }

    schedule_t *sched = NULL; // Init to NULL

    log_message(LOG_INFO, "execution-manager", "Initialized (Click Ctrl+C for a clean exit)");

    /* Control for new schedule */
    gboolean is_set_new_schedule = TRUE;


    /* -------------- Main Loop Execution -------------- */
    //while (keep_running) {
    int num_iter = PLAN_ITERATIONS;
    for(int i=0; i < num_iter && keep_running; i++){     // For test
        iteration = i; // Counter for measure the performance

        if (is_set_new_schedule){

            /* Set to false for the next iteration*/
            is_set_new_schedule = FALSE;

            if (sched != NULL) {
                schedule_free(sched);
                sched = NULL;   // Avoid double-free at exit
            }

            /* Create a schedule */
            gchar *schedule_name = PLAN_SCHEDULE_NAME;
            sched = schedule_new(schedule_name, "0.0.1");
            if (!sched) {
                log_message(LOG_ERROR, "execution-manager", "(%s) : scheduler creation failed", schedule_name);
                exit(EXIT_FAILURE);
            }

            /* Add the tasks of the manifest (tests/test_0_code/manifest.yaml): schedule_plan.h is
             * generated from it by tools/manifest_to_plan.py (start-test.sh does it before the build) */
            schedule_plan_add_tasks(sched);

            schedule_print(sched);
        } else {
            schedule_reset(sched);
        }

        /* Run the schedule */
        log_message(LOG_INFO, "execution-manager", "Start iteration %d/%d, progress percentage %d%%", i, num_iter, (i*100)/num_iter);
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

    log_message(LOG_INFO, "execution-manager", "Cleanup completed");
    return exit_code;
}