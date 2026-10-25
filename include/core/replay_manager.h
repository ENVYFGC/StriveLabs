#pragma once

#include "game_input.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class asw_inputs;
class GameState;
class Keybinds;
class Timeline;

class ReplayManager
{
public:
    static ReplayManager& instance();
    void initialize(GameState& gameState, Keybinds& keybindings, Timeline& timeline);

    void onMatchStart();
    void onRoundStart();
    void onReplayFrame();
    void onPreTick();
    void onBattleUpdate();
    void onBattleFrameProcessed();
    void update();
    void logStatus() const;

    bool isRewindDelayActive() const;
    // Replay Assist: 0 = off, 1 = standby (remaining = 1..0 of the lead-in), 2 = just went active.
    int assistState(float& remaining) const;
    int rewindCountdownSeconds() const;

    bool isTakeoverInput(const asw_inputs* input) const;
    // Selected replay player (1 or 2), or empty while watching playback.
    std::optional<int> takeoverPlayer() const;
    bool playbackEnabled() const;
    RECFLG_ENUM takeoverInput(RECFLG_ENUM recordedInput, bool menuOpen) const;
    void setInputSystem(uintptr_t system);

    std::vector<uint16_t> captureCurrentRound(int peer) const;
    bool startCapture(int peer, std::wstring& error);
    std::vector<uint16_t> stopCapture(std::wstring& error);
    bool captureActive() const;
    bool captureFull() const;
    uint32_t captureFrames() const;

private:
    ReplayManager();
    ~ReplayManager();
    ReplayManager(const ReplayManager&) = delete;
    ReplayManager& operator=(const ReplayManager&) = delete;

    struct Data;
    std::unique_ptr<Data> data;
};
