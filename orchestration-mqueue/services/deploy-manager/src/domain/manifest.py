# domain/manifest.py
from dataclasses import dataclass
from .images import Images
from .schedule import Schedule


@dataclass
class Manifest:
    images: Images
    schedule: Schedule
