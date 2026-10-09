<div align="center">

# ⚡ Orchestration Microservices mqueue

</div>

---

## 📁 Repository layout

```
.
├── common/                   # Shared C headers/sources (task IPC types, schedule)
│   └── include/
│       ├── task.h
│       └── task_ipc.h
├── sdk/
│   └── image-builder/        # Python CLI that builds per-task Docker images
├── services/
│   ├── deploy-manager/       # Python: spawns task-services + writes Redis
│   ├── execution-manager/    # C/GLib: reads schedule, dispatches via mqueue
│   └── task-wrapper/         # C/GLib: mqueue server base image + task harness
├── tests/
│   └── test_0_code/          # Active manifest + sample task sources
│       ├── manifest.yaml
│       └── stress_task_*/    # One folder per task (each with app_task.h/.c)
├── docs/                     # Cheat-sheet & repo dumper
├── docker-compose.yml
├── start-test.sh             # End-to-end build & run helper
└── README.md
```

---

## 📋 Prerequisites

- **Linux host with `PREEMPT_RT` kernel** (POSIX `mqueue`, `SCHED_FIFO` and `SYS_NICE` are Linux-only).
- **Docker Engine ≥ 24** with Docker Compose v2 (the `docker compose ...` syntax).
- **Python ≥ 3.10** and access to `/var/run/docker.sock`.
- Python packages used by the image-builder (declared in `sdk/image-builder/requirements.txt`):

  ```bash
  pip install -r sdk/image-builder/requirements.txt
  ```

> **Tip.** If you don't want to install the Python deps system-wide, use a virtualenv: `python3 -m venv .venv && source .venv/bin/activate && pip install -r sdk/image-builder/requirements.txt`.

> **POSIX mqueues.** If a previous run left stale queues behind, wipe them with `sudo rm -f /dev/mqueue/*` before relaunching.

---

## 🛠️ Manual workflow

Follow these instructions to start the system step-by-step:

```bash
# 1. Build the task-wrapper base image
docker build -t jeffadac/task-wrapper:latest \
    -f services/task-wrapper/Dockerfile .

# 2. Build all per-task images from the manifest
python3 sdk/image-builder/src/main.py \
    -f tests/test_0_code/manifest.yaml \
    -c tests/test_0_code/

# 3. Build the orchestrators
docker compose build deploy-manager execution-manager

# 4. Start Redis
docker compose up -d redis

# 5. Deploy task-services and populate Redis
docker compose up deploy-manager

# 6. Execute the schedule
docker compose up execution-manager

# 7. Cleanup
docker compose down
docker rm -f $(docker ps -aq --filter "name=stress_task_") 2>/dev/null || true
sudo rm -f /dev/mqueue/*
```

> Add `--no-cache` to the image-builder or `docker compose build --no-cache` to force a clean rebuild.

---

## 📝 Manifest reference

A manifest has two top-level sections:

- **`images`** — base image, optional remote repo, list of task images to build.
- **`schedule`** — list of tasks with their real-time parameters.

### Minimal example

