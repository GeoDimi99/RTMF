<div align="center">

# 🎼 Choreography Microservices MQueue
</div>


---


## 📁 Repository layout

```
.
├── common/                   # Shared C headers/sources (task struct, IPC msg format)
│   ├── include/
│   │   ├── task.h
│   │   └── task_ipc.h
│   └── src/
├── sdk/
│   └── image-builder/        # Python CLI that builds per-task Docker images
├── services/
│   ├── deploy-manager/       # Python: spawns task containers + writes Redis
│   └── task-wrapper/         # C: real-time runtime base image (one per task)
├── tests/
│   └── test_0_code/          # Sample test
│       ├── manifest.yaml     # Active schedule (DSL)
│       └── stress_task_*/    # Per-task workload sources (app_task.c/.h)
├── docker-compose.yml
├── start-test.sh             # End-to-end build & run helper
└── docs/
```

---


## 📋 Prerequisites

- **Linux host with `PREEMPT_RT` kernel** and POSIX message queue support (`/dev/mqueue`
  available and writable). 
- **Docker Engine ≥ 20.10** with Docker Compose v2 (the `docker compose ...`
  syntax).
- **Python ≥ 3.10** and access to `/var/run/docker.sock`.
- Python packages used by the image-builder (declared in `sdk/image-builder/requirements.txt`):

  ```bash
  pip install -r sdk/image-builder/requirements.txt
  ```

> **Tip.** If you don't want to install the Python deps system-wide, use a virtualenv: `python3 -m venv .venv && source .venv/bin/activate && pip install -r sdk/image-builder/requirements.txt`.

---

## 🛠️ Manual workflow

Follow this instruction to start the system in a manual way step by step:

```bash
# 1. Build the task-wrapper base image (Ubuntu + glib + json-glib + hiredis + sources)
docker build -t rtmf/task-wrapper:choreography-mqueue \
    -f services/task-wrapper/Dockerfile .

# 2. Build all per-task images from the manifest
#    (each image extends the base, drops in the task-specific app_task.{c,h},
#     compiles with cmake and sets ENTRYPOINT=./build/execution-manager)
python3 sdk/image-builder/src/main.py \
    -f tests/test_0_code/manifest.yaml \
    -c tests/test_0_code

# 3. Build the orchestrator
docker compose build deploy-manager

# 4. Wipe stale POSIX message queues from previous runs
sudo rm -f /dev/mqueue/*

# 5. Start Redis and deploy-manager
#    (deploy-manager parses the manifest, spawns one container per task with
#     the required real-time capabilities and writes the schedule into Redis)
docker compose up -d redis
docker compose up deploy-manager

# 6. Follow the task containers (each one is named after its image alias)
docker logs -f stress_task_1

# 7. Cleanup
docker compose down
docker rm -f $(docker ps -aq --filter "name=stress_task_") 2>/dev/null || true
sudo rm -f /dev/mqueue/*
```

> Add `--no-cache` to `docker build` or `docker compose build --no-cache` to force a clean rebuild.

---

## 📝 Manifest reference

A manifest has two top-level sections:

- **`images`** — base image, optional remote repo, list of task images to build.
- **`schedule`** — list of tasks with their real-time parameters.

### Minimal example

```yaml
version: "1.0"

images:
  base: "rtmf/task-wrapper:choreography-mqueue"
  repo: "jeffadac/task-wrapper"
  tasks:
    - alias: "stress_task_1"
      src:   "stress_task_1"

schedule:
  name: "Single-task demo"
  description: "One configurable CPU/IO stress task."
  iterations: 5

  tasks:
    - id: 1
      image: "stress_task_1"
      start: 50            # ms after schedule t₀
      deadline: 650        # ms after schedule t₀ (absolute)
      cpu_affinity: 5      # pin worker thread to CPU 5
      policy: "fifo"       # fifo | rr | deadline | other
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
| `images.tasks[].src` | string | Sub-folder under the build context containing `app_task.{c,h}`. |
| `schedule.iterations` | int | How many times the whole schedule is replayed back-to-back. |
| `schedule.tasks[].id` | int | Stable task identifier (used in logs and Redis). |
| `schedule.tasks[].start` | int (ms) | Time after t₀ when the task is fired. |
| `schedule.tasks[].deadline` | int (ms) | Absolute deadline (after t₀). |
| `schedule.tasks[].policy` | enum | `fifo` → `SCHED_FIFO`, `rr` → `SCHED_RR`, `deadline` → `SCHED_DEADLINE`, `other` → `SCHED_OTHER`. |
| `schedule.tasks[].priority` | int | RT priority `1..99` (only meaningful for `fifo`/`rr`). |
| `schedule.tasks[].cpu_affinity` | int | CPU index the worker thread is pinned to. |
| `schedule.tasks[].depends_on` | list | Choreography dependencies (peers this task waits for). |
| `schedule.tasks[].inputs` | map | Typed key/value inputs passed to `task_main()` as JSON. |
| `schedule.tasks[].outputs` | map | Declared output schema. |


### Adding a new task

1. Create `tests/<your_test>/<my_task>/app_task.h` and `app_task.c` (implements `task_main(...)` — see `tests/test_0_code/stress_task_1/app_task.h` for a template).
2. Add the alias under `images.tasks` in the manifest.
3. Add one or more entries under `schedule.tasks` referencing that alias.
4. Re-run `./start-test.sh` (or steps 2–6 of the manual workflow).


---



## 🖥️ Sample output

A successful run with the default six-task manifest looks like this:

```text
deploy-manager     | Schedule: name='Configurable CPU/IO Benchmark' iterations=5 tasks=6
deploy-manager     | Processing task 'stress_task_1'
deploy-manager     | Running container 'stress_task_1' from image 'stress_task_1'
deploy-manager     | Container 'stress_task_1' is running on network 'choreography-mqueue_default'
deploy-manager     | === FLAT REDIS SCHEDULE ===
deploy-manager     | schedule:length: 6
deploy-manager     | schedule:leader: stress_task_1
deploy-manager     | ...

