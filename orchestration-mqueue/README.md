<div align="center">

# ⚡ Orchestration Microservices mqueue

</div>

---

Orchestration pattern of RTMF: a central **execution-manager** reads the schedule from Redis
and dispatches each task to its **task-wrapper** container through POSIX message queues. The
**deploy-manager** starts one task-wrapper container per task image and loads the schedule into
Redis.

## 📁 Repository layout

```
.
├── common/                   # Shared C headers/sources (logger, task struct, IPC message format)
│   ├── include/
│   │   ├── logger.h
│   │   ├── task.h
│   │   └── task_ipc.h
│   └── src/
├── sdk/
│   └── image-builder/        # Python CLI that builds per-task Docker images
├── services/
│   ├── deploy-manager/       # Python: spawns task-wrappers + writes Redis
│   ├── execution-manager/    # C/GLib: reads schedule, dispatches via mqueue
│   └── task-wrapper/         # C/GLib: mqueue server base image + task harness
├── tests/
│   └── test_0_code/          # Active manifest + sample task sources
│       ├── manifest.yaml
│       └── stress_task_*/    # One folder per task (each with app_task.h/.c)
├── tools/                    # Performance report scripts (used by start-test.sh)
├── results/                  # One folder per test run (logs, perf.csv, stats.csv, report.pdf)
├── docs/                     # Cheat-sheet & repo dumper
├── docker-compose.yml        # redis + deploy-manager + execution-manager
├── start-test.sh             # End-to-end build, run & benchmark
└── README.md
```

---

## ⚙️ How it works

1. **deploy-manager** parses `manifest.yaml` and, for every entry of `images.tasks`, starts a
   task-wrapper container named after the image alias (`ipc_mode=host`,
   `cap_add=[SYS_NICE, IPC_LOCK]`, `rtprio=99`, `memlock=unlimited`, `cpuset 0-7`,
   env `TASK_NAME` / `TASK_QUEUE_NAME` = alias).
2. It then writes the schedule into Redis:
   - hash `schedule`: `name`, `version`, `description`, `iterations`, `length`
   - hash `scheduletask:<i>` (i = 1..length): `id`, `image`, `start`, `deadline`, `cpu_affinity`,
     `policy`, `priority`, `depends_on`, `inputs`, `outputs`
3. **execution-manager** (started by `docker compose`, `ipc: host`, pinned to CPU 4) waits until the
   `schedule` key exists in Redis, builds the timeline and runs it `iterations` times.
4. Message queues: the execution-manager listens on `/execution_manager_q`; every task-wrapper
   listens on `/<alias>_q` and waits until `/execution_manager_q` exists. At each activation the
   execution-manager sends a request to `/<alias>_q`; the task-wrapper runs `task_main()` in a
   thread with the requested policy/priority/affinity and sends the result back.

Several schedule tasks may use the same image: they are all served by the same container.

---

## 📋 Prerequisites

- **Linux host with `PREEMPT_RT` kernel** (POSIX `mqueue`, `SCHED_FIFO` and `SYS_NICE` are Linux-only).
- **Docker Engine ≥ 20.10** with Docker Compose v2 (the `docker compose ...` syntax).
- **Python ≥ 3.10** and access to `/var/run/docker.sock`.
- Python packages used by the image-builder (declared in `sdk/image-builder/requirements.txt`):

  ```bash
  pip install -r sdk/image-builder/requirements.txt
  ```

> **Tip.** If you don't want to install the Python deps system-wide, use a virtualenv: `python3 -m venv .venv && source .venv/bin/activate && pip install -r sdk/image-builder/requirements.txt`.

> **POSIX mqueues.** If a previous run left stale queues behind, wipe them with `sudo rm -f /dev/mqueue/*` before relaunching (`start-test.sh` does not do it for you).

---

## 🚀 Automated start

```bash
git clone https://github.com/GeoDimi99/RTMF.git
cd RTMF/orchestration-mqueue
./start-test.sh [test-name]
```

`start-test.sh` builds the task-wrapper base image and the per-task images, starts the whole
stack with `docker compose up --build -d`, streams the execution-manager logs until it exits,
saves all the logs and generates the performance report in `results/<test-name>/` (a random
`results/test_<id>/` when no name is given). Ctrl+C stops the test early; the report is built
from the data collected so far.

| Variable | Meaning |
|---|---|
| `MANIFEST=<path>` | Manifest used by the image-builder (default: `tests/test_0_code/manifest.yaml`). |
| `FORCE=1` | Overwrite `results/<test-name>/` if it already exists. |

> **Note.** The deploy-manager always runs the manifest copied into its image at build time
> (`tests/test_0_code/manifest.yaml` → `/tmp/manifest.yaml`, see `services/deploy-manager/Dockerfile`).
> When you use `MANIFEST=<path>`, copy that file over `tests/test_0_code/manifest.yaml` as well,
> otherwise the images are built from one manifest and deployed from another.

