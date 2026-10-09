#ifndef GRPC_CLIENT_H
#define GRPC_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

// Callback type for async task responses
// status: "STARTED", "COMPLETED", or "ERROR"
// result_json: JSON result (empty for STARTED)
// error_message: Error message (empty if no error)
// end_time_request: server timestamp (ns, CLOCK_MONOTONIC) when the task thread began execution
//                    (for the performance; only set when status == "COMPLETED", 0 otherwise)
// start_time_result: server timestamp (ns, CLOCK_MONOTONIC) when the task thread finished execution
//                    (for the performance; only set when status == "COMPLETED", 0 otherwise)
// core_id: CPU core the task thread actually ran on
//                    (for the performance; only set when status == "COMPLETED", 0 otherwise)
// user_data: User-provided context pointer
typedef void (*grpc_task_callback_t)(
    unsigned int task_id,
    const char* status,
    const char* result_json,
    const char* error_message,
    long end_time_request,
    long start_time_result,
    int core_id,
    void* user_data
);

// Execute a task via gRPC (synchronous - blocks until completion)
// Parameters:
//   task_service_address: Address of the task service (e.g., "task-service-sum:50051")
//   task_id: Unique task ID
//   task_name: Name of the task
//   inputs_json: JSON string with task inputs
//   priority: Task priority
//   policy: Scheduling policy (e.g., "fifo")
//   cpu_affinity: CPU core to pin the task thread to (-1 = no explicit pinning)
//   result_json_out: Buffer to store result JSON (can be NULL)
//   result_json_max_len: Maximum length of result buffer
// Returns: 0 on success, -1 on error
int grpc_execute_task(const char* task_service_address,
                      unsigned int task_id,
                      const char* task_name,
                      const char* inputs_json,
                      int priority,
                      const char* policy,
                      int cpu_affinity,
                      char* result_json_out,
                      int result_json_max_len);

// Execute a task via gRPC (asynchronous with streaming)
// Callback is called twice:
//   1. When task starts (status="STARTED")
//   2. When task completes (status="COMPLETED" or "ERROR")
// Parameters:
//   task_service_address: Address of the task service
//   task_id: Unique task ID
//   task_name: Name of the task
//   inputs_json: JSON string with task inputs
//   priority: Task priority
//   policy: Scheduling policy (e.g., "fifo")
//   cpu_affinity: CPU core to pin the task thread to (-1 = no explicit pinning)
//   callback: Function called for each response
//   user_data: User context passed to callback
// Returns: 0 on success, -1 on error
int grpc_execute_task_async(const char* task_service_address,
                            unsigned int task_id,
                            const char* task_name,
                            const char* inputs_json,
                            int priority,
                            const char* policy,
                            int cpu_affinity,
                            grpc_task_callback_t callback,
                            void* user_data);

// Opaque handle for cancellable gRPC calls
typedef void* grpc_call_handle_t;

// Execute a task via gRPC (asynchronous with cancellation support)
// Returns a handle that can be used to cancel the call
// Parameters: same as grpc_execute_task_async
// Returns: Handle on success, NULL on error
grpc_call_handle_t grpc_execute_task_async_cancellable(
                            const char* task_service_address,
                            unsigned int task_id,
                            const char* task_name,
                            const char* inputs_json,
                            int priority,
                            const char* policy,
                            int cpu_affinity,
                            grpc_task_callback_t callback,
                            void* user_data);

// Cancel an ongoing gRPC task call
// Parameters:
//   handle: Handle returned by grpc_execute_task_async_cancellable
void grpc_cancel_task(grpc_call_handle_t handle);

// Pre-warm a channel so it is READY before the next RPC.
// Call during idle time between iterations to prevent inline
// IDLE→CONNECTING→READY latency at task launch time.
// Parameters:
//   address: Address of the task service (e.g., "localhost:50051")
void grpc_warmup_channel(const char* address);

#ifdef __cplusplus
}
#endif

#endif // GRPC_CLIENT_H
