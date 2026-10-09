
class ParserError(Exception):
    """Base class for all exceptions in the Parser module."""
    pass

class ManifestNotFoundError(ParserError):
    """Raised when the YAML manifest file is missing."""
    pass

class ManifestValidationError(ParserError):
    """Raised when the YAML is valid but the logic/schema is wrong."""
    pass

class ContextDirectoryError(ParserError):
    """Raised when a source directory defined in the manifest is missing."""
    pass