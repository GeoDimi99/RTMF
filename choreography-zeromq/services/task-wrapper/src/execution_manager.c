#include "execution_manager.h"
#include "logger.h"
#include <stdlib.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <glib-unix.h>

gint iteration = 0; // Counter for measure the performance

/* ----------------- Execution Manager Helper Functions ----------------- */
const char* redis_get_hash_value(redisReply *r, const char *key) {
    for (size_t i = 0; i < r->elements; i += 2) {
        if (strcmp(r->element[i]->str, key) == 0) {
            return r->element[i+1]->str;
        }
    }
    return NULL;
}

GList* json_array_to_gstring_list(JsonArray *array) {
    if (!array) return NULL;

    GList *list = NULL;
    guint length = json_array_get_length(array);

    for (guint i = 0; i < length; i++) {
        const char *value = json_array_get_string_element(array, i);
        
        // Create a new GString object for each element
        GString *gs = g_string_new(value);
        
        // Append the GString pointer to the GList
        list = g_list_append(list, gs);
    }

    return list;
}

GSList* parse_input_list(const gchar *input_data) {
    g_return_val_if_fail(input_data != NULL, NULL);

    JsonParser *parser = json_parser_new();
    GError *error = NULL;

    if (!json_parser_load_from_data(parser, input_data, -1, &error)) {
        log_message(LOG_ERROR, "task-wrapper", "JSON parser: JSON load failed: %s", error->message);
        g_error_free(error);
        g_object_unref(parser);
        return NULL;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_ARRAY(root)) {
        log_message(LOG_ERROR, "task-wrapper", "JSON parser: JSON root is not an array");
        g_object_unref(parser);
        return NULL;
    }

    GSList *list = NULL;
    JsonArray *array = json_node_get_array(root);
    guint length = json_array_get_length(array);

    for (guint i = 0; i < length; i++) {
        JsonObject *obj = json_array_get_object_element(array, i);
        input_t *input_elem = g_new0(input_t, 1);
        
        if (convert_json_to_input(obj, input_elem) == -1) {
            g_free(input_elem);
            g_slist_free_full(list, g_free); 
            g_object_unref(parser);
            return NULL;
        }
        
        /* Prepending is O(1). We will reverse it at the end for O(N) total. */
        list = g_slist_prepend(list, input_elem);
    }

    g_object_unref(parser);
    return g_slist_reverse(list);
}

void free_input_list(GSList *list) {
    /* Assuming input_t is a flat structure, otherwise use a custom free wrapper */
    g_slist_free_full(list, g_free);
}


/* ----------------- Executor Manager Constructor/Distructors ----------------- */
/* ----------------- Result hand-off: task thread -> scheduler main loop ----------------- */
/* After the task, its thread must not block: a SCHED_FIFO thread that sleeps (e.g. on the
 * stdout lock or on the results mutex, contended by the sibling threads of the process) is
 * requeued at the TAIL of its priority list, behind a same-priority task already waiting on
 * its core, and resumes only after that whole task. So the task thread only pushes its
 * record on a lock-free list and wakes the main loop (eventfd); the main loop stores the
 * result, measures end_time_result and logs the performance, as the Execution Manager does
 * in the orchestration versions. */
static task_wrapper_input_t *results_head = NULL;  // Completed tasks (lock-free LIFO)
static gint results_efd = -1;                       // Wakes the main loop
static guint results_watch = 0;                     // Main loop source of results_efd
static gint results_boost_prio = 0;                 // SCHED_FIFO priority of the scheduler (hand-off boost)

/* Called by the task thread: lock-free push, then wake the main loop. tw_input is owned
 * (and freed) by the main loop from here on. */
static void results_post(task_wrapper_input_t *tw_input) {
    task_wrapper_input_t *head = __atomic_load_n(&results_head, __ATOMIC_RELAXED);
    do {
        tw_input->next = head;
    } while (!__atomic_compare_exchange_n(&results_head, &head, tw_input, TRUE, __ATOMIC_RELEASE, __ATOMIC_RELAXED));

    guint64 one = 1;
    if (write(results_efd, &one, sizeof(one)) != sizeof(one)) {
        /* Only on counter overflow: the record is already in the list, drained on the next wake-up */
    }
}

