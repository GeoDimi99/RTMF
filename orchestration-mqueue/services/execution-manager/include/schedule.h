#ifndef SCHEDULE_H
#define SCHEDULE_H

// Must come before the first system header is pulled in (glibc latches its
// feature-test macros on the first inclusion of <features.h>), so glibc's
// <sched.h> exposes SCHED_BATCH/SCHED_IDLE/SCHED_DEADLINE further down.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <mqueue.h>
#include "task_ipc.h"
#include <sched.h>

/* --- Utils Structures --- */

typedef struct {
    guint16 task_id;        // Call Task ID
    GString *task_name;     // Image Task Name
    gint policy;            // Task Policy
    gint8 priority;         // Task Priority
    gint cpu_affinity;      // CPU Affinity
    guint8 repetition;      // Task Repetition
    GSList *depends_on;     // List of Call ID
    GString *input_data;    // List of array input (JSON)
    mqd_t task_queue;       // Task Queue
} activation_data_t;

typedef struct {
    guint16 task_id;        // Call Task ID
    GString *task_name;     // Image Task Name
    mqd_t task_queue;       // Task Queue 
} expiration_data_t;

typedef struct {
    gint64 timestamp;       
    GSList *data_list;      // List of activation_data_t* or expiration_data_t* 
} timeline_entry_t;

typedef struct {
    GSList *output_list;   // List of GString* 
    guint8 remaining_runs;
} task_result_t;

/* --- Schedule Main Structure --- */

typedef struct {
    GString *schedule_name;         // Schedule Name (Debug)
    GString *schedule_version;      // Schedule Version
    GQueue *schedule_start_info;    // Priority Queue for the start information
    GQueue *schedule_end_info;      // Priority Queue for the end information
    GHashTable *schedule_results;   // Map: Task ID (guint16) -> task_result_t*
    gint64 schedule_duration;       // Schedule duration (used for know where stop the loop)
} schedule_t;

/* Schedule Constructor/Destructor */
schedule_t* schedule_new(const gchar *name, const gchar *version);
void schedule_free(schedule_t *sched);

/* Schedule Getters/Setters */
GSList *schedule_get_results(schedule_t *sched, guint16 id);
void schedule_set_result(schedule_t *sched, guint16 id, const gchar *output);


/* Schedule Methods */
void schedule_add_task(schedule_t *sched, guint16 id, const gchar *name, gint policy, gint8 priority, gint cpu_affinity, guint8 repetition,GSList *depends_on, gint64 start_time, gint64 end_time, const gchar *input);
void schedule_reset(schedule_t *sched);               

/* Other Methods */
gboolean schedule_is_task_completed(schedule_t *sched, guint16 id);
void schedule_print(schedule_t *sched);

/* Usefull functions */
int compare_versions(const gchar *v1, const gchar *v2);


#endif // SCHEDULE_H