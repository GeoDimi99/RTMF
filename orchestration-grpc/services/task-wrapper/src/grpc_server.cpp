#include <iostream>
#include <memory>
#include <string>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <limits.h>
#include <sys/mman.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>          // gettid()
#include <sys/resource.h>    // setpriority() for the nice value
#include <grpcpp/grpcpp.h>
#include "proto/task_service.grpc.pb.h"

// GLib/json-glib are C++-aware: include them outside extern "C" so that their
// C++-only parts (templates) keep C++ linkage. app_task.h re-includes them as no-ops.
#include <glib.h>
#include <json-glib/json-glib.h>

// Include C headers
extern "C" {
    #include "../include/app_task.h"
    #include "logger.h"
}

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerWriter;
using grpc::Status;
using taskservice::TaskExecutor;
using taskservice::TaskRequest;
using taskservice::TaskResponse;

// ============================================
// THREAD WRAPPER
// (Generic wrapper in grpc_server.cpp, like task_wrapper.c in MQ version)
// ============================================

// Carries the task input/output together with the performance timestamps/core_id
// captured from inside the task thread itself (for measuring performance).
// Allocated on the caller's stack: the caller always pthread_join()s before
// reading these fields, so there is no lifetime issue.
typedef struct {
    input_t* input;          // Task input (read-only for the task thread)
    output_t* output;        // Task output, heap-allocated by task_main() (g_free by the caller)
    long end_time_request;   // Thread start timestamp (ns) - beginning of task thread execution
    long start_time_result;  // Thread end timestamp (ns) - end of task thread execution
    int  core_id;            // CPU core the task thread actually ran on
    int  cpu_affinity;       // CPU core to pin the task thread to (-1 = no explicit pinning)
    int  policy;             // Scheduler policy of the task thread
    int  nice;               // Nice value applied by the thread itself (only for OTHER/BATCH)
} perf_thread_ctx_t;

extern "C" {
    void* task_main_wrapper(void* arg) {
        perf_thread_ctx_t* pctx = (perf_thread_ctx_t*)arg;

        /* Set CPU affinity from inside the thread (self-affinity), as requested by the manifest */
        if (pctx->cpu_affinity >= 0) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(pctx->cpu_affinity, &cpuset);
            int affinity_ret = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
            if (affinity_ret != 0) {
                log_message(LOG_WARN, "task-wrapper", "Failed to set CPU affinity to CPU %d: %s",
                            pctx->cpu_affinity, strerror(affinity_ret));
            }
        }

        /* Measure the beginning of the task thread execution (for the performance) */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        pctx->end_time_request = ts.tv_sec * 1000000000L + ts.tv_nsec;
        pctx->core_id = sched_getcpu();
        /* -------------------------------------------------- */

        /* For the non-RT policies the "priority" is the nice value: it cannot be set through
         * the pthread attributes, so the thread applies it to itself (nice is per-thread on Linux) */
        if ((pctx->policy == SCHED_OTHER || pctx->policy == SCHED_BATCH) && pctx->nice != 0) {
            if (setpriority(PRIO_PROCESS, gettid(), pctx->nice) == -1) {
                log_message(LOG_WARN, "task-wrapper", "Failed to set nice %d: %s", pctx->nice, strerror(errno));
            }
        }

        /* Execute the actual task (task-specific code) */
        pctx->output = (output_t*) task_main(pctx->input);

        /* Measure the end of the task thread execution (for the performance) */
        clock_gettime(CLOCK_MONOTONIC, &ts);
        pctx->start_time_result = ts.tv_sec * 1000000000L + ts.tv_nsec;
        /* -------------------------------------------------- */

        return NULL;
    }
}