/* Main loop: store the results of the completed tasks and log their performance */
static gboolean handle_task_results(gint fd, GIOCondition condition, gpointer user_data) {
    (void)condition; (void)user_data;
    guint64 count;
    if (read(fd, &count, sizeof(count)) != sizeof(count)) {
        /* EAGAIN: already drained by a previous wake-up */
    }

    /* Take the whole list and restore the completion order */
    task_wrapper_input_t *list = __atomic_exchange_n(&results_head, NULL, __ATOMIC_ACQUIRE);
    task_wrapper_input_t *ordered = NULL;
    while (list) {
        task_wrapper_input_t *next = list->next;
        list->next = ordered;
        ordered = list;
        list = next;
    }

    while (ordered) {
        task_wrapper_input_t *r = ordered;
        ordered = r->next;

        /* Write the result */
        schedule_set_result(r->sched, r->task_id, "{}");

        /* Measure the end_time_result (for the performance): after the result is stored in the schedule */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        glong end_time_result = ts.tv_sec * 1000000000L + ts.tv_nsec;
        /* -------------------------------------------------- */

        /* Log the performance metrics to be analysed */
        log_perf("task-wrapper", r->iteration, r->task_id, r->start_time_request, r->end_time_request, r->start_time_result, end_time_result, r->core_id);

        g_free(r);
    }
    return G_SOURCE_CONTINUE;
}

execution_manager_t* em_new(const gchar *name){
    g_return_val_if_fail(name != NULL, NULL);

    execution_manager_t *em = g_new0(execution_manager_t, 1);
    em->em_name = g_string_new(name);

    /* Create ZeroMQ context (sockets are created later in em_setup_zmq) */
    em->zmq_context = zmq_ctx_new();
    if (!em->zmq_context) {
        log_message(LOG_ERROR, "task-wrapper", "zmq_ctx_new failed");
        exit(EXIT_FAILURE);
    }
    em->zmq_pull = NULL;
    em->zmq_pub = NULL;
    em->zmq_sub_count = 0;
    em->zmq_push = NULL;
    em->zmq_sub = NULL;

    /* Connect to Redis */
    redisContext *c = redisConnect("redis", 6379);
    if (c != NULL && c->err ){
        log_message(LOG_ERROR, "task-wrapper", "Redis connection failed: %s", c->errstr);
        exit(EXIT_FAILURE);
    }
    em->redis_client = c;

    /* Result hand-off from the task threads to the main loop */
    /* The task threads do the hand-off at the scheduler priority (see task_wrapper_func) */
    struct sched_param em_param;
    if (sched_getscheduler(0) == SCHED_FIFO && sched_getparam(0, &em_param) == 0) {
        results_boost_prio = em_param.sched_priority;
    }

    results_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (results_efd < 0) {
        log_message(LOG_ERROR, "task-wrapper", "eventfd failed: %s", g_strerror(errno));
        exit(EXIT_FAILURE);
    }
    results_watch = g_unix_fd_add(results_efd, G_IO_IN, handle_task_results, NULL);

    return em;
}


void em_free(execution_manager_t *em){
    if (!em) return;

    g_string_free(em->em_name, TRUE);

    /* Close ZeroMQ sockets and context */
    if (em->zmq_pull) zmq_close(em->zmq_pull);
    if (em->zmq_pub) zmq_close(em->zmq_pub);
    if (em->zmq_push) zmq_close(em->zmq_push);
    if (em->zmq_sub) zmq_close(em->zmq_sub);
    if (em->zmq_context) zmq_ctx_destroy(em->zmq_context);

    redisFree(em->redis_client);

    if (results_watch) g_source_remove(results_watch);
    if (results_efd >= 0) close(results_efd);
    results_watch = 0;
    results_efd = -1;

    g_free(em);
}
/* ----------------- Executor Manager Getters/Setters ----------------- */
void em_set_leader(execution_manager_t *em, gboolean leader_flag){
    g_return_if_fail(em != NULL);

    em->is_leader = leader_flag;
    return; 
}


