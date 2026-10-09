# src/deploy/redis_loader.py
import redis
import json
from ..domain.schedule import Schedule
from ..logger import get_logger

logger = get_logger("deploy-manager")


class RedisLoader:
    """
    Loads schedule/task data into Redis for debugging purposes.
    """

    def __init__(self, host="localhost", port=6379, db=0):
        self.client = redis.Redis(host=host, port=port, db=db, decode_responses=True)

    def load_schedule(self, schedule: Schedule):
        """
        Push schedule and tasks into Redis.
        """
        self.client.hset("schedule", mapping={
            "name": schedule.name,
            "version": schedule.version,
            "description": schedule.description,
            "iterations": str(schedule.iterations),
            "length": str(len(schedule.tasks))
        })

        for i, task in enumerate(schedule.tasks, start=1):
            task_key = f"scheduletask:{i}"
            self.client.hset(task_key, mapping={
                "id": task.id,
                "image": task.image,
                "start": task.start,
                "deadline": task.deadline,
                "cpu_affinity": str(task.cpu_affinity),
                "policy": task.policy,
                "priority": str(task.priority),
                "depends_on": json.dumps(task.depends_on),
                "inputs": json.dumps(task.inputs),
                "outputs": json.dumps(task.outputs),
            })



    def debug_dump(self):
        """
        Read back all schedule/task data and log for debugging.
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