// Maps the "policy" string carried by a TaskRequest to the matching Linux
// scheduler constant. Unknown/empty strings fall back to SCHED_OTHER.
static int policy_from_string(const std::string& policy_str) {
    if (policy_str == "other" || policy_str == "normal") {
        return SCHED_OTHER; // SCHED_NORMAL is the kernel-side alias for the same value (0)
    } else if (policy_str == "batch") {
        return SCHED_BATCH;
    } else if (policy_str == "idle") {
        return SCHED_IDLE;
    } else if (policy_str == "fifo") {
        return SCHED_FIFO;
    } else if (policy_str == "rr") {
        return SCHED_RR;
    } else if (policy_str == "deadline") {
        // NOTE: SCHED_DEADLINE cannot be applied via pthread_attr_setschedpolicy()
        // (used below); it requires the sched_setattr(2) syscall with a struct
        // sched_attr. Threads requesting it will currently fail at creation time.
        return SCHED_DEADLINE;
    } else if (!policy_str.empty()) {
        log_message(LOG_WARN, "task-wrapper", "Unknown scheduler policy '%s', defaulting to SCHED_OTHER", policy_str.c_str());
    }
    return SCHED_OTHER;
}

// Parses the flat JSON object sent by the execution-manager (e.g.
// {"total_ops": 16, "io_percentage": 0}) into input_t through the task's
// convert_json_to_input(), the same API used by the message queue versions.
static int parse_task_input(const std::string& inputs_json, input_t* input) {
    JsonParser* parser = json_parser_new();
    int ret = -1;

    if (json_parser_load_from_data(parser, inputs_json.c_str(), -1, NULL)) {
        JsonNode* root = json_parser_get_root(parser);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            ret = convert_json_to_input(json_node_get_object(root), input);
        } else {
            log_message(LOG_ERROR, "task-wrapper", "Task inputs are not a JSON object: %s", inputs_json.c_str());
        }
    } else {
        log_message(LOG_ERROR, "task-wrapper", "Invalid JSON in task inputs: %s", inputs_json.c_str());
    }

    g_object_unref(parser);
    return ret;
}

// Implementation of TaskExecutor service
class TaskExecutorServiceImpl final : public TaskExecutor::Service {
    Status ExecuteTask(ServerContext* context, 
                      const TaskRequest* request,
                      TaskResponse* response) override {
        
        log_message(LOG_INFO, "task-wrapper", "Received task: %s (ID: %u)",
               request->task_name().c_str(), request->task_id());

        // Convert JSON input to input_t structure
        input_t input;
        memset(&input, 0, sizeof(input));
        if (parse_task_input(request->inputs_json(), &input) != 0) {
            response->set_status("ERROR");
            response->set_error_message("Failed to parse inputs");
            return Status::OK;
        }
        
        log_message(LOG_DEBUG, "task-wrapper", "Input parsed successfully");

        // Prepare thread attributes for real-time execution
        pthread_attr_t attr;
        struct sched_param param;
        pthread_t task_thread;

        pthread_attr_init(&attr);

        // Set stack size (avoid page faults)
        pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + 0x4000);

        // Note: CPU affinity is set by the thread itself in task_main_wrapper
        // (from request->cpu_affinity())

        // Set scheduling policy based on request
        std::string policy_str = request->policy();
        int policy = policy_from_string(policy_str);

        if (pthread_attr_setschedpolicy(&attr, policy) != 0) {
            log_message(LOG_WARN, "task-wrapper", "Failed to set RT policy (need root privileges)");
        }
        
        // Set priority: only FIFO/RR use a static priority, OTHER/BATCH/IDLE accept only 0
        // (the nice value is applied by the thread itself). A rejected param would make glibc
        // copy the caller's FIFO priority and pthread_create would fail with EINVAL.
        bool is_rt = (policy == SCHED_FIFO || policy == SCHED_RR);
        param.sched_priority = is_rt ? request->priority() : 0;
        int param_ret = pthread_attr_setschedparam(&attr, &param);
        if (param_ret != 0) {
            log_message(LOG_WARN, "task-wrapper", "Invalid priority %d (ret=%d)", request->priority(), param_ret);
        }
        
        // Use explicit scheduler inheritance
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