/* ----------------- ZeroMQ Setup ----------------- */
void em_setup_zmq(execution_manager_t *em, const gchar *leader_name) {
    g_return_if_fail(em != NULL);
    g_return_if_fail(leader_name != NULL);

    int linger = 0;

    if (em->is_leader) {
        /* Leader: bind PULL (to receive READY) and XPUB (to broadcast SYNC) via IPC */
        /* Remove stale socket files from previous runs */
        unlink("/ipc/sync_ready.sock");
        unlink("/ipc/sync_pub.sock");

        em->zmq_pull = zmq_socket(em->zmq_context, ZMQ_PULL);
        if (!em->zmq_pull) {
            log_message(LOG_ERROR, "task-wrapper", "zmq_socket(PULL) failed: %s", zmq_strerror(errno));
            exit(EXIT_FAILURE);
        }
        zmq_setsockopt(em->zmq_pull, ZMQ_LINGER, &linger, sizeof(linger));

        if (zmq_bind(em->zmq_pull, ZMQ_SYNC_PULL_ENDPOINT) != 0) {
            log_message(LOG_ERROR, "task-wrapper", "zmq_bind(PULL) failed: %s", zmq_strerror(errno));
            exit(EXIT_FAILURE);
        }
        log_message(LOG_INFO, "task-wrapper", "Leader: PULL bound on %s", ZMQ_SYNC_PULL_ENDPOINT);

        em->zmq_pub = zmq_socket(em->zmq_context, ZMQ_XPUB);
        if (!em->zmq_pub) {
            log_message(LOG_ERROR, "task-wrapper", "zmq_socket(XPUB) failed: %s", zmq_strerror(errno));
            exit(EXIT_FAILURE);
        }
        int verbose = 1;
        zmq_setsockopt(em->zmq_pub, ZMQ_XPUB_VERBOSE, &verbose, sizeof(verbose));
        zmq_setsockopt(em->zmq_pub, ZMQ_LINGER, &linger, sizeof(linger));

        if (zmq_bind(em->zmq_pub, ZMQ_SYNC_PUB_ENDPOINT) != 0) {
            log_message(LOG_ERROR, "task-wrapper", "zmq_bind(XPUB) failed: %s", zmq_strerror(errno));
            exit(EXIT_FAILURE);
        }
        log_message(LOG_INFO, "task-wrapper", "Leader: XPUB bound on %s", ZMQ_SYNC_PUB_ENDPOINT);

    } else {
        /* Worker: connect SUB (to receive SYNC broadcast) FIRST, then PUSH (to send READY) via IPC */
        em->zmq_sub = zmq_socket(em->zmq_context, ZMQ_SUB);
        if (!em->zmq_sub) {
            log_message(LOG_ERROR, "task-wrapper", "zmq_socket(SUB) failed: %s", zmq_strerror(errno));
            exit(EXIT_FAILURE);
        }
        zmq_setsockopt(em->zmq_sub, ZMQ_SUBSCRIBE, "", 0);
        zmq_setsockopt(em->zmq_sub, ZMQ_LINGER, &linger, sizeof(linger));

        if (zmq_connect(em->zmq_sub, ZMQ_SYNC_PUB_ENDPOINT) != 0) {
            log_message(LOG_ERROR, "task-wrapper", "zmq_connect(SUB) failed: %s", zmq_strerror(errno));
            exit(EXIT_FAILURE);
        }
        log_message(LOG_INFO, "task-wrapper", "Worker: SUB connected to %s", ZMQ_SYNC_PUB_ENDPOINT);

        em->zmq_push = zmq_socket(em->zmq_context, ZMQ_PUSH);
        if (!em->zmq_push) {
            log_message(LOG_ERROR, "task-wrapper", "zmq_socket(PUSH) failed: %s", zmq_strerror(errno));
            exit(EXIT_FAILURE);
        }
        zmq_setsockopt(em->zmq_push, ZMQ_LINGER, &linger, sizeof(linger));

        if (zmq_connect(em->zmq_push, ZMQ_SYNC_PULL_ENDPOINT) != 0) {
            log_message(LOG_ERROR, "task-wrapper", "zmq_connect(PUSH) failed: %s", zmq_strerror(errno));
            exit(EXIT_FAILURE);
        }
        log_message(LOG_INFO, "task-wrapper", "Worker: PUSH connected to %s", ZMQ_SYNC_PULL_ENDPOINT);
    }
}

