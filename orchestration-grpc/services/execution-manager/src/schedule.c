#include "schedule.h"
#include <stdio.h>
#include "logger.h"

/* ---- Helper & Comparison Functions ---- */

static gboolean is_version_valid(const gchar *version) {
    if (version == NULL) return FALSE;
    static GRegex *regex = NULL;
    if (G_UNLIKELY (regex == NULL)) {
        regex = g_regex_new("^([0-9]+)\\.([0-9]+)\\.([0-9]+)$", G_REGEX_OPTIMIZE, 0, NULL);
    }
    return g_regex_match(regex, version, 0, NULL);
}

/* Comparator for GQueue (Priority Queue logic) */
static gint compare_timeline_entries(gconstpointer a, gconstpointer b, gpointer user_data) {
    const timeline_entry_t *entry_a = (const timeline_entry_t *)a;
    const timeline_entry_t *entry_b = (const timeline_entry_t *)b;
    
    if (entry_a->timestamp < entry_b->timestamp) return -1;
    if (entry_a->timestamp > entry_b->timestamp) return 1;
    return 0;
}

/* ---- Destructors ---- */

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
        g_free(exp);
    }
}

static void timeline_entry_free(gpointer data, GDestroyNotify data_free_func) {
    timeline_entry_t *entry = (timeline_entry_t *)data;
    if (entry) {
        g_slist_free_full(entry->data_list, data_free_func);
        g_free(entry);
    }
}


static void start_entry_free_wrapper(gpointer data) {
    timeline_entry_free(data, (GDestroyNotify)activation_data_free);
}

static void end_entry_free_wrapper(gpointer data) {
    timeline_entry_free(data, (GDestroyNotify)expiration_data_free);
}

void task_result_free(gpointer data) {
    task_result_t *res = (task_result_t *)data;
    if (res && res->output_data) {
        g_string_free(res->output_data, TRUE);
    }
}

/* ---- Schedule Lifecycle ---- */

schedule_t* schedule_new(const gchar *name, const gchar *version) {
    g_return_val_if_fail(name != NULL, NULL);
    g_return_val_if_fail(version == NULL || is_version_valid(version), NULL);

    schedule_t *sched = g_new0(schedule_t, 1);
    sched->schedule_name = g_string_new(name);
    
    const gchar *v = (version == NULL) ? "0.0.0" : version;
    sched->schedule_version = g_string_new(v);

    sched->schedule_start_info = g_queue_new();
    sched->schedule_end_info = g_queue_new();
    
    sched->schedule_results = g_array_new(FALSE, TRUE, sizeof(task_result_t));
    g_array_set_clear_func(sched->schedule_results, (GDestroyNotify)task_result_free);

    sched->schedule_duration = 0;
    return sched;
}

void schedule_free(schedule_t *sched) {
    if (!sched) return;

    g_string_free(sched->schedule_name, TRUE);
    g_string_free(sched->schedule_version, TRUE);

    g_queue_free_full(sched->schedule_start_info, start_entry_free_wrapper);
    g_queue_free_full(sched->schedule_end_info, end_entry_free_wrapper);
    g_array_unref(sched->schedule_results);

    g_free(sched);
}

/* ---- Core Operations ---- */

void schedule_add_task(schedule_t *sched, 
                guint16 id, const gchar *name, sched_policy_t policy, 
                gint8 priority, guint8 repetition, GSList *depends_on, 
                gint64 start_time, gint64 end_time, const gchar *input) {

    g_return_if_fail(sched != NULL && name != NULL);
    g_return_if_fail(start_time >= 0 && start_time < end_time);

    
    activation_data_t *act = g_new0(activation_data_t, 1);
    act->task_id = id;
    act->task_name = g_string_new(name);
    act->policy = policy;
    act->priority = priority;
    act->repetition = repetition;
    act->depends_on = depends_on;
    act->input_data = g_string_new(input ? input : "{}");

    timeline_entry_t *start_entry = NULL;
    for (GList *l = sched->schedule_start_info->head; l != NULL; l = l->next) {
        timeline_entry_t *e = (timeline_entry_t *)l->data;
        if (e->timestamp == start_time) { start_entry = e; break; }
    }

    if (start_entry) {
        start_entry->data_list = g_slist_append(start_entry->data_list, act);
    } else {
        timeline_entry_t *new_e = g_new0(timeline_entry_t, 1);
        new_e->timestamp = start_time;
        new_e->data_list = g_slist_append(NULL, act);
        g_queue_insert_sorted(sched->schedule_start_info, new_e, compare_timeline_entries, NULL);
    }

    
    expiration_data_t *exp = g_new0(expiration_data_t, 1);
    exp->task_id = id;
    exp->task_name = g_string_new(name);

    timeline_entry_t *end_entry = NULL;
    for (GList *l = sched->schedule_end_info->head; l != NULL; l = l->next) {
        timeline_entry_t *e = (timeline_entry_t *)l->data;
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

    
    task_result_t res;
    res.output_data = g_string_new("{}");
    res.remaining_runs = repetition;
    
    if (id >= sched->schedule_results->len) {
        g_array_set_size(sched->schedule_results, id + 1);
    }
    g_array_index(sched->schedule_results, task_result_t, id) = res;

    if (end_time > sched->schedule_duration) {
        sched->schedule_duration = end_time;
    }
}

/* ---- Utils & Printers ---- */

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

void print_schedule(schedule_t *sched) {
    if (!sched) return;

    log_message(LOG_DEBUG, "execution-manager", "Schedule: %s (v%s) [%ld ms]",
            sched->schedule_name->str, sched->schedule_version->str, (long)sched->schedule_duration);

    log_message(LOG_DEBUG, "execution-manager", "Start timeline (sorted):");
    for (GList *l = sched->schedule_start_info->head; l != NULL; l = l->next) {
        timeline_entry_t *e = (timeline_entry_t *)l->data;
        for (GSList *s = e->data_list; s != NULL; s = s->next) {
            activation_data_t *a = (activation_data_t *)s->data;
            log_message(LOG_DEBUG, "execution-manager", "  %ld ms: task %u: %s", (long)e->timestamp, a->task_id, a->task_name->str);
        }
    }

    log_message(LOG_DEBUG, "execution-manager", "End timeline (sorted):");
    for (GList *l = sched->schedule_end_info->head; l != NULL; l = l->next) {
        timeline_entry_t *e = (timeline_entry_t *)l->data;
        for (GSList *s = e->data_list; s != NULL; s = s->next) {
            expiration_data_t *ex = (expiration_data_t *)s->data;
            log_message(LOG_DEBUG, "execution-manager", "  %ld ms: task %u: %s", (long)e->timestamp, ex->task_id, ex->task_name->str);
        }
    }
}