import os
from dotenv import load_dotenv

load_dotenv()

DISCORD_BOT_TOKEN = os.getenv("DISCORD_BOT_TOKEN", "")
GITHUB_TOKEN = os.getenv("GITHUB_TOKEN", "")
GITHUB_REPO = os.getenv("GITHUB_REPO", "")  # e.g. "username/ggst-combo-data"
SUBMISSION_CHANNEL_ID = int(os.getenv("SUBMISSION_CHANNEL_ID", "0"))
AUTO_PUBLISH = os.getenv("AUTO_PUBLISH", "false").lower() == "true"

# Path to _characters.json for autocomplete (fetched from GitHub on startup)
VALID_TAGS = {"corner", "midscreen", "meterless", "meter", "ch", "wallsplat"}
