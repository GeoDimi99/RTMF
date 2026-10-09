import argparse
import os
import sys
from pathlib import Path
from dotenv import load_dotenv
from publisher.publisher import Publisher
from dslparser.parser import DSL_Parser
from dslparser.exceptions import ParserError, ContextDirectoryError
from builder.builder import Builder
from logger import get_logger

logger = get_logger("image-builder")


def main():

    # Init phase: take input arguments from the user or set the default parameters
    parser = argparse.ArgumentParser(
        description="Image Builder CLI: Compiles microservice images based on a Model-Driven manifest."
    )

    parser.add_argument(
        "-f", "--file-manifest", 
        default="manifest.yaml",
        metavar="FILE",
        help="Path to the manifest file containing image aliases and task definitions (default: manifest.yaml)"
    )

    parser.add_argument(
        "-c", "--context", 
        default=".", 
        metavar="PATH",
        help="Directory context containing the source code for each microservice (default: current directory)"
    )

    args = parser.parse_args()

    # Init phase : set all builder variables 
    file_manifest = args.file_manifest
    context = args.context
    base_dir = os.path.dirname(__file__)
    env_path = Path(base_dir) / ".env"
    load_dotenv(dotenv_path=env_path)

    docker_user = os.getenv("DOCKER_USER")
    docker_password = os.getenv("DOCKER_TOKEN")

    

    # Parsing phase : read the manifest file
    try:
        dslparser = DSL_Parser(args.file_manifest)
        data = dslparser.parse()
        images = data.get('images', {})
        base_image = images.get('base', '')
        repository_url = images.get('repo', '')
        tasks = images.get('tasks', [])
        

        # Check if the 'src' directories exist relative to the context
        for img in tasks:
            src_path = os.path.join(args.context, img['src'])
            if not os.path.isdir(src_path):
                # We raise this here because main knows about the context path
                raise ContextDirectoryError(f"Source path '{src_path}' for alias '{img['alias']}' does not exist.")
        
        logger.info("Manifest validated. Ready to build.")

    except ParserError as e:
        # Professional CLI error reporting
        logger.error(f"Build Error: {e}")
        sys.exit(1)

    except Exception as e:
        # Catch-all for unexpected crashes
        logger.error(f"An unexpected internal error occurred: {e}")
        sys.exit(1)


    # --- Building phase ---
    logger.info(f"Starting build process for {len(images)} services...")
    
    image_builder = Builder(base_dir=base_dir, context_path=args.context, base_image=base_image)

    for img in tasks:
        alias = img['alias']  
        src_dir = img['src']  
        
        try:

            dockerfile_path = image_builder.generate_dockerfile(alias, src_dir)
            image_builder.run_build(image_tag=alias, dockerfile_path=dockerfile_path)
            
        except Exception as e:
            logger.error(f"Error building {alias}: {e}")
            continue
    
    # --- Publishing phase ---
    # publisher = Publisher()

    # for img in tasks:
    #     alias = img['alias']
    #     try:
    #         # Push using the DockerHub username
    #         publisher.publish(
    #             repository_url=repository_url, 
    #             local_image_name=alias
    #         )
    #     except Exception as e:
    #         logger.error(f"Skipping push for {alias} due to error.")


    

if __name__ == "__main__":
    main()