<div align="center">

# 🎼 Choreography Microservices ZeroMQ
</div>

---


## 📁 Repository layout

```
.
├── common/                   # Shared C headers/sources (IPC helpers, Redis client)
├── sdk/
│   └── image-builder/        # Python CLI that builds per-task Docker images
├── services/
│   ├── deploy-manager/       # Python: spawns task containers + writes Redis
│   └── task-wrapper/         # C: real-time runtime base image + task harness
├── tests/
│   └── test_0_code/          # Active mission (manifest + task sources)
│       ├── manifest.yaml
│       └── stress_task_1/    # Sample task source (app_task.c / app_task.h)
├── docs/                     # Misc. notes and an auto-generated project context
├── docker-compose.yml
├── start-test.sh             # End-to-end build & run helper
```

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

## 🛠️ Manual workflow

Follow this instruction to start the system in a manual way step by step:

```bash
# 1. Build the task-wrapper base image
docker build -t rtmf/task-wrapper:choreography-zeromq \
    -f services/task-wrapper/Dockerfile .

# 2. Build all per-task images from the manifest
python3 sdk/image-builder/src/main.py \
    -f tests/test_0_code/manifest.yaml -c tests/test_0_code

# 3. Build the deploy-manager
docker compose build deploy-manager

# 4. Create the shared IPC volume (Unix-socket rendezvous point)
docker volume create zmq_ipc

# 5. Start Redis
docker compose up -d redis

# 6. Run the deploy-manager — spawns task containers and loads the schedule
docker compose up deploy-manager

# 7. (the task containers now self-coordinate over ZeroMQ until completion)

# 8. Cleanup
docker compose down --remove-orphans
docker rm -f $(docker ps -aq --filter "name=stress_task_") 2>/dev/null || true
docker volume rm zmq_ipc 2>/dev/null || true
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
      policy: "fifo"       # fifo | rr | other
      priority: 50         # POSIX RT priority [1..99]
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
| `images.base` | string | Base image used by every task image. |
| `images.repo` | string | Optional Docker repo for tagging/pushing. |
| `images.tasks[].alias` | string | Local name of the image (matches `schedule.tasks[].image`). |
| `images.tasks[].src` | string | Sub-folder under the build context containing `app_task.c` / `app_task.h`. |
| `schedule.iterations` | int | How many times the whole schedule is replayed back-to-back. |
| `schedule.tasks[].id` | int | Stable task identifier (used in logs and Redis). |
| `schedule.tasks[].start` | int (ms) | Time after t₀ when the task is fired. |
| `schedule.tasks[].deadline` | int (ms) | Absolute deadline (after t₀). |
| `schedule.tasks[].policy` | enum | `fifo` → `SCHED_FIFO`, `rr` → `SCHED_RR`, `other` → `SCHED_OTHER`. |
| `schedule.tasks[].priority` | int | RT priority `1..99` (only meaningful for `fifo`/`rr`). |
| `schedule.tasks[].cpu_affinity` | int | CPU index the worker thread is pinned to. |
| `schedule.tasks[].depends_on` | list | Task IDs the task waits on within the same iteration. |
| `schedule.tasks[].inputs` | map | Typed key/value inputs passed to `task_main()` as JSON. |
| `schedule.tasks[].outputs` | map | Declared output schema. |


### Adding a new task

1. Insert `tests/test_0_code/my_task/app_task.c` + `app_task.h` (implements `task_main(...)` — see `tests/test_0_code/stress_task_1/` for a template).
2. Add the alias under `images.tasks` in the manifest.
3. Add one or more entries under `schedule.tasks` referencing that alias.
4. Re-run `./start-test.sh` (or steps 2–6 of the manual workflow).

> **Note.** The deploy-manager image bakes `tests/test_0_code/manifest.yaml` at build time (see `services/deploy-manager/Dockerfile`). When you change the manifest, rebuild the image — `./start-test.sh` already does this through `docker compose up --build`.


---


## 🖥️ Sample output

A successful run looks like this on a task container (`docker logs -f stress_task_1`):

```text
[INFO] Execution Manager: Start iteration 1/100, progress percentage 1 %
[SYNC] Leader: Waiting for 5 workers to be READY and subscribed...
[SYNC] Leader: Worker 1/5 is ready.
[SYNC] Leader: Worker 2/5 is ready.
[SYNC] Leader: Worker 3/5 is ready.
[SYNC] Leader: Worker 4/5 is ready.
[SYNC] Leader: Worker 5/5 is ready.
[SYNC] Leader: Barrier released. Sync time broadcast to 5 workers.
[SYNC] Final Synchronization Complete. T-Zero (Monotonic): 21404071341 us
[INFO] Execution Manager: Scheduler started! Waiting for events...
[INFO] Depends On: None
[INFO] ThreadCall 1: start thread function.
[THREAD] Core 5 | Executing: 16 CPU ops, 0 I/O ops
[INFO] ThreadCall 1: termination thread function.
[INFO] Execution Manager: Task 1 updated: 0 runs left.
[INFO] Execution Manager (handle_expiration): Final deadline reached. Quitting...
[INFO] Execution Manager: Scheduler terminated successfully.
...
[INFO] Execution Manager: Exit from the main loop. Cleanup ...
[INFO] Execution Manager: Cleanup completed.
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

## 🚀 Automated start

```bash
git clone <repo-url> choreography-zeromq
cd choreography-zeromq
./start-test.sh
```

---

## ❗ Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `name 'ImageBuilderError' is not defined` during step 3 | A `src:` folder declared in the manifest does not exist on disk | Create the missing `tests/test_0_code/<alias>/` folder, or remove the alias from `manifest.yaml` |
| `Container name "/redis" is already in use` | Stale container from a previous run | `start-test.sh` removes it automatically; otherwise: `docker rm -f redis deploy-manager` |
| Port `6379` already in use | A local Redis is already running on the host | `sudo systemctl stop redis` (or change the compose port) |
| Task containers hang at `[SYNC] Leader: Waiting for N workers...` | Some task containers crashed or never got an `IPC_LOCK` capability | `docker ps -a --filter "name=stress_task_"` and check the logs of the missing ones |
| `bind: address already in use` on `ipc:///ipc/sync_*.sock` | Stale Unix socket file in the shared volume | The leader unlinks them at start; if not, `docker volume rm zmq_ipc && docker volume create zmq_ipc` |
| `Permission denied` on `/var/run/docker.sock` | User not in `docker` group | `sudo usermod -aG docker $USER` and re-login |
| `SCHED_FIFO` / `mlock` errors | Container missing capabilities or `rtprio` ulimit | Verify `cap_add: [SYS_NICE, IPC_LOCK]` and the `rtprio`/`memlock` ulimits in `services/deploy-manager/src/deploy/runner.py` |
| Tasks miss their deadlines | CPU contention / wrong affinity | Use isolated CPUs (`isolcpus=...` boot param) and pin tasks via `cpu_affinity` |

---

<div align="center">


</div>
