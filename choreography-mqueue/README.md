<div align="center">

# 🎼 Choreography Microservices MQueue
</div>


---

Choreography pattern of RTMF: there is no central execution manager. The **deploy-manager**
starts one **task-wrapper** container per task image and loads a distributed execution plan into
Redis; every task-wrapper reads its own part of the plan, joins a start barrier through POSIX
message queues and then runs its own timeline.

## 📁 Repository layout

```
.
├── common/                   # Shared C headers/sources (logger, task struct, IPC msg format)
│   ├── include/
│   │   ├── logger.h
│   │   ├── task.h
│   │   └── task_ipc.h
│   └── src/
├── sdk/
│   └── image-builder/        # Python CLI that builds per-task Docker images
├── services/
│   ├── deploy-manager/       # Python: spawns task containers + writes Redis
│   └── task-wrapper/         # C: real-time runtime base image (local execution manager + task harness)
├── tests/
│   └── test_0_code/          # Sample test
│       ├── manifest.yaml     # Active schedule (DSL)
│       └── stress_task_*/    # Per-task workload sources (app_task.c/.h)
├── tools/                    # Performance report scripts (used by start-test.sh)
├── results/                  # One folder per test run (logs, perf.csv, stats.csv, report.pdf)
├── docs/                     # Cheat-sheet, repo dumper and generated project context
├── docker-compose.yml        # redis + deploy-manager
├── start-test.sh             # End-to-end build, run & benchmark
└── README.md
```

---

## ⚙️ How it works

1. **deploy-manager** parses `manifest.yaml` and, for every entry of `images.tasks`, starts a
   task-wrapper container named after the image alias. The containers join the compose network
   (so they can reach `redis`) and get `ipc_mode=host`, `cap_add=[SYS_NICE, IPC_LOCK]`,
   `rtprio=99`, `memlock=unlimited`, `cpuset 0-7`, env `TASK_NAME` / `TASK_QUEUE_NAME` = alias.
2. It then writes the distributed plan into a single Redis hash, `schedule_data`:
   - `schedule:name`, `schedule:version`, `schedule:description`, `schedule:iterations`
   - `schedule:images`: JSON list of the images, `schedule:length`: number of images
   - `schedule:duration`: latest deadline (ms), the length of one iteration
   - `schedule:leader`: the image of the task with the earliest `start`
   - `schedule:<image>:length` and `schedule:<image>:<idx>`: the tasks of each image, as JSON
3. Every task-wrapper (entrypoint `./build/execution-manager`) polls Redis until `schedule_data`
   exists, loads only the tasks of its own image and opens its queue `/<alias>_q`.
4. **Start barrier**: the workers send `READY` to the leader's queue; when all of them are ready
   the leader broadcasts `SYNC` with a common t₀. Then every container runs its own timeline
   `iterations` times and exits when the final deadline is reached.

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

## 🚀 Automated start

```bash
git clone https://github.com/GeoDimi99/RTMF.git
cd RTMF/choreography-mqueue
./start-test.sh [test-name]
```

`start-test.sh` builds the task-wrapper base image and the per-task images, wipes
`/dev/mqueue/*`, starts the stack with `docker compose up --build -d`, follows every task
container until all of them finish, saves the logs and generates the performance report in
`results/<test-name>/` (a random `results/test_<id>/` when no name is given). Ctrl+C stops the
test early; the report is built from the data collected so far.

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

Follow these instructions to start the system step by step:

```bash
# 1. Build the task-wrapper base image (Ubuntu + glib + json-glib + hiredis + sources);
#    the tag must match images.base in the manifest
docker build -t rtmf/task-wrapper:choreography-mqueue \
    -f services/task-wrapper/Dockerfile .

# 2. Build all per-task images from the manifest
#    (each image extends the base, drops in the task-specific app_task.{c,h},
#     compiles with cmake and sets ENTRYPOINT=./build/execution-manager)
python3 sdk/image-builder/src/main.py \
    -f tests/test_0_code/manifest.yaml \
    -c tests/test_0_code

# 3. Build the deploy-manager (this also bakes the manifest into it)
docker compose build deploy-manager

# 4. Wipe stale POSIX message queues from previous runs
sudo rm -f /dev/mqueue/*

# 5. Start Redis and deploy-manager
#    (deploy-manager parses the manifest, spawns one container per task image with
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

- **`images`**: base image, optional remote repo, list of task images to build.
- **`schedule`**: list of tasks with their real-time parameters.

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
| `images.tasks[].src` | string | Sub-folder under the build context containing `app_task.{c,h}`. |
| `schedule.name` / `description` / `version` | string | Descriptive metadata (`version` defaults to `0.0.0`). |
| `schedule.iterations` | int | How many times the whole schedule is replayed back-to-back (values `< 1` run it once). |
| `schedule.tasks[].id` | int | Task identifier (used in logs, Redis and `perf.csv`). **Required.** |
| `schedule.tasks[].image` | string | Image alias the task runs in. Must be declared under `images.tasks`. **Required.** |
| `schedule.tasks[].start` | int (ms) | Time after t₀ when the task is fired. The earliest one selects the leader image. **Required.** |
| `schedule.tasks[].deadline` | int (ms) | Absolute deadline (after t₀). The latest one is the iteration length. **Required.** |
| `schedule.tasks[].cpu_affinity` | int | CPU index the worker thread is pinned to. **Required.** |
| `schedule.tasks[].policy` | enum | `fifo` → `SCHED_FIFO`, `rr` → `SCHED_RR`, `other`/`normal` → `SCHED_OTHER`, `batch` → `SCHED_BATCH`, `idle` → `SCHED_IDLE`. `deadline` is accepted by the parser but not supported yet (it needs `sched_setattr`). **Required.** |
| `schedule.tasks[].priority` | int | RT priority `1..99` for `fifo`/`rr`; nice value `-20..19` for `other`/`normal`/`batch`; ignored for `idle`. **Required.** |
| `schedule.tasks[].depends_on` | list | Reserved for future DAG support: currently ignored. |
| `schedule.tasks[].inputs` | map | Typed inputs; the `value`s are passed to `task_main()` as a JSON object (`{"total_ops": 17, ...}`). |
| `schedule.tasks[].outputs` | map | Declared output schema (informative). |


### Adding a new task

1. Create `tests/test_0_code/<my_task>/app_task.h` and `app_task.c` (implements `task_main(...)`; see `tests/test_0_code/stress_task_1/` for a template). The task API is the same as in the other containerised RTMF implementations.
2. Add the alias under `images.tasks` in the manifest.
3. Add one or more entries under `schedule.tasks` referencing that alias.
4. Re-run `./start-test.sh` (or steps 2–6 of the manual workflow).


---



## 🖥️ Sample output

Excerpt of a run with the default six-task manifest (100 iterations):

```text
deploy-manager     | [INFO] deploy-manager: Processing image 'stress_task_1'
deploy-manager     | [INFO] deploy-manager: Running container 'stress_task_1' from image 'stress_task_1'
deploy-manager     | [INFO] deploy-manager: Container 'stress_task_1' is running on network 'choreography-mqueue_default' (ID: a892e21e2500)
...

