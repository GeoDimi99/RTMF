# domain/task.py
from dataclasses import dataclass, field
from typing import Dict, List, Any, Optional

@dataclass
class Task:
    name: str
    policy: str
    priority: int
    cpu_affinity: Optional[int] = None  # CPU core the task thread is pinned to (optional)
    start: Optional[int] = None  # Start time in seconds
    deadline: Optional[int] = None  # Absolute deadline in seconds from schedule start
    depends_on: List[str] = field(default_factory=list)
    inputs: Dict[str, Any] = field(default_factory=dict)
    outputs: Dict[str, Any] = field(default_factory=dict)
