# src/domain/schedule.py
from dataclasses import dataclass
from typing import List
from .task import Task  # import from domain

@dataclass
class Schedule:
    name: str
    version: str
    description: str
    iterations: int
    tasks: List[Task]
