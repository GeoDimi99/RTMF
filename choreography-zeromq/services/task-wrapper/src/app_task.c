#include "app_task.h"


/* --- Internal Workload Functions --- */

static void do_cpu_op() {
    double val = 10;
    for (int i = 0; i < CPU_INTENSITY; i++) {
        sqrt(val);
        val += 1;
    }
    
}

static void do_io_op(int fd) {
    if (fd < 0) return;
    char buf[1024] = {0};
    if (write(fd, buf, sizeof(buf)) > 0) {
        fdatasync(fd); 
    }
}

/* --- JSON Parsing Logic --- */

int convert_json_to_input(JsonObject *obj, input_t* input) {
    g_return_val_if_fail(obj != NULL, -1);
    g_return_val_if_fail(input != NULL, -1);

    input->total_ops = json_object_has_member(obj, "total_ops") ? 
                       json_object_get_int_member(obj, "total_ops") : 100;
    //g_print("[DEBUG] Execution Manager (converter): %d\n", input->total_ops);

    input->io_percentage = json_object_has_member(obj, "io_percentage") ? 
                           json_object_get_int_member(obj, "io_percentage") : 0;

    return 0;
}

gchar* convert_output_to_json(const output_t* output) {
    JsonObject *obj = json_object_new();
    json_object_set_int_member(obj, "result", output->result);

    JsonNode *root = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(root, obj);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    gchar *res = json_generator_to_data(gen, NULL);

    g_object_unref(gen);
    json_node_free(root);
    return res;
}

/* --- Standardized Main Task Logic --- */

void* task_main(void* arg) {
    // Cast the generic pointer back to our known input type
    input_t *input = (input_t *)arg;
    if (input == NULL) return NULL;

    // Thread Setup
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    // Workload Distribution
    int io_ops = (input->total_ops * input->io_percentage) / 100;
    int cpu_ops = input->total_ops - io_ops;
    int core = sched_getcpu();


    //  Execution First CPU Phase
    for (int i = 0; i < cpu_ops / 2; i++) {
        do_cpu_op();
        if (i % 50 == 0) pthread_testcancel(); 
    }

    // Execution First I/O Phase
    int fd = open(IO_TMP_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    for (int i = 0; i < io_ops / 2; i++) {
        do_io_op(fd);
        if (i % 50 == 0) pthread_testcancel();
    }
    if (fd >= 0) close(fd);

    // Execution Second CPU Phase
    for (int i = 0; i < cpu_ops / 2; i++) {
        do_cpu_op();
        if (i % 50 == 0) pthread_testcancel(); 
    }

    // Execution Second I/O Phase
    fd = open(IO_TMP_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    for (int i = 0; i < io_ops / 2; i++) {
        do_io_op(fd);
        if (i % 50 == 0) pthread_testcancel();
    }
    if (fd >= 0) close(fd);

    // Return Output
    // We allocate the output_t on the heap so it persists after the thread joins
    output_t *res = g_new0(output_t, 1);
    res->result = 0; 
    
    return (void*)res; 
}