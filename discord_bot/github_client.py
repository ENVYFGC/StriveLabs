"""GitHub API integration for reading/writing combo data files."""

import json
import base64
import logging
from github import Github, GithubException

logger = logging.getLogger(__name__)


class GitHubComboClient:
    def __init__(self, token: str, repo_name: str):
        self.github = Github(token)
        self.repo = self.github.get_repo(repo_name)

    def load_character_registry(self) -> dict:
        """Fetch and parse _characters.json from the repo."""
        try:
            contents = self.repo.get_contents("combo_data/_characters.json", ref="main")
            return json.loads(base64.b64decode(contents.content))
        except GithubException as e:
            logger.error(f"Failed to load _characters.json: {e}")
            return {"characters": []}

    def get_character_ids(self) -> set[str]:
        """Get set of valid character IDs from registry."""
        registry = self.load_character_registry()
        return {c["id"] for c in registry.get("characters", []) if "id" in c}

    def get_character_choices(self) -> list[tuple[str, str]]:
        """Get (id, name) pairs for autocomplete."""
        registry = self.load_character_registry()
        return [(c["id"], c["name"]) for c in registry.get("characters", [])
                if "id" in c and "name" in c]

    def read_character_file(self, char_id: str) -> tuple[dict | None, str | None]:
        """Read combo_data/{char_id}.json. Returns (data, sha) or (None, None)."""
        path = f"combo_data/{char_id}.json"
        try:
            contents = self.repo.get_contents(path, ref="main")
            data = json.loads(base64.b64decode(contents.content))
            return data, contents.sha
        except GithubException as e:
            if e.status == 404:
                return None, None
            raise

    def push_combo(self, char_id: str, version: str, starter: str,
                   combo_entry: dict, contributor: str, max_retries: int = 3) -> str:
        """Add a combo to the character file on GitHub. Returns commit URL.

        Handles conflicts by retrying the read-modify-write cycle.
        Creates the file if it doesn't exist.
        """
        path = f"combo_data/{char_id}.json"
        message = f"Add {starter} combo for {char_id} (by {contributor})"

        for attempt in range(max_retries):
            try:
                data, sha = self.read_character_file(char_id)

                if data is None:
                    # Create new file
                    data = {
                        version: {
                            starter: {
                                "note": f"Combos with {starter} as starter",
                                "combos": [combo_entry],
                            }
                        }
                    }
                    result = self.repo.create_file(
                        path=path,
                        message=message,
                        content=json.dumps(data, indent=4, ensure_ascii=False),
                        branch="main",
                    )
                    return result["commit"].html_url
                else:
                    # Update existing file
                    data.setdefault(version, {})
                    data[version].setdefault(starter, {
                        "note": f"Combos with {starter} as starter",
                        "combos": [],
                    })
                    data[version][starter]["combos"].append(combo_entry)

                    result = self.repo.update_file(
                        path=path,
                        message=message,
                        content=json.dumps(data, indent=4, ensure_ascii=False),
                        sha=sha,
                        branch="main",
                    )
                    return result["commit"].html_url

            except GithubException as e:
                if e.status == 409 and attempt < max_retries - 1:
                    logger.warning(f"Conflict on {path}, retrying ({attempt + 1}/{max_retries})")
                    continue
                raise

        raise RuntimeError(f"Failed to push combo after {max_retries} retries")
