<div align="center">

# ⚡ Orchestration Microservices gRPC
</div>

---

Orchestration pattern of RTMF: a central **execution-manager** reads the schedule from Redis
and dispatches each task to its **task-wrapper** container through gRPC. The **deploy-manager**
starts one task-wrapper container per task image and loads the schedule into Redis.

## 📁 Repository layout

```
.
├── common/                   # Shared C headers/sources (logger, task types, IPC types)
├── proto/                    # gRPC / protobuf definitions
│   └── task_service.proto
├── sdk/
│   └── image-builder/        # Python CLI that builds per-task Docker images
├── services/
│   ├── deploy-manager/       # Python: spawns task-services + writes Redis
│   ├── execution-manager/    # C/C++: reads schedule, dispatches via gRPC
│   └── task-wrapper/         # C/C++: gRPC server base image + task harness
├── tests/test_0_code/
│   ├── manifest.yaml         # Active schedule
│   └── stress_task_1..6/     # Task sources (app_task.h + app_task.c)
├── tools/                    # Performance report scripts (used by start-test.sh)
├── results/                  # One folder per test run (logs, perf.csv, stats.csv, report.pdf)
├── docker-compose.yml
└── start-test.sh             # End-to-end build, run & benchmark
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

## 🚀 Automated start

```bash
git clone https://github.com/GeoDimi99/RTMF.git
cd RTMF/orchestration-grpc
./start-test.sh [test-name]
```

`start-test.sh` builds every image, runs the whole schedule, saves the logs and generates the
performance report in `results/<test-name>/` (a random `results/test_<id>/` when no name is given).

| Variable | Meaning |
|---|---|
| `MANIFEST=<path>` | Manifest to run (default: `tests/test_0_code/manifest.yaml`). A custom one is copied over the default. |
| `FORCE=1` | Overwrite `results/<test-name>/` if it already exists. |

---

## 🛠️ Manual workflow

Follow these instructions to start the system step by step:

```bash
# 1. Build the task-wrapper base image
docker build -t rtmf/task-wrapper:orchestration-grpc \
    -f services/task-wrapper/Dockerfile .

# 2. Build all per-task images from the manifest
python3 sdk/image-builder/src/main.py -f tests/test_0_code/manifest.yaml -c tests/test_0_code

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

> **Note.** The manifest is copied into the deploy-manager image at build time
> (`/app/tests/test_0_code/manifest.yaml`). After editing the manifest, rebuild the
> deploy-manager (step 3) as well as the task images (step 2).

---

## 📝 Manifest reference

A manifest has two top-level sections:

- **`images`** — base image, optional remote repo, list of task images to build.
- **`schedule`** — list of tasks with their real-time parameters.

### Minimal example

```yaml
version: "1.0"

images:
  base: "rtmf/task-wrapper:orchestration-grpc"
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
| `images.base` | string | Base image used by every task image. |
| `images.repo` | string | Optional Docker repo for tagging/pushing (publishing is currently disabled). |
| `images.tasks[].alias` | string | Local name of the image (matches `schedule.tasks[].image`). |
| `images.tasks[].src` | string | Sub-folder under the build context containing `app_task.h` and `app_task.c`. |
| `schedule.iterations` | int | How many times the whole schedule is replayed back-to-back. |
| `schedule.tasks[].id` | int | Stable task identifier (used in logs and Redis). |
| `schedule.tasks[].image` | string | Image alias the task runs in. Several tasks may share the same image. |
| `schedule.tasks[].start` | int (ms) | Time after t₀ when the task is fired. |
| `schedule.tasks[].deadline` | int (ms) | Absolute deadline (after t₀). The execution manager cancels the RPC if exceeded. |
| `schedule.tasks[].policy` | enum | `fifo` → `SCHED_FIFO`, `rr` → `SCHED_RR`, `other`/`normal` → `SCHED_OTHER`, `batch` → `SCHED_BATCH`, `idle` → `SCHED_IDLE`. Default: `fifo`. |
| `schedule.tasks[].priority` | int | RT priority `1..99` for `fifo`/`rr`; nice value `-20..19` for `other`/`normal`/`batch`. Default: `50`. |
| `schedule.tasks[].cpu_affinity` | int | CPU index the worker thread is pinned to (optional). |
| `schedule.tasks[].depends_on` | list | Reserved for future DAG support. |
| `schedule.tasks[].inputs` | map | Typed key/value inputs, passed to the task as a JSON object (`{"total_ops": 17, ...}`). |
| `schedule.tasks[].outputs` | map | Declared output schema. |


### Task API

A task is two files, `app_task.h` and `app_task.c`. The API is the same in every RTMF
implementation, so the same task sources can be used by all of them:

```c
/* app_task.h */
typedef struct { int total_ops; int io_percentage; } input_t;   /* task-specific */
typedef struct { int result; } output_t;                         /* task-specific */

