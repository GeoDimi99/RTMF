#include "schedule.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>

/* -----------------Helper Functions ----------------- */

static gboolean is_version_valid(const gchar *version) {
    if (version == NULL) return FALSE;
    static GRegex *regex = NULL;
    if (G_UNLIKELY(regex == NULL)) {
        regex = g_regex_new("^([0-9]+)\\.([0-9]+)\\.([0-9]+)$", G_REGEX_OPTIMIZE, 0, NULL);
    }
    return g_regex_match(regex, version, 0, NULL);
}

static gint compare_timeline_entries(gconstpointer a, gconstpointer b, gpointer user_data) {
    const timeline_entry_t *entry_a = (const timeline_entry_t *)a;
    const timeline_entry_t *entry_b = (const timeline_entry_t *)b;
    return (entry_a->timestamp < entry_b->timestamp) ? -1 : (entry_a->timestamp > entry_b->timestamp) ? 1 : 0;
}


static void g_string_free_wrapper(gpointer data) {
    if (data) g_string_free((GString *)data, TRUE);
}

static void activation_data_free(gpointer data) {
    activation_data_t *act = (activation_data_t *)data;
    if (act) {
        g_string_free(act->task_name, TRUE);
        g_string_free(act->input_data, TRUE);
        g_slist_free(act->depends_on);
        g_free(act);
    }
}

static void expiration_data_free(gpointer data) {
    expiration_data_t *exp = (expiration_data_t *)data;
    if (exp) {
        g_string_free(exp->task_name, TRUE);
        /* Note: we close only here the queue that is shared with activation data, for avoid double close */
        if (exp->task_queue != (mqd_t)-1) {
            mq_close(exp->task_queue);
        }
        g_free(exp);
    }
}

static void timeline_entry_free_generic(gpointer data, GDestroyNotify data_free_func) {
    timeline_entry_t *entry = (timeline_entry_t *)data;
    if (entry) {
        g_slist_free_full(entry->data_list, data_free_func);
        g_free(entry);
    }
}

static void start_entry_free_wrapper(gpointer data) {
    timeline_entry_free_generic(data, (GDestroyNotify)activation_data_free);
}

static void end_entry_free_wrapper(gpointer data) {
    timeline_entry_free_generic(data, (GDestroyNotify)expiration_data_free);
}

static void task_result_free(gpointer data) {
    task_result_t *res = (task_result_t *)data;
    if (res) {
        g_slist_free_full(res->output_list, g_string_free_wrapper);
        g_free(res);
    }
}

/* ----------------- Schedule Constructor/Destructor ----------------- */

schedule_t* schedule_new(const gchar *name, const gchar *version) {
    g_return_val_if_fail(name != NULL, NULL);
    g_return_val_if_fail(version == NULL || is_version_valid(version), NULL);

    /* Struct memory allocation */
    schedule_t *sched = g_new0(schedule_t, 1);
    sched->schedule_name = g_string_new(name);
    sched->schedule_version = g_string_new(version ? version : "0.0.0");

    /* Timeline Start/End Queue Initialization */
    sched->schedule_start_info = g_queue_new();
    sched->schedule_end_info = g_queue_new();
    
    /* HashTable Initialization */
    sched->schedule_results = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, task_result_free);

    sched->schedule_duration = 0;
    return sched;
}

void schedule_free(schedule_t *sched) {
    if (!sched) return;
    g_string_free(sched->schedule_name, TRUE);
    g_string_free(sched->schedule_version, TRUE);
    g_queue_free_full(sched->schedule_start_info, start_entry_free_wrapper);
    g_queue_free_full(sched->schedule_end_info, end_entry_free_wrapper);
    g_hash_table_destroy(sched->schedule_results);
    g_free(sched);
}


/* ----------------- Schedule Getters/Setters ----------------- */

GSList *schedule_get_results(schedule_t *sched, guint16 id)
{
    if (!sched) return NULL;

    task_result_t *res = g_hash_table_lookup(
        sched->schedule_results,
        GINT_TO_POINTER(id)
    );

    GSList *results = res ? res->output_list : NULL;

    return results;
}



