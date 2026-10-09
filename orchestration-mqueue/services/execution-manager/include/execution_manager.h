#ifndef EXECUTION_MANAGER_H
#define EXECUTION_MANAGER_H

// Must come before the first system header is pulled in (glibc latches its
// feature-test macros on the first inclusion of <features.h>), so glibc's
// <sched.h> (via schedule.h below) exposes SCHED_BATCH/SCHED_IDLE/SCHED_DEADLINE.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <stdio.h>
#include <errno.h>
#include <mqueue.h>
#include <hiredis/hiredis.h> 

#include <time.h> // For measure the performance 

#include "schedule.h"
#include "execution_manager.h"
#include "task_ipc.h"




#define DEFAULT_EXECUTION_MANAGER_NAME "execution_manager"



extern gint iteration; // For test propose 


/* Execution Manager Stucture */
typedef struct execution_manager_t{
    GString *em_name;               // Execution Manager Name (Debug)
    mqd_t em_queue;                 // Execution Manager Queue (Receive)
    redisContext* redis_client;     // Redis Client (for Schedule)

} execution_manager_t;



typedef struct {
    gpointer data;      // GSList di activation_data_t
    gint64 timestamp;   // For logging (Debug)
} start_context_t;

typedef struct {
    gpointer data;       // GSList of expiration_data_t
    GMainLoop *loop;     // Reference for end the loop
    gboolean is_last;    // Flag that indicate the last event of the loop
    gint64 timestamp;    // For logging (Debug)
    schedule_t *sched;   // Reference to the schedule
} deadline_context_t;

typedef struct {
    execution_manager_t *em;    //
    schedule_t *sched;          //
} result_context_t;

typedef struct {
    schedule_t *sched;
    GMainLoop *loop;
} completion_context_t;




/* Executor Manager Constructor/Distructors */
execution_manager_t* em_new(const gchar *name);
void em_free(execution_manager_t *em);


/* Exection Manager Activities*/
void em_run_schedule(execution_manager_t *em, schedule_t *sched);
void em_wait_for_schedule(execution_manager_t *em);
schedule_t * em_read_schedule(execution_manager_t *em);



/* Execution Manager Event Handlers */
gboolean handle_initialization(gpointer user_data);
gboolean handle_expiration(gpointer user_data);
gboolean handle_result_message(GIOChannel *source, GIOCondition condition, gpointer user_data);
gboolean check_completion(gpointer user_data);


#endif // EXECUTION_MANAGER_H