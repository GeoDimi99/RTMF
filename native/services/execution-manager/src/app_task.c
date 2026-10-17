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

    g_print("[DEBUG] Thread: core: %d, tot_ops: %d, io_percentage: %d\n", core, input->total_ops, input->io_percentage);

    //  Execution First CPU Phase
    for (int i = 0; i < cpu_ops / 2; i++) {
        do_cpu_op();
        if (i % 50 == 0) pthread_testcancel();
    }

    // Execution First I/O Phase
    // int fd = open(IO_TMP_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    // for (int i = 0; i < io_ops / 2; i++) {
    //     do_io_op(fd);
    //     if (i % 50 == 0) pthread_testcancel();
    // }
    // if (fd >= 0) close(fd);

    // Execution Second CPU Phase
    for (int i = 0; i < cpu_ops / 2; i++) {
        do_cpu_op();
        if (i % 50 == 0) pthread_testcancel();
    }

    // Execution Second I/O Phase
    // fd = open(IO_TMP_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    // for (int i = 0; i < io_ops / 2; i++) {
    //     do_io_op(fd);
    //     if (i % 50 == 0) pthread_testcancel();
    // }
    // if (fd >= 0) close(fd);

    // Return Output
    // We allocate the output_t on the heap so it persists after the thread joins
    output_t *res = g_new0(output_t, 1);
    res->result = 0;

    return (void*)res;
}