int    convert_json_to_input(JsonObject *obj, input_t *input);   /* JSON inputs -> input_t */
gchar *convert_output_to_json(const output_t *output);           /* output_t -> JSON (g_free'd by the wrapper) */
void  *task_main(void *arg);   /* arg is an input_t*; returns a g_new0-allocated output_t* (g_free'd by the wrapper) */
```

The task-wrapper (`services/task-wrapper/src/grpc_server.cpp`) parses the request, runs
`task_main()` in a thread with the requested policy/priority/affinity and sends back the result.


### Adding a new task

1. Create `tests/test_0_code/my_task/` with `app_task.h` and `app_task.c` (see `tests/test_0_code/stress_task_1/` for a template).
2. Add the alias under `images.tasks` in the manifest.
3. Add one or more entries under `schedule.tasks` referencing that alias.
4. Re-run `./start-test.sh` (or steps 2–6 of the manual workflow).


---



## 🖥️ Sample output

Excerpt of a run with the default manifest (6 tasks, 100 iterations):

```text
deploy-manager      | Parsed schedule 'Configurable CPU/IO Benchmark' with 6 tasks.
deploy-manager      | Found 6 task image(s) for 6 task(s)
deploy-manager      | Deploying container for image 'stress_task_1' on port 50051
deploy-manager      | Running container 'task-service-stress_task_1' from image 'stress_task_1'
deploy-manager      | gRPC server at localhost:50051 is ready!
...
deploy-manager      | Schedule data loaded into Redis successfully.

execution-manager   | Execution Manager running with SCHED_FIFO priority 95
execution-manager   | Loading schedule: 6 task(s), 100 iteration(s)
execution-manager   | T=144 ms: task 1 'stress_task_1' COMPLETED
execution-manager   | Result: {"result":0}
execution-manager   | [PERF] iteration=0 task_id=1 ... total_ms=93.307 core_id=4
```

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
docker logs -f execution-manager-grpc
docker logs -f task-service-stress_task_1

# List task-services
docker ps --filter "name=task-service-"

# Full cleanup
docker compose down -v
docker rm -f $(docker ps -aq --filter "name=task-service-") 2>/dev/null || true
```

---

## ❗ Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `protoc: command not found` during build | Stale execution-manager image | `docker compose build --no-cache execution-manager` |
| `gRPC connection failed` | task-service containers not up yet | `docker ps --filter "name=task-service-"` and check their logs |
| `Permission denied` on `/var/run/docker.sock` | User not in `docker` group | `sudo usermod -aG docker $USER` and re-login |
| `SCHED_FIFO` errors | Container missing `SYS_NICE` | Ensure `cap_add: [SYS_NICE]` is present in `docker-compose.yml` |
| `Container name "/redis" is already in use` | Stale container from a previous run | `start-test.sh` removes it automatically; otherwise: `docker rm -f redis` |
| Port `6379` already in use | A local Redis is already running | `sudo systemctl stop redis` (or change the compose port) |
| Tasks miss their deadlines | CPU contention / wrong affinity | Use isolated CPUs (`isolcpus=...` boot param) and pin tasks via `cpu_affinity` |
| The run ignores manifest changes | Manifest baked into the deploy-manager image | Rebuild it: `docker compose build deploy-manager` |

---

<div align="center">


</div>
