import redis
import json
from collections import defaultdict
from ..domain.schedule import Schedule
from ..logger import get_logger

logger = get_logger("deploy-manager")

class RedisLoader:
    def __init__(self, host="localhost", port=6379, db=0):
        # We use a single dictionary mapping for a flat Redis structure
        self.client = redis.Redis(host=host, port=port, db=db, decode_responses=True)

    def load_schedule(self, schedule: Schedule):
        """
        Push schedule and tasks into Redis using a flat key-value format.
        """
        # 1. Group tasks by image and find schedule metrics
        tasks_by_image = defaultdict(list)
        max_deadline = 0
        earliest_start = float('inf')
        leader_image = ""

        unique_images = list(set(task.image for task in schedule.tasks))

        for task in schedule.tasks:
            tasks_by_image[task.image].append(task)
            
            # Calculate duration (latest deadline)
            if task.deadline > max_deadline:
                max_deadline = task.deadline
            
            # Identify leader (earliest start time)
            if task.start < earliest_start:
                earliest_start = task.start
                leader_image = task.image

        # 2. Prepare the flat mapping
        # Using a dictionary to send all updates in one hset call
        payload = {
            "schedule:name": schedule.name,
            "schedule:version": schedule.version,
            "schedule:description": schedule.description,
            "schedule:length": str(len(unique_images)),
            "schedule:duration": str(max_deadline),
            "schedule:images": json.dumps(unique_images),
            "schedule:leader": leader_image,
            "schedule:iterations": str(schedule.iterations)
        }

        # 3. Add image-specific task data
        for img_name, tasks in tasks_by_image.items():
            # Add length of tasks for this specific image
            payload[f"schedule:{img_name}:length"] = str(len(tasks))
            
            # Add each task as a JSON string under its index
            for idx, task in enumerate(tasks):
                task_dict = {
                    "id": task.id,
                    "start": task.start,
                    "deadline": task.deadline,
                    "cpu_affinity": str(task.cpu_affinity),
                    "policy": task.policy,
                    "priority": str(task.priority),
                    "depends_on": json.dumps(task.depends_on),
                    "inputs": json.dumps(task.inputs),
                    "outputs": json.dumps(task.outputs),
                }
                payload[f"schedule:{img_name}:{idx}"] = json.dumps(task_dict)

        # 4. Write to Redis (using a Hash named 'schedule_flat' or similar)
        # Note: If you want these as top-level Redis keys (not a Hash), 
        # use self.client.mset(payload) instead.
        self.client.hset("schedule_data", mapping=payload)

    def debug_log(self):
        """
        Read back the flat structure and print.
        """
        data = self.client.hgetall("schedule_data")
        logger.debug("=== FLAT REDIS SCHEDULE ===")
        for key in sorted(data.keys()):
            logger.debug(f"{key}: {data[key]}")