stress_task_1      | [INFO] Execution Manager: Initialized (Click Ctrl+C for a clean exit)
stress_task_1      | [INFO] Execution Manager: Start iteration 0, progress percentage 0 %
stress_task_1      | [THREAD] Core 5 | Executing: 17 CPU ops, 0 I/O ops
stress_task_1      | [INFO] Execution Manager: Start iteration 1, progress percentage 1 %
...
```

---

## 🧰 Useful commands

```bash
# Inspect Redis content
docker exec -it redis redis-cli
> HGETALL schedule_data

# Tail logs
docker logs -f deploy-manager
docker logs -f stress_task_1
docker logs --tail 200 stress_task_2

# List task containers
docker ps --filter "ancestor=stress_task_1" --filter "ancestor=stress_task_2"
docker ps --format 'table {{.Names}}\t{{.Image}}\t{{.Status}}'

# Inspect what entrypoint a task image was built with
docker inspect stress_task_1 \
    --format 'Entrypoint={{.Config.Entrypoint}}  Cmd={{.Config.Cmd}}'

# Wipe POSIX message queues between runs (mandatory!)
sudo rm -f /dev/mqueue/*

# Full cleanup
docker compose down -v
docker rm -f $(docker ps -aq --filter "name=stress_task_") 2>/dev/null || true
```

---

## 🚀 Automated start

```bash
git clone <repo-url> choreography-mqueue
cd choreography-mqueue
./start-test.sh
```

---

## ❗ Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Tasks exit immediately with `Exited (0)` and `COMMAND="/bin/bash"` | Per-task images were built without an entrypoint (image-builder step failed or skipped) | Re-run `python3 sdk/image-builder/src/main.py -f <manifest> -c <ctx>` and check it ends with `Successfully built <alias>`. Verify with `docker inspect <alias> --format '{{.Config.Entrypoint}}'`. |
| `Container name "/redis" is already in use` | Stale container from a previous run under a different compose project | The latest `start-test.sh` removes it automatically; otherwise: `docker rm -f redis deploy-manager` |
| `mq_open: Permission denied` / tasks hang on first message | `/dev/mqueue` not writable from the container | Make sure POSIX message queues are exposed by the kernel; `deploy-manager` already starts the wrappers with `ipc_mode=host`. |
| Tasks miss their deadlines | CPU contention / wrong affinity | Use isolated CPUs (`isolcpus=...` boot param) and pin tasks via `cpu_affinity`. |
| `sched_setscheduler failed: Operation not permitted` | Container missing `SYS_NICE` capability or `rtprio` ulimit | The `deploy-manager` already passes `cap_add=[SYS_NICE, IPC_LOCK]` and `rtprio=99`; don't run wrappers manually without those flags. |
| Stale messages between runs cause weird hangs | POSIX message queues persist in the kernel until reboot or manual delete | `sudo rm -f /dev/mqueue/*` before each run (the script does this for you). |
| `Permission denied` on `/var/run/docker.sock` | User not in `docker` group | `sudo usermod -aG docker $USER` and re-login. |
| Port `6379` already in use | A local Redis is already running | `sudo systemctl stop redis` (or change the compose port). |
| `cmake: not found` while the image-builder is building a task | Base image was rebuilt without the build toolchain | Force a clean rebuild: `docker rmi rtmf/task-wrapper:choreography-mqueue && docker build --no-cache -t rtmf/task-wrapper:choreography-mqueue -f services/task-wrapper/Dockerfile .` |

---

<div align="center">

</div>
