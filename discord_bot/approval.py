"""Approval workflow — pending queue with Approve/Reject buttons."""

import json
import logging
from pathlib import Path

import discord
from discord.ui import View, Button

from github_client import GitHubComboClient
from validation import build_combo_entry

logger = logging.getLogger(__name__)

PENDING_FILE = Path(__file__).parent / "pending.json"


def _load_pending() -> dict:
    if PENDING_FILE.exists():
        try:
            return json.loads(PENDING_FILE.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError):
            pass
    return {}


def _save_pending(data: dict):
    PENDING_FILE.write_text(json.dumps(data, indent=2, ensure_ascii=False), encoding="utf-8")


def add_pending(message_id: str, submission: dict):
    """Store a pending submission keyed by the review message ID."""
    pending = _load_pending()
    pending[message_id] = submission
    _save_pending(pending)


def remove_pending(message_id: str) -> dict | None:
    """Remove and return a pending submission."""
    pending = _load_pending()
    submission = pending.pop(message_id, None)
    _save_pending(pending)
    return submission


def get_pending(message_id: str) -> dict | None:
    pending = _load_pending()
    return pending.get(message_id)


def build_submission_embed(submission: dict) -> discord.Embed:
    """Build a rich embed for the review channel."""
    char_name = submission.get("character_name", submission["character_id"])
    embed = discord.Embed(
        title=f"Combo Submission — {char_name}",
        color=discord.Color.gold(),
    )
    embed.add_field(name="Version", value=submission["version"], inline=True)
    embed.add_field(name="Starter", value=submission["starter"], inline=True)
    embed.add_field(name="Contributor", value=submission["contributor"], inline=True)
    embed.add_field(name="Notation", value=f"```{submission['notation'][:1000]}```", inline=False)
    embed.add_field(name="Notes", value=submission["notes"], inline=False)

    if submission.get("link"):
        embed.add_field(name="Video", value=submission["link"], inline=False)

    tags = submission.get("tags", [])
    if tags:
        embed.add_field(name="Tags", value=" ".join(f"`{t}`" for t in tags), inline=False)

    embed.set_footer(text="Pending review")
    return embed


class ApprovalView(View):
    """Persistent view with Approve/Reject buttons."""

    def __init__(self, github_client: GitHubComboClient):
        super().__init__(timeout=None)
        self.github_client = github_client

    @discord.ui.button(label="Approve", style=discord.ButtonStyle.green, custom_id="combo_approve")
    async def approve(self, interaction: discord.Interaction, button: Button):
        message_id = str(interaction.message.id)
        submission = remove_pending(message_id)

        if not submission:
            await interaction.response.send_message("Submission not found or already handled.", ephemeral=True)
            return

        try:
            combo_entry = build_combo_entry(
                notation=submission["notation"],
                notes=submission["notes"],
                link=submission.get("link", ""),
                contributor=submission["contributor"],
            )
            commit_url = self.github_client.push_combo(
                char_id=submission["character_id"],
                version=submission["version"],
                starter=submission["starter"],
                combo_entry=combo_entry,
                contributor=submission["contributor"],
            )

            embed = interaction.message.embeds[0] if interaction.message.embeds else None
            if embed:
                embed.color = discord.Color.green()
                embed.set_footer(text=f"Approved by {interaction.user.display_name}")
                await interaction.message.edit(embed=embed, view=None)

            await interaction.response.send_message(
                f"Combo approved and pushed to GitHub.\n{commit_url}",
                ephemeral=False,
            )
        except Exception as e:
            logger.error(f"Failed to push approved combo: {e}")
            # Re-add to pending since it failed
            add_pending(message_id, submission)
            await interaction.response.send_message(
                f"Failed to push to GitHub: {e}",
                ephemeral=True,
            )

    @discord.ui.button(label="Reject", style=discord.ButtonStyle.red, custom_id="combo_reject")
    async def reject(self, interaction: discord.Interaction, button: Button):
        message_id = str(interaction.message.id)
        submission = remove_pending(message_id)

        if not submission:
            await interaction.response.send_message("Submission not found or already handled.", ephemeral=True)
            return

        embed = interaction.message.embeds[0] if interaction.message.embeds else None
        if embed:
            embed.color = discord.Color.red()
            embed.set_footer(text=f"Rejected by {interaction.user.display_name}")
            await interaction.message.edit(embed=embed, view=None)

        await interaction.response.send_message(
            f"Combo submission rejected.",
            ephemeral=False,
        )
