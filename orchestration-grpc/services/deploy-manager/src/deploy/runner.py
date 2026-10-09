import docker
from ..logger import get_logger
from ..exceptions import DeployManagerError

logger = get_logger("deploy-manager")

class DockerContainerRunner:
    """
    Runs Docker containers for task services with the required real-time privileges.
    """

    def __init__(self):
        self.client = docker.from_env()

    def run_task_service(
        self,
        image_tag: str,
        container_name: str = None,
        detach: bool = True,
        grpc_port: int = 50051
    ):
        """
        Run a task-service container with privileged capabilities and ulimits.
        """

        try:
            # Remove existing container if it exists
            try:
                existing = self.client.containers.get(container_name)
                logger.info(f"Stopping and removing existing container '{container_name}'")
                existing.stop()
                existing.remove()
            except docker.errors.NotFound:
                pass  # Container does not exist, OK

            logger.info(f"Running container '{container_name}' from image '{image_tag}'")

            container = self.client.containers.run(
                image=image_tag,
                name=container_name,
                detach=detach,
                tty=True,
                ipc_mode="host",
                network_mode="host",  # Use host networking for minimal gRPC latency
                cap_add=["SYS_NICE"],
                environment={
                    "TASK_NAME": image_tag,
                    "TASK_QUEUE_NAME": image_tag,
                    "GRPC_PORT": str(grpc_port),  # Assign unique port to each task service
                    },
                ulimits=[
                    docker.types.Ulimit(name="rtprio", soft=99, hard=99),
                    docker.types.Ulimit(name="memlock", soft=-1, hard=-1),
                ],
                cpuset_cpus="0-7",  # Task-wrapper main thread on core 4, task threads pinned per-task by cpu_affinity (any of the 8 cores)
                tmpfs={"/tmp": "size=64m,mode=1777"},  # RAM-backed /tmp: fdatasync() returns instantly like on native
                #remove=True,
            )

            logger.info(f"Container '{container_name}' is running (ID: {container.short_id})")
            return container

        except docker.errors.DockerException as e:
            raise DeployManagerError(f"Failed to run container '{container_name}': {e}")
