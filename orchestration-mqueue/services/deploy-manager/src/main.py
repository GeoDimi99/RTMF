from pathlib import Path
from .manifest.parser import ManifestParser
from .manifest.exceptions import ParserError
from .deploy.runner import DockerContainerRunner
from .deploy.exceptions import ContainerRunnerError
from .redisdb.redis_loader import RedisLoader  # NEW
from .redisdb.exceptions import RedisLoaderError
from .exceptions import DeployManagerError
from .logger import get_logger
import sys

logger = get_logger("deploy-manager")


def main():

    # Init phase 
    mission_path = Path("/tmp")
    # mission_path = Path("/home/vboxuser/projects/RT-microservices-choreography-pe/tests/test_0_code")

    # Parse manifest phase
    parser = ManifestParser(str(mission_path / "manifest.yaml"))
    try:
        manifest = parser.parse()
    except ParserError as e:
        logger.error(f"Deploy Manager failed: {e}")
        sys.exit(1)


    # Container Running phase - driven by the 'images:' section of the DSL
    docker_runner = DockerContainerRunner()
    try:
        # Run one task-wrapper container per declared image
        for image in manifest.images.tasks:
            logger.info(f"Processing image '{image.alias}'")

            # Run container
            docker_runner.run_task_service(
                image_tag=image.alias,
                container_name=image.alias,
            )
    except ContainerRunnerError as e:
        logger.error(f"Deploy Manager failed: {e}")
        sys.exit(1)

    # Redis Loading phase - driven by the 'schedule:' section of the DSL
    redis_loader = RedisLoader(host="redis", port=6379)

    try:
        redis_loader.load_schedule(manifest.schedule)
        redis_loader.debug_dump()  # REMOVE AFTER DEBUGGING
    except RedisLoaderError as e:
        logger.error(f"Deploy Manager failed: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
