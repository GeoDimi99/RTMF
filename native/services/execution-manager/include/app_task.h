#ifndef APP_TASK_H
#define APP_TASK_H

#define _GNU_SOURCE
#include <sched.h>
#include <stdlib.h>
#include <glib.h>
//#include <json-glib/json-glib.h>
#include <pthread.h>
#include <math.h>

#include <fcntl.h>
#include <unistd.h>

#define CPU_INTENSITY 1000000 
#define IO_TMP_FILE "/tmp/rt_bench.bin"

/* --- Data Structures --- */

typedef struct {
    int total_ops;     // Total number of operations to perform
    int io_percentage; // % of total_ops that are I/O (0-100)
} input_t;

typedef struct {
    int result;        // Status code (e.g., 0 for success)
} output_t;


/* Standard Thread Function Signature */
void* task_main(void* arg);

#endif