void schedule_set_result(schedule_t *sched, guint16 id, const gchar *output) {
    g_return_if_fail(sched != NULL);
    g_return_if_fail(output != NULL);

    /* Defensive check to prevent crashes on use-after-free scenarios. */
    if (G_UNLIKELY(sched->schedule_results == NULL)) {
        log_message(LOG_ERROR, "execution-manager", "in schedule_set_result: the schedule's result table is NULL. This indicates a severe memory issue (e.g., use-after-free) for task ID %u.", id);
        return;
    }

    /* Find the result associated to the ID in the HashTable */
    task_result_t *res = g_hash_table_lookup(sched->schedule_results, GINT_TO_POINTER((gint)id));

    if (res == NULL) {
        log_message(LOG_WARN, "execution-manager", "in schedule_set_result: Task ID %u not found.", id);
        return;
    }

    
    /* 1. Add the new output in the list */
    GString *new_output = g_string_new(output);
    res->output_list = g_slist_append(res->output_list, new_output);

    /* 2. Decrement the remainning runs (if greather than 0) */

    if (res->remaining_runs > 0) {
        res->remaining_runs--;
    }

    log_message(LOG_INFO, "execution-manager", "Task %u updated: %u runs left.", id, res->remaining_runs);
}


/* ----------------- Schedule Methods ----------------- */

void schedule_add_task(schedule_t *sched, 
                guint16 id, const gchar *name, gint policy, 
                gint8 priority, gint cpu_affinity, guint8 repetition, GSList *depends_on, 
                gint64 start_time, gint64 end_time, const gchar *input) {

    g_return_if_fail(sched != NULL && name != NULL);
    g_return_if_fail(start_time >= 0 && start_time < end_time);

    /* 1. Open Queue POSIX (Polling) */
    gchar *q_name = g_strdup_printf("/%s_q", name);
    mqd_t qd = (mqd_t)-1;

    /* Polling Loop: Wait until the Task Wrapper creates its queue.
     * this avoid a race condition where the EM run befor that TW is ready. */
    while (TRUE) {
        qd = mq_open(q_name, O_WRONLY | O_NONBLOCK);
        if (qd != (mqd_t)-1) {
            break; // Successo
        }
        if (errno == ENOENT) {
            log_message(LOG_INFO, "execution-manager", "Waiting for task queue '%s'...", q_name);
            g_usleep(500000); // Wait and retry
        } else {
            log_message(LOG_ERROR, "execution-manager", "mq_open failed for %s: %s", q_name, g_strerror(errno));
            g_free(q_name);
            return; // Fatal error
        }
    }

    if (qd == (mqd_t)-1) {
        log_message(LOG_ERROR, "execution-manager", "mq_open failed for %s: %s", q_name, g_strerror(errno));
        g_free(q_name);
        return;
    }
    g_free(q_name);

    /* 2. Create Activation Data */
    activation_data_t *act = g_new0(activation_data_t, 1);
    act->task_id = id;
    act->task_name = g_string_new(name);
    act->policy = policy;
    act->priority = priority;
    act->cpu_affinity = cpu_affinity;
    act->repetition = repetition;
    act->depends_on = g_slist_copy(depends_on);
    act->input_data = g_string_new(input ? input : "[{}]");
    act->task_queue = qd;

    /* 3. Insert in timeline queue */
    timeline_entry_t *st_entry = NULL;
    for (GList *l = sched->schedule_start_info->head; l; l = l->next) {
        timeline_entry_t *e = l->data;
        if (e->timestamp == start_time) { st_entry = e; break; }
    }
    if (st_entry) {
        st_entry->data_list = g_slist_append(st_entry->data_list, act);
    } else {
        timeline_entry_t *new_e = g_new0(timeline_entry_t, 1);
        new_e->timestamp = start_time;
        new_e->data_list = g_slist_append(NULL, act);
        g_queue_insert_sorted(sched->schedule_start_info, new_e, compare_timeline_entries, NULL);
    }

    /* 4. Create Expiration Data */
    expiration_data_t *exp = g_new0(expiration_data_t, 1);
    exp->task_id = id;
    exp->task_name = g_string_new(name);
    exp->task_queue = qd;

    /* 5. Insert in timeline queue */
    timeline_entry_t *end_entry = NULL;
    for (GList *l = sched->schedule_end_info->head; l; l = l->next) {
        timeline_entry_t *e = l->data;
        if (e->timestamp == end_time) { end_entry = e; break; }
    }
    if (end_entry) {
        end_entry->data_list = g_slist_append(end_entry->data_list, exp);
    } else {
        timeline_entry_t *new_e = g_new0(timeline_entry_t, 1);
        new_e->timestamp = end_time;
        new_e->data_list = g_slist_append(NULL, exp);
        g_queue_insert_sorted(sched->schedule_end_info, new_e, compare_timeline_entries, NULL);
    }

    /* 6. Init results in HashTable */
    task_result_t *res = g_new0(task_result_t, 1);
    res->remaining_runs = repetition;
    res->output_list = NULL;
    g_hash_table_insert(sched->schedule_results, GINT_TO_POINTER((gint)id), res);

    if (end_time > sched->schedule_duration)
        sched->schedule_duration = end_time;
}

