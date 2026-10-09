#ifndef TASK_WRAPPER_H
#define TASK_WRAPPER_H

#include <glib.h>
#include <json-glib/json-glib.h>

#include <errno.h>
#include <fcntl.h>

#include <unistd.h>
#include <sched.h>
#include <pthread.h> 
#include <mqueue.h>
#include <sys/resource.h>  // setpriority() for the nice value

#include <time.h>  // For measure the performance 


#include "task_ipc.h"
#include "app_task.h"




#define DEFAULT_TASK_NAME "task_wrapper"

/* Main Structures for Task Wrapper */
typedef struct {
    GString *task_name;                 // Task Image Name (Debug)
    GString *task_queue_name;           // The name of the task queue for unlink
    mqd_t task_queue;                   // Task Queue
    mqd_t em_queue;                     // Event Manager Queue   
    GHashTable* task_session_table;     // Map: Call Task ID (guint16) -> GSList pointer to pthread_t* 

} task_wrapper_t;

/* Usefull structures */
typedef struct {
    guint16 session_id;                 // Call Task ID
    task_wrapper_t *tw;                 // Task Wrapper Struct

    glong start_time_request;           // For measure the performance

    gint policy;                        // Scheduler policy of the task thread
    gint8 priority;                     // RT priority (FIFO/RR) or nice value (OTHER/BATCH)
    
    input_t *input;                     // Input Data
    output_t *output;                   // Output Data
} task_input_t;

typedef struct {
    gpointer tw;
    guint16 session_id;
    pthread_t thread_id;
} thread_cleanup_data_t;



/* Constructor/Distructor */
task_wrapper_t* task_wrapper_new(const gchar *task_name, const gchar *task_queue_name, const gchar *em_name);
void task_wrapper_free(task_wrapper_t *tw);

/* Manager Threads/Sessions */
void task_wrapper_add_thread(task_wrapper_t *tw, guint16 session_id, pthread_t thread_id);
void task_wrapper_remove_thread(task_wrapper_t *tw, guint16 session_id, pthread_t thread_id);
GSList* task_wrapper_get_threads(task_wrapper_t *tw, guint16 session_id);

void * task_wrapper_run_thread(void *arg);

/* Parsing Input Operations */
GSList* parse_input_list(const gchar *input_data);
void free_input_list(GSList *list);


/* Message Event Handlers */
gboolean handle_input_message(GIOChannel *source, GIOCondition condition, gpointer user_data);
gboolean handle_end_thread(gpointer data);


#endif /* TASK_WRAPPER_H */