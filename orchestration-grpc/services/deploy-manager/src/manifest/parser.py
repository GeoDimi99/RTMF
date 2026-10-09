# src/manifest/parser.py
from typing import List
import yaml
from ..exceptions import DeployManagerError
from ..logger import get_logger
from ..domain.task import Task
from ..domain.schedule import Schedule
from ..domain.images import ImageTask, Images
from ..domain.manifest import Manifest

logger = get_logger("deploy-manager")

class ManifestParser:
    # All Linux scheduler policies supported by the task-wrapper's mapping
    VALID_POLICIES = {"other", "normal", "batch", "idle", "fifo", "rr", "deadline"}

    def __init__(self, manifest_path: str):
        self.manifest_path = manifest_path

    def parse(self) -> Manifest:
        """
        Parse a YAML manifest file and return a Manifest composed of:
          - Images: the task-wrapper images to build/run ('images' section)
          - Schedule: the real-time schedule to upload to Redis ('schedule' section)
        """
        try:
            with open(self.manifest_path, "r") as f:
                data = yaml.safe_load(f)
        except FileNotFoundError:
            raise DeployManagerError(f"Manifest file not found: {self.manifest_path}")
        except yaml.YAMLError as e:
            raise DeployManagerError(f"Error parsing YAML manifest: {e}")

        # Validate top-level fields
        if "images" not in data:
            raise DeployManagerError("Manifest missing required field: 'images'")
        if "schedule" not in data:
            raise DeployManagerError("Manifest missing required field: 'schedule'")

        images = self._parse_images(data["images"])

        schedule_data = data["schedule"]
        tasks_data = schedule_data.get("tasks", [])

        # Parse tasks into domain Task objects
        tasks: List[Task] = []
        for t in tasks_data:
            try:
                # Map the new manifest format to Task domain object
                task = Task(
                    name=t["image"],  # Use image name as task name
                    policy=t.get("policy", "fifo").lower(),  # Default to fifo if not specified
                    priority=int(t.get("priority", 50)),  # Default priority 50
                    cpu_affinity=int(t["cpu_affinity"]) if t.get("cpu_affinity") is not None else None,
                    start=t.get("start"),  # Start time in milliseconds (optional)
                    deadline=t.get("deadline"),  # Absolute deadline in milliseconds (optional)
                    depends_on=t.get("depends_on", []),
                    inputs=t.get("inputs", {}),
                    outputs=t.get("outputs", {}),
                )
            except KeyError as e:
                raise DeployManagerError(f"Task missing required field: {e}")

            # Validate policy
            if task.policy not in self.VALID_POLICIES:
                raise DeployManagerError(f"Invalid policy '{task.policy}' in task {task.name}")

            # Validate that the task references a known image alias
            if task.name not in images.aliases():
                raise DeployManagerError(
                    f"Task '{task.name}' references unknown image alias '{task.name}'; "
                    f"expected one of {sorted(images.aliases())} (see 'images.tasks')"
                )

            tasks.append(task)

        # Build Schedule domain object
        schedule = Schedule(
            name=schedule_data.get("name", "unnamed"),
            version=schedule_data.get("version", "0.0.0"),
            description=schedule_data.get("description", ""),
            tasks=tasks,
            iterations=int(schedule_data.get("iterations", 1))
        )

        logger.info(f"Parsed schedule '{schedule.name}' with {len(tasks)} tasks.")
        return Manifest(images=images, schedule=schedule)

    def _parse_images(self, images_data: dict) -> Images:
        """
        Parse the 'images' section into an Images domain object.
        """
        tasks_data = images_data.get("tasks", [])

        tasks: List[ImageTask] = []
        for img in tasks_data:
            try:
                tasks.append(ImageTask(alias=img["alias"], src=img["src"]))
            except KeyError as e:
                raise DeployManagerError(f"Image entry missing required field: {e}")

        if not tasks:
            raise DeployManagerError("Manifest field 'images.tasks' must contain at least one image")

        return Images(
            base=images_data.get("base", ""),
            repo=images_data.get("repo", ""),
            tasks=tasks,
        )