void schedule_reset(schedule_t *sched) {
    g_return_if_fail(sched != NULL);

    GHashTableIter iter;
    gpointer key, value;


    /* Iterate on all the results that are stored in the HashTable */
    g_hash_table_iter_init(&iter, sched->schedule_results);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        guint16 task_id = (guint16)GPOINTER_TO_INT(key);
        task_result_t *res = (task_result_t *)value;

        /* 1. Clean the list of the outputs */
        if (res->output_list) {
            g_slist_free_full(res->output_list, g_string_free_wrapper);
            res->output_list = NULL;
        }
        
        res->output_list = NULL;

        /* 2. Restore the remaining_runs */
        gboolean found = FALSE;
        for (GList *l = sched->schedule_start_info->head; l && !found; l = l->next) {
            timeline_entry_t *entry = l->data;
            for (GSList *s = entry->data_list; s; s = s->next) {
                activation_data_t *act = s->data;
                if (act->task_id == task_id) {
                    res->remaining_runs = act->repetition;
                    found = TRUE;
                    break;
                }
            }
        }
    }
}


/* ----------------- Other Methods -----------------*/

gboolean schedule_is_task_completed(schedule_t *sched, guint16 id)
{
    if (!sched) return FALSE;

    task_result_t *res = g_hash_table_lookup(
        sched->schedule_results,
        GINT_TO_POINTER(id)
    );

    if (!res) return FALSE;

    return (res->remaining_runs == 0);
}


void schedule_print(schedule_t *sched) {
    if (!sched) return;

    log_message(LOG_DEBUG, "execution-manager", "=== SCHEDULE: %s (v%s) [%ld ms] ===",
            sched->schedule_name->str, sched->schedule_version->str, (long)sched->schedule_duration);

    log_message(LOG_DEBUG, "execution-manager", "--- TIMELINE (START) ---");
    for (GList *l = sched->schedule_start_info->head; l; l = l->next) {
        timeline_entry_t *e = l->data;
        GString *line = g_string_new(NULL);
        g_string_append_printf(line, "%4ld ms:", (long)e->timestamp);
        for (GSList *s = e->data_list; s; s = s->next) {
            activation_data_t *a = s->data;
            g_string_append_printf(line, " Activate Task %u (%s)", a->task_id, a->task_name->str);
        }
        log_message(LOG_DEBUG, "execution-manager", "%s", line->str);
        g_string_free(line, TRUE);
    }

    log_message(LOG_DEBUG, "execution-manager", "--- TASK RESULTS (HashTable) ---");
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, sched->schedule_results);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        guint16 id = (guint16)GPOINTER_TO_INT(key);
        task_result_t *res = value;
        log_message(LOG_DEBUG, "execution-manager", "Task ID %u: Runs Left: %u, Last Output: %s",
                id, res->remaining_runs, (res->output_list) ? ((GString*)res->output_list->data)->str : "N/A");
    }
    log_message(LOG_DEBUG, "execution-manager", "==========================================");
}




/* ----------------- Usefull functions ----------------- */

int compare_versions(const gchar *v1, const gchar *v2) {
    if (!is_version_valid(v1) || !is_version_valid(v2)) return -2;
    gchar **p1 = g_strsplit(v1, ".", 3);
    gchar **p2 = g_strsplit(v2, ".", 3);
    int res = 0;
    for (int i = 0; i < 3; i++) {
        int n1 = atoi(p1[i]), n2 = atoi(p2[i]);
        if (n1 > n2) { res = 1; break; }
        if (n1 < n2) { res = -1; break; }
    }
    g_strfreev(p1); g_strfreev(p2);
    return res;
}
