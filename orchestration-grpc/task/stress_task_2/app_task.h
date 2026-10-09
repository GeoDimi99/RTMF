#ifndef APP_TASK_CONFIGURABLE_H
#define APP_TASK_CONFIGURABLE_H

#define _GNU_SOURCE
#include <sched.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include "jsmn.h"
#include "task_ipc.h"   /* Required for task_service_state_t */

/* --- Configuration --- */
#define CPU_INTENSITY 1000000 
#define IO_TMP_FILE "/tmp/rt_bench.bin"
#define MAX_JSON_TOKENS 128
#define MAX_TASK_JSON_OUT 4096

/* --- Data Structures --- */

typedef struct {
    int total_ops;     // Total number of operations to perform
    int io_percentage; // % of total_ops that are I/O (0-100)
} input_t;

typedef struct {
    int result;        // Status code (e.g., 0 for success)
} output_t;

/* Shared Context Structure */
typedef struct {
    pthread_mutex_t lock;
    task_service_state_t status;
    input_t input;
    output_t output;
} task_context_t;

/* --- Helper: JSON Parsing with jsmn --- */

static int jsoneq(const char *json, jsmntok_t *tok, const char *s) {
    if (tok->type == JSMN_STRING && 
        (int)strlen(s) == tok->end - tok->start &&
        strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
        return 0;
    }
    return -1;
}

static int parse_int(const char *json, jsmntok_t *tok) {
    char buf[32];
    int len = tok->end - tok->start;
    if (len >= 32) len = 31;
    strncpy(buf, json + tok->start, len);
    buf[len] = '\0';
    return atoi(buf);
}

/* --- JSON Conversion Functions --- */

int convert_input(const char *json_str, input_t *input) {
    if (!json_str || !input) return -1;
    
    // Initialize defaults
    input->total_ops = 100;
    input->io_percentage = 0;
    
    jsmn_parser parser;
    jsmntok_t tokens[MAX_JSON_TOKENS];
    
    jsmn_init(&parser);
    int num_tokens = jsmn_parse(&parser, json_str, strlen(json_str), 
                                 tokens, MAX_JSON_TOKENS);
    
    if (num_tokens < 1 || tokens[0].type != JSMN_OBJECT) {
        return -1;
    }
    
    // Parse JSON object
    for (int i = 1; i < num_tokens; i++) {
        if (jsoneq(json_str, &tokens[i], "total_ops") == 0) {
            input->total_ops = parse_int(json_str, &tokens[i + 1]);
            i++;
        } else if (jsoneq(json_str, &tokens[i], "io_percentage") == 0) {
            input->io_percentage = parse_int(json_str, &tokens[i + 1]);
            i++;
        }
    }
    
    // Validate
    if (input->io_percentage < 0) input->io_percentage = 0;
    if (input->io_percentage > 100) input->io_percentage = 100;
    if (input->total_ops < 1) input->total_ops = 1;
    
    return 0;
}

int convert_output(output_t *output, char *json_out) {
    if (!output || !json_out) return -1;
    
    snprintf(json_out, 1024, "{\"result\": %d}", output->result);
    return 0;
}

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
    // Write data
    if (write(fd, buf, sizeof(buf)) > 0) {
        // Sync to disk (real I/O operation)
        fdatasync(fd); 
    }
}

/* --- Main Task Logic --- */

void* task_main(void* arg) {
    task_context_t *ctx = (task_context_t *)arg;
    
    // 1. Update state to RUNNING
    pthread_mutex_lock(&ctx->lock);
    ctx->status = RUNNING;
    pthread_mutex_unlock(&ctx->lock);
    
    // 2. Thread Setup
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
    
    // 3. Workload Distribution
    int io_ops = (ctx->input.total_ops * ctx->input.io_percentage) / 100;
    int cpu_ops = ctx->input.total_ops - io_ops;
    int core = sched_getcpu();
    
    printf("[THREAD] Core %d | Executing: %d CPU ops, %d I/O ops (total %d, io_pct %d%%)\n", 
           core, cpu_ops, io_ops, ctx->input.total_ops, ctx->input.io_percentage);
    
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

    
    printf("[THREAD] ✅ Task completed successfully on core %d\n", core);
    
    // 6. Write Output protected by Mutex
    pthread_mutex_lock(&ctx->lock);
    ctx->output.result = 0;  // Success
    ctx->status = COMPLETED;
    pthread_mutex_unlock(&ctx->lock);
    
    return NULL;
}

#endif /* APP_TASK_CONFIGURABLE_H */