---

## 🛠️ Manual workflow

Follow these instructions to start the system step-by-step:

```bash
# 1. Build the task-wrapper base image (tag must match images.base in the manifest)
docker build -t rtmf/task-wrapper:orchestration-mqueue \
    -f services/task-wrapper/Dockerfile .

# 2. Build all per-task images from the manifest
python3 sdk/image-builder/src/main.py \
    -f tests/test_0_code/manifest.yaml \
    -c tests/test_0_code/

# 3. Build the orchestrators (this also bakes the manifest into deploy-manager)
docker compose build deploy-manager execution-manager

# 4. Start Redis
docker compose up -d redis

# 5. Deploy task-wrappers and populate Redis
docker compose up deploy-manager

# 6. Execute the schedule
docker compose up execution-manager

# 7. Cleanup
docker compose down
docker rm -f $(docker ps -aq --filter "name=stress_task_") 2>/dev/null || true
sudo rm -f /dev/mqueue/*
```

> Use `docker compose build --no-cache` to force a clean rebuild of the orchestrators.

---

## 📝 Manifest reference

A manifest has two top-level sections:

- **`images`**: base image, optional remote repo, list of task images to build.
- **`schedule`**: list of tasks with their real-time parameters.

### Minimal example

```yaml
version: "1.0"

images:
  base: "rtmf/task-wrapper:orchestration-mqueue"
  repo: "jeffadac/task-wrapper"
  tasks:
    - alias: "stress_task_1"
      src:   "stress_task_1"

schedule:
  name: "Single-task demo"
  description: "One configurable CPU/IO stress task."
  iterations: 10

  tasks:
    - id: 1
      image: "stress_task_1"
      start: 50            # ms after schedule t₀
      deadline: 650        # ms after schedule t₀ (absolute)
      cpu_affinity: 5      # pin worker thread to CPU 5
      policy: "fifo"       # fifo | rr | other | normal | batch | idle
      priority: 50         # RT priority [1..99] for fifo/rr, nice value otherwise
      depends_on: []
      inputs:
        total_ops:     { type: int, value: 17 }
        io_percentage: { type: int, value: 0  }
      outputs:
        result:        { type: int }
```

### Field semantics

| Field | Type | Meaning |
|---|---|---|
| `images.base` | string | Base image used by every task image. Must match the tag built in step 1. |
| `images.repo` | string | Docker repo for tagging/pushing (publishing is currently disabled in the image-builder). |
| `images.tasks[].alias` | string | Local name of the image and of its container (matches `schedule.tasks[].image`). |
| `images.tasks[].src` | string | Sub-folder under the build context containing `app_task.h` and `app_task.c`. |
| `schedule.name` / `description` / `version` | string | Descriptive metadata (`version` defaults to `0.0.0`). |
| `schedule.iterations` | int | How many times the whole schedule is replayed back-to-back (values `< 1` run it once). |
| `schedule.tasks[].id` | int | Task identifier (used in logs, Redis and `perf.csv`). **Required.** |
| `schedule.tasks[].image` | string | Image alias the task runs in. Must be declared under `images.tasks`. **Required.** |
| `schedule.tasks[].start` | int (ms) | Time after t₀ when the task is fired. **Required.** |
| `schedule.tasks[].deadline` | int (ms) | Absolute deadline (after t₀). **Required.** |
| `schedule.tasks[].cpu_affinity` | int | CPU index the worker thread is pinned to. **Required.** |
| `schedule.tasks[].policy` | enum | `fifo` → `SCHED_FIFO`, `rr` → `SCHED_RR`, `other`/`normal` → `SCHED_OTHER`, `batch` → `SCHED_BATCH`, `idle` → `SCHED_IDLE`. `deadline` is accepted by the parser but not supported yet (it needs `sched_setattr`). **Required.** |
| `schedule.tasks[].priority` | int | RT priority `1..99` for `fifo`/`rr`; nice value `-20..19` for `other`/`normal`/`batch`; ignored for `idle`. **Required.** |
| `schedule.tasks[].depends_on` | list | Reserved for future DAG support: currently ignored. |
| `schedule.tasks[].inputs` | map | Typed inputs; the `value`s are passed to the task as a JSON object (`{"total_ops": 17, ...}`). |
| `schedule.tasks[].outputs` | map | Declared output schema (informative). |

### Task API

A task is two files, `app_task.h` and `app_task.c`. The API is the same in every containerised
RTMF implementation, so the same task sources can be used by all of them:

```c
/* app_task.h */
typedef struct { int total_ops; int io_percentage; } input_t;   /* task-specific */
typedef struct { int result; } output_t;                         /* task-specific */

int    convert_json_to_input(JsonObject *obj, input_t *input);   /* JSON inputs -> input_t */
gchar *convert_output_to_json(const output_t *output);           /* output_t -> JSON */
void  *task_main(void *arg);   /* arg is an input_t*; returns a g_new0-allocated output_t* */
```