/* ----------------- RT Timer Thread for Precise Activation ----------------- */
/* ----------------- Executor Manager Activities ----------------- */

void em_run_schedule(execution_manager_t *em, schedule_t *sched) {
    g_return_if_fail(em != NULL);
    g_return_if_fail(sched != NULL);

    ipc_msg_t msg;
    gint64 time_zero_us;

    if (em->is_leader) {
        /* LEADER LOGIC: Wait for all workers (READY + SUB confirmed) */
        guint target_count = sched->schedule_num_workers;
        log_message(LOG_INFO, "task-wrapper", "Leader: Waiting for %d workers to be READY and subscribed...", target_count);

        guint ready_count = 0;

        /* Poll both PULL (READY) and XPUB (subscriptions) simultaneously */
        zmq_pollitem_t poll_items[2];
        poll_items[0].socket = em->zmq_pull;
        poll_items[0].events = ZMQ_POLLIN;
        poll_items[1].socket = em->zmq_pub;
        poll_items[1].events = ZMQ_POLLIN;

        while (ready_count < target_count || em->zmq_sub_count < target_count) {
            zmq_poll(poll_items, 2, -1); /* block until activity */

            /* Check PULL socket for READY messages */
            if (poll_items[0].revents & ZMQ_POLLIN) {
                int rc = zmq_recv(em->zmq_pull, &msg, sizeof(msg), 0);
                if (rc != -1 && msg.type == MSG_TASK_READY) {
                    ready_count++;
                    log_message(LOG_INFO, "task-wrapper", "Leader: Worker %d/%d is ready", ready_count, target_count);
                }
            }

            /* Check XPUB socket for subscription notifications */
            if (poll_items[1].revents & ZMQ_POLLIN) {
                char buf[32];
                int rc = zmq_recv(em->zmq_pub, buf, sizeof(buf), 0);
                if (rc > 0 && buf[0] == 1) { /* 1 = subscribe event */
                    em->zmq_sub_count++;
                    log_message(LOG_INFO, "task-wrapper", "Leader: SUB confirmed %d/%d", em->zmq_sub_count, target_count);
                }
            }
        }

        /* All ready! Set T-Zero to (Now + 500ms) to account for network/latency */
        time_zero_us = g_get_monotonic_time() + 500000;

        /* Broadcast T-Zero to all workers via ZMQ PUB (single send, all SUB receive) */
        memset(&msg, 0, sizeof(msg));
        msg.type = MSG_TASK_SYNC;
        msg.data.sync_time_us = time_zero_us;

        if (zmq_send(em->zmq_pub, &msg, sizeof(msg), 0) == -1) {
            log_message(LOG_ERROR, "task-wrapper", "Failed to PUB SYNC: %s", zmq_strerror(errno));
        }
        log_message(LOG_INFO, "task-wrapper", "Leader: Barrier released. Sync time broadcast to %d workers", target_count);

    } else {
        /* WORKER LOGIC: Signal ready and wait via ZMQ */
        memset(&msg, 0, sizeof(msg));
        msg.type = MSG_TASK_READY;
        msg.task_id = 0;

        log_message(LOG_INFO, "task-wrapper", "Worker: Signaling READY to leader...");
        if (zmq_send(em->zmq_push, &msg, sizeof(msg), 0) == -1) {
            log_message(LOG_ERROR, "task-wrapper", "Failed to PUSH READY: %s", zmq_strerror(errno));
        }

        /* Wait for the Leader to broadcast SYNC via PUB/SUB */
        log_message(LOG_INFO, "task-wrapper", "Worker: Waiting for SYNC signal from leader...");

        int rc = zmq_recv(em->zmq_sub, &msg, sizeof(msg), 0); /* blocking */
        if (rc != -1 && msg.type == MSG_TASK_SYNC) {
            time_zero_us = msg.data.sync_time_us;
        } else {
            log_message(LOG_ERROR, "task-wrapper", "Worker: Failed to receive SYNC: %s", zmq_strerror(errno));
            return;
        }
        log_message(LOG_INFO, "task-wrapper", "Worker: Received sync time. Starting timers...");
    }
    log_message(LOG_INFO, "task-wrapper", "Final Synchronization Complete. T-Zero (Monotonic): %ld us", (long)time_zero_us);
    //return;
    /* THE REST OF YOUR FUNCTION (Unchanged timers logic) */
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);

    

    /* Plan the scheudle DEADLINES */
    for (GList *l = sched->schedule_end_info->head; l != NULL; l = l->next) {
        timeline_entry_t *entry = (timeline_entry_t *)l->data;

        
        deadline_context_t *ctx = g_new0(deadline_context_t, 1);
        ctx->data = entry->data_list;
        ctx->loop = loop;
        ctx->timestamp = entry->timestamp;
        ctx->is_last = (l->next == NULL); 
        ctx->sched = sched;

        gint64 target_mono_us = time_zero_us + (entry->timestamp * 1000);

        GSource *source = g_timeout_source_new(0);
        g_source_set_ready_time(source, target_mono_us);
        g_source_set_callback(source, handle_expiration, ctx, NULL);
        g_source_attach(source, g_main_loop_get_context(loop));
        g_source_unref(source);
    }

    /* Plan the schedule STARTS */
    for (GList *l = sched->schedule_start_info->head; l != NULL; l = l->next) {
        timeline_entry_t *entry = (timeline_entry_t *)l->data;

        start_context_t *ctx = g_new0(start_context_t, 1);
        ctx->data = entry->data_list;
        ctx->timestamp = entry->timestamp;
        ctx->sched = sched;

        

        gint64 target_mono_us = time_zero_us + (entry->timestamp * 1000);

        GSource *source = g_timeout_source_new(0);
        g_source_set_ready_time(source, target_mono_us);
        g_source_set_callback(source, handle_initialization, ctx, NULL);
        g_source_attach(source, g_main_loop_get_context(loop));
        g_source_unref(source);
    }

    log_message(LOG_INFO, "task-wrapper", "Scheduler started! Waiting for events...");
    g_main_loop_run(loop);

    g_main_loop_unref(loop);
    log_message(LOG_INFO, "task-wrapper", "Scheduler terminated successfully");
}

