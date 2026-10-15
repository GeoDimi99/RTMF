<div align="center">

# 🎼 Choreography Microservices ZeroMQ
</div>

---

Choreography pattern of RTMF: there is no central execution manager. The **deploy-manager**
starts one **task-wrapper** container per task image and loads a distributed execution plan into
Redis; every task-wrapper reads its own part of the plan, joins a start barrier over ZeroMQ
(Unix-domain sockets in a shared volume) and then runs its own timeline.

## 📁 Repository layout

```
.
├── common/                   # Shared C headers/sources (logger, task struct, ZeroMQ endpoints + sync msg format)
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
│   └── test_0_code/          # Active mission (manifest + task sources)
│       ├── manifest.yaml
│       └── stress_task_*/    # Per-task workload sources (app_task.c / app_task.h)
├── tools/                    # Performance report scripts (used by start-test.sh)
├── results/                  # One folder per test run (logs, perf.csv, stats.csv, report.pdf)
├── docs/                     # Misc. notes and an auto-generated project context
├── docker-compose.yml        # redis + deploy-manager
├── start-test.sh             # End-to-end build, run & benchmark
└── README.md
```

---

## ⚙️ How it works

1. **deploy-manager** parses `manifest.yaml` and, for every entry of `images.tasks`, starts a
   task-wrapper container named after the image alias. The containers join the compose network
   (so they can reach `redis`), mount the named volume `zmq_ipc` on `/ipc` and get
   `cap_add=[SYS_NICE, IPC_LOCK]`, `rtprio=99`, `memlock=unlimited`, `cpuset 0-7`,
   env `TASK_NAME` / `TASK_QUEUE_NAME` = alias.
2. It then writes the distributed plan into a single Redis hash, `schedule_data`:
   - `schedule:name`, `schedule:version`, `schedule:description`, `schedule:iterations`
   - `schedule:images`: JSON list of the images, `schedule:length`: number of images
   - `schedule:duration`: latest deadline (ms), the length of one iteration
   - `schedule:leader`: the image of the task with the earliest `start`
   - `schedule:<image>:length` and `schedule:<image>:<idx>`: the tasks of each image, as JSON
3. Every task-wrapper (entrypoint `./build/execution-manager`) polls Redis until `schedule_data`
   exists and loads only the tasks of its own image.
4. **Start barrier over ZeroMQ**: the leader binds a `PULL` socket on `ipc:///ipc/sync_ready.sock`
   and an `XPUB` socket on `ipc:///ipc/sync_pub.sock`; every worker connects a `PUSH` and a `SUB`
   socket, then sends `READY`. When all the workers are ready the leader broadcasts the common t₀.
   Then every container runs its own timeline `iterations` times and exits when the final deadline
   is reached.

---

## 📋 Prerequisites

- **Linux host with `PREEMPT_RT` kernel** (real-time scheduling, `SYS_NICE` and Unix-domain ZeroMQ sockets are Linux-only).
- **Docker Engine ≥ 20.10** with Docker Compose v2 (the `docker compose ...` syntax).
- **Python ≥ 3.10** and access to `/var/run/docker.sock`.
- Python packages used by the image-builder (declared in `sdk/image-builder/requirements.txt`):

  ```bash
  pip install -r sdk/image-builder/requirements.txt
  ```

> **Tip.** If you don't want to install the Python deps system-wide, use a virtualenv: `python3 -m venv .venv && source .venv/bin/activate && pip install -r sdk/image-builder/requirements.txt`.

> **Real-time priorities.** The deploy-manager grants each task container `cap_add: [SYS_NICE, IPC_LOCK]` and `ulimits: rtprio=99, memlock=unlimited`. If your kernel does not allow this for unprivileged users, run Docker with extra privileges or relax `/etc/security/limits.conf`.

---

## 🚀 Automated start

```bash
git clone https://github.com/GeoDimi99/RTMF.git
cd RTMF/choreography-zeromq
./start-test.sh [test-name]
```

