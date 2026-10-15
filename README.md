<div align="center">

# RTMF - Real-Time Microservices Framework
### *A Framework for Automated Deployment and Coordination*

</div>

---

## What this repository contains

This repository hosts the **reference implementations** of the framework described in the paper.

---

## Repository layout

```
.
├── native/                 # Bare-metal Execution Manager (no Docker), baseline
├── orchestration-grpc/     # Orchestration  +  gRPC          IPC
├── orchestration-mqueue/   # Orchestration  +  POSIX mqueue  IPC
├── choreography-mqueue/    # Choreography   +  POSIX mqueue  IPC
├── choreography-zeromq/    # Choreography   +  ZeroMQ        IPC
└── tools/
    └── rt_vs_nonrt_ttest.py  # Student/Welch t-tests: RT vs non-RT runs of one implementation
```

Each sub-folder is self-contained: it has its own `README.md`, `start-test.sh`,
manifest (`tests/test_0_code/manifest.yaml`, except `native/`), `tools/` (report scripts)
and `results/` (one folder per test run).

---

## The architectures

| Project | Pattern | IPC between containers | Containers |
|---|---|---|---|
| [`orchestration-grpc/`](orchestration-grpc/) | Orchestration | gRPC (host network, one port per task image) | redis, deploy-manager, execution-manager, one task-wrapper per image |
| [`orchestration-mqueue/`](orchestration-mqueue/) | Orchestration | POSIX message queues (`ipc: host`) | redis, deploy-manager, execution-manager, one task-wrapper per image |
| [`choreography-mqueue/`](choreography-mqueue/) | Choreography | POSIX message queues (`ipc: host`) | redis, deploy-manager, one task-wrapper per image |
| [`choreography-zeromq/`](choreography-zeromq/) | Choreography | ZeroMQ over Unix sockets (shared `zmq_ipc` volume) | redis, deploy-manager, one task-wrapper per image |
| [`native/`](native/) | Single host process, multi-threaded | none | none (runs on the host) |

The four containerised implementations share the same building blocks:

- **Image builder** (`sdk/image-builder/`, runs on the developer machine): reads the manifest and
  builds one Docker image per task on top of the implementation's task-wrapper base image.
- **Deploy manager** (`services/deploy-manager/`, container): reads the manifest, starts one
  task-wrapper container per task image and writes the execution plan into **Redis**.
- **Orchestration**: a central **execution manager** reads the plan from Redis and triggers the
  task-wrappers (workers).
- **Choreography**: there is no central execution manager. Every task-wrapper reads its own part
  of the plan from Redis, joins a start barrier with the other containers and runs its own timeline.

The manifest format (`images` + `schedule` sections) and the task API (`app_task.h` /
`app_task.c`) are the same in all four containerised implementations, so the same task sources
can be used by all of them. The Redis layout of the execution plan is implementation-specific
(see each sub-project's README).

---

## Common prerequisites

Every sub-project needs:

- **Linux host with `PREEMPT_RT` kernel**: `SCHED_FIFO`, `mlockall`, `SYS_NICE`, POSIX mqueue and Unix
  sockets.
- **Docker Engine ≥ 20.10** with Docker Compose v2 (`docker compose ...`), not needed by `native/`.
- **Python ≥ 3.10** with access to `/var/run/docker.sock`.
- The image-builder Python deps:

  ```bash
  pip install pyyaml python-dotenv docker
  ```

The report scripts (`tools/perf_report.py`) need `matplotlib`, `numpy` and `PyYAML`; `start-test.sh`
installs them into `tools/.venv` when they are missing.

---

## Running a test

```bash
git clone https://github.com/GeoDimi99/RTMF.git
cd RTMF/<implementation>
./start-test.sh [test-name]          # native/: sudo ./start-test.sh [test-name]
```

Each run writes `results/<test-name>/` (a random `results/test_<id>/` when no name is given) with
the container logs, a copy of the manifest, `perf.csv`, `stats.csv` and `report.pdf`.
Set `FORCE=1` to overwrite an existing results folder.

To compare a real-time run against non real-time runs of the same implementation:

```bash
python3 tools/rt_vs_nonrt_ttest.py --impl choreography-mqueue \
    --rt test_single_core_1 --nonrt test_single_core_nort_2
```

The CSV and PDF are written to `<implementation>/results/`.

---


<div align="center">

</div>