void em_wait_for_schedule(execution_manager_t *em) {
    while (1) {
        // Updated to check for the new hash name
        redisReply *r = redisCommand(em->redis_client, "EXISTS schedule_data");
        if (r && r->integer == 1) {
            freeReplyObject(r);
            return;
        }
        if (r) freeReplyObject(r);
        log_message(LOG_INFO, "task-wrapper", "Waiting for schedule_data in Redis...");
        sleep(1);
    }
}



schedule_t* em_read_schedule(execution_manager_t *em) {
    // 1. Fetch the entire flat hash from Redis
    redisReply *r = redisCommand(em->redis_client, "HGETALL schedule_data");
    if (!r || r->type == REDIS_REPLY_ERROR || r->elements == 0) {
        if (r) freeReplyObject(r);
        return NULL;
    }

    const char *name = redis_get_hash_value(r, "schedule:name");
    const char *version = redis_get_hash_value(r, "schedule:version");
    const char *leader = redis_get_hash_value(r, "schedule:leader");
    const char *duration_str = redis_get_hash_value(r, "schedule:duration");
    const char *images_json = redis_get_hash_value(r, "schedule:images");
    const char *iterations_str = redis_get_hash_value(r, "schedule:iterations");

    // 2. Parse the list of image names
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, images_json, -1, NULL)) {
        log_message(LOG_WARN, "task-wrapper", "Failed to parse images JSON");
        freeReplyObject(r);
        return NULL; 
    }

    JsonArray *images_array = json_node_get_array(json_parser_get_root(parser));
    GList* images_list = json_array_to_gstring_list(images_array);

    GList *l, *next;

    for (l = images_list; l != NULL; l = next) {
        next = l->next; // Always capture next at the start of the loop
        GString *gs = (GString *)l->data;

        if (g_strcmp0(em->em_name->str, gs->str) == 0) {
            // Unlink the current node from the list
            images_list = g_list_remove_link(images_list, l);
            
            // Free the data (GString)
            g_string_free(gs, TRUE);
            
            // Free the specific list node structure
            g_list_free_1(l);

            break;
        }
    }
    

    if (!name || !version || !leader || !images_json) {
        freeReplyObject(r);
        return NULL;
    }

    // Set Leader Flag
    em_set_leader(em, g_strcmp0(em->em_name->str, leader) == 0);

    // Setup ZeroMQ sockets based on role
    em_setup_zmq(em, leader);

    // Create the schedule
    gint iterations = iterations_str ? atoi(iterations_str) : -1;
    schedule_t *sched = schedule_new(name, version, leader, images_list, duration_str ? g_ascii_strtoll(duration_str, NULL, 10) : 0, iterations);

    // Task Loading into the schedule
    gchar *img_name = em->em_name->str;
    char len_key[128];
    snprintf(len_key, sizeof(len_key), "schedule:%s:length", img_name);
    const char *img_task_count_str = redis_get_hash_value(r, len_key);
    int img_task_count = img_task_count_str ? atoi(img_task_count_str) : 0;

    for (int j = 0; j < img_task_count; j++) {
        char task_key[128];
        snprintf(task_key, sizeof(task_key), "schedule:%s:%d", img_name, j);
        const char *task_json = redis_get_hash_value(r, task_key);

        if (task_json) {
            JsonParser *t_parser = json_parser_new();
            if (json_parser_load_from_data(t_parser, task_json, -1, NULL)) {
                JsonObject *t_obj = json_node_get_object(json_parser_get_root(t_parser));

                // FIELD MAPPING LOGIC

                // Numeric fields (Direct)
                guint16 t_id = (guint16)json_object_get_int_member(t_obj, "id");
                gint64 start_time = json_object_get_int_member(t_obj, "start");
                gint64 end_time = json_object_get_int_member(t_obj, "deadline");

                // Stringified Numeric fields ("1", "10") -> Use atoi()
                const char *cpu_str = json_object_get_string_member(t_obj, "cpu_affinity");
                const char *prio_str = json_object_get_string_member(t_obj, "priority");
                
                gint cpu = cpu_str ? atoi(cpu_str) : 0;
                gint8 priority = prio_str ? (gint8)atoi(prio_str) : 0;

                // Policy String ("other", "normal", "batch", "idle", "fifo", "rr", "deadline")
                const char *policy_name = json_object_get_string_member(t_obj, "policy");
                gint policy = SCHED_OTHER; // Default to 0
                if (policy_name != NULL) {
                    if (g_ascii_strcasecmp(policy_name, "other") == 0 ||
                        g_ascii_strcasecmp(policy_name, "normal") == 0) {
                        policy = SCHED_OTHER; // SCHED_NORMAL is the kernel-side alias for the same value (0)
                    } else if (g_ascii_strcasecmp(policy_name, "batch") == 0) {
                        policy = SCHED_BATCH;
                    } else if (g_ascii_strcasecmp(policy_name, "idle") == 0) {
                        policy = SCHED_IDLE;
                    } else if (g_ascii_strcasecmp(policy_name, "fifo") == 0) {
                        policy = SCHED_FIFO;
                    } else if (g_ascii_strcasecmp(policy_name, "rr") == 0) {
                        policy = SCHED_RR;
                    } else if (g_ascii_strcasecmp(policy_name, "deadline") == 0) {
                        // -------------------------- TODO --------------------------------------------
                        // WARRNING: SCHED_DEADLINE cannot be applied via pthread_attr_setschedpolicy()
                        // (used below); it requires the sched_setattr(2) syscall with a struct sched_attr.
                        // ----------------------------------------------------------------------------
                        policy = SCHED_DEADLINE;
                    } else {
                        log_message(LOG_WARN, "task-wrapper", "Unknown scheduler policy '%s', defaulting to SCHED_OTHER", policy_name);
                    }
                }

                // D. Inputs (Read as string to prevent JSON nested-node errors)
                const char *json_input = json_object_get_string_member(t_obj, "inputs");
                log_message(LOG_DEBUG, "task-wrapper", "input: %s", json_input ? json_input : "[{}]");

                GSList* t_input =  parse_input_list(g_strdup(json_input ? json_input : "[{}]"));

                schedule_add_task(
                    sched,
                    t_id,
                    img_name,
                    NULL, 
                    policy,
                    priority,
                    cpu,
                    1, 
                    NULL, 
                    start_time,
                    end_time,
                    t_input
                );
            }
            g_object_unref(t_parser);
        }
    }

    g_object_unref(parser);
    freeReplyObject(r);
    return sched;
}


