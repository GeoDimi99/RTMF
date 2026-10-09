import os
import docker
from string import Template
from logger import get_logger

logger = get_logger("image-builder")


class Builder:

    def __init__(self, base_dir, context_path, base_image):
        self.context_path = context_path
        self.base_image = base_image
        self.template_path = os.path.join(base_dir, "builder", "template", "Dockerfile.j2")
        
        try:
            # Connect to the local Docker daemon using environment variables
            self.client = docker.from_env()
        except Exception as e:
            raise RuntimeError(f"Could not connect to Docker daemon: {e}")

    def _get_template(self):
        if not os.path.exists(self.template_path):
            raise FileNotFoundError(f"Template not found at: {self.template_path}")
        with open(self.template_path, "r") as f:
            return Template(f.read())

    def generate_dockerfile(self, service_name, source_dir):
        """Generates a Dockerfile inside the specific microservice directory."""
        template = self._get_template()
        content = template.substitute(
            base_image=self.base_image,
            source_dir=source_dir
        )
        
        # Place Dockerfile inside the source directory (e.g., test-0/sum/Dockerfile.sum)
        target_dir = os.path.join(self.context_path, source_dir)
        df_name = f"Dockerfile.{service_name}"
        df_path = os.path.join(target_dir, df_name)
        
        with open(df_path, "w") as f:
            f.write(content)
        return df_path

    def run_build(self, image_tag, dockerfile_path):
        """Builds the image using the Docker SDK."""
        logger.info(f"--- Starting Build: {image_tag} ---")

        # The SDK needs the Dockerfile path relative to the build context
        relative_df_path = os.path.relpath(dockerfile_path, self.context_path)

        try:
            # .build() returns a generator for the log output
            # 'path' is the build context (where all microservice folders live)
            logger.debug(f"context: {self.context_path}")
            generator = self.client.api.build(
                path=self.context_path,
                dockerfile=relative_df_path,
                tag=image_tag,
                decode=True,
                rm=True,  # Automatically remove intermediate containers
                pull=False
            )

            for chunk in generator:
                if 'stream' in chunk:
                    # Log build/compilation output
                    stream_line = chunk['stream'].strip()
                    if stream_line:
                        logger.debug(stream_line)
                elif 'error' in chunk:
                    raise Exception(f"Docker Build Error: {chunk['error']}")

            logger.info(f"--- Successfully built {image_tag} ---")

        except Exception as e:
            logger.error(f"Build failed: {e}")
            raise
        finally:
            # Clean up: Remove the generated Dockerfile
            if os.path.exists(dockerfile_path):
                os.remove(dockerfile_path)