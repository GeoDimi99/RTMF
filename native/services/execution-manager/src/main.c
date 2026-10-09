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
#include <stdlib.h>


#define DEFAULT_EM_CPU_AFFINITY 4
#define DEFAULT_TASK_CPU_AFFINITY 5


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
    sched_setscheduler(0, SCHED_FIFO, &param);


    /* Registre the signal handler for a clean clousure */
    signal(SIGINT, int_handler);

    /* ------ Init Execution Manager ------ */
    int exit_code = 0;
    execution_manager_t *em = em_new(DEFAULT_EXECUTION_MANAGER_NAME);
    if (!em) {
        log_message(LOG_ERROR, "execution-manager", "em_new failed.");
        exit(EXIT_FAILURE);
    }

    schedule_t *sched = NULL; // Init to NULL

    log_message(LOG_INFO, "execution-manager", "Execution Manager Initialized");
    log_message(LOG_INFO, "execution-manager", "Click Ctrl+C for a clean exit.");

    /* Control for new schedule */
    gboolean is_set_new_schedule = TRUE;


    /* -------------- Main Loop Execution -------------- */
    //while (keep_running) {
    int num_iter = 100;
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
            gchar *schedule_name = "schedule";
            sched = schedule_new(schedule_name, "0.0.1");
            if (!sched) {
                log_message(LOG_ERROR, "execution-manager", "(%s) : scheduler creation failed.", schedule_name);
                exit(EXIT_FAILURE);
            }

            

            // void schedule_add_task(schedule_t *sched, guint16 id, const gchar *name, GThreadFunc task_exec, gint policy, gint8 priority, gint cpu_affinity, guint8 repetition, GSList *depends_on,  gint64 start_time, gint64 end_time, gpointer input);
            

            /* ***********************************
            *  TEST 1: SINGLE TASK - 
            *  - NUM OPERATION 100 , 
            *  - IO BURST VARIABLE
            ************************************** */
            // input_t *func_input = g_new0(input_t, 1); 

            /* Test 1: Single Task - Num operation 100, IO Burst: 0% */
            // func_input->total_ops = 800;
            // func_input->io_percentage = 0;
            // schedule_add_task(sched, 1, "stress_task_1", task_main, SCHED_FIFO, 1, DEFAULT_TASK_CPU_AFFINITY, 1, NULL, 1000, 5000, func_input);

            /* Test 1: Single Task - Num operation 4, IO Burst: 25% */
            // func_input->total_ops = 800;
            // func_input->io_percentage = 25;
            // schedule_add_task(sched, 2, "stress_task_1", task_main, SCHED_FIFO, 1, DEFAULT_TASK_CPU_AFFINITY, 1, NULL, 1000, 5000, func_input);

            /* Test 1: Single Task - Num operation 4, IO Burst: 50% */
            // func_input->total_ops = 800;
            // func_input->io_percentage = 50;
            // schedule_add_task(sched, 3, "stress_task_1", task_main, SCHED_FIFO, 1, DEFAULT_TASK_CPU_AFFINITY, 1, NULL, 1000, 5000, func_input);

            /* Test 1: Single Task - Num operation 4, IO Burst: 75% */
            // func_input->total_ops = 800;
            // func_input->io_percentage = 75;
            // schedule_add_task(sched, 4, "stress_task_1", task_main, SCHED_FIFO, 1, DEFAULT_TASK_CPU_AFFINITY, 1, NULL, 1000, 5000, func_input);

            /* Test 1: Single Task - Num operation 4, IO Burst: 100% */
            // func_input->total_ops = 800;
            // func_input->io_percentage = 100;
            // schedule_add_task(sched, 5, "stress_task_1", task_main, SCHED_FIFO, 1, DEFAULT_TASK_CPU_AFFINITY, 1, NULL, 1000, 5000, func_input);

            /* ***********************************
            *  TEST 2: FOUR TASKS SEQUENTIAL - 
            *  - NUM OPERATION 100 , 
            *  - IO BURST VARIABLE
            ************************************** */
            // input_t *func_input_1 = g_new0(input_t, 1);
            // func_input_1->total_ops = 800;
            // func_input_1->io_percentage = 50;
            // schedule_add_task(sched, 1, "stress_task_1", task_main, SCHED_FIFO, 50, DEFAULT_TASK_CPU_AFFINITY, 1, NULL, 10 * 1000, 30 * 1000, func_input_1);

            // input_t *func_input_2 = g_new0(input_t, 1);
            // func_input_2->total_ops = 800;
            // func_input_2->io_percentage = 50;
            // schedule_add_task(sched, 2, "stress_task_2", task_main, SCHED_FIFO, 50, DEFAULT_TASK_CPU_AFFINITY, 1, NULL, 15 * 1000, 35 * 1000, func_input_2);

            // input_t *func_input_3 = g_new0(input_t, 1);
            // func_input_3->total_ops = 800;
            // func_input_3->io_percentage = 50;
            // schedule_add_task(sched, 3, "stress_task_3", task_main, SCHED_FIFO, 50, DEFAULT_TASK_CPU_AFFINITY, 1, NULL, 20 * 1000, 40 * 1000, func_input_3);

            // input_t *func_input_4 = g_new0(input_t, 1);
            // func_input_4->total_ops = 800;
            // func_input_4->io_percentage = 50;
            // schedule_add_task(sched, 4, "stress_task_4", task_main, SCHED_FIFO, 50, DEFAULT_TASK_CPU_AFFINITY, 1, NULL, 25 * 1000, 45 * 1000, func_input_4);


             /* ***********************************
            *  TEST 3: FOUR TASKS SOVRAPOSITION 2-2 - 
            *  - NUM OPERATION 100 , 
            *  - IO BURST VARIABLE
            ************************************** */
            // input_t *func_input_1 = g_new0(input_t, 1);
            // func_input_1->total_ops = 800;
            // func_input_1->io_percentage = 50;
            // schedule_add_task(sched, 1, "stress_task_1", task_main, SCHED_FIFO, 50, DEFAULT_TASK_CPU_AFFINITY, 1, NULL, 10 * 1000, 30 * 1000, func_input_1);

            // input_t *func_input_2 = g_new0(input_t, 1);
            // func_input_2->total_ops = 800;
            // func_input_2->io_percentage = 50;
            // schedule_add_task(sched, 2, "stress_task_2", task_main, SCHED_FIFO, 50, DEFAULT_TASK_CPU_AFFINITY, 1, NULL, 11 * 1000, 31 * 1000, func_input_2);

            // input_t *func_input_3 = g_new0(input_t, 1);
            // func_input_3->total_ops = 800;
            // func_input_3->io_percentage = 50;
            // schedule_add_task(sched, 3, "stress_task_3", task_main, SCHED_FIFO, 50, DEFAULT_TASK_CPU_AFFINITY, 1, NULL, 20 * 1000, 40 * 1000, func_input_3);

            // input_t *func_input_4 = g_new0(input_t, 1);
            // func_input_4->total_ops = 800;
            // func_input_4->io_percentage = 50;
            // schedule_add_task(sched, 4, "stress_task_4", task_main, SCHED_FIFO, 50, DEFAULT_TASK_CPU_AFFINITY, 1, NULL, 21 * 1000, 41 * 1000, func_input_4);

            /* ***********************************
            *  TEST 4: FOUR TASKS SOVRAPOSITION 2-2 DIFFERENT PRIORITY - 
            *  - NUM OPERATION 100 , 
            *  - IO BURST VARIABLE
            ************************************** */
            // input_t *func_input_1 = g_new0(input_t, 1);
            // func_input_1->total_ops = 800;
            // func_input_1->io_percentage = 50;
            // schedule_add_task(sched, 1, "stress_task_1", task_main, SCHED_FIFO, 50, DEFAULT_TASK_CPU_AFFINITY, 1, NULL, 10 * 1000, 30 * 1000, func_input_1);

            // input_t *func_input_2 = g_new0(input_t, 1);
            // func_input_2->total_ops = 800;
            // func_input_2->io_percentage = 50;
            // schedule_add_task(sched, 2, "stress_task_2", task_main, SCHED_FIFO, 65, DEFAULT_TASK_CPU_AFFINITY, 1, NULL, 11 * 1000, 31 * 1000, func_input_2);

            // input_t *func_input_3 = g_new0(input_t, 1);
            // func_input_3->total_ops = 800;
            // func_input_3->io_percentage = 50;
            // schedule_add_task(sched, 3, "stress_task_3", task_main, SCHED_FIFO, 65, DEFAULT_TASK_CPU_AFFINITY, 1, NULL, 20 * 1000, 40 * 1000, func_input_3);

            // input_t *func_input_4 = g_new0(input_t, 1);
            // func_input_4->total_ops = 800;
            // func_input_4->io_percentage = 50;
            // schedule_add_task(sched, 4, "stress_task_4", task_main, SCHED_FIFO, 50, DEFAULT_TASK_CPU_AFFINITY, 1, NULL, 21 * 1000, 41 * 1000, func_input_4);

            /* ***********************************
            *  TEST 5: FOUR TASKS SOVRAPOSITION 1-3 SAME PRIORITY - 
            *  - NUM OPERATION 100 , 
            *  - IO BURST VARIABLE
            ************************************** */
            // input_t *func_input_1 = g_new0(input_t, 1);
            // func_input_1->total_ops = 800;
            // func_input_1->io_percentage = 50;
            // schedule_add_task(sched, 1, "stress_task_1", task_main, SCHED_FIFO, 50, DEFAULT_TASK_CPU_AFFINITY, 1, NULL, 10 * 1000, 20 * 1000, func_input_1);

            // input_t *func_input_2 = g_new0(input_t, 1);
            // func_input_2->total_ops = 800;
            // func_input_2->io_percentage = 50;
            // schedule_add_task(sched, 2, "stress_task_2", task_main, SCHED_FIFO, 50, DEFAULT_TASK_CPU_AFFINITY, 1, NULL, 20 * 1000, 30 * 1000, func_input_2);

            // input_t *func_input_3 = g_new0(input_t, 1);
            // func_input_3->total_ops = 800;
            // func_input_3->io_percentage = 50;
            // schedule_add_task(sched, 3, "stress_task_3", task_main, SCHED_FIFO, 50, DEFAULT_TASK_CPU_AFFINITY, 1, NULL, 21 * 1000, 31 * 1000, func_input_3);

            // input_t *func_input_4 = g_new0(input_t, 1);
            // func_input_4->total_ops = 800;
            // func_input_4->io_percentage = 50;
            // schedule_add_task(sched, 4, "stress_task_4", task_main, SCHED_FIFO, 50, DEFAULT_TASK_CPU_AFFINITY, 1, NULL, 21 * 1000, 31 * 1000, func_input_4);

            /* ***********************************
            *  TEST 6: FOUR TASKS SOVRAPOSITION 1-3 DIFFERENT PRIORITY - 
            *  - NUM OPERATION 100 , 
            *  - IO BURST VARIABLE
            ************************************** */
           input_t *func_input_1 = g_new0(input_t, 1);
