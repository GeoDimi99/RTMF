# domain/task.py
from dataclasses import dataclass, field
from typing import Dict, List, Any

@dataclass
class Task:
    id: int
    image: str
    start: int
    deadline: int
    cpu_affinity : int
    policy: str
    priority: int
    depends_on: List[str] = field(default_factory=list)
    inputs: List[Dict[str, Any]] = field(default_factory=list)
    outputs: Dict[str, Any] = field(default_factory=dict)
