#pragma once

#include <atomic>
#include <functional>

class GameState;
class Keybinds;

class Timeline
{
public:
    static Timeline& instance();
    void initialize(GameState& gameState, Keybinds& keybindings);

    // The callback lets playback logic (e.g. ReplayManager) react to queued hotkeys
    // and request Pause/Step before Timeline decides whether this tick may advance.
    // It runs once synchronously after hotkey delivery, keeping Timeline independent
    // of that logic. Rewind uses this to allow one restored frame before pausing.
    void onPreTick(const std::function<void()>& callback);

    void setPaused(bool paused);
    bool isPaused() const;

    void advanceFrame();
    bool canAdvanceFrame() const;

    void pressHotkey(int action);
    int mode() const;

private:
    Timeline() = default;

    GameState* gameState = nullptr;
    Keybinds* keybindings = nullptr;
    std::atomic<bool> paused{false};
    std::atomic<int> pendingSteps{0};
    std::atomic<unsigned> pendingHotkeys{0};
    bool isFrameAllowed = true;
};
