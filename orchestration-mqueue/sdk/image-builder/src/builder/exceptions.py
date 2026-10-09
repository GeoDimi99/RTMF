
class BuilderError(Exception):
    """Base class for all exceptions in the Builder module."""
    pass


class DockerConnectionError(BuilderError):
    """Docker client not connect to Docker daemon."""
    pass