stress_task_1      | [INFO] task-wrapper: Initialized (Click Ctrl+C for a clean exit)
stress_task_1      | [INFO] task-wrapper: Waiting for schedule_data in Redis...
stress_task_1      | [DEBUG] execution-manager: Name: Configurable CPU/IO Benchmark
stress_task_1      | [DEBUG] execution-manager: Duration: 1300 ms
stress_task_1      | [DEBUG] execution-manager: @ 50     ms | ID: 1   | stress_task_1    | Core: 5  | Prio: 50  | FIFO
stress_task_1      | [PERF] task-wrapper: iteration=0 task_id=1 ... q_em_tw_ms=0.185 t_in_tw_ms=86.778 q_tw_em_ms=0.019 total_ms=86.983 core_id=5
...
stress_task_1      | [INFO] task-wrapper: handle_expiration: final deadline reached. Quitting...
stress_task_1      | [INFO] task-wrapper: Cleanup completed
```

The `[PERF]` lines of all task containers are what `tools/perf_report.py` turns into `perf.csv`,
`stats.csv` and `report.pdf`.

---

## 🧰 Useful commands

```bash
# Inspect Redis content
docker exec -it redis redis-cli
> HGETALL schedule_data
> HGET schedule_data schedule:leader

# Tail logs
docker logs -f deploy-manager
docker logs -f stress_task_1
docker logs --tail 200 stress_task_2

# List task containers
docker ps --filter "name=stress_task_"
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

## ❗ Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Tasks exit immediately with `Exited (0)` and `COMMAND="/bin/bash"` | Per-task images were built without an entrypoint (image-builder step failed or skipped) | Re-run `python3 sdk/image-builder/src/main.py -f <manifest> -c <ctx>` and check it ends with `Successfully built <alias>`. Verify with `docker inspect <alias> --format '{{.Config.Entrypoint}}'`. |
| Task containers keep printing `Waiting for schedule_data in Redis...` | The deploy-manager failed before writing the plan | `docker logs deploy-manager` |
| `Container name "/redis" is already in use` | Stale container from a previous run under a different compose project | `start-test.sh` removes it automatically; otherwise: `docker rm -f redis deploy-manager` |
| `mq_open: Permission denied` / tasks hang on first message | `/dev/mqueue` not writable from the container | Make sure POSIX message queues are exposed by the kernel; `deploy-manager` already starts the wrappers with `ipc_mode=host`. |
| Tasks miss their deadlines | CPU contention / wrong affinity | Use isolated CPUs (`isolcpus=...` boot param) and pin tasks via `cpu_affinity`. |
| `sched_setscheduler failed: Operation not permitted` | Container missing `SYS_NICE` capability or `rtprio` ulimit | The `deploy-manager` already passes `cap_add=[SYS_NICE, IPC_LOCK]` and `rtprio=99`; don't run wrappers manually without those flags. |
| Stale messages between runs cause weird hangs | POSIX message queues persist in the kernel until reboot or manual delete | `sudo rm -f /dev/mqueue/*` before each run (`start-test.sh` does this for you). |
| The run ignores manifest changes | Manifest baked into the deploy-manager image | Rebuild it: `docker compose build deploy-manager` (`start-test.sh` already does it) |
| `Permission denied` on `/var/run/docker.sock` | User not in `docker` group | `sudo usermod -aG docker $USER` and re-login. |
| Port `6379` already in use | A local Redis is already running | `sudo systemctl stop redis` (or change the compose port). |
| `cmake: not found` while the image-builder is building a task | Base image was rebuilt without the build toolchain | Force a clean rebuild: `docker rmi rtmf/task-wrapper:choreography-mqueue && docker build --no-cache -t rtmf/task-wrapper:choreography-mqueue -f services/task-wrapper/Dockerfile .` |

---

<div align="center">

</div>