        // Create real-time thread (using generic wrapper that pins CPU affinity)
        perf_thread_ctx_t pctx = { &input, NULL, 0, 0, 0, request->has_cpu_affinity() ? request->cpu_affinity() : -1, policy, request->priority() };
        if (pthread_create(&task_thread, &attr, task_main_wrapper, &pctx) != 0) {
            response->set_status("ERROR");
            response->set_error_message("Failed to create task thread");
            pthread_attr_destroy(&attr);
            return Status::OK;
        }

        pthread_attr_destroy(&attr);

        // Wait for task completion
        pthread_join(task_thread, NULL);

        log_message(LOG_INFO, "task-wrapper", "Task completed");

        // Convert output to JSON
        gchar* result_json = pctx.output ? convert_output_to_json(pctx.output) : NULL;
        g_free(pctx.output);
        if (!result_json) {
            response->set_status("ERROR");
            response->set_error_message("Failed to serialize output");
            return Status::OK;
        }

        // Set response
        response->set_task_id(request->task_id());
        response->set_status("COMPLETED");
        response->set_result_json(result_json);

        /* Performance data (for the performance) */
        response->set_end_time_request(pctx.end_time_request);
        response->set_start_time_result(pctx.start_time_result);
        response->set_core_id(pctx.core_id);
        /* -------------------------------------------------- */

        log_message(LOG_DEBUG, "task-wrapper", "Result: %s", result_json);

        // Cleanup
        g_free(result_json);

