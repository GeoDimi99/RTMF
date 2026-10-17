from pathlib import Path
from .logger import get_logger
from .manifest.parser import ManifestParser
from .deploy.runner import DockerContainerRunner
from .database.redis_loader import RedisLoader
from .exceptions import DeployManagerError
import sys
import socket
import time

logger = get_logger("deploy-manager")

# gRPC runs over Unix domain sockets (like the ipc:// endpoints of choreography-zeromq):
# loopback TCP is received by ksoftirqd (SCHED_OTHER) on the PREEMPT_RT kernel, so a task
# computing on the core of the middleware delayed every request/result until it ended.
# The task services and the execution manager share this directory (see runner.py and
# docker-compose.yml); the port number only identifies the socket of each task service.
GRPC_IPC_DIR = "/ipc"


def grpc_socket_path(port):
    return f"{GRPC_IPC_DIR}/task_{port}.sock"


def wait_for_grpc_ready(socket_path, timeout=30, retry_interval=0.5):
    """
    Wait for gRPC server to be ready by attempting to connect to its Unix socket.
    """
    logger.info(f"Waiting for gRPC server at unix://{socket_path} to be ready...")
    start_time = time.time()

    while time.time() - start_time < timeout:
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.settimeout(1)
            result = sock.connect_ex(socket_path)
            sock.close()

            if result == 0:
                logger.info(f"gRPC server at unix://{socket_path} is ready!")
                return True
        except Exception as e:
            pass

        time.sleep(retry_interval)

    logger.warning(f"gRPC server at unix://{socket_path} not ready after {timeout}s")
    return False

def main():
    # Use local manifest instead of cloning from GitHub
    manifest_path = Path("/app/tests/test_0_code/manifest.yaml")

    task_service_path = Path("/app/task-wrapper")
    task_service_include = task_service_path / "include"

    docker_runner = DockerContainerRunner()
    # With host networking, use localhost instead of service name
    redis_loader = RedisLoader(host="localhost", port=6379)

    try:
        # Parse manifest
        parser = ManifestParser(str(manifest_path))
        manifest = parser.parse()

        # Images to deploy come from the 'images:' section of the DSL.
        # Multiple schedule tasks can share the same image (container), each execution creates a new thread
        image_aliases = [image.alias for image in manifest.images.tasks]

        logger.info(f"Found {len(image_aliases)} task image(s) for {len(manifest.schedule.tasks)} task(s)")

        # Assign incremental ports to each image (50051, 50052, 50053, ...)
        BASE_PORT = 50051
        image_to_port = {}
        for idx, image_name in enumerate(image_aliases):
            image_to_port[image_name] = BASE_PORT + idx

        # Run ONE container per declared image
        for image_name in image_aliases:
            port = image_to_port[image_name]
            logger.info(f"Deploying container for image '{image_name}' on port {port}")

            container_name = f"task-service-{image_name}"
            docker_runner.run_task_service(
                image_tag=image_name,
                container_name=container_name,
                grpc_port=port,
            )

            # Wait for gRPC server to be ready on the socket of the assigned port
            socket_path = grpc_socket_path(port)
            if not wait_for_grpc_ready(socket_path, timeout=30):
                raise DeployManagerError(f"gRPC server for '{container_name}' failed to become ready on {socket_path}")

        # Load schedule and tasks into Redis - driven by the 'schedule:' section of the DSL
        # This loads ALL tasks, even if they share the same image/container
        # Pass image_to_port mapping so each task knows which port to connect to
        redis_loader.load_schedule(manifest.schedule, image_to_port=image_to_port)
        redis_loader.debug_dump()  # REMOVE AFTER DEBUGGING
        logger.info("Schedule data loaded into Redis successfully.")


    except DeployManagerError as e:
        logger.error(f"Deploy Manager failed: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