`start-test.sh` removes the previous containers and recreates the `zmq_ipc` volume, builds the
task-wrapper base image and the per-task images, starts the stack with `docker compose up --build -d`,
follows every task container until all of them finish, saves the logs and generates the
performance report in `results/<test-name>/` (a random `results/test_<id>/` when no name is given).
Ctrl+C stops the test early; the report is built from the data collected so far.

| Variable | Meaning |
|---|---|
| `MISSION_DIR=<dir>` | Directory with `manifest.yaml` and one source folder per task, used by the image-builder (default: `tests/test_0_code`). |
| `FORCE=1` | Overwrite `results/<test-name>/` if it already exists. |

> **Note.** The deploy-manager always runs the manifest copied into its image at build time
> (`tests/test_0_code/manifest.yaml` → `/tmp/manifest.yaml`, see `services/deploy-manager/Dockerfile`).
> When you use `MISSION_DIR=<dir>`, copy `<dir>/manifest.yaml` over `tests/test_0_code/manifest.yaml`
> as well, otherwise the images are built from one manifest and deployed from another.

---

## 🛠️ Manual workflow

Follow these instructions to start the system step by step:

```bash
# 1. Build the task-wrapper base image (tag must match images.base in the manifest)
docker build -t rtmf/task-wrapper:choreography-zeromq \
    -f services/task-wrapper/Dockerfile .

# 2. Build all per-task images from the manifest
python3 sdk/image-builder/src/main.py \
    -f tests/test_0_code/manifest.yaml -c tests/test_0_code

# 3. Build the deploy-manager (this also bakes the manifest into it)
docker compose build deploy-manager

# 4. Create the shared IPC volume (Unix-socket rendezvous point)
docker volume create zmq_ipc

# 5. Start Redis
docker compose up -d redis

# 6. Run the deploy-manager: spawns task containers and loads the schedule
docker compose up deploy-manager

# 7. (the task containers now self-coordinate over ZeroMQ until completion)
docker logs -f stress_task_1

# 8. Cleanup
docker compose down --remove-orphans
docker rm -f $(docker ps -aq --filter "name=stress_task_") 2>/dev/null || true
docker volume rm zmq_ipc 2>/dev/null || true
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
  base: "rtmf/task-wrapper:choreography-zeromq"
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
        total_ops:     { type: int, value: 16 }
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
| `images.tasks[].src` | string | Sub-folder under the build context containing `app_task.c` / `app_task.h`. |
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
| `schedule.tasks[].inputs` | map | Typed inputs; the `value`s are passed to `task_main()` as a JSON object (`{"total_ops": 16, ...}`). |
| `schedule.tasks[].outputs` | map | Declared output schema (informative). |


### Adding a new task

1. Create `tests/test_0_code/<my_task>/app_task.c` + `app_task.h` (implements `task_main(...)`; see `tests/test_0_code/stress_task_1/` for a template). The task API is the same as in the other containerised RTMF implementations.
2. Add the alias under `images.tasks` in the manifest.
3. Add one or more entries under `schedule.tasks` referencing that alias.
4. Re-run `./start-test.sh` (or steps 2–7 of the manual workflow).

---

## 🖥️ Sample output

Excerpt of a run with the default six-task manifest (100 iterations); `stress_task_1` is the leader:

```text
deploy-manager     | [INFO] deploy-manager: Processing image 'stress_task_1'
deploy-manager     | [INFO] deploy-manager: Running container 'stress_task_1' from image 'stress_task_1'
deploy-manager     | [INFO] deploy-manager: Container 'stress_task_1' is running on network 'choreography-zeromq_default' (ID: 4755fc27ebc7)
...