        return Status::OK;
    }

    // Asynchronous task execution with streaming responses
    Status ExecuteTaskAsync(ServerContext* context,
                           const TaskRequest* request,
                           ServerWriter<TaskResponse>* writer) override {

        log_message(LOG_INFO, "task-wrapper", "Async request received for task: %s (ID: %u)",
               request->task_name().c_str(), request->task_id());
        
        // Convert JSON input to input_t structure
        input_t input;
        memset(&input, 0, sizeof(input));
        if (parse_task_input(request->inputs_json(), &input) != 0) {
            TaskResponse error_response;
            error_response.set_task_id(request->task_id());
            error_response.set_status("ERROR");
            error_response.set_error_message("Failed to parse inputs");
            writer->Write(error_response);
            return Status::OK;
        }
        
        log_message(LOG_DEBUG, "task-wrapper", "Async input parsed successfully");

        // Prepare thread attributes for real-time execution
        pthread_attr_t attr;
        struct sched_param param;
        pthread_t task_thread;
        bool use_rt = true;

        pthread_attr_init(&attr);

        // 1. Set stack size (matching message queue order)
        int stack_ret = pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + 0x4000);
        log_message(LOG_DEBUG, "task-wrapper", "Set stack size: %s", stack_ret == 0 ? "OK" : "FAILED");

        // Note: CPU affinity will be set AFTER pthread_create
        // Setting it in attributes with pthread_attr_setaffinity_np causes errno=22 with SCHED_FIFO

        // 3. Set scheduling policy
        memset(&param, 0, sizeof(param));

        std::string policy_str = request->policy();
        int priority_val = request->priority();

        log_message(LOG_DEBUG, "task-wrapper", "Requested policy='%s', priority=%d", policy_str.c_str(), priority_val);

        int policy = policy_from_string(policy_str);

        // The explicit policy is applied also for OTHER/BATCH/IDLE: otherwise the thread would
        // inherit the SCHED_FIFO 85 of the gRPC server thread.
        bool is_rt = (policy == SCHED_FIFO || policy == SCHED_RR);
        {
            log_message(LOG_DEBUG, "task-wrapper", "Setting policy '%s' with priority %d", policy_str.c_str(), priority_val);

            int policy_ret = pthread_attr_setschedpolicy(&attr, policy);
            log_message(LOG_DEBUG, "task-wrapper", "Set policy '%s': %s (ret=%d)", policy_str.c_str(),
                   policy_ret == 0 ? "OK" : "FAILED", policy_ret);

            if (policy_ret != 0) {
                log_message(LOG_WARN, "task-wrapper", "Failed to set RT policy, using SCHED_OTHER");
                policy = SCHED_OTHER;
                use_rt = false;
            } else {
                // 4. Set priority: only FIFO/RR use a static priority, OTHER/BATCH/IDLE accept
                // only 0 (the nice value is applied by the thread itself)
                param.sched_priority = is_rt ? priority_val : 0;
                int param_ret = pthread_attr_setschedparam(&attr, &param);
                log_message(LOG_DEBUG, "task-wrapper", "Set priority %d: %s (ret=%d)",
                       priority_val, param_ret == 0 ? "OK" : "FAILED", param_ret);

                if (param_ret != 0) {
                    log_message(LOG_WARN, "task-wrapper", "Invalid priority %d for policy '%s' (ret=%d)", priority_val, policy_str.c_str(), param_ret);
                    use_rt = false;
                } else {
                    // 5. Use PTHREAD_EXPLICIT_SCHED (LAST - matching message queue)
                    int sched_ret = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
                    log_message(LOG_DEBUG, "task-wrapper", "Set inherit sched: %s (ret=%d)",
                           sched_ret == 0 ? "OK" : "FAILED", sched_ret);

                    if (sched_ret == 0) {
                        log_message(LOG_DEBUG, "task-wrapper", "RT attributes configured successfully");
                    } else {
                        use_rt = false;
                    }
                }
            }
 }
        
        // Create thread with RT attributes (or fallback to default if RT failed)
        perf_thread_ctx_t pctx = { &input, NULL, 0, 0, 0, request->has_cpu_affinity() ? request->cpu_affinity() : -1, policy, priority_val };
        int ret = pthread_create(&task_thread, &attr, task_main_wrapper, &pctx);
        pthread_attr_destroy(&attr);

        if (ret != 0) {
            // RT failed, try again with no attributes
            log_message(LOG_WARN, "task-wrapper", "Thread creation with RT failed (errno=%d), trying without RT...", ret);
            use_rt = false;  // updates the flag
            ret = pthread_create(&task_thread, NULL, task_main_wrapper, &pctx);
        }
        
        if (ret != 0) {
            TaskResponse error_response;
            error_response.set_task_id(request->task_id());
            error_response.set_status("ERROR");
            error_response.set_error_message("Failed to create task thread");
            writer->Write(error_response);
            return Status::OK;
        }
        
        log_message(LOG_DEBUG, "task-wrapper", "Thread created successfully (RT=%s), waiting for task completion...", use_rt ? "yes" : "no");
        
        // Note: CPU affinity is set by the thread itself in task_main_wrapper

        // Wait for task completion (blocking join)
        pthread_join(task_thread, NULL);

        // Check if cancelled during execution
        bool task_cancelled = context->IsCancelled();
        
        // If task was cancelled, cleanup and return
        if (task_cancelled) {
            g_free(pctx.output);
            return Status::OK;
        }

        // Convert output to JSON
        gchar* result_json = pctx.output ? convert_output_to_json(pctx.output) : NULL;
        g_free(pctx.output);
        if (!result_json) {
            TaskResponse error_response;
            error_response.set_task_id(request->task_id());
            error_response.set_status("ERROR");
            error_response.set_error_message("Failed to serialize output");
            writer->Write(error_response);
            return Status::OK;
        }
        
        // SEND RESULT - Task completed
        TaskResponse result_response;
        result_response.set_task_id(request->task_id());
        result_response.set_status("COMPLETED");
        result_response.set_result_json(result_json);

        /* Performance data (for the performance) */
        result_response.set_end_time_request(pctx.end_time_request);
        result_response.set_start_time_result(pctx.start_time_result);
        result_response.set_core_id(pctx.core_id);
        /* -------------------------------------------------- */

        writer->Write(result_response);

        log_message(LOG_DEBUG, "task-wrapper", "Async result sent: %s", result_json);

        // Cleanup
        g_free(result_json);
        
        return Status::OK;
    }
};

