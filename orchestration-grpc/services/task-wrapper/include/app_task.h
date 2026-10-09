#ifndef TASK_ENTRY_H
#define TASK_ENTRY_H

#include <string.h>
#include <stdio.h>
#include <stdlib.h>     /* Required for atoi */
#include <pthread.h>    /* Required for pthread_mutex_t */
#include "jsmn.h"
#include "task_ipc.h"   /* Required for task_service_state_t (IDLE, RUNNING, COMPLETED) */
#include "logger.h"

/* --- Data Structures --- */

typedef struct {
    int a;
    int b;
} input_t;

typedef struct {
    int result;
} output_t;

/* * Shared Context Structure.
 * This resides in the heap and is accessed by both Main and Thread.
 */
typedef struct {
    pthread_mutex_t lock;           // Protects access to status and output
    task_service_state_t status;    // Current state (IDLE, RUNNING, COMPLETED)
    input_t input;                  // Input data (Read-only for the thread)
    output_t output;                // Output data (Written by the thread)
} task_context_t;



/* --- Helper Function --- */

/*
 * Compares a JSMN token with a C string.
 * It handles both standard JSON strings (JSMN_STRING) 
 * and unquoted keys (JSMN_PRIMITIVE) if strict mode is off.
 */
static int json_eq(const char *json, jsmntok_t *tok, const char *s) {
    if (tok->type == JSMN_STRING || tok->type == JSMN_PRIMITIVE) {
        if ((int)strlen(s) == tok->end - tok->start &&
            strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
            return 1;
        }
    }
    return 0;
}

/* --- Task Functions --- */

/* * Input Conversion: JSON String -> Struct 
 */
int convert_input(char* input_json, input_t* input){
    
    /* Prepare parser and token array */
    jsmn_parser p;
    jsmntok_t t[128];   // We expect no more than 128 JSON tokens
    
    jsmn_init(&p);
    
    /* Parsing execution */
    int r = jsmn_parse(&p, input_json, strlen(input_json), t, 128); 

    if (r < 0) {
        // Handle error codes if necessary
        return -1;
    }

    /* Scanning the results */
    /* Assume the top-level element is an object */
    if (r < 1 || t[0].type != JSMN_OBJECT) {
        return -1;
    }

    /* Loop through tokens (skipping the root object at index 0) */
    for (int i = 1; i < r; i++) {
        
        /* Check for key "a" */
        if (json_eq(input_json, &t[i], "a")) {
            char buf[32];
            int len = t[i+1].end - t[i+1].start;
            
            // Safety check for buffer size
            if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
            
            // Copy value to buffer to null-terminate it for atoi
            memcpy(buf, input_json + t[i+1].start, len);
            buf[len] = '\0';
            
            input->a = atoi(buf);
            
            i++; // Skip the value token
        }
        /* Check for key "b" */
        else if (json_eq(input_json, &t[i], "b")) {
            char buf[32];
            int len = t[i+1].end - t[i+1].start;
            
            if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
            
            memcpy(buf, input_json + t[i+1].start, len);
            buf[len] = '\0';
            
            input->b = atoi(buf);
            
            i++; // Skip the value token
        }
    }
    
    return 0;
}

/* * Output Conversion: Struct -> JSON String
 */
int convert_output(output_t* output, char* output_json) {
    /* Note: Ensure 'output_json' buffer is large enough in the caller */
    return sprintf(output_json, "{\"result\": %d}", output->result);
}


/* * MAIN TASK (Worker Thread)
 * Receives the Context pointer, performs calculations, and writes the result.
 */
void *task_main(void *arg){
    task_context_t *ctx = (task_context_t *)arg;

    /* 1. Update state to RUNNING */
    pthread_mutex_lock(&ctx->lock);
    ctx->status = RUNNING; 
    pthread_mutex_unlock(&ctx->lock);

    log_message(LOG_DEBUG, "task-wrapper", "Processing: %d + %d", ctx->input.a, ctx->input.b);

    /* 2. Real-Time Logic (Calculations) */
    /* * Accessing ctx->input is thread-safe here because 
     * the Main thread does not modify it after creation.
     */
    int res = ctx->input.a / ctx->input.b;

    // Simulate heavy workload if needed
    // usleep(1000); 

    /* 3. Write Output protected by Mutex */
    pthread_mutex_lock(&ctx->lock);
    ctx->output.result = res;
    ctx->status = COMPLETED;
    pthread_mutex_unlock(&ctx->lock);

    log_message(LOG_DEBUG, "task-wrapper", "Job done. Result: %d", res);

    return NULL;
}

#endif