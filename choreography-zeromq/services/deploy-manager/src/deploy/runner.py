import docker
import socket
from .exceptions import ContainerRunnerError
from ..logger import get_logger

logger = get_logger("deploy-manager")


class DockerContainerRunner:
    """
    Runs Docker containers for task services with the required real-time privileges.
    """

    def __init__(self):
        self.client = docker.from_env()

    def _get_current_network(self):
        """
        Detects the network this container is currently attached to.
        This allows dynamically created containers to see the 'redis' service.
        """
        try:
            # Get the ID of the current container via its hostname
            current_container_id = socket.gethostname()
            this_container = self.client.containers.get(current_container_id)
            
            # Get the name of the first network (usually '[project]_default')
            networks = list(this_container.attrs['NetworkSettings']['Networks'].keys())
            return networks[0] if networks else "bridge"
        except Exception:
            # Fallback to default bridge if detection fails
            return "bridge"

    def run_task_service(
        self,
        image_tag: str,
        container_name: str = "task-wrapper",
        detach: bool = True,
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
                pass

            logger.info(f"Running container '{container_name}' from image '{image_tag}'")
            
            # Detect the network to join
            target_network = self._get_current_network()

            container = self.client.containers.run(
                image=image_tag,
                name=container_name,
                detach=detach,
                remove=False,
                network=target_network,
                volumes={
                    "zmq_ipc": {"bind": "/ipc", "mode": "rw"},
                },
                # tmpfs={
                #     "/tmp": "size=64m,mode=1777"
                # },
                cap_add=["SYS_NICE", "IPC_LOCK"],
                environment={
                    "TASK_NAME": image_tag,
                    "TASK_QUEUE_NAME": image_tag,
                },
                ulimits=[
                    docker.types.Ulimit(name="rtprio", soft=99, hard=99),
                    docker.types.Ulimit(name="memlock", soft=-1, hard=-1),
                ],
                cpuset_cpus="0-7",
            )

            logger.info(f"Container '{container_name}' is running on network '{target_network}' (ID: {container.short_id})")
            return container

        except docker.errors.DockerException as e:
            raise ContainerRunnerError(f"Failed to run container '{container_name}': {e}")