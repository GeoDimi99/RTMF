# src/deploy/redis_loader.py
import redis
import json
from ..logger import get_logger
from ..domain.schedule import Schedule

logger = get_logger("deploy-manager")

class RedisLoader:
    """
    Loads schedule/task data into Redis for debugging purposes.
    """

    def __init__(self, host="localhost", port=6379, db=0):
        self.client = redis.Redis(host=host, port=port, db=db, decode_responses=True)

    def load_schedule(self, schedule: Schedule, image_to_port: dict = None):
        """
        Push schedule and tasks into Redis.
        
        Args:
            schedule: Schedule object with tasks
            image_to_port: Mapping from image name to gRPC port (e.g., {"sum": 50051})
        """
        self.client.hset("schedule", mapping={
            "name": schedule.name,
            "version": schedule.version,
            "description": schedule.description,
            "length": str(len(schedule.tasks)),
            "iterations": str(schedule.iterations)
        })

        for i, task in enumerate(schedule.tasks, start=1):
            task_key = f"scheduletask:{i}"
            task_data = {
                "name": task.name,
                "policy": task.policy,
                "priority": str(task.priority),
                "depends_on": json.dumps(task.depends_on),
                "inputs": json.dumps(task.inputs),
                "outputs": json.dumps(task.outputs),
            }
            # Add CPU affinity if present
            if task.cpu_affinity is not None:
                task_data["cpu_affinity"] = str(task.cpu_affinity)

            # Add start time if present
            if task.start is not None:
                task_data["start"] = str(task.start)
            
            # Add deadline if present
            if task.deadline is not None:
                task_data["deadline"] = str(task.deadline)
            
            # Add service port based on task image name
            if image_to_port and task.name in image_to_port:
                task_data["service_port"] = str(image_to_port[task.name])
            else:
                task_data["service_port"] = "50051"  # Default fallback
            
            self.client.hset(task_key, mapping=task_data)


        logger.info(f"Loaded {len(schedule.tasks)} tasks into Redis.")

    def debug_dump(self):
        """
        Read back all schedule/task data and print for debugging.
        """
        schedule_data = self.client.hgetall("schedule")
        logger.debug("=== SCHEDULE ===")
        for k, v in schedule_data.items():
            logger.debug(f"{k}: {v}")

        logger.debug("=== TASKS ===")
        length = int(schedule_data.get("length", 0))
        for i in range(1, length + 1):
            task_key = f"scheduletask:{i}"
            task_data = self.client.hgetall(task_key)
            logger.debug(f"{task_key}: {task_data}")