/* ----------------- Executor Manager Usefull Functions ----------------- */

void* task_wrapper_func(void* data){

    task_wrapper_input_t* tw_input = (task_wrapper_input_t*)data;

    /* Read the thread context arguments */
    guint16 task_id = tw_input->task_id;
    gpointer input = tw_input->data;
    GThreadFunc thread_func = tw_input->thread_func;

    /* Measure the end_time_request (for the performance) */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    glong end_time_request = ts.tv_sec * 1000000000L + ts.tv_nsec;
    /* -------------------------------------------------- */

    /* Core the task thread is actually scheduled on (for the performance) */
    gint core_id = sched_getcpu();
    /* -------------------------------------------------- */

    /* For the non-RT policies the "priority" is the nice value: it cannot be set through
     * the pthread attributes, so the thread applies it to itself (nice is per-thread on Linux) */
    if ((tw_input->policy == SCHED_OTHER || tw_input->policy == SCHED_BATCH) && tw_input->priority != 0) {
        if (setpriority(PRIO_PROCESS, gettid(), tw_input->priority) == -1) {
            log_message(LOG_WARN, "task-wrapper", "Failed to set nice %d for Task ID %u: %s", tw_input->priority, task_id, g_strerror(errno));
        }
    }

    /* Run the thread function */
    gpointer res = task_main(input);

    /* Measure the start_time_result (for the performance) */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    glong start_time_result = ts.tv_sec * 1000000000L + ts.tv_nsec;
    /* -------------------------------------------------- */

    /* Hand the result to the scheduler main loop: it stores it, measures end_time_result and
     * logs the performance (see results_post). No blocking call before this point. */
    tw_input->end_time_request = end_time_request;
    tw_input->start_time_result = start_time_result;
    tw_input->core_id = core_id;

    /* On PREEMPT_RT the kernel lock of the eventfd written by results_post() is a sleeping lock,
     * shared with the main loop: if the thread sleeps on it, it is woken at the TAIL of its
     * SCHED_FIFO priority and waits for a whole same-priority task of its core. So RT task
     * threads do the hand-off at the scheduler priority and then go back to their own. */
    gint policy = tw_input->policy;
    struct sched_param task_param = { .sched_priority = tw_input->priority };
    gboolean boost = (policy == SCHED_FIFO || policy == SCHED_RR) && results_boost_prio > task_param.sched_priority;
    if (boost) {
        struct sched_param boost_param = { .sched_priority = results_boost_prio };
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &boost_param);
    }

    results_post(tw_input);      // tw_input is owned by the main loop from here on

    if (boost) {
        pthread_setschedparam(pthread_self(), policy, &task_param);
    }

    log_message(LOG_DEBUG, "task-wrapper", "ThreadCall %u: termination thread function", task_id);

    /* Cleanup (tw_input is freed by the main loop) */
    g_free(res);
    return NULL;

}

