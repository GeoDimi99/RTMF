#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <sched.h>
#include <signal.h> // For signals
#include <unistd.h> // For sleep()

#include "schedule.h"
#include "execution_manager.h"
#include "logger.h"


/* Gloabal flag for check the main loop */
static volatile gboolean keep_running = TRUE;


/* Signal Handler for SIGINT (Ctrl+C) */
void int_handler(int dummy) {
    (void)dummy;
    log_message(LOG_INFO, "execution-manager", "SIGINT received.");
    keep_running = FALSE;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv; 

    /* Registre the signal handler for a clean clousure */
    signal(SIGINT, int_handler);

    /* Set Core 4 */
    cpu_set_t set;
    CPU_ZERO(&set);                                     // clear cpu mask
    CPU_SET(4, &set);             // set cpu 4
    sched_setaffinity(0, sizeof(cpu_set_t), &set);      // 0 is the calling process

    /* Setting scheduler policy and priority*/
    struct sched_param param;
    param.sched_priority = 90; 
    sched_setscheduler(0, SCHED_FIFO, &param);


    /* ------ Init Execution Manager ------ */
    log_message(LOG_INFO, "execution-manager", "=========== Execution Manager Init ========");
    int exit_code = 0;
    execution_manager_t *em = em_new(DEFAULT_EXECUTION_MANAGER_NAME);
    if (!em) {
        log_message(LOG_ERROR, "execution-manager", "em_new failed.");
        exit(EXIT_FAILURE);
    }
    
    schedule_t *sched = NULL; // Init to NULL

    /* Wait for new schedule */
    em_wait_for_schedule(em);
    gboolean is_set_new_schedule = TRUE;


    log_message(LOG_INFO, "execution-manager", "====== Execution Manager Initialized ======");
    log_message(LOG_INFO, "execution-manager", "==== Execution Manager Start Main Loop ====");
    log_message(LOG_INFO, "execution-manager", "Click Ctrl+C for a clean exit.");



    /* -------------- Main Loop Execution -------------- */
    //while (keep_running) {
    int num_iter = 100; // for test
    for(int i=0; i < num_iter && keep_running; i++){     
        
        iteration = i; // Counter for test

        if (is_set_new_schedule) {

            /* Set to false for the next iteration*/
            is_set_new_schedule = FALSE;

            if (sched != NULL) {
                schedule_free(sched);
                sched = NULL;   // Avoid double-free at exit 
            }

            sched = em_read_schedule(em);
        
            /* Create a schedule */
            // gchar *schedule_name = "schedule";
            // sched = schedule_new(schedule_name, "0.0.1");
            // if (!sched) {
            //     g_error("[ERROR] Execution Manager (%s) : scheduler creation failed.", schedule_name);
            // }

            // /* Test 1: Single Task - Num operation 4 , IO Burst: 0% */
            // //schedule_add_task(sched, 1, "stress_task", SCHED_FIFO, 10, 1, 1, NULL, 1000, 2000, "[{\"total_ops\":4, \"io_percentage\":0}]");
            // schedule_add_task(sched, 1, "stress_task_1", SCHED_FIFO, 10, 1, 1, NULL, 1000, 20000, "[{\"total_ops\":50, \"io_percentage\":0}]");
            // schedule_add_task(sched, 2, "stress_task_2", SCHED_FIFO, 10, 1, 1, NULL, 1000, 20000, "[{\"total_ops\":50, \"io_percentage\":0}]");
            // schedule_add_task(sched, 3, "stress_task_3", SCHED_FIFO, 10, 1, 1, NULL, 1000, 20000, "[{\"total_ops\":50, \"io_percentage\":0}]");
            // schedule_add_task(sched, 4, "stress_task_4", SCHED_FIFO, 10, 1, 1, NULL, 1000, 20000, "[{\"total_ops\":50, \"io_percentage\":0}]");
            

            /* Test 1: Single Task - Num operation 4 , IO Burst: 25% */
            //schedule_add_task(sched, 1, "stress_task", SCHED_FIFO, 10, 1, 1, NULL, 1000, 2000, "[{\"total_ops\":4, \"io_percentage\":25}]");
            

            /* Test 1: Single Task - Num operation 4 , IO Burst: 50% */
            //schedule_add_task(sched, 1, "stress_task", SCHED_FIFO, 10, 1, 1, NULL, 1000, 2000, "[{\"total_ops\":4, \"io_percentage\":50}]");

            /* Test 1: Single Task - Num operation 4 , IO Burst: 75% */
            //schedule_add_task(sched, 1, "stress_task", SCHED_FIFO, 10, 1, 1, NULL, 1000, 2000, "[{\"total_ops\":4, \"io_percentage\":75}]");

            /* Test 1: Single Task - Num operation 4 , IO Burst: 100% */
            // schedule_add_task(sched, 1, "stress_task", SCHED_FIFO, 10, 1, 1, NULL, 1000, 2000, "[{\"total_ops\":4, \"io_percentage\":100}]");
            
            
            


            //schedule_add_task(sched, 2, "subtract", SCHED_POLICY_FIFO, 8, 1, 1, NULL,
            //                  1 * 1000, 7 * 1000, "[{\"a\":20, \"b\":8}]");

            //schedule_add_task(sched, 3, "multiply", SCHED_POLICY_FIFO, 6, 1, 1, NULL,
            //                  2 * 1000, 7 * 1000, "[{\"a\":4, \"b\":7}]");

            schedule_print(sched);
        } else {
            schedule_reset(sched);
        }

        /* Run the schedule */
        log_message(LOG_INFO, "execution-manager", "Start iteration %d, progress percentage %d %%", i, (i*100)/num_iter );
        em_run_schedule(em, sched);
        


        //if (keep_running) {
            //g_print("\n[INFO] Execution Manager: Schedule Completed. Reboot in 5 seconds... (or push Ctrl+C for exit)...\n\n");
            
            /* Slee for 5 second, but check flag each second */
            //for (int i = 0; i < 5 && keep_running; i++) {
                //sleep(1);
            //}
        //}

        // schedule_reset(sched); // DO NOT reset a schedule that is about to be freed. This causes memory corruption.
    }

    
    log_message(LOG_INFO, "execution-manager", "==== Execution Manager Exit Main Loop ====");
    log_message(LOG_INFO, "execution-manager", "Exit from the main loop. Cleanup ...");

    if (em) em_free(em);
    if (sched) schedule_free(sched);

    log_message(LOG_INFO, "execution-manager", "Cleanup completed.");
    log_message(LOG_INFO, "execution-manager", "==== Execution Manager Fish ====");
    return exit_code;
}
