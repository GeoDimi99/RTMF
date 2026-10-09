#ifndef EXECUTION_MANAGER_H
#define EXECUTION_MANAGER_H

#define _GNU_SOURCE
#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <mqueue.h>
#include <time.h>            // For measure the performance
#include <unistd.h>          // gettid()
#include <sys/resource.h>    // setpriority() for the nice value
#include <hiredis/hiredis.h>


#include "schedule.h"
#include "app_task.h"
#include "task_ipc.h"



#define DEFAULT_EXECUTION_MANAGER_NAME "execution_manager"

extern gint iteration; // Counter for measure the performance


/* Execution Manager Stucture */
typedef struct execution_manager_t{
    GString *em_name;               // Execution Manager Name
    gboolean is_leader;             // Execution Manager Role (is leader or not)
    mqd_t em_queue;                 // Execution Manager Queue (Receive)
    redisContext* redis_client;     // Redis Client (for Schedule)
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


typedef struct {
    guint16 task_id;            // Task ID
    gpointer data;              // Task input
    GThreadFunc thread_func;    // Task function
    schedule_t *sched;          // Reference to the schedule for store the result
    glong start_time_request;   // For measure the performance (dispatch time, before thread creation)
    gint policy;                // Scheduler policy of the task thread
    gint8 priority;             // RT priority (FIFO/RR) or nice value (OTHER/BATCH)
} task_wrapper_input_t;




/* Executor Manager Constructor/Distructors */
execution_manager_t* em_new(const gchar *name);
void em_free(execution_manager_t *em);

/* Execution Manager Getters/Setters */
void em_set_leader(execution_manager_t *em, gboolean leader_flag);


/* Execution Manager Activities*/
void em_run_schedule(execution_manager_t *em, schedule_t *sched);
void em_wait_for_schedule(execution_manager_t *em);
schedule_t * em_read_schedule(execution_manager_t *em);

/* Exectuion Manager Usefull Functions  */
void* task_wrapper_func(void* data);

/* Execution Manager Event Handlers */
gboolean handle_initialization(gpointer user_data);
gboolean handle_expiration(gpointer user_data);

#endif // EXECUTION_MANAGER_H