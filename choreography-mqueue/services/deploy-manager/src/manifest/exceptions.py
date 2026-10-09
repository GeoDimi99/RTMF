class ParserError(Exception):
    """Base class for all exceptions in the Parser module."""
    pass

class ManifestNotFoundError(ParserError):
    """Raised when the YAML manifest file is missing."""
    pass