stress_task_1      | [INFO] task-wrapper: Initialized (Click Ctrl+C for a clean exit)
stress_task_1      | [INFO] task-wrapper: Waiting for schedule_data in Redis...
stress_task_1      | [INFO] task-wrapper: Leader: PULL bound on ipc:///ipc/sync_ready.sock
stress_task_1      | [INFO] task-wrapper: Leader: XPUB bound on ipc:///ipc/sync_pub.sock
stress_task_1      | [INFO] execution-manager: >>> SCHEDULE INFO
stress_task_1      | [INFO] execution-manager:     Duration:  1300 ms
stress_task_1      | [INFO] execution-manager:     @ 50     ms | ID: 1   | stress_task_1    | Core: 5  | Prio: 50  | FIFO
stress_task_1      | [PERF] task-wrapper: iteration=0 task_id=1 ... q_em_tw_ms=0.197 t_in_tw_ms=87.295 q_tw_em_ms=0.006 total_ms=87.498 core_id=5
...
stress_task_1      | [INFO] task-wrapper: handle_expiration: Final deadline reached. Quitting...
stress_task_1      | [INFO] task-wrapper: Cleanup completed

stress_task_2      | [INFO] task-wrapper: Worker: SUB connected to ipc:///ipc/sync_pub.sock
stress_task_2      | [INFO] task-wrapper: Worker: PUSH connected to ipc:///ipc/sync_ready.sock
```

The `[PERF]` lines of all task containers are what `tools/perf_report.py` turns into `perf.csv`,
`stats.csv` and `report.pdf`.

---

## 🧰 Useful commands

```bash
# Inspect Redis content
docker exec -it redis redis-cli
> KEYS *
> HGETALL schedule_data
> HGET schedule_data schedule:leader

# Tail logs
docker logs -f deploy-manager
docker logs -f stress_task_1

# List task containers
docker ps --filter "name=stress_task_"

# Inspect the shared IPC volume (where the ZMQ Unix sockets live)
docker volume inspect zmq_ipc

# Full cleanup
docker compose down --remove-orphans
docker rm -f $(docker ps -aq --filter "name=stress_task_") 2>/dev/null || true
docker volume rm zmq_ipc 2>/dev/null || true
```

---

## ❗ Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `Source path '<path>' for alias '<alias>' does not exist.` (image-builder) | A `src:` folder declared in the manifest does not exist on disk | Create the missing `tests/test_0_code/<alias>/` folder, or remove the alias from `manifest.yaml` |
| Task containers keep printing `Waiting for schedule_data in Redis...` | The deploy-manager failed before writing the plan | `docker logs deploy-manager` |
| `Container name "/redis" is already in use` | Stale container from a previous run | `start-test.sh` removes it automatically; otherwise: `docker rm -f redis deploy-manager` |
| Port `6379` already in use | A local Redis is already running on the host | `sudo systemctl stop redis` (or change the compose port) |
| Task containers hang at the start barrier | Some task containers crashed before sending `READY` | `docker ps -a --filter "name=stress_task_"` and check the logs of the missing ones |
| `zmq_bind(PULL) failed` / `zmq_bind(XPUB) failed` | Stale Unix socket file in the shared volume | The leader unlinks them at start; if not, `docker volume rm zmq_ipc && docker volume create zmq_ipc` |
| The run ignores manifest changes | Manifest baked into the deploy-manager image | Rebuild it: `docker compose build deploy-manager` (`start-test.sh` already does it) |
| `Permission denied` on `/var/run/docker.sock` | User not in `docker` group | `sudo usermod -aG docker $USER` and re-login |
| `SCHED_FIFO` / `mlock` errors | Container missing capabilities or `rtprio` ulimit | Verify `cap_add: [SYS_NICE, IPC_LOCK]` and the `rtprio`/`memlock` ulimits in `services/deploy-manager/src/deploy/runner.py` |
| Tasks miss their deadlines | CPU contention / wrong affinity | Use isolated CPUs (`isolcpus=...` boot param) and pin tasks via `cpu_affinity` |

---

<div align="center">

</div>
