#include <iostream>
#include <memory>
#include <string>
#include <chrono>
#include <map>
#include <mutex>
#include <grpcpp/grpcpp.h>
#include "proto/task_service.grpc.pb.h"
#include "grpc_client.h"
extern "C" {
#include "logger.h"
}

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using taskservice::TaskExecutor;
using taskservice::TaskRequest;
using taskservice::TaskResponse;

// ============================================
// CHANNEL POOL - Reuse gRPC channels
// ============================================
static std::map<std::string, std::shared_ptr<Channel>> g_channel_pool;
static std::mutex g_channel_pool_mutex;

// Get or create a channel for the given address
static std::shared_ptr<Channel> get_or_create_channel(const char* address) {
    std::lock_guard<std::mutex> lock(g_channel_pool_mutex);
    
    std::string addr_str(address);
    auto it = g_channel_pool.find(addr_str);
    
    if (it != g_channel_pool.end()) {
        // Check channel health before reusing.
        // true = try_to_connect: if the channel is IDLE, immediately start
        // reconnecting so the upcoming RPC finds the channel closer to READY.
        grpc_connectivity_state state = it->second->GetState(true);
        if (state == GRPC_CHANNEL_TRANSIENT_FAILURE || state == GRPC_CHANNEL_SHUTDOWN) {
            log_message(LOG_WARN, "execution-manager", "Channel pool: channel for %s in bad state (%d), recreating", address, (int)state);
            g_channel_pool.erase(it);
        } else {
            return it->second;
        }
    }

    // Create new channel with optimized settings
    log_message(LOG_INFO, "execution-manager", "Channel pool: creating new channel for %s", address);
    
    grpc::ChannelArguments args;
    // Enable keepalive to maintain connection
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 10000);  // 10 seconds
    args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10000);  // 10 seconds
    args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);  // Keep TCP connection alive during idle gaps between iterations
    // Prevent channel from transitioning READY→IDLE during long inter-task gaps.
    // Default idle timeout in gRPC Core is ~30s; tasks spaced >30s apart cause
    // state=0 (IDLE) on reuse, forcing a reconnect (+1-2ms latency).
    args.SetInt(GRPC_ARG_CLIENT_IDLE_TIMEOUT_MS, INT_MAX);
    // Allow the server to send keepalive pings to this client as frequently as
    // every 5 s (server is configured with KEEPALIVE_TIME_MS=10000).  Without
    // this the gRPC-core default (300 s) causes the client to treat the server's
    // 10-s pings as a protocol violation, responding with GOAWAY and dropping
    // the connection — which is the root cause of state=0 (IDLE) on reuse.
    args.SetInt(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 5000);
    // Increase max concurrent streams
    args.SetInt(GRPC_ARG_MAX_CONCURRENT_STREAMS, 100);
    // Optimize for low latency
    args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 0);
    
    std::shared_ptr<Channel> channel = grpc::CreateCustomChannel(
        address,
        grpc::InsecureChannelCredentials(),
        args
    );
    
    g_channel_pool[addr_str] = channel;
    return channel;
}

