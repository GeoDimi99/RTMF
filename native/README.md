<div align="center">

# ⚡ RealTime Native Execution Manager

</div>

---

## 📁 Repository layout

```
.
├── common/
│   ├── include/task.h               # Shared task descriptor (policy, priority, deps)
│   └── src/task.c
├── services/
│   └── execution-manager/           # The Execution Manager service
│       ├── CMakeLists.txt
│       ├── include/
│       │   ├── app_task.h           # Workload (CPU + I/O) interface
│       │   ├── execution_manager.h  # EM types, event handlers, task wrapper
│       │   └── schedule.h           # Schedule / timeline data structures
│       └── src/
│           ├── app_task.c           # Synthetic CPU/IO workload
│           ├── execution_manager.c  # Activation/expiration handlers, threads
│           ├── main.c               # Entry point + schedule definition
│           └── schedule.c           # Schedule construction & lifecycle
├── docs/
│   └── generate_context.py          # Dev helper: dump source tree to a single file
├── start-test.sh                    # End-to-end build & run helper
└── README.md
```

## 📋 Prerequisites

- **Linux host with `PREEMPT_RT` kernel** (`SCHED_FIFO`, `mlockall`, `SYS_NICE` and POSIX timers are required for deterministic scheduling).




---

## 🛠️ Manual workflow

Follow these steps to build and start the system step by step:

```bash
# 1. Configure the build
cmake -S services/execution-manager -B services/execution-manager/build

# 2. Compile the Execution Manager
cmake --build services/execution-manager/build

# 3. Run it (sudo required for SCHED_FIFO + mlockall)
sudo ./services/execution-manager/build/execution-manager

# 4. Clean the build tree
rm -rf services/execution-manager/build
```

> Add `--clean-first` to step 2, or simply remove the `build/` directory, to
> force a clean rebuild.

---

### Adding a task

```c
schedule_add_task(
    sched,
    /* id            */ 1,
    /* name          */ "stress_task_1",
    /* exec function */ task_main,
    /* policy        */ SCHED_FIFO,
    /* priority      */ 50,
    /* cpu_affinity  */ 5,
    /* repetition    */ 1,
    /* depends_on    */ NULL,
    /* start_time_ms */ 50,
    /* end_time_ms   */ 650,
    /* input         */ func_input
);
```



`


---


<div align="center">

</div>