### Adding a new task

1. Create `tests/test_0_code/<my_task>/app_task.h` and `app_task.c` (see `tests/test_0_code/stress_task_1/` as a template).
2. Add the alias under `images.tasks` in the manifest.
3. Add one or more entries under `schedule.tasks` referencing that alias.
4. Re-run `./start-test.sh` (or steps 2–6 of the manual workflow).

---

## 🖥️ Sample output

Excerpt of a run with the default manifest (6 tasks, 100 iterations):

```text
deploy-manager     | [INFO] deploy-manager: Processing image 'stress_task_1'
deploy-manager     | [INFO] deploy-manager: Running container 'stress_task_1' from image 'stress_task_1'
deploy-manager     | [INFO] deploy-manager: Container 'stress_task_1' is running (ID: bf34f146ba84)
...
stress_task_1      | [INFO] task-wrapper: stress_task_1: Waiting for Execution Manager queue '/execution_manager_q'...
stress_task_1      | [INFO] task-wrapper: stress_task_1: Connected to Execution Manager queue.
stress_task_1      | [INFO] task-wrapper: === Task Wrapper (stress_task_1) Start Main Loop ===

execution-manager  | [INFO] execution-manager: =========== Execution Manager Init ========
execution-manager  | [INFO] execution-manager: ====== Execution Manager Initialized ======
execution-manager  | [INFO] execution-manager: ==== Execution Manager Start Main Loop ====
execution-manager  | [DEBUG] execution-manager: === SCHEDULE: Configurable CPU/IO Benchmark (v0.0.0) [1300 ms] ===
execution-manager  | [DEBUG] execution-manager:   50 ms: Activate Task 1 (stress_task_1)
execution-manager  | [PERF] execution-manager: iteration=0 task_id=1 ... q_em_tw_ms=0.361 t_in_tw_ms=86.954 q_tw_em_ms=0.083 total_ms=87.397 core_id=4
...
execution-manager  | [INFO] execution-manager: ==== Execution Manager Exit Main Loop ====
execution-manager  | [INFO] execution-manager: Cleanup completed.
```

The `[PERF]` lines are what `tools/perf_report.py` turns into `perf.csv`, `stats.csv` and `report.pdf`.

---

## 🧰 Useful commands

```bash
# Inspect Redis content
docker exec -it redis redis-cli
> KEYS *
> HGETALL schedule
> HGETALL scheduletask:1

# Tail logs
docker logs -f deploy-manager
docker logs -f execution-manager
docker logs -f stress_task_1

# List task-wrapper containers
docker ps --filter "name=stress_task_"

# Inspect live POSIX message queues on the host
ls -l /dev/mqueue/

# Full cleanup
docker compose down -v
docker rm -f $(docker ps -aq --filter "name=stress_task_") 2>/dev/null || true
sudo rm -f /dev/mqueue/*
```

---

## ❗ Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `mq_open: Permission denied` | Containers not sharing the host IPC namespace | Compose / deploy-manager already pass `ipc: host`; ensure you didn't override it |
| Task-wrapper stuck on `Waiting for Execution Manager queue '/execution_manager_q'...` | The execution-manager is not running yet | It retries every 0.5 s; start the execution-manager (`docker compose up execution-manager`) |
| Execution-manager does not start the schedule | The `schedule` key is not in Redis yet | It polls Redis every second; check that the deploy-manager finished (`docker logs deploy-manager`) |
| `mlockall failed: Cannot allocate memory` | Container missing `memlock=-1` ulimit | Use the provided compose / deploy-manager (already configured) |
| `sched_setscheduler: Operation not permitted` | Container missing `SYS_NICE` capability or `rtprio` ulimit | Same as above: use the provided compose / deploy-manager |
| Stale task results across runs | POSIX queues persist on the host | `sudo rm -f /dev/mqueue/*` before relaunching |
| The run ignores manifest changes | Manifest baked into the deploy-manager image | Rebuild it: `docker compose build deploy-manager` |
| `Permission denied` on `/var/run/docker.sock` | User not in the `docker` group | `sudo usermod -aG docker $USER` and re-login |
| `Container name "/redis" is already in use` | Stale container from a previous run | `docker rm -f redis` (or use `./start-test.sh`, which cleans up first) |
| Port `6379` already in use | A local Redis is already running | `sudo systemctl stop redis` (or change the port in `docker-compose.yml`) |
| Tasks miss their deadlines | CPU contention / wrong affinity | Use isolated CPUs (`isolcpus=...` boot param) and pin tasks via `cpu_affinity` |
| `docker compose: command not found` | Compose v2 plugin missing | Install `docker-compose-plugin` (Debian/Ubuntu) |

---

<div align="center">


</div>
