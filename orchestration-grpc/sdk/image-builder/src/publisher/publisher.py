import docker
import os
import sys
from logger import get_logger

logger = get_logger("image-builder")

class Publisher:
    def __init__(self):
        try:
            self.client = docker.from_env()
        except Exception as e:
            raise RuntimeError(f"Failed to connect to Docker: {e}")

    def publish(self, repository_url, local_image_name):
        """
        local_image_name: the alias used in build (e.g., 'sum')
        docker_user: your DockerHub username
        """
        # DockerHub format: username/repository:tag
        remote_tag = f"{repository_url}:{local_image_name}"
        
        try:
            # 1. Find the local image and tag it for DockerHub
            image = self.client.images.get(local_image_name)
            image.tag(remote_tag)
            logger.info(f"Tagged {local_image_name} as {remote_tag}")

            # 2. Push to DockerHub
            logger.info("Pushing to DockerHub...")
            # We don't specify a registry URL here because DockerHub is the default
            for line in self.client.images.push(remote_tag, stream=True, decode=True):
                if 'status' in line:
                    logger.debug(f"{local_image_name}: {line['status']}")
                if 'error' in line:
                    raise Exception(f"Push error: {line['error']}")

            logger.info(f"Successfully published to DockerHub: {remote_tag}")

        except Exception as e:
            logger.error(f"Failed to publish {local_image_name}: {e}")
            raise