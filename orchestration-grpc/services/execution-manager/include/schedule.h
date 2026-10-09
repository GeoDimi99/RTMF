#ifndef SCHEDULE_H
#define SCHEDULE_H

#include <glib.h>
#include <stdlib.h>
#include "task.h"

/* --- Utils Structures --- */

typedef struct {
    guint16 task_id;
    GString *task_name;
    sched_policy_t policy;
    gint8 priority;
    guint8 repetition;
    GSList *depends_on; 
    GString *input_data;        
} activation_data_t;

typedef struct {
    guint16 task_id;
    GString *task_name;
} expiration_data_t;

/* Generic structure for a point on the timeline (Start or End) */
typedef struct {
    gint64 timestamp;
    GSList *data_list;
} timeline_entry_t;

typedef struct {
    GString *output_data;
    guint8 remaining_runs;    
} task_result_t;

/* --- Schedule Main Structure --- */

typedef struct {
    GString *schedule_name;
    GString *schedule_version;
    GQueue *schedule_start_info;
    GQueue *schedule_end_info;
    GArray *schedule_results;
    gint64 schedule_duration;
} schedule_t;

/* --- Function Prototypes --- */

schedule_t* schedule_new(const gchar *name, const gchar *version);
void schedule_free(schedule_t *sched);

void schedule_add_task(schedule_t *sched, 
                guint16 id, 
                const gchar *name, 
                sched_policy_t policy, 
                gint8 priority, 
                guint8 repetition,
                GSList *depends_on,  
                gint64 start_time, 
                gint64 end_time, 
                const gchar *input);

int compare_versions(const gchar *v1, const gchar *v2);
void print_schedule(schedule_t *sched);

#endif // SCHEDULE_H