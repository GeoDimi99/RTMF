# src/manifest/manifest_fetcher.py
from pathlib import Path
from git import Repo, GitCommandError
from ..logger import get_logger
from ..exceptions import DeployManagerError

logger = get_logger("deploy-manager")

class ManifestFetcher:

    def __init__(self, url: str, local_path: Path):
        self.url = url
        self.local_path = local_path

    def fetch(self) -> None:
        """
        Clone the repository if it doesn't exist, otherwise pull the latest changes.
        """
        try:
            if self.local_path.exists():
                logger.info(f"Repository exists at {self.local_path}, pulling latest changes...")
                repo = Repo(self.local_path)
                origin = repo.remotes.origin
                origin.pull()
                logger.info("Repository updated successfully.")
            else:
                logger.info(f"Cloning repository from {self.url} to {self.local_path}...")
                Repo.clone_from(self.url, self.local_path)
                logger.info("Repository cloned successfully.")

        except GitCommandError as e:
            raise DeployManagerError(f"Git error: {e}")
