#ifndef TASK_H
#define TASK_H

#include <glib.h>
#include <sched.h>


#define MIN_TASK_PRIORITY -20
#define MAX_TASK_PRIORITY 99

/* Scheduling Policies */
typedef enum {
    SCHED_POLICY_OTHER      = 0,
    SCHED_POLICY_FIFO       = 1,
    SCHED_POLICY_RR         = 2,
    SCHED_POLICY_DEADLINE   = 3
} sched_policy_t;

/* Task struct */
typedef struct {
    guint16 task_id;            // TaskID: it's the id of the task call, the possible values 0 - 65.536 
    GString *task_name;         // Image name: image name of the task wrapper
    sched_policy_t policy;      // Policy: OTHER, FIFO, RR, DEADLINE;
    gint8 priority;             // Priority: OTHER {-20..19}, RT {1..99} 
    guint8 repetition;          // Repetition: how many times to call the task
    GSList *depends_on;         // Depends on: is a list with TaskIDs 
    gint64 start_time;          // Start time
    gint64 end_time;            // Deadline
    GString *input;             // Input for the task call
} task_t;

/* Task Constructors */
task_t* task_new(guint16 id, 
                const gchar *name, 
                sched_policy_t policy, 
                gint8 priority, 
                guint8 repetition, 
                gint64 start_time, 
                gint64 end_time, 
                const gchar *input);


/* Task Distructor */
void task_free(task_t *task);

/* Task Additional Operation */
void task_add_dependency(task_t *task, 
                        guint16 dep_id);


#endif /* TASK_H */
