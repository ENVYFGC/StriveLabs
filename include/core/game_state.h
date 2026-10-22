#pragma once

#include "game_input.h"

#include <vector>

class UREDGameCommon;

class GameState
{
public:
    GameState();
    void initialize();

    bool checkMode();
    PAD_ID getMainQuadrant();
    bool checkModeReplay() const;
    bool checkModeTraining() const;
    void checkRound();

    bool mode_battle_logged = false;  // logs which game modes run the mod, for user reports
    bool resetting = false;
    bool matchStarted = true;
    bool roundActive = false;
    // Set on BATTLE_START; consumed once by the caller to log the replay anchor.
    bool roundJustStarted = false;

private:
    using funcGetGameMode_t = int (*)(UREDGameCommon*);
    funcGetGameMode_t orig_GetGameMode{};
    UREDGameCommon* GameCommon = nullptr;
    int last_mode;
    bool in_allowed_mode = false;
    std::vector<int> allowed_modes;
};