void RunServer(const std::string& server_address) {
    TaskExecutorServiceImpl service;
    
    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    
    // ============================================
    // PERFORMANCE OPTIMIZATIONS
    // ============================================
    
    // Increase thread pool for handling concurrent requests
    // Default is usually 2, we increase to handle multiple tasks simultaneously
    builder.SetSyncServerOption(ServerBuilder::SyncServerOption::NUM_CQS, 4);
    builder.SetSyncServerOption(ServerBuilder::SyncServerOption::MIN_POLLERS, 2);
    builder.SetSyncServerOption(ServerBuilder::SyncServerOption::MAX_POLLERS, 8);
    
    // Increase max concurrent streams per connection
    builder.AddChannelArgument(GRPC_ARG_MAX_CONCURRENT_STREAMS, 100);
    
    // Enable keepalive to maintain connections
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, 10000);  // 10 seconds
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 5000);  // 5 seconds
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    
    // Increase message size limits (if needed for large payloads)
    builder.SetMaxReceiveMessageSize(4 * 1024 * 1024);  // 4MB
    builder.SetMaxSendMessageSize(4 * 1024 * 1024);     // 4MB
    
    // Optimize for low latency
    builder.AddChannelArgument(GRPC_ARG_HTTP2_BDP_PROBE, 0);  // Disable bandwidth probing
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 5000);
    
    std::unique_ptr<Server> server(builder.BuildAndStart());
    log_message(LOG_INFO, "task-wrapper", "Listening on %s", server_address.c_str());
    log_message(LOG_DEBUG, "task-wrapper", "Performance optimizations enabled: 2-8 pollers, 4 completion queues, "
                "max concurrent streams 100, keepalive 10s interval, low latency optimizations active");

    server->Wait();
}

int main(int argc, char** argv) {
    // Memory locking for real-time determinism
    // NOTE: use only MCL_CURRENT (not MCL_FUTURE) to avoid forcing residency of
    // all future pages allocated by gRPC/protobuf. This drastically reduces the
    // RSS observed by `docker stats` while still locking the pages already mapped
    // at startup (code, .so files, static data).
    if (mlockall(MCL_CURRENT) == -1) {
        log_message(LOG_WARN, "task-wrapper", "mlockall failed (need root privileges)");
    }

    // Set Real-Time Scheduling: SCHED_FIFO with priority 85
    struct sched_param rt_param;
    rt_param.sched_priority = 85;
    if (sched_setscheduler(0, SCHED_FIFO, &rt_param) != 0) {
        log_message(LOG_WARN, "task-wrapper", "Failed to set RT scheduling (need root or CAP_SYS_NICE)");
        log_message(LOG_WARN, "task-wrapper", "Running with SCHED_OTHER");
    } else {
        log_message(LOG_INFO, "task-wrapper", "Running with SCHED_FIFO priority 85");
    }

    // Get task name and port from environment
    const char* task_name = getenv("TASK_NAME");
    const char* grpc_port = getenv("GRPC_PORT");

    std::string server_address = "0.0.0.0:";
    server_address += grpc_port ? grpc_port : "50051";  // Default to 50051 if not set

    // Pin gRPC server and its internal threads to CPU 4.
    // Task execution threads (task_main_wrapper) are pinned to CPU 5.
    cpu_set_t server_cpuset;
    CPU_ZERO(&server_cpuset);
    CPU_SET(4, &server_cpuset);
    if (sched_setaffinity(0, sizeof(cpu_set_t), &server_cpuset) != 0) {
        log_message(LOG_WARN, "task-wrapper", "Failed to set CPU affinity to CPU 4");
    } else {
        log_message(LOG_INFO, "task-wrapper", "gRPC server pinned to CPU 4");
    }

    if (task_name) {
        log_message(LOG_INFO, "task-wrapper", "Starting server for task: %s", task_name);
    }
    log_message(LOG_INFO, "task-wrapper", "Listening on %s", server_address.c_str());

    RunServer(server_address);

    return 0;
}
