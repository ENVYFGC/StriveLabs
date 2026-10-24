#pragma once

#include <atomic>

class GameState;
class Keybinds;

class Timeline
{
public:
    static Timeline& instance();
    void initialize(GameState& gameState, Keybinds& keybindings);

    void onPreTick();

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
