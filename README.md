<div align="center">

# Making Containerized Real-Time Microservices Practical
### *A Framework for Automated Deployment and Coordination*

</div>

---

## What this repository contains

This repository hosts the **reference implementations** of the framework described in the paper.

---

## Repository layout

```
.
├── native/                          # Bare-metal Execution Manager (no Docker)
├── orchestration-grpc/  # Orchestration  +  gRPC  IPC
├── orchestration-mqueue/            # Orchestration  +  POSIX mqueue IPC
├── choreography-mqueue/             # Choreography   +  POSIX mqueue IPC
└── choreography-zeromq/             # Choreography   +  ZeroMQ IPC
```

Each sub-folder has its implementation with its own `README.md`,
`start-test.sh`, `docker-compose.yml` and manifest.

---

## The architectures

| Project | Pattern |
|---|---|
| [`orchestration-grpc/`](orchestration-grpc/) | Orchestration |
| [`orchestration-mqueue/`](orchestration-mqueue/) | Orchestration | 
| [`choreography-mqueue/`](choreography-mqueue/) | Choreography | 
| [`choreography-zeromq/`](choreography-zeromq/) | Choreography | s
| [`native/`](native/) | Single host process, multi-threaded | 

---

## Common prerequisites

Every sub-project needs:

- **Linux host with `PREEMPT_RT` kernel** — `SCHED_FIFO`, `mlockall`, `SYS_NICE`, POSIX mqueue and Unix
  sockets.
- **Docker Engine ≥ 20.10** with Docker Compose v2 (`docker compose ...`).
- **Python ≥ 3.10** with access to `/var/run/docker.sock`.
- The image-builder Python deps:

  ```bash
  pip install pyyaml python-dotenv docker
  ```


---


<div align="center">

</div>