// C interface for calling from C code
extern "C" {
    
// Execute a task via gRPC
// Returns 0 on success, -1 on error
int grpc_execute_task(const char* task_service_address,
                      unsigned int task_id,
                      const char* task_name,
                      const char* inputs_json,
                      int priority,
                      const char* policy,
                      int cpu_affinity,
                      char* result_json_out,
                      int result_json_max_len) {
    
    // Get or create a channel from the pool (reuses existing connections)
    std::shared_ptr<Channel> channel = get_or_create_channel(task_service_address);
    
    // Create stub
    std::unique_ptr<TaskExecutor::Stub> stub = TaskExecutor::NewStub(channel);
    
    // Prepare request
    TaskRequest request;
    request.set_task_id(task_id);
    request.set_task_name(task_name);
    request.set_inputs_json(inputs_json);
    request.set_priority(priority);
    request.set_policy(policy);
    if (cpu_affinity >= 0) request.set_cpu_affinity(cpu_affinity);
    
    // Container for response
    TaskResponse response;
    ClientContext context;
    
    // Set deadline (5 seconds from now)
    std::chrono::system_clock::time_point deadline =
        std::chrono::system_clock::now() + std::chrono::seconds(5);
    context.set_deadline(deadline);
    // Don't wait for ready - channel should already be connected from pool
    // context.set_wait_for_ready(true);
    
    log_message(LOG_INFO, "execution-manager", "Calling task '%s' at %s", task_name, task_service_address);
    log_message(LOG_DEBUG, "execution-manager", "Inputs: %s", inputs_json);

    // Make the RPC call
    Status status = stub->ExecuteTask(&context, request, &response);

    if (!status.ok()) {
        log_message(LOG_ERROR, "execution-manager", "RPC failed: %s", status.error_message().c_str());
        return -1;
    }

    log_message(LOG_INFO, "execution-manager", "Task completed with status: %s", response.status().c_str());
    log_message(LOG_DEBUG, "execution-manager", "Result: %s", response.result_json().c_str());

    // Copy result to output buffer
    if (result_json_out && result_json_max_len > 0) {
        snprintf(result_json_out, result_json_max_len, "%s", response.result_json().c_str());
    }

    if (response.status() == "COMPLETED") {
        return 0;
    } else {
        log_message(LOG_ERROR, "execution-manager", "Task failed: %s", response.error_message().c_str());
        return -1;
    }
}

// Execute a task via gRPC asynchronously with streaming responses
// Uses async CompletionQueue API: cq.Next() blocks in epoll_wait properly,
// eliminating the busy-poll tight loop of the synchronous ClientReader.
int grpc_execute_task_async(const char* task_service_address,
                            unsigned int task_id,
                            const char* task_name,
                            const char* inputs_json,
                            int priority,
                            const char* policy,
                            int cpu_affinity,
                            grpc_task_callback_t callback,
                            void* user_data) {

    const int MAX_ATTEMPTS = 2;
    for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {

        std::shared_ptr<Channel> channel = get_or_create_channel(task_service_address);
        std::unique_ptr<TaskExecutor::Stub> stub = TaskExecutor::NewStub(channel);

        TaskRequest request;
        request.set_task_id(task_id);
        request.set_task_name(task_name);
        request.set_inputs_json(inputs_json);
        request.set_priority(priority);
        request.set_policy(policy);
        if (cpu_affinity >= 0) request.set_cpu_affinity(cpu_affinity);

        ClientContext context;
        std::chrono::system_clock::time_point deadline =
            std::chrono::system_clock::now() + std::chrono::seconds(60);
        context.set_deadline(deadline);
        context.set_wait_for_ready(true);

        if (attempt > 1) {
            log_message(LOG_WARN, "execution-manager", "Async retry attempt %d for task '%s' at %s", attempt, task_name, task_service_address);
        }
        log_message(LOG_INFO, "execution-manager", "Async calling task '%s' at %s", task_name, task_service_address);
        log_message(LOG_DEBUG, "execution-manager", "Async inputs: %s", inputs_json);

        // Async CompletionQueue: cq.Next() uses blocking epoll_wait — no busy-polling
        grpc::CompletionQueue cq;
        void* const TAG_INIT   = (void*)1;
        void* const TAG_READ   = (void*)2;
        void* const TAG_FINISH = (void*)3;

        // Start the async streaming call; TAG_INIT delivered when ready to send
        std::unique_ptr<grpc::ClientAsyncReader<TaskResponse>> reader =
            stub->AsyncExecuteTaskAsync(&context, request, &cq, TAG_INIT);

        void* tag;
        bool ok;
        int response_count = 0;

        // Wait for call initialisation
        bool init_ok = cq.Next(&tag, &ok) && ok;

        if (init_ok) {
            // Queue first read
            TaskResponse response;
            reader->Read(&response, TAG_READ);

            while (cq.Next(&tag, &ok)) {
                if (tag != TAG_READ) continue;
                if (!ok) break;  // stream ended normally

                response_count++;
                std::string status_str  = response.status();
                std::string result_json = response.result_json();
                std::string error_msg   = response.error_message();

                log_message(LOG_DEBUG, "execution-manager", "Async response #%d - status: %s", response_count, status_str.c_str());

                if (callback) {
                    callback(response.task_id(), status_str.c_str(), result_json.c_str(),
                             error_msg.c_str(), response.end_time_request(),
                             response.start_time_result(), response.core_id(), user_data);
                }

                if (status_str == "ERROR") break;

                // Queue next read
                reader->Read(&response, TAG_READ);
            }
        }

        // Always Finish to obtain final RPC status and release server resources
        grpc::Status grpc_status;
        reader->Finish(&grpc_status, TAG_FINISH);

        // Drain until TAG_FINISH (discard any late TAG_READ events)
        while (cq.Next(&tag, &ok)) {
            if (tag == TAG_FINISH) break;
        }

        cq.Shutdown();
        while (cq.Next(&tag, &ok)) {}  // drain residual events

        if (!init_ok || !grpc_status.ok()) {
            log_message(LOG_ERROR, "execution-manager", "Async RPC failed: %s",
                    grpc_status.error_message().c_str());
            {
                std::lock_guard<std::mutex> lock(g_channel_pool_mutex);
                g_channel_pool.erase(std::string(task_service_address));
                log_message(LOG_WARN, "execution-manager", "Channel pool: evicted broken channel for %s", task_service_address);
            }
            if (response_count == 0 && attempt < MAX_ATTEMPTS) {
                log_message(LOG_WARN, "execution-manager", "Broken connection, retrying with fresh channel...");
                continue;
            }
            if (response_count == 0 && callback) {
                callback(task_id, "ERROR", "", grpc_status.error_message().c_str(), 0, 0, 0, user_data);
            }
            return -1;
        }

        log_message(LOG_INFO, "execution-manager", "Async stream completed, received %d responses", response_count);
        return 0;
    }

    return -1;
}

// Internal structure for cancellable call (async CQ version)
struct GrpcCallContext {
    std::shared_ptr<Channel> channel;
    std::unique_ptr<TaskExecutor::Stub> stub;
    ClientContext context;
    grpc::CompletionQueue cq;
    std::unique_ptr<grpc::ClientAsyncReader<TaskResponse>> reader;
    bool cancelled;

    GrpcCallContext() : cancelled(false) {}
};

// Execute a task via gRPC with cancellation support
// Uses async CompletionQueue: TryCancel() causes cq.Next() to return ok=false,
// breaking the read loop cleanly without busy-polling.
grpc_call_handle_t grpc_execute_task_async_cancellable(
                            const char* task_service_address,
                            unsigned int task_id,
                            const char* task_name,
                            const char* inputs_json,
                            int priority,
                            const char* policy,
                            int cpu_affinity,
                            grpc_task_callback_t callback,
                            void* user_data) {

    GrpcCallContext* ctx = new GrpcCallContext();

    ctx->channel = get_or_create_channel(task_service_address);
    ctx->stub    = TaskExecutor::NewStub(ctx->channel);

    TaskRequest request;
    request.set_task_id(task_id);
    request.set_task_name(task_name);
    request.set_inputs_json(inputs_json);
    request.set_priority(priority);
    request.set_policy(policy);
    if (cpu_affinity >= 0) request.set_cpu_affinity(cpu_affinity);

    std::chrono::system_clock::time_point deadline =
        std::chrono::system_clock::now() + std::chrono::seconds(60);
    ctx->context.set_deadline(deadline);
    ctx->context.set_wait_for_ready(true);

    log_message(LOG_INFO, "execution-manager", "Cancellable call for task '%s' at %s", task_name, task_service_address);

    void* const TAG_INIT   = (void*)1;
    void* const TAG_READ   = (void*)2;
    void* const TAG_FINISH = (void*)3;

    ctx->reader = ctx->stub->AsyncExecuteTaskAsync(&ctx->context, request, &ctx->cq, TAG_INIT);

    void* tag;
    bool ok;
    int response_count = 0;

    bool init_ok = ctx->cq.Next(&tag, &ok) && ok;

    if (init_ok) {
        TaskResponse response;
        ctx->reader->Read(&response, TAG_READ);

        while (ctx->cq.Next(&tag, &ok)) {
            if (tag != TAG_READ) continue;
            if (!ok) break;  // stream ended or cancelled

            response_count++;
            std::string status_str  = response.status();
            std::string result_json = response.result_json();
            std::string error_msg   = response.error_message();

            log_message(LOG_DEBUG, "execution-manager", "Cancellable response #%d - status: %s", response_count, status_str.c_str());

            if (ctx->cancelled) {
                log_message(LOG_WARN, "execution-manager", "Cancellable call was cancelled");
                break;
            }

            if (callback) {
                callback(response.task_id(), status_str.c_str(), result_json.c_str(),
                         error_msg.c_str(), response.end_time_request(),
                         response.start_time_result(), response.core_id(), user_data);
            }

            if (status_str == "ERROR" || status_str == "CANCELLED") break;

            ctx->reader->Read(&response, TAG_READ);
        }
    }

    // Finish and drain
    grpc::Status grpc_status;
    ctx->reader->Finish(&grpc_status, TAG_FINISH);
    while (ctx->cq.Next(&tag, &ok)) {
        if (tag == TAG_FINISH) break;
    }
    ctx->cq.Shutdown();
    while (ctx->cq.Next(&tag, &ok)) {}

    if (!grpc_status.ok() && !ctx->cancelled) {
        log_message(LOG_ERROR, "execution-manager", "Cancellable RPC failed: %s",
                grpc_status.error_message().c_str());
    }

    log_message(LOG_INFO, "execution-manager", "Cancellable stream completed, received %d responses", response_count);

    delete ctx;
    return nullptr;
}

// Cancel an ongoing gRPC task call
// TryCancel() causes any pending cq.Next() to return ok=false, cleanly unwinding the read loop.
void grpc_cancel_task(grpc_call_handle_t handle) {
    if (!handle) {
        log_message(LOG_WARN, "execution-manager", "Cannot cancel NULL handle");
        return;
    }

    GrpcCallContext* ctx = static_cast<GrpcCallContext*>(handle);

    log_message(LOG_INFO, "execution-manager", "Cancelling gRPC call...");
    ctx->cancelled = true;
    ctx->context.TryCancel();
}

// Pre-warm a channel so it is READY before the next task fires.
// Call this during idle time between iterations to eliminate the
// IDLE→CONNECTING→READY transitions that would otherwise be paid inline
// when the RPC is sent.
void grpc_warmup_channel(const char* address) {
    std::shared_ptr<Channel> channel = get_or_create_channel(address);
    grpc_connectivity_state state = channel->GetState(true);
    if (state != GRPC_CHANNEL_READY) {
        auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(2000);
        channel->WaitForConnected(deadline);
    }
}

} // extern "C"