```yaml
version: "1.0"

images:
  base: "jeffadac/task-wrapper:latest"
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
      policy: "fifo"       # fifo | rr | other
      priority: 50         # POSIX RT priority [1..99]
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
| `images.base` | string | Base image used by every task image. |
| `images.repo` | string | Optional Docker repo for tagging/pushing. |
| `images.tasks[].alias` | string | Local name of the image (matches `schedule.tasks[].image`). |
| `images.tasks[].src` | string | Sub-folder under the build context containing `app_task.h`/`app_task.c`. |
| `schedule.iterations` | int | How many times the whole schedule is replayed back-to-back. |
| `schedule.tasks[].id` | int | Stable task identifier (used in logs and Redis). |
| `schedule.tasks[].start` | int (ms) | Time after t₀ when the task is fired. |
| `schedule.tasks[].deadline` | int (ms) | Absolute deadline (after t₀). |
| `schedule.tasks[].policy` | enum | `fifo` → `SCHED_FIFO`, `rr` → `SCHED_RR`, `other` → `SCHED_OTHER`. |
| `schedule.tasks[].priority` | int | RT priority `1..99` (only meaningful for `fifo`/`rr`). |
| `schedule.tasks[].cpu_affinity` | int | CPU index the worker thread is pinned to. |
| `schedule.tasks[].depends_on` | list | Reserved for future DAG support. |
| `schedule.tasks[].inputs` | map | Typed key/value inputs passed to `task_main()` as JSON. |
| `schedule.tasks[].outputs` | map | Declared output schema. |

### Adding a new task

1. Create `tests/test_0_code/<my_task>/app_task.h` and `app_task.c` (see `tests/test_0_code/stress_task_1/` as a template — implement `task_main(...)` and the JSON in/out converters).
2. Add the alias under `images.tasks` in the manifest.
3. Add one or more entries under `schedule.tasks` referencing that alias.
4. Re-run `./start-test.sh` (or steps 2–6 of the manual workflow).

---

## 🖥️ Sample output

A successful run with the default manifest looks like this:

```text
deploy-manager           | Processing task 'stress_task_1'
deploy-manager           | Running container 'stress_task_1' from image 'stress_task_1'
deploy-manager           | Container 'stress_task_1' is running (ID: ab12cd34ef)
deploy-manager           | ✅ Schedule data loaded into Redis successfully.

execution-manager        | =========== Execution Manager Init ========
execution-manager        | ====== Execution Manager Initialized ======
execution-manager        | ==== Execution Manager Start Main Loop ====
execution-manager        | [INFO] Execution Manager: Draining stale messages from the queue...
execution-manager        | [INFO] Execution Manager: Start iteration 1, progress percentage 1 %
execution-manager        | [INFO] Execution Manager: Sent REQUEST for Task ID 1
execution-manager        | [INFO] Execution Manager: Received result for Task ID 1: {"result": 0}
execution-manager        | [INFO] Execution Manager: Task 1 updated: 99 runs left.
execution-manager        | ==== Execution Manager Exit Main Loop ====
```

---

## 🧰 Useful commands

```bash
# Inspect Redis content
docker exec -it redis redis-cli
> KEYS *
> HGETALL scheduletask:1

# Tail logs
docker logs -f deploy-manager
docker logs -f execution-manager
docker logs -f stress_task_1

# List task-service containers
docker ps --filter "name=stress_task_"

# Inspect live POSIX message queues on the host
ls -l /dev/mqueue/

# Full cleanup
docker compose down -v
docker rm -f $(docker ps -aq --filter "name=stress_task_") 2>/dev/null || true
sudo rm -f /dev/mqueue/*
```

---

## 🚀 Automated start

```bash
git clone <repo-url> orchestration-mqueue
cd orchestration-mqueue
./start-test.sh
```

---

## ❗ Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `mq_open: Permission denied` | Containers not sharing the host IPC namespace | Compose / launcher already pass `ipc: host`; ensure you didn't override it |
| `mq_open failed for /<task>_q: No such file or directory` | The task-wrapper container hasn't created its queue yet | The Execution Manager polls and retries automatically; confirm the container is up with `docker ps` |
| `mlockall failed: Cannot allocate memory` | Container missing `memlock=-1` ulimit | Use the provided compose / launcher (already configured) |
| `sched_setscheduler: Operation not permitted` | Container missing `SYS_NICE` capability or `rtprio` ulimit | Same as above — use the provided compose / launcher |
| Stale task results across runs | POSIX queues persist on the host | `sudo rm -f /dev/mqueue/*` before relaunching |
| `Permission denied` on `/var/run/docker.sock` | User not in the `docker` group | `sudo usermod -aG docker $USER` and re-login |
| `Container name "/redis" is already in use` | Stale container from a previous run | `docker rm -f redis` (or use `./start-test.sh`, which cleans up first) |
| Port `6379` already in use | A local Redis is already running | `sudo systemctl stop redis` (or change the port in `docker-compose.yml`) |
| Tasks miss their deadlines | CPU contention / wrong affinity | Use isolated CPUs (`isolcpus=...` boot param) and pin tasks via `cpu_affinity` |
| `docker compose: command not found` | Compose v2 plugin missing | Install `docker-compose-plugin` (Debian/Ubuntu) |

---

<div align="center">


</div>