func_input_1->total_ops = 17;
func_input_1->io_percentage = 0;
schedule_add_task(sched, 1, "stress_task_1", task_main, SCHED_FIFO, 50, 5, 1, NULL, 50, 650, func_input_1);

input_t *func_input_2 = g_new0(input_t, 1);
func_input_2->total_ops = 17;
func_input_2->io_percentage = 0;
schedule_add_task(sched, 2, "stress_task_2", task_main, SCHED_FIFO, 50, 5, 1, NULL, 300, 1300, func_input_2);

input_t *func_input_3 = g_new0(input_t, 1);
func_input_3->total_ops = 17;
func_input_3->io_percentage = 0;
schedule_add_task(sched, 3, "stress_task_3", task_main, SCHED_FIFO, 70, 5, 1, NULL, 350, 950, func_input_3);

input_t *func_input_4 = g_new0(input_t, 1);
func_input_4->total_ops = 17;
func_input_4->io_percentage = 0;
schedule_add_task(sched, 4, "stress_task_4", task_main, SCHED_FIFO, 60, 5, 1, NULL, 370, 970, func_input_4);

input_t *func_input_5 = g_new0(input_t, 1);
func_input_5->total_ops = 17;
func_input_5->io_percentage = 0;
schedule_add_task(sched, 5, "stress_task_5", task_main, SCHED_FIFO, 60, 5, 1, NULL, 600, 1200, func_input_5);

input_t *func_input_6 = g_new0(input_t, 1);
func_input_6->total_ops = 17;
func_input_6->io_percentage = 0;
schedule_add_task(sched, 6, "stress_task_6", task_main, SCHED_FIFO, 60, 5, 1, NULL, 650, 1250, func_input_6);


            schedule_print(sched);
        } else {
            schedule_reset(sched);
        }

        /* Run the schedule */
        log_message(LOG_INFO, "execution-manager", "Start iteration %d, progress percentage %d%%", i, (i*100)/num_iter);
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