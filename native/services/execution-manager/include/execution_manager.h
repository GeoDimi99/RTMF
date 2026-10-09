#ifndef EXECUTION_MANAGER_H
#define EXECUTION_MANAGER_H

#define _GNU_SOURCE
#include <glib.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

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


typedef struct {
    guint16 task_id;            // Task ID
    gpointer data;              // Task input
    GThreadFunc thread_func;    // Task function
    schedule_t *sched;          // Reference to the schedule for store the result
    glong start_time_request;   // For measure the performance (dispatch time, before thread creation)
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