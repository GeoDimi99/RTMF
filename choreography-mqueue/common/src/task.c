#include "task.h"

/* Task Constructors */
task_t* task_new(guint16 id, const gchar *name, sched_policy_t policy, gint8 priority, guint8 repetition, gint64 start_time, gint64 end_time, const gchar *input){
    
    /* Validate input */
    g_return_val_if_fail(name != NULL, NULL);
    g_return_val_if_fail(policy >= 0 && policy <= 3 , NULL);
    g_return_val_if_fail(priority >= MIN_TASK_PRIORITY && priority <= MAX_TASK_PRIORITY, NULL);
    g_return_val_if_fail(repetition > 0, NULL);
    g_return_val_if_fail(start_time >= 0, NULL);
    g_return_val_if_fail(start_time < end_time , NULL);


    /* Struct memory allocation */
    task_t *task = g_new0(task_t, 1);

    /* Setup struct fields */
    task->task_id = id;
    task->task_name =  g_string_new(name);
    task->policy = policy;
    task->priority = priority;
    task->repetition = repetition;
    task->start_time = start_time;
    task->end_time = end_time;
    if (input)  task->input = g_string_new(input); else task->input = g_string_new("");
    task->depends_on = NULL;

    /* Return the created task */
    return task;
}


/* Task Distructor */

void task_free(task_t *task) {
    if (!task) return;

    /* Free main strings */
    g_string_free(task->task_name, TRUE);
    g_string_free(task->input, TRUE);

    /* Free the depends_on list  */
    g_slist_free(task->depends_on);

    // Libera la struct
    g_free(task);
}

/* Task Additional Operation */
void task_add_dependency(task_t *task, guint16 dep_id) {
    g_return_if_fail(task != NULL);

    task->depends_on = g_slist_append(task->depends_on, GUINT_TO_POINTER(dep_id));
}
