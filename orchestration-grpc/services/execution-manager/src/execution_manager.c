#include "execution_manager.h"
#include "schedule.h"
#include <glib.h>
#include "logger.h"

/* Struttura per passare più dati alle callback (privata a questo file) */
typedef struct {
    gpointer data;       /* La GSList dei task (o la start_entry_t) */
    GMainLoop *loop;     /* Il riferimento al loop per chiamare quit */
    gboolean is_last;    /* Flag per sapere se è l'ultimo evento */
} context_t;

/* Dichiarazioni forward per le callback statiche */
static gboolean handle_expiration(gpointer user_data);
static gboolean handle_start_task(gpointer user_data);


/* Callback per la terminazione dei task */
static gboolean handle_expiration(gpointer user_data) {
    context_t *ctx = (context_t *)user_data;
    GSList *tasks = (GSList *)ctx->data;

    log_message(LOG_DEBUG, "execution-manager", "Deadline reached: terminating %u task(s)", g_slist_length(tasks));

    // TODO: Esegui qui la logica di chiusura specifica per i task...

    if (ctx->is_last) {
        log_message(LOG_INFO, "execution-manager", "Last deadline reached, shutting down MainLoop...");
        g_main_loop_quit(ctx->loop);
    }

    g_free(ctx); // Liberiamo il contesto temporaneo
    return G_SOURCE_REMOVE;
}

/* Callback per l'avvio dei task */
static gboolean handle_start_task(gpointer user_data) {
    context_t *ctx = (context_t *)user_data;
    start_entry_t *entry = (start_entry_t *)ctx->data;

    log_message(LOG_DEBUG, "execution-manager", "Starting task scheduled at relative time %ld ms", entry->start_time);
    
    // TODO: Esegui qui la logica di avvio...

    g_free(ctx); // Liberiamo il contesto temporaneo
    return G_SOURCE_REMOVE;
}

void em_run_schedule(schedule_t *sched) {
    g_return_if_fail(sched != NULL);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    gint64 time_zero_us = g_get_monotonic_time();
    gint64 max_offset_ms = -1;

    /* 1. Trova il timestamp massimo per sapere quando chiudere il loop */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, sched->schedule_end_info);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        gint64 offset = *(gint64 *)key;
        if (offset > max_offset_ms) max_offset_ms = offset;
    }

    /* 2. Pianifica le TERMINAZIONI (Deadlines) */
    g_hash_table_iter_init(&iter, sched->schedule_end_info);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        gint64 offset_ms = *(gint64 *)key;
        GSList *tasks_to_expire = (GSList *)value;

        context_t *ctx = g_new0(context_t, 1);
        ctx->data = tasks_to_expire;
        ctx->loop = loop;
        ctx->is_last = (offset_ms == max_offset_ms);

        gint64 target_mono_us = time_zero_us + (offset_ms * 1000);

        GSource *source = g_timeout_source_new(0);
        g_source_set_ready_time(source, target_mono_us);
        g_source_set_callback(source, handle_expiration, ctx, NULL);
        g_source_attach(source, g_main_loop_get_context(loop));
        g_source_unref(source);
    }

    /* 3. Pianifica gli AVVII (Starts) */
    for (GList *l = g_queue_peek_head_link(sched->schedule_start_info); l != NULL; l = l->next) {
        start_entry_t *entry = (start_entry_t *)l->data;

        context_t *ctx = g_new0(context_t, 1);
        ctx->data = entry;
        ctx->loop = loop;
        ctx->is_last = FALSE;

        gint64 target_mono_us = time_zero_us + (entry->start_time * 1000);

        GSource *source = g_timeout_source_new(0);
        g_source_set_ready_time(source, target_mono_us);
        g_source_set_callback(source, handle_start_task, ctx, NULL);
        g_source_attach(source, g_main_loop_get_context(loop));
        g_source_unref(source);
    }

    log_message(LOG_INFO, "execution-manager", "Scheduler started, waiting for events...");
    g_main_loop_run(loop);

    g_main_loop_unref(loop);
    log_message(LOG_INFO, "execution-manager", "Scheduler terminated successfully");
}
