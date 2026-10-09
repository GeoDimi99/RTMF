<div align="center">

# ⚡ Orchestration Microservices gRPC
</div>

---


## 📁 Repository layout

```
.
├── common/                   # Shared C headers/sources (logger, IPC, JSON utils)
├── proto/                    # gRPC / protobuf definitions
│   └── task_service.proto
├── sdk/
│   └── image-builder/        # Python CLI that builds per-task Docker images
├── services/
│   ├── deploy-manager/       # Python: spawns task-services + writes Redis
│   ├── execution-manager/    # C/C++: reads schedule, dispatches via gRPC
│   └── task-wrapper/         # C/C++: gRPC server base image + task harness
├── task/
│   ├── manifest.yaml         # Active schedule
│   └── stress_task_1/        # Sample task source (app_task.h)
├── examples/
│   └── multi_task/           # Six-task example (manifest + sources)
├── docker-compose.yml
├── run-all.sh                # End-to-end build & run helper
```

---


## 📋 Prerequisites

- **Linux host with `PREEMPT_RT` kernel** (host networking + `SYS_NICE` are Linux-only).
- **Docker Engine ≥ 20.10** with Docker Compose v2 (the `docker compose ...` syntax).
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
# 1. Build the task-wrapper base image
docker build -t jeffadac/realtime-microservices:task-wrapper \
    -f services/task-wrapper/Dockerfile .

# 2. Build all per-task images from the manifest
python3 sdk/image-builder/src/main.py -f task/manifest.yaml -c task

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
docker rm -f $(docker ps -aq --filter "name=task-service-") 2>/dev/null || true
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
  base: "jeffadac/realtime-microservices:task-wrapper"
  repo: "jeffadac/realtime-microservices"
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
| `images.tasks[].src` | string | Sub-folder under the build context containing `app_task.h`. |
| `schedule.iterations` | int | How many times the whole schedule is replayed back-to-back. |
| `schedule.tasks[].id` | int | Stable task identifier (used in logs and Redis). |
| `schedule.tasks[].start` | int (ms) | Time after t₀ when the task is fired. |
| `schedule.tasks[].deadline` | int (ms) | Absolute deadline (after t₀). The execution manager cancels the RPC if exceeded. |
| `schedule.tasks[].policy` | enum | `fifo` → `SCHED_FIFO`, `rr` → `SCHED_RR`, `other` → `SCHED_OTHER`. |
| `schedule.tasks[].priority` | int | RT priority `1..99` (only meaningful for `fifo`/`rr`). |
| `schedule.tasks[].cpu_affinity` | int | CPU index the worker thread is pinned to. |
| `schedule.tasks[].depends_on` | list | Reserved for future DAG support. |
| `schedule.tasks[].inputs` | map | Typed key/value inputs passed to `task_main()` as JSON. |
| `schedule.tasks[].outputs` | map | Declared output schema. |


### Adding a new task

1. Insert `task/my_task/new_task.h` (implements `task_main(...)` — see `task/stress_task_1/app_task.h` for a template).
2. Add the alias under `images.tasks` in the manifest.
3. Add one or more entries under `schedule.tasks` referencing that alias.
4. Re-run `./run-all.sh` (or steps 2–6 of the manual workflow).


---



## 🖥️ Sample output

A successful run with the default single-task manifest looks like this:

```text
deploy-manager           | Found 1 unique task image(s) for 1 task(s)
deploy-manager           | Deploying container for image 'stress_task_1' on port 50051
deploy-manager           | ✅ gRPC server at localhost:50051 is ready!
deploy-manager           | Schedule data loaded into Redis successfully.

execution-manager-grpc   | ✅ Execution Manager running with SCHED_FIFO priority 99
execution-manager-grpc   | [SCHEDULER] Starting iteration 1/10
execution-manager-grpc   | [GRPC THREAD 1] Starting...
execution-manager-grpc   | ✅    Task 1 'stress_task_1' STARTED
execution-manager-grpc   | 🎉    Task 1 'stress_task_1' COMPLETED
execution-manager-grpc   |    [RESULT] {"result": 0}
execution-manager-grpc   | [SCHEDULER] 🏁 Iteration 1/10 complete!
```

---

## 🧰 Useful commands

```bash
# Inspect Redis content
docker exec -it redis redis-cli
> KEYS *
> GET schedule

# Tail logs
docker logs -f deploy-manager
docker logs -f execution-manager-grpc
docker logs -f task-service-stress_task_1

# List task-services
docker ps --filter "name=task-service-"

# Full cleanup
docker compose down -v
docker rm -f $(docker ps -aq --filter "name=task-service-") 2>/dev/null || true
```

---

## 🚀 Automated start

```bash
git clone <repo-url> realtime-microservices
cd realtime-microservices
./start-test.sh
```

---

---

## ❗ Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `protoc: command not found` during build | Stale execution-manager image | `docker compose build --no-cache execution-manager` |
| `gRPC connection failed` | task-service containers not up yet | `docker ps --filter "name=task-service-"` and check their logs |
| `Permission denied` on `/var/run/docker.sock` | User not in `docker` group | `sudo usermod -aG docker $USER` and re-login |
| `SCHED_FIFO` errors | Container missing `SYS_NICE` | Ensure `cap_add: [SYS_NICE]` is present in `docker-compose.yml` |
| `Container name "/redis" is already in use` | Stale container from a previous run | The latest `run-all.sh` removes it automatically; otherwise: `docker rm -f redis` |
| Port `6379` already in use | A local Redis is already running | `sudo systemctl stop redis` (or change the compose port) |
| Tasks miss their deadlines | CPU contention / wrong affinity | Use isolated CPUs (`isolcpus=...` boot param) and pin tasks via `cpu_affinity` |

---

<div align="center">


</div>
