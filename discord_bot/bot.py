"""GGST Combo Submission Bot — Discord bot for community combo contributions."""

import logging
import discord
from discord import app_commands
from discord.ext import commands

from config import (
    DISCORD_BOT_TOKEN,
    GITHUB_TOKEN,
    GITHUB_REPO,
    SUBMISSION_CHANNEL_ID,
    AUTO_PUBLISH,
)
from github_client import GitHubComboClient
from combo_modal import ComboSubmitModal
from validation import validate_combo, build_combo_entry, auto_tag
from approval import ApprovalView, add_pending, build_submission_embed

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

intents = discord.Intents.default()
bot = commands.Bot(command_prefix="!", intents=intents)

github_client: GitHubComboClient | None = None
character_choices: list[tuple[str, str]] = []  # (id, name) pairs


@bot.event
async def on_ready():
    global github_client, character_choices

    logger.info(f"Bot ready as {bot.user}")

    # Init GitHub client and load character list
    if GITHUB_TOKEN and GITHUB_REPO:
        github_client = GitHubComboClient(GITHUB_TOKEN, GITHUB_REPO)
        character_choices = github_client.get_character_choices()
        logger.info(f"Loaded {len(character_choices)} characters from GitHub")
    else:
        logger.warning("GITHUB_TOKEN or GITHUB_REPO not set — running in dry-run mode")

    # Register persistent approval buttons
    if github_client:
        bot.add_view(ApprovalView(github_client))

    # Sync slash commands
    try:
        synced = await bot.tree.sync()
        logger.info(f"Synced {len(synced)} commands")
    except Exception as e:
        logger.error(f"Failed to sync commands: {e}")


@bot.tree.command(name="addcombo", description="Submit a combo to the community database")
@app_commands.describe(character="Character name")
async def addcombo(interaction: discord.Interaction, character: str):
    """Open the combo submission modal for a character."""
    # Validate character
    char_id = None
    char_name = None
    for cid, cname in character_choices:
        if cid == character or cname.lower() == character.lower():
            char_id = cid
            char_name = cname
            break

    if not char_id:
        await interaction.response.send_message(
            f"Unknown character: `{character}`. Use autocomplete to pick from the roster.",
            ephemeral=True,
        )
        return

    modal = ComboSubmitModal(
        character_id=char_id,
        character_name=char_name,
        callback=handle_combo_submit,
    )
    await interaction.response.send_modal(modal)


@addcombo.autocomplete("character")
async def character_autocomplete(
    interaction: discord.Interaction, current: str
) -> list[app_commands.Choice[str]]:
    """Autocomplete character names from the registry."""
    current_lower = current.lower()
    matches = []
    for cid, cname in character_choices:
        if current_lower in cname.lower() or current_lower in cid:
            matches.append(app_commands.Choice(name=cname, value=cid))
        if len(matches) >= 25:  # Discord limit
            break
    return matches


async def handle_combo_submit(
    interaction: discord.Interaction, *,
    character_id: str, character_name: str,
    version: str, starter: str,
    notation: str, notes: str, link: str,
    contributor: str,
):
    """Called when the modal is submitted."""
    # Validate
    char_ids = {cid for cid, _ in character_choices}
    errors = validate_combo(notation, notes, link, char_ids, character_id)
    if errors:
        await interaction.response.send_message(
            "Validation errors:\n" + "\n".join(f"- {e}" for e in errors),
            ephemeral=True,
        )
        return

    # Build entry
    combo_entry = build_combo_entry(notation, notes, link, contributor)
    tags = auto_tag(notation, notes)

    submission = {
        "character_id": character_id,
        "character_name": character_name,
        "version": version.strip(),
        "starter": starter.strip(),
        "notation": notation.strip(),
        "notes": notes.strip(),
        "link": link.strip() if link else "",
        "contributor": contributor,
        "tags": tags,
    }

    if AUTO_PUBLISH and github_client:
        # Direct publish mode
        try:
            commit_url = github_client.push_combo(
                char_id=character_id,
                version=version.strip(),
                starter=starter.strip(),
                combo_entry=combo_entry,
                contributor=contributor,
            )
            await interaction.response.send_message(
                f"Combo submitted and published!\n{commit_url}",
                ephemeral=False,
            )
        except Exception as e:
            logger.error(f"Failed to push combo: {e}")
            await interaction.response.send_message(
                f"Combo validated but failed to publish: {e}",
                ephemeral=True,
            )
        return

    # Approval mode — post to submissions channel
    if not SUBMISSION_CHANNEL_ID:
        await interaction.response.send_message(
            "Combo validated! But no submission channel is configured. "
            "Ask an admin to set SUBMISSION_CHANNEL_ID.",
            ephemeral=True,
        )
        return

    channel = bot.get_channel(SUBMISSION_CHANNEL_ID)
    if not channel:
        await interaction.response.send_message(
            "Submission channel not found. Ask an admin to check the bot config.",
            ephemeral=True,
        )
        return

    embed = build_submission_embed(submission)
    view = ApprovalView(github_client) if github_client else None
    review_msg = await channel.send(embed=embed, view=view)

    # Store pending submission keyed by message ID
    add_pending(str(review_msg.id), submission)

    await interaction.response.send_message(
        f"Combo submitted for review in <#{SUBMISSION_CHANNEL_ID}>!",
        ephemeral=True,
    )


def main():
    if not DISCORD_BOT_TOKEN:
        logger.error("DISCORD_BOT_TOKEN not set. Create a .env file from .env.example")
        return
    bot.run(DISCORD_BOT_TOKEN)


if __name__ == "__main__":
    main()