/* ----------------- Executor Manager Event Handlers ----------------- */

gboolean handle_initialization(gpointer user_data) {
    start_context_t *ctx = (start_context_t *)user_data;
    GSList *tasks = (GSList *)ctx->data;
    schedule_t* sched = ctx->sched;


    if (tasks == NULL) {
        log_message(LOG_ERROR, "task-wrapper", "No tasks");
        g_free(ctx);
        return G_SOURCE_REMOVE;
    }

    for (GSList *l = tasks; l != NULL; l = l->next) {
        
        /* Read the current task information */
        activation_data_t *task = (activation_data_t *)l->data;

        /* Prepare the thread (wrapper) input */
        task_wrapper_input_t* tw_input = g_new0(task_wrapper_input_t, 1);
        tw_input->task_id = task->task_id;
        tw_input->data = task->input_data->data;
        tw_input->thread_func = task->task_exec;
        tw_input->sched = sched;
        tw_input->policy = task->policy;
        tw_input->priority = task->priority;
        tw_input->iteration = iteration;    // Iteration of this activation (for the performance)

        /* Measure the start_time_request (for the performance) */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        tw_input->start_time_request = ts.tv_sec * 1000000000L + ts.tv_nsec;
        /* --------------------------------------------------- */


        /* Prepare the thread */
        pthread_attr_t attr;
        pthread_attr_init(&attr);

        /* Set CPU Affinity core */
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(task->cpu_affinity, &set);
        gint affinity_err = pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &set);
        if (affinity_err != 0) {
            log_message(LOG_WARN, "task-wrapper", "Failed to set CPU affinity for Task ID %u. Error: %d (%s)", task->task_id, affinity_err, g_strerror(affinity_err));
        }

        /* Setting scheduler policy and priority */
        /* Only FIFO/RR use a static priority: OTHER/BATCH/IDLE accept only 0 (the nice value is
         * applied by the thread itself). */
        struct sched_param param;
        gboolean is_rt = (task->policy == SCHED_FIFO || task->policy == SCHED_RR);
        param.sched_priority = is_rt ? sched_get_priority_min(task->policy) : 0;

        pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN);
        pthread_attr_setschedpolicy(&attr, task->policy);
        gint param_err = pthread_attr_setschedparam(&attr, &param);
        if (param_err != 0) {
            log_message(LOG_WARN, "task-wrapper", "Invalid priority %d for Task ID %u. Error: %d (%s)", param.sched_priority, task->task_id, param_err, g_strerror(param_err));
        }
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

        pthread_t thread;
        gint rc = pthread_create(&thread, &attr, task_wrapper_func, tw_input);
        pthread_attr_destroy(&attr); // Clean up attributes
        if (rc) {
            log_message(LOG_ERROR, "task-wrapper", "pthread_create failed with code %d (%s) for Task ID %u", rc, g_strerror(rc), task->task_id);
            continue;
        }

        /* Raise the RT thread to its target priority: it is queued at the tail of that priority list */
        if (is_rt) {
            param.sched_priority = task->priority;
            gint prio_err = pthread_setschedparam(thread, task->policy, &param);
            if (prio_err != 0) {
                log_message(LOG_WARN, "task-wrapper", "Invalid priority %d for Task ID %u. Error: %d (%s)", task->priority, task->task_id, prio_err, g_strerror(prio_err));
            }
        }
        pthread_detach(thread);

        // Iterate through GSList of dependencies
        if (task->depends_on) {
            GString *deps = g_string_new(NULL);
            for (GSList *dep = task->depends_on; dep != NULL; dep = dep->next) {
                // Assuming the list stores integers cast to pointers, or pointers to guint
                // If they are pointers to uint: *(guint*)dep->data
                // If they are IDs stored directly in the pointer: GPOINTER_TO_UINT(dep->data)
                g_string_append_printf(deps, "%u ", GPOINTER_TO_UINT(dep->data));
            }
            log_message(LOG_DEBUG, "task-wrapper", "Depends On (IDs): %s", deps->str);
            g_string_free(deps, TRUE);
        } else {
            log_message(LOG_DEBUG, "task-wrapper", "Depends On: None");
        }
    }

    g_free(ctx); 
    return G_SOURCE_REMOVE;
}



gboolean handle_expiration(gpointer user_data) {
    deadline_context_t *ctx = (deadline_context_t *)user_data;
    GSList *tasks = (GSList *)ctx->data;

    if (tasks == NULL) {
        log_message(LOG_INFO, "task-wrapper", "No tasks to expire");
    } else {
        for (GSList *l = tasks; l != NULL; l = l->next) {
            expiration_data_t *exp = (expiration_data_t *)l->data;

            /* Check if the task is jet completed*/
            if (schedule_is_task_completed(ctx->sched, exp->task_id)) {
                log_message(LOG_INFO, "task-wrapper", "Task %u already completed", exp->task_id);
                continue;
            }
            log_message(LOG_INFO, "task-wrapper", "Sent ABORT for Task ID %u", exp->task_id);
        }
    }

    if (ctx->is_last) {
        log_message(LOG_INFO, "task-wrapper", "handle_expiration: Final deadline reached. Quitting...");
        g_main_loop_quit(ctx->loop);
    }

    g_free(ctx); 
    return G_SOURCE_REMOVE;
}