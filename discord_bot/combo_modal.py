"""Discord Modal for combo submission."""

import discord
from discord.ui import Modal, TextInput


class ComboSubmitModal(Modal, title="Submit a Combo"):
    """Modal form with 5 text fields for combo entry."""

    version = TextInput(
        label="Version",
        placeholder="V148",
        default="V148",
        max_length=20,
        required=True,
    )

    starter = TextInput(
        label="Starter",
        placeholder="e.g. cS, fS, 2D, 5H",
        max_length=50,
        required=True,
    )

    notation = TextInput(
        label="Notation",
        style=discord.TextStyle.paragraph,
        placeholder="cS > 6H > 236K~K > 5K > cS > HSVV WS!",
        max_length=500,
        required=True,
    )

    notes = TextInput(
        label="Notes",
        placeholder="Midscreen Meterless BnB",
        max_length=200,
        required=True,
    )

    link = TextInput(
        label="Video Link (optional)",
        placeholder="https://www.youtube.com/watch?v=...",
        max_length=200,
        required=False,
    )

    def __init__(self, character_id: str, character_name: str, callback):
        super().__init__()
        self.character_id = character_id
        self.character_name = character_name
        self._callback = callback

    async def on_submit(self, interaction: discord.Interaction):
        await self._callback(
            interaction,
            character_id=self.character_id,
            character_name=self.character_name,
            version=self.version.value,
            starter=self.starter.value,
            notation=self.notation.value,
            notes=self.notes.value,
            link=self.link.value,
            contributor=interaction.user.display_name,
        )
