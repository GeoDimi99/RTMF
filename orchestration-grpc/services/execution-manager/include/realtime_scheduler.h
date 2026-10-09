#ifndef REALTIME_SCHEDULER_H
#define REALTIME_SCHEDULER_H

#include <hiredis/hiredis.h>

#ifdef __cplusplus
extern "C" {
#endif

// Execute a schedule using event-driven real-time scheduler
// Parameters:
//   redis: Redis connection to read tasks
//   num_tasks: Number of tasks to execute
//   iterations: How many times to repeat the full schedule (tasks are loaded once, timers reset each run)
// This function uses epoll + timerfd for precise timing and timeout management
void execute_schedule_with_event_loop(redisContext *redis, int num_tasks, int iterations);

#ifdef __cplusplus
}
#endif

#endif // REALTIME_SCHEDULER_H
