# domain/images.py
from dataclasses import dataclass, field
from typing import List, Set


@dataclass
class ImageTask:
    alias: str
    src: str


@dataclass
class Images:
    base: str
    repo: str
    tasks: List[ImageTask] = field(default_factory=list)

    def aliases(self) -> Set[str]:
        return {t.alias for t in self.tasks}
