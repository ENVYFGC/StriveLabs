#include "timeline.h"

#include "game_state.h"
#include "keybindings.h"

Timeline& Timeline::instance()
{
    static Timeline timeline;
    return timeline;
}

void Timeline::initialize(GameState& gameState, Keybinds& keybindings)
{
    this->gameState = &gameState;
    this->keybindings = &keybindings;
}

// The timeline's pause skips the fight and replay updates frame by frame while the
// game keeps drawing, so its Play button can still be clicked. (F3 instead holds
// the whole update loop, which stops drawing too.) Its buttons queue hotkey
// presses for the start of the next frame, where the mod reads F6 / F9.
void Timeline::onPreTick(const std::function<void()>& callback)
{
    // Rollback may call the hooked updates before this tick's final decision.
    isFrameAllowed = true;
    for (unsigned mask = pendingHotkeys.exchange(0), action = 0; mask; mask >>= 1, action++)
        if (mask & 1) keybindings->setButtonIndex(static_cast<int>(action), true);

    callback();

    if (!gameState->checkMode())
    {
        paused = false;
        pendingSteps = 0;
    }

    // Battle and replay must use the same decision even if a battle update
    // finishes the first restored frame and changes the rewind state.
    isFrameAllowed = !isPaused();
    if (!isFrameAllowed && pendingSteps > 0)
    {
        pendingSteps--;
        isFrameAllowed = true;
    }
}

void Timeline::setPaused(bool paused)
{
    this->paused = paused;
    if (!paused) pendingSteps = 0;
}

bool Timeline::isPaused() const
{
    return paused;
}

void Timeline::advanceFrame()
{
    pendingSteps++;
}

bool Timeline::canAdvanceFrame() const
{
    return isFrameAllowed;
}

void Timeline::pressHotkey(int action)
{
    pendingHotkeys |= 1u << action;
}

int Timeline::mode() const
{
    return !gameState->checkMode() ? 0 : gameState->checkModeReplay() ? 2 : 1;
}
