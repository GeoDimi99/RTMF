# src/manifest/parser.py
from typing import List
import yaml
from .exceptions import ManifestNotFoundError, ParserError
from ..domain.task import Task
from ..domain.schedule import Schedule
from ..domain.images import ImageTask, Images
from ..domain.manifest import Manifest


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
            raise ManifestNotFoundError(f"Manifest file not found: {self.manifest_path}")
        except yaml.YAMLError as e:
            raise ParserError(f"Error parsing YAML manifest: {e}")

        # Validate top-level fields
        if "images" not in data:
            raise ParserError("Manifest missing required field: 'images'")
        if "schedule" not in data:
            raise ParserError("Manifest missing required field: 'schedule'")

        images = self._parse_images(data["images"])

        schedule_data = data["schedule"]
        tasks_data = schedule_data.get("tasks", [])

        # Parse tasks into domain Task objects
        tasks: List[Task] = []
        for t in tasks_data:
            try:
                task = Task(
                    id=int(t["id"]),
                    image=t["image"],
                    start=int(t["start"]),
                    deadline=int(t["deadline"]),
                    cpu_affinity=int(t["cpu_affinity"]),
                    policy=t["policy"].lower(),
                    priority=int(t["priority"]),
                    inputs=[{k: v["value"] for k, v in t.get("inputs", {}).items()}],
                    outputs= {k: v["type"] for k, v in t.get("outputs", {}).items()} #t.get("outputs",{})
                )
            except KeyError as e:
                raise ParserError(f"Task missing required field: {e}")

            # Validate policy
            if task.policy not in self.VALID_POLICIES:
                raise ParserError(f"Invalid policy '{task.policy}' in task {task.id}")

            # Validate that the task references a known image alias
            if task.image not in images.aliases():
                raise ParserError(
                    f"Task {task.id} references unknown image alias '{task.image}'; "
                    f"expected one of {sorted(images.aliases())} (see 'images.tasks')"
                )

            tasks.append(task)

        # Build Schedule domain object
        schedule = Schedule(
            name=schedule_data.get("name", "unnamed"),
            version=schedule_data.get("version", "0.0.0"),
            description=schedule_data.get("description", ""),
            iterations=int(schedule_data.get("iterations", "-1")),
            tasks=tasks
        )

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
                raise ParserError(f"Image entry missing required field: {e}")

        if not tasks:
            raise ParserError("Manifest field 'images.tasks' must contain at least one image")

        return Images(
            base=images_data.get("base", ""),
            repo=images_data.get("repo", ""),
            tasks=tasks,
        )
