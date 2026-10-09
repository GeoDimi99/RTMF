import yaml
import os
from .exceptions import ManifestNotFoundError, ManifestValidationError

class DSL_Parser:
    def __init__(self, file_path):
        self.file_path = file_path

    def parse(self):
        if not os.path.exists(self.file_path):
            raise ManifestNotFoundError(f"Manifest file not found: {self.file_path}")

        try:
            with open(self.file_path, 'r') as file:
                data = yaml.safe_load(file)
        except yaml.YAMLError as e:
            raise ManifestValidationError(f"Invalid YAML syntax: {e}")

        return data