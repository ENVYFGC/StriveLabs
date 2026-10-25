#include "game_state.h"

#include "arcsys.h"
#include "combo_log.h"
#include "sigscan.h"

#include <Unreal/UObjectGlobals.hpp>

#include <algorithm>
#include <cstdio>

using namespace RC;
using namespace RC::Unreal;

class UREDGameCommon : public Unreal::UObject
{
public:
    FIELD(0x2F3, PAD_ID, m_MainQuadrant);
};

namespace {
// Enums
enum GAME_MODE : int32_t
{
    GAME_MODE_DEBUG_BATTLE = 0x0,
    GAME_MODE_ADVERTISE = 0x1,
    GAME_MODE_MAINTENANCEVS = 0x2,
    GAME_MODE_ARCADE = 0x3,
    GAME_MODE_MOM = 0x4,
    GAME_MODE_SPARRING = 0x5,
    GAME_MODE_VERSUS = 0x6,
    GAME_MODE_VERSUS_PREINSTALL = 0x7,
    GAME_MODE_TRAINING = 0x8,
    GAME_MODE_TOURNAMENT = 0x9,
    GAME_MODE_RANNYU_VERSUS = 0xA,
    GAME_MODE_EVENT = 0xB,
    GAME_MODE_SURVIVAL = 0xC,
    GAME_MODE_STORY = 0xD,
    GAME_MODE_MAINMENU = 0xE,
    GAME_MODE_TUTORIAL = 0xF,
    GAME_MODE_LOBBYTUTORIAL = 0x10,
    GAME_MODE_CHALLENGE = 0x11,
    GAME_MODE_KENTEI = 0x12,
    GAME_MODE_MISSION = 0x13,
    GAME_MODE_GALLERY = 0x14,
    GAME_MODE_LIBRARY = 0x15,
    GAME_MODE_NETWORK = 0x16,
    GAME_MODE_REPLAY = 0x17,
    GAME_MODE_LOBBYSUB = 0x18,
    GAME_MODE_MAINMENU_QUICK_BATTLE = 0x19,
    GAME_MODE_UNDECIDED = 0x1A,
    GAME_MODE_INVALID = 0x1B,
};
}

GameState::GameState()
    : last_mode(GAME_MODE_DEBUG_BATTLE)
    , allowed_modes{GAME_MODE_TRAINING, GAME_MODE_REPLAY, GAME_MODE_MISSION, GAME_MODE_UNDECIDED}
{
}

void GameState::initialize()
{
    const uintptr_t GetGameMode_Addr = sigscan::get().scan("\x0F\xB6\x81\xF0\x02\x00\x00\xC3", "xxxxxxxx");
    orig_GetGameMode = reinterpret_cast<funcGetGameMode_t>(GetGameMode_Addr);
}

bool GameState::checkMode()
{
    if (!GameCommon)
    {
        GameCommon = reinterpret_cast<UREDGameCommon*>(UObjectGlobals::FindFirstOf(FName(STR("REDGameCommon"))));
    }
    if (!GameCommon) return false;
    if (int current_mode = orig_GetGameMode(GameCommon); current_mode != last_mode)
    {
        //       RC::Output::send<LogLevel::Warning>(STR("Mode Change: {}\n"), current_mode);
        last_mode = current_mode;
        mode_battle_logged = false;
        { char b[64]; snprintf(b, sizeof(b), "mode: %d (%s)", current_mode,
            (std::find(allowed_modes.begin(), allowed_modes.end(), current_mode) != allowed_modes.end()) ? "mod active" : "mod idle");
          COMBO_LOG(b); }
        in_allowed_mode = (std::find(allowed_modes.begin(), allowed_modes.end(), current_mode) != allowed_modes.
            end());
    }
    return in_allowed_mode;
}

PAD_ID GameState::getMainQuadrant()
{
    if (!GameCommon)
    {
        GameCommon = reinterpret_cast<UREDGameCommon*>(UObjectGlobals::FindFirstOf(FName(STR("REDGameCommon"))));
    }
    if (!GameCommon) return PAD_ID_INVALID;

    return GameCommon->m_MainQuadrant;
}

bool GameState::checkModeReplay() const
{
    return last_mode == GAME_MODE_REPLAY;
}

bool GameState::checkModeTraining() const
{
    return last_mode == GAME_MODE_TRAINING;
}

void GameState::checkRound()
{
    resetting = false;
    auto* events = asw_events::get();
    auto count = events->event_count;
    count = std::min<unsigned long>(count, 10);
    for (unsigned int idx = 0; idx < count; ++idx)
    {
        auto e_type = events->events[idx].type;
        // if(e_type < 41) RC::Output::send<LogLevel::Warning>(STR("Event {}: {}\n"), idx, (int)e_type);
        if (e_type == BOM_EVENT_RESET || e_type == BOM_EVENT_DECISION)
        {
            resetting = true;
            roundActive = false;
        }
        if (e_type == BOM_EVENT_BATTLE_START)
        {
            resetting = true;
            roundActive = true;

            roundJustStarted = true;
        }
    }
}
