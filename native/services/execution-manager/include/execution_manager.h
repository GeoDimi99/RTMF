#ifndef EXECUTION_MANAGER_H
#define EXECUTION_MANAGER_H

#define _GNU_SOURCE
#include <glib.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>            // For measure the performance
#include <unistd.h>          // gettid()
#include <sys/resource.h>    // setpriority() for the nice value

#include "schedule.h"
#include "execution_manager.h"



#define DEFAULT_EXECUTION_MANAGER_NAME "execution_manager"

extern gint iteration; // Counter for measure the performance

/* Execution Manager Stucture */
typedef struct execution_manager_t{
    GString *em_name;               // Execution Manager Name
} execution_manager_t;



typedef struct {
    gpointer data;      // GList of activation_data_t
    gint64 timestamp;   
    schedule_t *sched;
} start_context_t;

typedef struct {
    gpointer data;       // GSList of expiration_data_t 
    GMainLoop *loop;     // Reference to end the process 
    gboolean is_last;    // Flag that indicat if is the last event 
    gint64 timestamp;    
    schedule_t *sched;
} deadline_context_t;


typedef struct task_wrapper_input_t {
    guint16 task_id;            // Task ID
    gpointer data;              // Task input
    GThreadFunc thread_func;    // Task function
    schedule_t *sched;          // Reference to the schedule for store the result
    gint policy;                // Scheduler policy of the task thread
    gint8 priority;             // RT priority (FIFO/RR) or nice value (OTHER/BATCH)
    glong start_time_request;   // For measure the performance (dispatch time, before thread creation)
    /* Result record, filled by the task thread and handed to the scheduler main loop */
    gint iteration;             // Iteration the activation belongs to (for the performance)
    glong end_time_request;     // Beginning of the task thread execution (for the performance)
    glong start_time_result;    // End of the task thread execution (for the performance)
    gint core_id;               // Core the task thread ran on (for the performance)
    struct task_wrapper_input_t *next;  // Lock-free list of the completed tasks
} task_wrapper_input_t;




/* Executor Manager Constructor/Distructors */
execution_manager_t* em_new(const gchar *name);
void em_free(execution_manager_t *em);


/* Exection Manager Activities*/
void em_run_schedule(execution_manager_t *em, schedule_t *sched);

/* Exectuion Manager Usefull Functions  */
void* task_wrapper_func(void* data);

/* Execution Manager Event Handlers */
gboolean handle_initialization(gpointer user_data);
gboolean handle_expiration(gpointer user_data);

#endif // EXECUTION_MANAGER_H