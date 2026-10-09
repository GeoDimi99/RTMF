#ifndef SCHEDULE_H
#define SCHEDULE_H

#include <glib.h>
#include <sched.h>
#include <pthread.h>        // Mutex manager

/* --- Utils Structures --- */

typedef struct {
    guint16 task_id;            // Call Task ID
    GString *task_name;         // Task Name
    GThreadFunc task_exec;      // Pointer to Function that contain the task
    gint policy;                // Policy: SCHED_OTHER, SCHED_FIFO, SCHED_RR or SCHED_DEADLINE
    gint8 priority;             // Priority
    gint cpu_affinity;          // CPU Affinity
    guint8 repetition;          // Number that the task must repeate
    GSList *depends_on;         // List of Call ID 
    gpointer input_data;        // Pointer to the input of the task
} activation_data_t;

typedef struct {
    guint16 task_id;            // Call Task ID 
    GString *task_name;         
} expiration_data_t;

typedef struct {
    gint64 timestamp;
    GSList *data_list; /* List of activation_data_t* or expiration_data_t* */
} timeline_entry_t;

typedef struct {
    GSList *output_list;   /* List of GString* */
    guint8 remaining_runs;
} task_result_t;

/* --- Schedule Main Structure --- */

typedef struct {
    GString *schedule_name;
    GString *schedule_version;
    GQueue *schedule_start_info;
    GQueue *schedule_end_info;
    GHashTable *schedule_results;   // Map: Task ID (guint16) -> task_result_t* 
    pthread_mutex_t schedule_results_mutex;
    gint64 schedule_duration;
} schedule_t;


/* Schedule Constructor/Destructor */
schedule_t* schedule_new(const gchar *name, const gchar *version);
void schedule_free(schedule_t *sched);

/* Schedule Getters/Setters */
GSList *schedule_get_results(schedule_t *sched, guint16 id);
void schedule_set_result(schedule_t *sched, guint16 id, const gchar *output);

/* Schedule Methods */
void schedule_add_task(schedule_t *sched, guint16 id, const gchar *name, GThreadFunc task_exec, gint policy, gint8 priority, gint cpu_affinity, guint8 repetition, GSList *depends_on,  gint64 start_time, gint64 end_time, gpointer input);
void schedule_reset(schedule_t *sched);

/* Other Methods */
gboolean schedule_is_task_completed(schedule_t *sched, guint16 id);
void schedule_print(schedule_t *sched);


/* Usefull functions */
int compare_versions(const gchar *v1, const gchar *v2);


#endif // SCHEDULE_H