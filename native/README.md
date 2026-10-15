<div align="center">

# ⚡ RealTime Native Execution Manager

</div>

---

Baseline of RTMF without containers: a single host process (the **Execution Manager**) runs the
whole schedule, launching every task as a real-time thread. There is no manifest, no Redis and
no Docker: the schedule is defined in C in `services/execution-manager/src/main.c`.

## 📁 Repository layout

```
.
├── common/
│   ├── include/
│   │   ├── logger.h                 # Logging + [PERF] lines
│   │   └── task.h                   # Shared task descriptor (policy, priority, deps)
│   └── src/
│       ├── logger.c
│       └── task.c
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
├── tools/                           # Performance report scripts (used by start-test.sh)
├── results/                         # One folder per test run (log, perf.csv, stats.csv, report.pdf)
├── docs/
│   └── generate_context.py          # Dev helper: dump source tree to a single file
├── start-test.sh                    # End-to-end build, run & benchmark
└── README.md
```

## 📋 Prerequisites

- **Linux host with `PREEMPT_RT` kernel** (`SCHED_FIFO`, `mlockall`, `SYS_NICE` and POSIX timers are required for deterministic scheduling).
- **CMake ≥ 3.20**, a C compiler, `pkg-config` and GLib (`libglib2.0-dev`, which also provides `gio-2.0`).
- **Python 3** for the performance report (`start-test.sh` installs `matplotlib`, `numpy` and
  `PyYAML` into `tools/.venv` when they are missing).
- Root privileges (or `CAP_SYS_NICE` + `CAP_IPC_LOCK`) for `SCHED_FIFO` and `mlockall`.

---

## 🚀 Automated start

```bash
git clone https://github.com/GeoDimi99/RTMF.git
cd RTMF/native
sudo ./start-test.sh [test-name]
```

`start-test.sh` configures and builds the Execution Manager with CMake, runs it, saves its
output in `results/<test-name>/execution-manager.log` (a random `results/test_<id>/` when no name
is given) and generates the performance report there. Ctrl+C stops the run early; the report is
built from the data collected so far.

| Variable | Meaning |
|---|---|
| `CMAKE=<path>` | cmake binary to use (default: auto-detected). |
| `FORCE=1` | Overwrite `results/<test-name>/` if it already exists. |

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

## ⚙️ Runtime behaviour

- The Execution Manager pins itself to CPU 4 (`DEFAULT_EM_CPU_AFFINITY`), locks its memory and
  runs with `SCHED_FIFO` priority 95.
- The schedule is replayed 100 times (`num_iter` in `main.c`); every task emits a `[PERF]` line
  that `tools/perf_report.py` turns into `perf.csv`, `stats.csv` and `report.pdf`.
- The active schedule is the six-task benchmark used by the containerised implementations
  (`stress_task_1..6`, `SCHED_FIFO`, CPU 5); other test scenarios are kept commented out in `main.c`.

### Adding a task

Edit the schedule in `services/execution-manager/src/main.c`:

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

`func_input` is a `g_new0`-allocated `input_t` (see `include/app_task.h`).

---


<div align="center">

</div>
