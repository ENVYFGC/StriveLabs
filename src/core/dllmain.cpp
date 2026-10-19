#include <algorithm>
#include <polyhook2/Detour/x64Detour.hpp>

#include "arcsys.h"
#include "bind_watcher.h"
#include "draw_utils.h"
#include "framebar.h"
#include "hitboxes.h"
#include "keybinds.h"
#include "imgui_overlay.h"
namespace crash_log { void install(const std::filesystem::path& dir); }
namespace select_memory { void init(const std::filesystem::path& dir); void save(); }
#include "menu.h"
#include "recordings.h"
#include "sigscan.h"
#include <set>
#include "tension_overlay.h"
#include "combo_log.h"
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>
using namespace std::chrono_literals;

#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <UE4SSProgram.hpp>
#include <Unreal/World.hpp>
#include <UnrealDef.hpp>

#define WIN32_LEAN_AND_MEAN
#include "Windows.h"
#include <Psapi.h>
#pragma comment(lib, "Psapi.lib")
#include <Xinput.h>
#pragma comment(lib, "xinput9_1_0.lib")

#define STRIVEFRAMEDATA_API __declspec(dllexport)


/* Definitions */

// Enums
enum GAMEKEY_FLAG
{
    GKF_NULL = 0x0,
    GKF_UP = 0x1,
    GKF_U = 0x1,
    GKF_RIGHT = 0x2,
    GKF_R = 0x2,
    GKF_UR = 0x3,
    GKF_DOWN = 0x4,
    GKF_STK_DMASK = 0x4,
    GKF_D = 0x4,
    GKF_RD = 0x6,
    GKF_LEFT = 0x8,
    GKF_L = 0x8,
    GKF_LU = 0x9,
    GKF_STK_LRMASK = 0xA,
    GKF_DL = 0xC,
    GKF_STK_MASK = 0xF,
    GKF_LSTICK_UP = 0x10,
    GKF_LSTICK_RIGHT = 0x20,
    GKF_LSTICK_DOWN = 0x40,
    GKF_LSTICK_LEFT = 0x80,
    GKF_RSTICK_UP = 0x100,
    GKF_RSTICK_RIGHT = 0x200,
    GKF_RSTICK_DOWN = 0x400,
    GKF_RSTICK_LEFT = 0x800,
    GKF_X = 0x1000,
    GKF_Y = 0x2000,
    GKF_A = 0x4000,
    GKF_B = 0x8000,
    GKF_R1 = 0x10000,
    GKF_R2 = 0x20000,
    GKF_L1 = 0x40000,
    GKF_L2 = 0x80000,
    GKF_L3 = 0x100000,
    GKF_R3 = 0x200000,
    GKF_START = 0x400000,
    GKF_SELECT = 0x800000,
    GKF_SWALL = 0xFFF000,
};

enum RECFLG_ENUM : Unreal::uint16
{
    RECFLG_TENKEY_MASK = 0xF,
    RECFLG_flgU = 0x1,
    RECFLG_flgD = 0x2,
    RECFLG_flgL = 0x4,
    RECFLG_flgR = 0x8,
    RECFLG_1 = 0x6,
    RECFLG_2 = 0x2,
    RECFLG_3 = 0xA,
    RECFLG_4 = 0x4,
    RECFLG_5 = 0x0,
    RECFLG_6 = 0x8,
    RECFLG_7 = 0x5,
    RECFLG_8 = 0x1,
    RECFLG_9 = 0x9,
    RECFLG_A = 0x10,
    RECFLG_B = 0x20,
    RECFLG_C = 0x40,
    RECFLG_D = 0x80,
    RECFLG_E = 0x100,
    RECFLG_F = 0x200,
    RECFLG_G = 0x400,
    RECFLG_H = 0x800,
    RECFLG_I = 0x1000,
    RECFLG_J = 0x2000,
    RECFLG_K = 0x4000,
    RECFLG_SP_NET = 0x8000,
};

enum PAD_ID : Unreal::uint8
{
    PAD_ID_BEGIN = 0x0,
    PAD_ID_1CON = 0x0,
    PAD_ID_2CON = 0x1,
    PAD_ID_3CON = 0x2,
    PAD_ID_4CON = 0x3,
    PAD_ID_MAX = 0x4,
    PAD_ID_CPU = 0x4,
    PAD_ID_MAX_WITH_CPU = 0x5,
    PAD_ID_ANY = 0x6,
    PAD_ID_INVALID = 0x7,
};

// Classes
class UREDGameCommon : public Unreal::UObject
{
public:
    FIELD(0x2F3, PAD_ID, m_MainQuadrant);
};

class BattleReplayManager
{
public:
    FIELD(0x5, bool, m_NextSeq);
    FIELD(0x6, bool, m_ChangeSeqDisable);
    FIELD(0x7, bool, m_PrevSeq);
    FIELD(0x8, int, m_PlayScale);
};

struct ReplayHead
{
    uint32_t m_Cursor{};
    uint32_t m_Count{};
    uint32_t m_SolidPosition{};
};

struct ReplayPeer
{
    uint32_t* m_pBuffer{};
    ReplayHead m_ReadHead{};
    ReplayHead m_WriteHead{};
    uint32_t m_OperationType{};
    uint32_t m_LastPushKey{};
};

class ReplayManager_Internal
{
public:
    ARRAY_FIELD(0xD5438, ReplayPeer[6], m_ReplayBuffer);
};

// Functions
using funcAHUDPostRender_t = void (*)(void*);
using funcACamUpdateCamera_t = void (*)(void*, float);
using funcMatchStart_t = void (*)(AREDGameState_Battle*);
using funcGetGameMode_t = int (*)(UREDGameCommon*);
using funcUpdateBattle_t = void (*)(AREDGameState_Battle*, float);
using funcUpdateBattleInput_t = void (*)(asw_inputs*, RECFLG_ENUM);
using funcGameKeyToRecFlag_t = RECFLG_ENUM (*)(PAD_ID, GAMEKEY_FLAG, bool);
using funcUpdateSystem_t = void (*)(uintptr_t);
using funcUpdateReplay_t = void (*)(BattleReplayManager*);
using funcSaveBackup_t = void (*)(asw_rollback*);
using funcRollback_t = void (*)(asw_rollback*);
using funcGetReplayManager_t = uintptr_t (*)();
using funcUpdateOnPreTick_t = void (*)(AREDGameState_Battle*, float);

// Hooks
void hook_AHUDPostRender(void*);
void hook_ACamUpdateCamera(void*, float);
void hook_MatchStart(AREDGameState_Battle*);
void replayCaptureCancel();
void timelineSetPaused(bool paused);
static bool g_skip_frame;
void hook_UpdateBattle(AREDGameState_Battle*, float);

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

/* Utilities */

const void* vtable_hook(const void** vtable, const int index, const void* hook)
{
    DWORD old_protect;
    VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old_protect);
    const auto* orig = vtable[index];
    vtable[index] = hook;
    VirtualProtect(&vtable[index], sizeof(void*), old_protect, &old_protect);
    return orig;
}

/* Variables */

// Functions
funcGetGameMode_t orig_GetGameMode;
funcAHUDPostRender_t orig_AHUDPostRender;
funcACamUpdateCamera_t orig_ACamUpdateCamera;
funcMatchStart_t orig_MatchStart;
funcUpdateBattle_t orig_UpdateBattle;
funcUpdateBattleInput_t orig_UpdateBattleInput;
funcGameKeyToRecFlag_t orig_GameKeyToRecFlag;
funcUpdateSystem_t orig_UpdateSystem;
funcUpdateReplay_t orig_UpdateReplay;
funcSaveBackup_t orig_SaveBackup;
funcRollback_t orig_Rollback;
// The game's rollback is also its effect manager. Rollback() moves every effect
// spawned at or after the restored frame into a "waiting to be reused" list; the
// netplay loop then re-simulates and finally destroys whatever was not reused,
// with these two calls (one per effect kind). Our F9 load does not re-simulate,
// so without them the leftovers live on and the restored timeline spawns new
// ones on top: the HC reticle / FD effect stacking.
using funcEffectCleanup_t = void (*)(asw_rollback*);
funcEffectCleanup_t orig_EffectCleanupA = nullptr;
funcEffectCleanup_t orig_EffectCleanupB = nullptr;
funcGetReplayManager_t orig_GetReplayManager;
uintptr_t orig_BattleKeyCheck;
funcUpdateOnPreTick_t orig_UpdateOnPreTick;

// Pre-resolved pointer to the game's is_battle flag byte.  Resolved at init
// from whichever candidate (sigscan, or a known hardcoded offset per game
// patch) produces a byte whose current value is 0 or 1.  If no candidate
// survives validation, stays null and the takeover hook skips the toggle.
uint8_t* is_battle_ptr = nullptr;

// Cursor for each side at the last BATTLE_START -- the anchor a capture slices
// from, since buffer index 0 is the start of the whole match, not the round.
// Entry index each peer's stream sat at when the match's first round call
// fired. Latched once (g_anchor_set) so later rounds don't move it.
uint32_t g_round_anchor[6] = {};
bool g_anchor_set = false;
static bool is_ptr_in_game_module(const void* p);

// State Data
UE4SSProgram* Program;

// Trackers
class StateMgr
{
    UREDGameCommon* GameCommon = nullptr;
    int last_mode = GAME_MODE_DEBUG_BATTLE;
public:
    bool mode_battle_logged = false;  // logs which game modes run the mod, for user reports
private:
    bool in_allowed_mode = false;
    std::vector<int> allowed_modes = {GAME_MODE_TRAINING, GAME_MODE_REPLAY, GAME_MODE_MISSION, GAME_MODE_UNDECIDED};

public:
    bool checkMode()
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

    PAD_ID getMainQuadrant()
    {
        if (!GameCommon)
        {
            GameCommon = reinterpret_cast<UREDGameCommon*>(UObjectGlobals::FindFirstOf(FName(STR("REDGameCommon"))));
        }
        if (!GameCommon) return PAD_ID_INVALID;

        return GameCommon->m_MainQuadrant;
    }

    bool checkModeReplay() const
    {
        return last_mode == GAME_MODE_REPLAY;
    }

    bool checkModeTraining() const
    {
        return last_mode == GAME_MODE_TRAINING;
    }

    void checkRound()
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

    bool resetting = false;
    bool matchStarted = true;
    bool roundActive = false;
    // Set on BATTLE_START; consumed once by the caller to log the replay anchor.
    bool roundJustStarted = false;
} game_state;

class Keybinds
{
public:
    static const int BUTTON_COUNT = binds::COUNT;

    // Aliases into the rebindable table (see keybinds.h); rebinding updates these too.
    int& TOGGLE_FRAMEBAR_BUTTON = binds::key[binds::FRAMEBAR];
    int& TOGGLE_HITBOX_BUTTON   = binds::key[binds::HITBOXES];
    int& PAUSE_BUTTON           = binds::key[binds::PAUSE];
    int& ADVANCE_BUTTON         = binds::key[binds::ADVANCE];
    int& TOGGLE_MENU_BUTTON     = binds::key[binds::MENU];
    int& TOGGLE_SAVE_BUTTON     = binds::key[binds::SAVE];
    int& TAKEOVER_P1_BUTTON     = binds::key[binds::TAKEOVER_P1];
    int& TAKEOVER_P2_BUTTON     = binds::key[binds::TAKEOVER_P2];
    int& TOGGLE_LOAD_BUTTON     = binds::key[binds::LOAD];
    int& MENU_UP_BUTTON         = binds::key[binds::UP];
    int& MENU_DOWN_BUTTON       = binds::key[binds::DOWN];
    int& MENU_LEFT_BUTTON       = binds::key[binds::LEFT];
    int& MENU_RIGHT_BUTTON      = binds::key[binds::RIGHT];

    int getButtonIndex(int input)
    {
        if (input == 0) return -1;
        for (int i = 0; i < BUTTON_COUNT; i++)
        {
            if (binds::key[i] == input) return i;
        }
        return -1;
    }

    void setButtonIndex(int index, bool val)
    {
        if (index >= 0 && index < BUTTON_COUNT) buttonStates[index] = val;
    }

    bool setButtonState(int input, bool val)
    {
        int index = getButtonIndex(input);
        if (index == -1) return false;
        buttonStates[index] = val;
        return true;
    }

    bool getButtonState(int input)
    {
        int index = getButtonIndex(input);
        return index != -1 && buttonStates[index];
    }

    void resetButton(int input)
    {
        int index = getButtonIndex(input);
        if (index != -1) buttonStates[index] = false;
    }

    void resetButtons()
    {
        for (int i = 0; i < BUTTON_COUNT; i++) buttonStates[i] = false;
    }

    void loadButtons()
    {
        for (int i = 0; i < BUTTON_COUNT; i++) BindWatcherI::addToFilter(binds::key[i]);
    }

    void checkBinds(bool await = false)
    {
        auto inputs = BindWatcherI::getInputs(await);
        for (const auto& input : inputs)
        {
            if (binds::capturing >= 0) binds::offerKey(input);
            else setButtonState(input, true);
        }
    }

private:
    bool buttonStates[BUTTON_COUNT] = {};
} keybindings;

static void pollGamepad();
static void trackEffectClasses();

class PauseManager
{
    bool isPaused = false;
    bool shouldAdvance = false;

public:
    bool advancing() const
    {
        return isPaused && shouldAdvance;
    }

    bool cinematicShouldAdvance()
    {
        return isPaused && !shouldAdvance;
    }

    void updateBinds()
    {
        if (keybindings.getButtonState(keybindings.PAUSE_BUTTON))
        {
            keybindings.resetButton(keybindings.PAUSE_BUTTON);
            isPaused = !isPaused;
        }

        if (keybindings.getButtonState(keybindings.ADVANCE_BUTTON))
        {
            keybindings.resetButton(keybindings.ADVANCE_BUTTON);
            shouldAdvance = true;
        }
    }

    void checkPause()
    {
        if (advancing())
        {
            shouldAdvance = false;
            return;
        }

        updateBinds();

        if (ModMenu::instance().pauseType() == 0)
        {
            while (isPaused && !shouldAdvance)
            {
                keybindings.checkBinds(false);
                pollGamepad();
                Sleep(1);
                updateBinds();
            }
        }
    }

    void reset()
    {
        isPaused = false;
        shouldAdvance = false;
    }

    void stopAdvance() { shouldAdvance = false; }
} pause_manager;

class ReplayManager
{
    asw_inputs* analyzer{};
    ReplayHead replayHead[6]{};
    ReplayManager_Internal* manager{};

public:
    void setManager(ReplayManager_Internal* inManager)
    {
        manager = inManager;
    }

    bool isAnalyzerSame(const asw_inputs* in_analyzer) const
    {
        return analyzer == in_analyzer;
    }

    // Raw input stream for a peer. Entries are uint32 words; the low
    // 16 bits are believed to match RECFLG but that is unverified, hence the
    // raw dump at capture time.
    const uint32_t* peerBuffer(int idx) const
    {
        if (!manager || idx < 0 || idx > 5) return nullptr;
        return manager->m_ReplayBuffer[idx].m_pBuffer;
    }

    // Read cursor for one of the 6 replay peers (0..2 = P1 side, 3..5 = P2).
    // Returns false if the manager is not set yet.
    bool peerReadHead(int idx, ReplayHead& out) const
    {
        if (!manager || idx < 0 || idx > 5) return false;
        out = manager->m_ReplayBuffer[idx].m_ReadHead;
        return true;
    }

    void restoreReplayHead() const
    {
        if (!game_state.checkModeReplay()) return;
        
        for (int i = 0; i < 6; i++)
        {
            manager->m_ReplayBuffer[i].m_ReadHead = replayHead[i];
        }
    }

    void rewindReplayHead() const
    {
        for (int i = 0; i < 6; i++)
        {
            auto& peer = manager->m_ReplayBuffer[i];
            if (const auto count = --peer.m_ReadHead.m_Count; count < 0x8CA0)
            {
                if (const auto cursor = peer.m_ReadHead.m_Count; peer.m_pBuffer[cursor] >> 16 < cursor)
                    peer.m_ReadHead.m_Count--;
            }
            peer.m_ReadHead.m_SolidPosition--;
        }
    }
    
    void saveBackup()
    {
        if (isTakeover)
        {
            reset();
        }
        isTakeover = true;
        for (int i = 0; i < 6; i++)
        {
            replayHead[i] = manager->m_ReplayBuffer[i].m_ReadHead;
        }
    }

    void update()
    {
        if (asw_state::get()->RoundCount != roundCount)
        {
            roundCount = asw_state::get()->RoundCount;
            asw_engine::get()->GameFrame = 0;
        }

        if (keybindings.getButtonState(keybindings.TOGGLE_SAVE_BUTTON))
        {
            keybindings.resetButton(keybindings.TOGGLE_SAVE_BUTTON);
            if (isTakeover)
            {
                reset();
            }
            isTakeover = true;
            auto rollback = asw_rollback::get();
            rollback->CurrentBackupDataIndex = 18;
            orig_SaveBackup(rollback);
            for (int i = 0; i < 6; i++)
            {
                replayHead[i] = manager->m_ReplayBuffer[i].m_ReadHead;
            }
        }

        if (keybindings.getButtonState(keybindings.TAKEOVER_P1_BUTTON))
        {
            keybindings.resetButton(keybindings.TAKEOVER_P1_BUTTON);
            toggleTakeover(&asw_engine::get()->inputs[0]);
        }

        if (keybindings.getButtonState(keybindings.TAKEOVER_P2_BUTTON))
        {
            keybindings.resetButton(keybindings.TAKEOVER_P2_BUTTON);
            toggleTakeover(&asw_engine::get()->inputs[3]);
        }

        if (keybindings.getButtonState(keybindings.TOGGLE_LOAD_BUTTON))
        {
            keybindings.resetButton(keybindings.TOGGLE_LOAD_BUTTON);
            if (isTakeover)
            {
                loadState = true;
            }
        }
    }

    void reset()
    {
        isTakeover = false;
        loadState = false;
        analyzer = nullptr;
    }

    // Same side's key again = stop taking over. The replay then plays on by itself.
    // With a rewind point, jump back to it first: from where takeover left off the
    // fight no longer matches the recording, so the replay inputs would make no sense.
    void toggleTakeover(asw_inputs* side)
    {
        if (analyzer != side)
        {
            analyzer = side;
            return;
        }
        analyzer = nullptr;
        if (isTakeover) loadState = true;
    }

    bool isTakeover = false;
    bool loadState = false;
    int roundCount = -1;
} replay_manager;

class GAME_KeyControler
{
public:
    FIELD(0x78, uint32_t, m_PureKey);
};

class AASystemRED
{
public:
    ARRAY_FIELD(0x1F8, GAME_KeyControler*[4], m_BattleKey);
} * system_red;

class UeTracker
{
    Unreal::AActor* paused = nullptr;
    bool renderingHooked = false;

    void hookFuncs()
    {
        if (renderingHooked) return;
        renderingHooked = true;

        /* HUD Rendering vtable hook*/
        const auto** AHUD_vtable = (const void**)get_rip_relative(
            sigscan::get().scan("\x48\x8D\x05\x00\x00\x00\x00\xC6\x83\x18\x03", "xxx????xxxx") + 3);
        orig_AHUDPostRender = (funcAHUDPostRender_t)vtable_hook(AHUD_vtable, 214, hook_AHUDPostRender);

        const auto** ACamera_vtable = (const void**)get_rip_relative(
            sigscan::get().scan("\x48\x8D\x05\x00\x00\x00\x00\x48\x89\x07\x48\x8D\x8F\x00\x00\x00\x00\xC7\x87\x00\x00\x00\x00\xFF\xFF\xFF\xFF", "xxx????xxxxxx????xx????xxxx") + 3);
        orig_ACamUpdateCamera = (funcACamUpdateCamera_t)vtable_hook(ACamera_vtable, 208, hook_ACamUpdateCamera);
    }

    void findProp()
    {
        paused = (*UWorld::GWorld)->PersistentLevel->WorldSettings->PauserPlayerState;
    }

public:
    void reset()
    {
        paused = nullptr;
    }

    void setup()
    {
        reset();
        hookFuncs();
        findProp();
        return;
    }

    bool isUePaused()
    {
        return paused != nullptr;
    }
} tracker;

/* Hooks */

FrameBar the_bar;
TensionOverlay tension_overlay;

// Overlays (frame bar, hitboxes, tension) stay hidden from a new match load until its
// first round starts, so they do not draw over the versus splash with the previous
// fight's numbers. Training resets and wall breaks do not load a match, so they keep showing.
static bool g_overlays_visible = true;
// Loading screens (e.g. changing character from the training menu) keep the mode at
// training but stop the battle update, so also require a battle update in the last 300 ms.
static std::atomic<unsigned long long> g_last_battle_update{0};

// Render thread: true while a training or replay fight is on screen (not menus,
// loading screens or character select). StriveLabs windows hide otherwise.
bool modScreenActive() { return GetTickCount64() - g_last_battle_update < 300; }

void hook_MatchStart(AREDGameState_Battle* GameState)
{
    replayCaptureCancel();  // a new match or replay: the old capture start means nothing
    select_memory::save();  // remember this character select's picks for the next restart
    timelineSetPaused(false);
    g_overlays_visible = false;
    game_state.matchStarted = true;
    game_state.roundActive = false;
    // New match (or a freshly loaded replay) -- let the next round call
    // re-anchor the capture range.
    g_anchor_set = false;
    // A training position reset also lands here. Keep the hold frame on
    // through it, so the user stays paused at the new position.
    if (game_state.checkModeTraining()) pause_manager.stopAdvance();
    else pause_manager.reset();
    tracker.reset();
    replay_manager.reset();
    tension_overlay.reset();
    // Combo menu is initialized once in on_unreal_init, not per-match

    DrawTool::instance().matchStart();


    orig_MatchStart(GameState);
}

void hook_AHUDPostRender(void* hud)
{
    if (!game_state.checkMode())
    {
        g_overlays_visible = false;
        ModMenu::instance().hide();  // left training / replay: close the F5 menu
        orig_AHUDPostRender(hud);
        return;
    }

    if (!pause_manager.advancing())
        orig_AHUDPostRender(hud);

    // Draw our overlays AFTER the game's HUD so we render on top
    if (DrawTool::instance().update(hud))
    {
        if (g_overlays_visible && GetTickCount64() - g_last_battle_update < 300)
        {
            if (ModMenu::instance().hitboxEnabled()) drawAllBoxes();
            if (ModMenu::instance().barEnabled()) the_bar.draw();
            tension_overlay.draw();
        }
        ModMenu::instance().draw();
    }
}

void hook_ACamUpdateCamera(void* cam, float DeltaTime)
{
    if (!game_state.checkMode())
    {
        orig_ACamUpdateCamera(cam, DeltaTime);
        return;
    }

    if (pause_manager.advancing()) return;
    orig_ACamUpdateCamera(cam, DeltaTime);
}

// Gamepad: Back+Start toggles the menu, D-pad navigates. Feeds the same button
// states the keyboard hook does, so the menu code does not know the difference.
// Polls all 4 XInput slots every battle tick. DirectInput-only pads are not covered.
static void pollGamepad()
{
    // One merged mask across all pads, with triggers and left stick folded in (see binds::heldPadMask).
    static WORD prev = 0;
    const WORD now = binds::heldPadMask(), edge = now & ~prev;
    prev = now;
    if (!edge) return;
    if (binds::capturing >= 0) { binds::offerPad(now); return; }
    for (int a = 0; a < binds::COUNT; a++)
    {
        const WORD m = binds::pad[a];
        if (m && (now & m) == m && (edge & m)) keybindings.setButtonIndex(a, true);
    }
}

void hook_UpdateBattle(AREDGameState_Battle* GameState, float DeltaTime)
{
    if (!game_state.checkMode())
    {
        orig_UpdateBattle(GameState, DeltaTime);
        return;
    }
    g_last_battle_update = GetTickCount64();
    if (!game_state.mode_battle_logged) { game_state.mode_battle_logged = true; COMBO_LOG("battle code running in this mode"); }

    std::this_thread::sleep_for(std::chrono::milliseconds(ModMenu::instance().delayAmount()));

    pollGamepad();  // before anything reads button states this frame
    trackEffectClasses();
    pause_manager.checkPause();

    if (game_state.checkModeReplay()) replay_manager.update();

    if (ModMenu::instance().pauseType() == 0 && pause_manager.advancing()) return;
    if (ModMenu::instance().pauseType() == 1 && pause_manager.cinematicShouldAdvance()) return;

    ModMenu::instance().update(PressedKeys{
        keybindings.getButtonState(keybindings.TOGGLE_FRAMEBAR_BUTTON),
        keybindings.getButtonState(keybindings.TOGGLE_HITBOX_BUTTON),
        keybindings.getButtonState(keybindings.TOGGLE_MENU_BUTTON),
        keybindings.getButtonState(keybindings.MENU_UP_BUTTON),
        keybindings.getButtonState(keybindings.MENU_DOWN_BUTTON),
        keybindings.getButtonState(keybindings.MENU_RIGHT_BUTTON),
        keybindings.getButtonState(keybindings.MENU_LEFT_BUTTON)
    });
    keybindings.resetButtons();

    game_state.checkRound();

    // Replay input buffers are one continuous ~36000-frame stream per side for
    // the WHOLE match, including intro/menu frames (an intro skip is recorded
    // like any other input), so index 0 is not round start. Capture the read
    // cursors at BATTLE_START -- that is the per-round anchor needed to slice a
    // single round's inputs out of the stream.
    if (game_state.roundJustStarted)
    {
        game_state.roundJustStarted = false;
        g_overlays_visible = true;
        // Round call is the sync point: an armed stream begins here, and
        // re-begins on every reset, so the dummy's inputs line up with the
        // round rather than with when the user opened the menu.
        recordings::streamOnRoundStart();
        // Replay peers are one per player seat: 0 = P1, 1 = P2. (The 0-2 / 3-5
        // grouping is asw_inputs' live/committed/backup triplets and does not
        // apply here -- peers 2-5 stay empty in a 1v1 replay, which is why
        // capturing from peer 3 produced nothing.)
        ReplayHead p1{}, p2{};
        if (game_state.checkModeReplay()
            && replay_manager.peerReadHead(0, p1) && replay_manager.peerReadHead(1, p2))
        {
            auto* eng = asw_engine::get();
            char dbg[256];
            snprintf(dbg, sizeof(dbg),
                "ROUND_START GameFrame=%d  P1 cur/cnt/solid=%u/%u/%u  P2=%u/%u/%u  anchored=%d",
                eng ? eng->GameFrame : -1,
                p1.m_Cursor, p1.m_Count, p1.m_SolidPosition,
                p2.m_Cursor, p2.m_Count, p2.m_SolidPosition,
                g_anchor_set ? 1 : 0);
            COMBO_LOG(dbg);
            // Anchor on the FIRST round call only. Later rounds must not move
            // it, or pausing at the round-1 KO would capture nothing because
            // round 2's anchor already sits past it.
            if (!g_anchor_set)
            {
                g_round_anchor[0] = p1.m_Cursor;
                g_round_anchor[1] = p2.m_Cursor;
                g_anchor_set = true;
            }
        }
    }
    if (game_state.resetting)
    {
        the_bar.reset();
        tension_overlay.reset();
    }

    if (g_skip_frame) return;  // timeline pause: no fight update this frame
    orig_UpdateBattle(GameState, DeltaTime);

    if (game_state.matchStarted)
    {
        game_state.matchStarted = false;
        tracker.setup();
    }

    if (!tracker.isUePaused() && game_state.roundActive)
    {
        the_bar.addFrame();
    }

}

// True if `p` lies inside the currently-loaded game executable's image.
// Guards against sigscan matching the wrong callsite and producing a
// bogus RIP-relative pointer that would otherwise corrupt arbitrary memory.
static bool is_ptr_in_game_module(const void* p)
{
    if (!p) return false;
    static uintptr_t mod_start = 0, mod_end = 0;
    if (!mod_start) {
        HMODULE mod = GetModuleHandleA("GGST-Win64-Shipping.exe");
        if (!mod) mod = GetModuleHandleA(NULL);
        if (!mod) return false;
        MODULEINFO info{};
        if (!GetModuleInformation(GetCurrentProcess(), mod, &info, sizeof(info)))
            return false;
        mod_start = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
        mod_end   = mod_start + info.SizeOfImage;
    }
    const auto v = reinterpret_cast<uintptr_t>(p);
    return v >= mod_start && v < mod_end;
}

// A replay buffer entry is run-length encoded, NOT one word per frame:
//   low 16  = RECFLG flags
//   high 16 = how many frames those flags are held
// Verified live: peer 0's 825 entries sum to 3799 frames, which is exactly its
// m_SolidPosition; peer 1's 310 entries sum to 3784, plus its m_Count of 15 =
// 3799 as well. So the head fields are entry/frame counters, not both frames:
//   m_Cursor        = current ENTRY index
//   m_Count         = frames already consumed inside the current entry
//   m_SolidPosition = absolute FRAME position
// Taking the low half alone would collapse every hold to a single frame.
// Replay entries keep the direction as a numpad digit in the low 4 bits, screen
// absolute (6 = right for both players), and buttons in the same bits as RECFLG.
// Verified live: P1 3 -> fed 0x0A (down+right), P2 6 -> fed 0x08 (right), 0x13 -> 0x1A.
// Training slots and .txt files use RECFLG direction bits, so convert.
static uint16_t replayToRecflg(uint32_t entry)
{
    static constexpr uint16_t kNumpad[16] = {
        0x0,                      // 0: none
        0x6, 0x2, 0xA,            // 1 2 3: down-left, down, down-right
        0x4, 0x0, 0x8,            // 4 5 6: left, neutral, right
        0x5, 0x1, 0x9,            // 7 8 9: up-left, up, up-right
        0, 0, 0, 0, 0, 0};
    return static_cast<uint16_t>((entry & 0xFFF0) | kNumpad[entry & 0xF]);
}

std::vector<uint16_t> captureReplayInputs(int peer, uint32_t fromEntry,
                                          uint32_t entryCount, uint32_t tailFrames)
{
    std::vector<uint16_t> out;
    const uint32_t* buf = replay_manager.peerBuffer(peer);
    if (!buf) { COMBO_LOG("capture: peer buffer null"); return out; }

    char dbg[320];
    std::string raw;
    for (uint32_t i = 0; i < entryCount; i++)
    {
        const uint32_t w = buf[fromEntry + i];
        if (i < 8) { char t[16]; snprintf(t, sizeof(t), "%08X ", w); raw += t; }

        const uint16_t flags = replayToRecflg(w);
        uint32_t hold = w >> 16;
        // A zero-length run would silently drop an input; treat it as one frame.
        if (hold == 0) hold = 1;
        // Guard against a torn read producing an absurd run length.
        if (hold > recordings::kFlagBuffer) hold = recordings::kFlagBuffer;
        out.insert(out.end(), hold, flags);
    }
    // The entry playback is partway through contributes only the frames already
    // consumed, otherwise the capture runs past where the user paused.
    if (tailFrames)
    {
        const uint16_t flags = replayToRecflg(buf[fromEntry + entryCount]);
        out.insert(out.end(), tailFrames, flags);
    }

    snprintf(dbg, sizeof(dbg),
             "capture peer=%d entries=%u tail=%u -> %zu frames  raw: %s",
             peer, entryCount, tailFrames, out.size(), raw.c_str());
    COMBO_LOG(dbg);
    return out;
}

// Capture one side's inputs from the FIRST round call of the match up to
// wherever playback has reached, so pausing at the round-1 KO yields the whole
// round. Returns empty if not in a replay.
std::vector<uint16_t> captureCurrentRound(int peer)
{
    std::vector<uint16_t> out;
    if (!game_state.checkModeReplay()) { COMBO_LOG("capture: not a replay"); return out; }
    ReplayHead head{};
    if (!replay_manager.peerReadHead(peer, head)) { COMBO_LOG("capture: no head"); return out; }

    const uint32_t from = g_round_anchor[peer];
    if (head.m_Cursor < from) { COMBO_LOG("capture: cursor behind anchor"); return out; }
    if (head.m_Cursor == from && head.m_Count == 0)
    {
        COMBO_LOG("capture: nothing since round start");
        return out;
    }
    return captureReplayInputs(peer, from, head.m_Cursor - from, head.m_Count);
}

void hook_UpdateBattleInput(asw_inputs* Analyzer, RECFLG_ENUM recFlag)
{
    // Menu open: swallow pad/keyboard so the D-pad drives the menu, not the character.
    // In a replay only the side being taken over is the player's; every other side
    // is fed from the replay itself, and zeroing it froze both characters and
    // desynced the replay.
    if (ModMenu::instance().isShowing() &&
        (!game_state.checkModeReplay() || (!replay_manager.isAnalyzerSame(nullptr) && replay_manager.isAnalyzerSame(Analyzer))))
        recFlag = static_cast<RECFLG_ENUM>(0);

    // Live stream: feed our own per-frame flags to the P2 (dummy) analyzer.
    // asw_engine::inputs[0..2] belong to P1 and [3..5] to P2, so [3] is the
    // dummy. One word per engine frame keeps this frame-exact -- the reason
    // this exists rather than a virtual controller, which pushes on a wall
    // clock and drifts whenever the game hitches.
    if (recordings::streamActive())
    {
        if (auto* engine = asw_engine::get())
        {
            if (Analyzer == &engine->inputs[3])
            {
                const auto streamed = static_cast<RECFLG_ENUM>(recordings::streamNext());
                orig_UpdateBattleInput(Analyzer, streamed);
                return;
            }
        }
    }

    if (!game_state.checkModeReplay() || !replay_manager.isAnalyzerSame(Analyzer))
    {
        orig_UpdateBattleInput(Analyzer, recFlag);
        return;
    }

    // is_battle_ptr is pre-resolved at init from multiple candidates and only
    // set if it actually points to a byte valued 0 or 1 (i.e., a real boolean
    // flag).  If probing failed on this game build, or system state isn't
    // ready, fall through with the original recFlag — no crash, just no
    // live key-mapping override.
    const auto pad = game_state.getMainQuadrant();
    if (!binds::use_mod_binds && (is_battle_ptr == nullptr || system_red == nullptr
        || system_red->m_BattleKey[pad] == nullptr))
    {
        orig_UpdateBattleInput(Analyzer, recFlag);
        return;
    }

    if (binds::use_mod_binds)
    {
        // Mod's own takeover binds: keyboard + XInput, independent of the game's key config.
        // Menu open: the inputs drive the menu instead.
        orig_UpdateBattleInput(Analyzer, static_cast<RECFLG_ENUM>(ModMenu::instance().isShowing() ? 0u : binds::takeoverRecFlag()));
        return;
    }

    const auto bak = *is_battle_ptr;
    *is_battle_ptr = 1;

    const auto gameKeyFlag = static_cast<GAMEKEY_FLAG>(system_red->m_BattleKey[pad]->m_PureKey);
    const auto newRecflag = orig_GameKeyToRecFlag(pad, gameKeyFlag, false);

    *is_battle_ptr = bak;
    orig_UpdateBattleInput(Analyzer, ModMenu::instance().isShowing() ? static_cast<RECFLG_ENUM>(0) : newRecflag);
}

void hook_UpdateSystem(uintptr_t System)
{
    orig_UpdateSystem(System);
    system_red = (AASystemRED*)System;
}


// ---- Replay capture: up to 40 seconds of one side's inputs ------------------
// Start remembers the side's replay read position; stop collects everything from
// there to the current position. Frames are counted as the replay plays, and the
// menu stops the capture itself once 2400 frames (one training slot) are in.
namespace {
    bool g_cap_active = false;
    int g_cap_peer = 1;           // replay peer 0 = P1 seat, 1 = P2 seat
    ReplayHead g_cap_start{};
    uint32_t g_cap_frames = 0;
}

void replayCaptureCancel() { g_cap_active = false; }
bool replayCaptureActive() { return g_cap_active; }
uint32_t replayCaptureFrames() { return g_cap_frames; }
bool replayCaptureFull() { return g_cap_active && g_cap_frames >= recordings::kMaxFrames; }

bool replayCaptureStart(int peer, std::wstring& err)
{
    if (!game_state.checkModeReplay()) { err = L"Open a replay first"; return false; }
    ReplayHead h{};
    if (!replay_manager.peerReadHead(peer, h)) { err = L"The replay isn't ready yet"; return false; }
    g_cap_active = true;
    g_cap_peer = peer;
    g_cap_start = h;
    g_cap_frames = 0;
    return true;
}

std::vector<uint16_t> replayCaptureStop(std::wstring& err)
{
    std::vector<uint16_t> out;
    if (!g_cap_active) return out;
    g_cap_active = false;
    ReplayHead cur{};
    if (!replay_manager.peerReadHead(g_cap_peer, cur)) { err = L"The replay isn't ready"; return out; }
    if (cur.m_Cursor < g_cap_start.m_Cursor ||
        (cur.m_Cursor == g_cap_start.m_Cursor && cur.m_Count < g_cap_start.m_Count))
    {
        err = L"The replay went back before the capture started";
        return out;
    }
    out = captureReplayInputs(g_cap_peer, g_cap_start.m_Cursor, cur.m_Cursor - g_cap_start.m_Cursor, cur.m_Count);
    // The first entry was partly played before the capture started.
    const size_t skip = std::min<size_t>(g_cap_start.m_Count, out.size());
    out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(skip));
    if (out.size() > recordings::kMaxFrames) out.resize(recordings::kMaxFrames);
    if (out.empty()) err = L"Nothing was captured";
    return out;
}

void hook_UpdateReplay(BattleReplayManager* Manager)
{
    if (g_skip_frame) return;  // timeline pause: the replay waits too
    if (replay_manager.isAnalyzerSame(nullptr))
    {
        orig_UpdateReplay(Manager);
        if (g_cap_active) g_cap_frames++;  // one replay frame played
    }
}

// Auto rewind point.  The engine owns all 20 BackupData slots, so instead of
// keeping our own snapshot ring we simply re-place the single manual save point
// on a timer.  Rewind depth is therefore "up to N seconds ago" rather than
// arbitrary scrubbing, but it costs no extra memory and reuses the save/load
// path that is already proven to work in replays.


// ---- F9 effect repair -------------------------------------------------------
// Every fight object (asw_entity) links one Unreal effect actor at +0x43C8, and
// the actor links back to its owner at +0x668. F9 restores the fight objects but
// not the actors. Online, the game's rollback tracks actors and cleans up after
// re-simulating; offline that tracking is off. So after F9:
//   - actors whose owner no longer links back to them are orphans (the stuck
//     HC reticle, stacked FD): release them with the game's own steps
//   - objects linking an actor that is gone or released get the link cleared,
//     the way the game does when a link fails (avoids a dangling pointer)
using funcFxRelease_t = void (*)(void*);
funcFxRelease_t orig_FxRelease1 = nullptr;
funcFxRelease_t orig_FxRelease2 = nullptr;
static std::set<std::wstring> g_fx_classes;

// Second kind of linked actor: fight object +0x43A0 <-> actor +0x29C98, with an
// "unlinked" flag at actor +0x2A094. The game's second rollback cleanup retires
// them per class: clear the link, set the flag, call that class's release. A
// rewind across a round change left one linked to a fight slot that no longer
// owned it, and the actor's update then crashed reading through that link.
using funcActorClass_t = Unreal::UClass* (*)();
using funcActorRelease_t = void (*)(void*);
funcActorClass_t orig_ActorClassA = nullptr, orig_ActorClassB = nullptr;
funcActorRelease_t orig_ActorReleaseA = nullptr, orig_ActorReleaseB = nullptr;
static std::set<std::wstring> g_actor_classes;

// Active objects own their links. whole_pool adds the inactive slots too: F9
// restores their links as well, and the next reset releases every slot's link.
static void collectFightObjects(std::vector<uint8_t*>& out, bool whole_pool = false)
{
    auto* eng = asw_engine::get();
    if (!eng) return;
    const int n = (std::min)(whole_pool ? eng->pool_count : eng->entity_count, 107);
    for (int i = 0; i < n; i++)
        if (auto* e = reinterpret_cast<uint8_t*>(whole_pool ? eng->pool[i] : eng->entities[i])) out.push_back(e);
}

// Each battle frame: remember the class of every linked effect actor, so F9 can list all of them.
static void trackEffectClasses()
{
    static std::set<void*> seen;
    std::vector<uint8_t*> objs;
    collectFightObjects(objs);
    for (auto* e : objs)
    {
        if (auto* a = *reinterpret_cast<Unreal::UObject**>(e + 0x43C8)) {
            auto* cls = a->GetClassPrivate();
            if (cls && seen.insert(cls).second) g_fx_classes.insert(cls->GetName());
        }
        if (auto* a = *reinterpret_cast<Unreal::UObject**>(e + 0x43A0)) {
            auto* cls = a->GetClassPrivate();
            if (cls && seen.insert(cls).second) g_actor_classes.insert(cls->GetName());
        }
    }
}

static void repairEffectsAfterLoad()
{
    if (!orig_FxRelease1 || !orig_FxRelease2 || g_fx_classes.empty()) return;
    std::vector<uint8_t*> objs;
    collectFightObjects(objs);
    std::set<void*> linked;
    for (auto* e : objs)
        if (auto* a = *reinterpret_cast<void**>(e + 0x43C8)) linked.insert(a);

    std::vector<Unreal::UObject*> actors;
    for (const auto& name : g_fx_classes) UObjectGlobals::FindAllOf(name.c_str(), actors);

    std::set<void*> usable;
    int released = 0, cleared = 0;
    for (auto* obj : actors)
    {
        auto* a = reinterpret_cast<uint8_t*>(obj);
        if (a[0x432] & 0x80) continue;                          // already released
        usable.insert(a);
        if (*reinterpret_cast<void**>(a + 0x668) == nullptr) continue;  // not linked to a fight object
        if (linked.count(a)) continue;                           // still owned
        *reinterpret_cast<void**>(a + 0x668) = nullptr;          // same steps as the game's cleanup
        a[0x432] |= 0x80;
        using vfn_t = void (*)(void*, bool);
        (*reinterpret_cast<vfn_t*>(*reinterpret_cast<uintptr_t*>(a) + 0x340))(a, true);
        orig_FxRelease1(a);
        orig_FxRelease2(a);
        usable.erase(a);
        released++;
    }
    std::vector<uint8_t*> slots;
    collectFightObjects(slots, true);
    for (auto* e : slots)
    {
        auto*& link = *reinterpret_cast<void**>(e + 0x43C8);
        if (link && !usable.count(link))
        {
            link = nullptr;
            e[0x5534] = 1;
            cleared++;
        }
    }
    // Second kind: actors whose fight object no longer links back to them
    int released_b = 0;
    if (orig_ActorClassA && orig_ActorClassB && orig_ActorReleaseA && orig_ActorReleaseB && !g_actor_classes.empty())
    {
        std::set<void*> linked_b;
        for (auto* e : objs)
            if (auto* a = *reinterpret_cast<void**>(e + 0x43A0)) linked_b.insert(a);
        std::vector<Unreal::UObject*> bs;
        for (const auto& name : g_actor_classes) UObjectGlobals::FindAllOf(name.c_str(), bs);
        Unreal::UClass* class_a = orig_ActorClassA();
        Unreal::UClass* class_b = orig_ActorClassB();
        for (auto* obj : bs)
        {
            auto* a = reinterpret_cast<uint8_t*>(obj);
            auto*& link = *reinterpret_cast<void**>(a + 0x29C98);
            if (!link || linked_b.count(a)) continue;
            link = nullptr;
            a[0x2A094] = 1;
            if (class_a && obj->IsA(class_a)) orig_ActorReleaseA(obj);
            else if (class_b && obj->IsA(class_b)) orig_ActorReleaseB(obj);
            released_b++;
        }
    }
    COMBO_LOG("F9 effects: released " + std::to_string(released) + ", cleared " + std::to_string(cleared)
              + ", actors released " + std::to_string(released_b)
              + ", tracked classes " + std::to_string(g_fx_classes.size()) + "+" + std::to_string(g_actor_classes.size()));
}

// ---- F9 DANGER banner sync ---------------------------------------------------
// The Negative Penalty DANGER banner is driven by two game calls, show(side, 2, 0)
// and hide(side), made only while the penalty value changes in normal play. F9
// restores the value without either call, so the banner could stay up. After F9,
// compare the restored value with the game's own threshold and call the right one.
using funcDangerShow_t = void (*)(int side, int mode, int param);
using funcDangerHide_t = void (*)(int side);
funcDangerShow_t orig_DangerShow = nullptr;
funcDangerHide_t orig_DangerHide = nullptr;

static void syncDangerBanner()
{
    if (!orig_DangerShow || !orig_DangerHide) return;
    auto* eng = asw_engine::get();
    if (!eng) return;
    for (int side = 0; side < 2; side++)
    {
        auto* e = reinterpret_cast<const uint8_t*>(eng->players[side].entity);
        if (!e) continue;
        const int value = *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(eng) + side * 0x198 + 0x170);
        const int threshold = *reinterpret_cast<const int16_t*>(e + 0x9CB6) * 100;  // same check as the game
        if (value > threshold) orig_DangerShow(side, 2, 0);
        else orig_DangerHide(side);
    }
}

static void placeRewindPoint()
{
    auto rollback = asw_rollback::get();
    if (!rollback) return;
    rollback->CurrentBackupDataIndex = 18;
    orig_SaveBackup(rollback);
    if (game_state.checkModeReplay()) replay_manager.saveBackup();
}


// ---- Timeline window controls --------------------------------------------------
// The timeline's pause skips the fight and replay updates frame by frame while the
// game keeps drawing, so its Play button can still be clicked. (F3 instead holds
// the whole update loop, which stops drawing too.) Its buttons queue hotkey
// presses for the start of the next frame, where the mod reads F6 / F9.
static std::atomic<bool> g_soft_pause{false};
static std::atomic<int> g_soft_steps{0};
static std::atomic<unsigned> g_pending_hotkeys{0};

void timelinePressHotkey(int action) { g_pending_hotkeys |= 1u << action; }
void timelineSetPaused(bool paused) { g_soft_pause = paused; if (!paused) g_soft_steps = 0; }
bool timelinePaused() { return g_soft_pause; }
void timelineStep() { g_soft_steps++; }
int timelineMode() { return !game_state.checkMode() ? 0 : game_state.checkModeReplay() ? 2 : 1; }

void hook_UpdateOnPreTick(AREDGameState_Battle* pThis, float DeltaSeconds)
{
    keybindings.checkBinds(false);
    for (unsigned m = g_pending_hotkeys.exchange(0), a = 0; m; m >>= 1, a++)
        if (m & 1) keybindings.setButtonIndex(static_cast<int>(a), true);

    // Timeline pause: skip this frame's fight and replay updates (one step lets one through).
    g_skip_frame = false;
    if (g_soft_pause && game_state.checkMode())
    {
        if (g_soft_steps > 0) g_soft_steps--;
        else g_skip_frame = true;
    }
    else if (g_soft_pause) g_soft_pause = false;  // left training / replay

    if (game_state.checkMode())
    {
        // No automatic rewind point. A timer that re-saved every N seconds
        // destroyed the user's manual F6 point before they could return to it
        // (F9 would only go back as far as the last automatic save), and in a
        // replay it re-snapshotted all six read heads and reset takeover state
        // on that same timer.

        if (keybindings.getButtonState(keybindings.TOGGLE_SAVE_BUTTON))
        {
            keybindings.resetButton(keybindings.TOGGLE_SAVE_BUTTON);
            placeRewindPoint();
        }
        if (replay_manager.loadState || (!game_state.checkModeReplay()
            && keybindings.getButtonState(keybindings.TOGGLE_LOAD_BUTTON)))
        {
            keybindings.resetButton(keybindings.TOGGLE_LOAD_BUTTON);
            replay_manager.loadState = false;
            auto rollback = asw_rollback::get();
            rollback->CurrentBackupDataIndex = 0;
            rollback->RollbackFrame = 1;
            rollback->TotalRollbackFrames = 0;
            auto backupData = rollback->BackupData[19];
            orig_Rollback(rollback);
            if (orig_EffectCleanupA && orig_EffectCleanupB)
            {
                orig_EffectCleanupA(rollback);
                orig_EffectCleanupB(rollback);
            }
            repairEffectsAfterLoad();
            syncDangerBanner();
            rollback->BackupData[19] = backupData;
            rollback->RollbackFrame = 0;
            rollback->TotalRollbackFrames = 0;
            replay_manager.restoreReplayHead();
        }
    }

    orig_UpdateOnPreTick(pThis, DeltaSeconds);
}

/* Mod Definition */
class StriveFrameData : public CppUserModBase
{
public:
    PLH::x64Detour* UpdateBattle_Detour;
    PLH::x64Detour* MatchStart_Detour;
    PLH::x64Detour* UpdateBattleInput_Detour;
    PLH::x64Detour* UpdateSystem_Detour;
    PLH::x64Detour* UpdateReplay_Detour;
    PLH::x64Detour* UpdateOnPreTick_Detour;

    StriveFrameData()
        : CppUserModBase()
    {
        ModName = STR("Strive Frame Data");
        ModVersion = STR("1.05");
        ModDescription = STR("A tool to display frame advantage.");
        ModAuthors = STR("pbozai, Sevoi");
        UpdateBattle_Detour = nullptr;
        MatchStart_Detour = nullptr;
        // Do not change this unless you want to target a UE4SS version
        // other than the one you're currently building with somehow.
        // ModIntendedSDKVersion = STR("2.6");

        Output::send<LogLevel::Verbose>(STR("StriveLabs Started\n"));
    }

    ~StriveFrameData() override
    {
    }

    auto on_update() -> void override
    {
    }

    auto on_unreal_init() -> void override
    {
        Program = &UE4SSProgram::get_program();
        const uint64_t UpdateBattle_Addr = sigscan::get().scan(
            "\x40\x53\x57\x41\x54\x41\x55\x48\x81\xEC\x88\x00\x00\x00", "xxxxxxxxxxxxxx");
        UpdateBattle_Detour = new PLH::x64Detour(UpdateBattle_Addr, reinterpret_cast<uint64_t>(&hook_UpdateBattle),
                                                 reinterpret_cast<uint64_t*>(&orig_UpdateBattle));
        UpdateBattle_Detour->hook();

        // https://github.com/TheLettuceClub/StriveSAMMI/blob/7292cc225bd4de2b389e92b227c1b2793609c4e1/GGSTSammi/src/dllmain.cpp#L435
        const uint64_t MatchStart_Addr = sigscan::get().scan(
            "\x48\x89\x5c\x24\x10\x55\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x8d\x6c\x24\xf0\x48\x81\xec\x10\x01\x00\x00\x0f\x29\xb4\x24",
            "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
        MatchStart_Detour = new PLH::x64Detour(MatchStart_Addr, reinterpret_cast<uint64_t>(&hook_MatchStart),
                                               reinterpret_cast<uint64_t*>(&orig_MatchStart));
        MatchStart_Detour->hook();

        const uint64_t UpdateBattleInput_Addr = sigscan::get().scan(
            "\x0F\xB7\x41\x02\x44\x0F\xB7\xD2", "xxxxxxxx");
        UpdateBattleInput_Detour = new PLH::x64Detour(UpdateBattleInput_Addr,
                                                      reinterpret_cast<uint64_t>(&hook_UpdateBattleInput),
                                                      reinterpret_cast<uint64_t*>(&orig_UpdateBattleInput));
        UpdateBattleInput_Detour->hook();

        const uint64_t UpdateSystem_Addr = sigscan::get().scan(
            "\x48\x89\x5C\x24\x00\x48\x89\x74\x24\x00\x57\x48\x83\xEC\x00\x33\xDB\x48\x8B\xF1\x8B\xFB",
            "xxxx?xxxx?xxxx?xxxxxxx");
        UpdateSystem_Detour = new PLH::x64Detour(UpdateSystem_Addr, reinterpret_cast<uint64_t>(&hook_UpdateSystem),
                                                 reinterpret_cast<uint64_t*>(&orig_UpdateSystem));
        UpdateSystem_Detour->hook();

        const uint64_t UpdateReplay_Addr = sigscan::get().scan(
            "\x40\x53\x41\x56\x48\x83\xEC\x68\x45\x32\xF6",
            "xxxxxxxxxxx");
        UpdateReplay_Detour = new PLH::x64Detour(UpdateReplay_Addr, reinterpret_cast<uint64_t>(&hook_UpdateReplay),
                                                 reinterpret_cast<uint64_t*>(&orig_UpdateReplay));
        UpdateReplay_Detour->hook();

        const uint64_t UpdateOnPreTick_Addr = sigscan::get().scan(
            "\xE8\x00\x00\x00\x00\x33\xED\xC6\x86",
            "x????xxxx") - 0x45;
        UpdateOnPreTick_Detour = new PLH::x64Detour(UpdateOnPreTick_Addr, reinterpret_cast<uint64_t>(&hook_UpdateOnPreTick),
                                                 reinterpret_cast<uint64_t*>(&orig_UpdateOnPreTick));
        UpdateOnPreTick_Detour->hook();

        const uintptr_t GetGameMode_Addr = sigscan::get().scan("\x0F\xB6\x81\xF0\x02\x00\x00\xC3", "xxxxxxxx");
        orig_GetGameMode = reinterpret_cast<funcGetGameMode_t>(GetGameMode_Addr);

        const uintptr_t GameKeyToRecFlag_Addr = sigscan::get().scan(
            "\x89\x54\x24\x00\x89\x4C\x24\x00\x53\x55\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x83\xEC\x00\x8B\xFA",
            "xxx?xxx?xxxxxxxxxxxxxxx?xx");
        orig_GameKeyToRecFlag = reinterpret_cast<funcGameKeyToRecFlag_t>(GameKeyToRecFlag_Addr);

        const uintptr_t SaveBackup = sigscan::get().scan(
            "\x48\x89\x74\x24\x00\x57\x48\x83\xEC\x00\x8B\x41\x00\x48\x8B\xF9\xFF\xC0",
            "xxxx?xxxx?xx?xxxxx");
        orig_SaveBackup = reinterpret_cast<funcSaveBackup_t>(SaveBackup);

        const uintptr_t Rollback_Addr = sigscan::get().scan(
            "\x48\x89\x5C\x24\x00\x48\x89\x6C\x24\x00\x41\x56\x48\x83\xEC\x00\x8B\x81",
            "xxxx?xxxx?xxxxx?xx");
        orig_Rollback = reinterpret_cast<funcRollback_t>(Rollback_Addr);

        // Netplay battle loop, last re-simulated frame:
        //   call GetRollback ; mov rcx,rax ; call CleanupA ; call GetRollback ; mov rcx,rax ; call CleanupB ; call X ; test al,al
        // Optional: if the pattern moves in a game update, F9 still works, effects just are not cleaned.
        try
        {
            const uintptr_t hit = sigscan::get().scan(
                "\xE8\x00\x00\x00\x00\x48\x8B\xC8\xE8\x00\x00\x00\x00\xE8\x00\x00\x00\x00\x48\x8B\xC8\xE8\x00\x00\x00\x00\xE8\x00\x00\x00\x00\x84\xC0",
                "x????xxxx????x????xxxx????x????xx");
            const auto rel = [](uintptr_t call) { return call + 5 + *reinterpret_cast<int32_t*>(call + 1); };
            if (rel(hit) == rel(hit + 13))  // both getters must be the same function
            {
                orig_EffectCleanupA = reinterpret_cast<funcEffectCleanup_t>(rel(hit + 8));
                orig_EffectCleanupB = reinterpret_cast<funcEffectCleanup_t>(rel(hit + 21));
                // Inside the second cleanup, per actor:
                //   mov rbx,[rbx+50] ; call ClassA
                //   mov rcx,rbx ; je +3 ; mov rcx,r12 ; call ReleaseA ; call ClassB
                //   mov rbx,r12 ; mov rcx,rbx ; call ReleaseB
                const auto* cb = reinterpret_cast<const uint8_t*>(orig_EffectCleanupB);
                auto find = [cb](std::initializer_list<int> pat) -> int {
                    const int n = static_cast<int>(pat.size());
                    for (int i = 0; i < 0x500 - n; i++) {
                        int k = 0;
                        for (int b : pat) { if (b >= 0 && cb[i + k] != b) break; k++; }
                        if (k == n) return i + n - 1;  // index of the final E8
                    }
                    return -1;
                };
                const int ca = find({0x48, 0x8B, 0x5B, 0x50, 0xE8});
                const int ra = find({0x48, 0x8B, 0xCB, 0x74, 0x03, 0x49, 0x8B, 0xCC, 0xE8});
                const int rb = find({0x49, 0x8B, 0xDC, 0x48, 0x8B, 0xCB, 0xE8});
                if (ca >= 0 && ra >= 0 && rb >= 0 && cb[ra + 5] == 0xE8)
                {
                    const auto at = [cb](int i) { return reinterpret_cast<uintptr_t>(cb + i); };
                    orig_ActorClassA = reinterpret_cast<funcActorClass_t>(rel(at(ca)));
                    orig_ActorReleaseA = reinterpret_cast<funcActorRelease_t>(rel(at(ra)));
                    orig_ActorClassB = reinterpret_cast<funcActorClass_t>(rel(at(ra + 5)));
                    orig_ActorReleaseB = reinterpret_cast<funcActorRelease_t>(rel(at(rb)));
                }
                const auto* body = reinterpret_cast<const uint8_t*>(orig_EffectCleanupA);
                for (int i = 0; i < 0x400; i++)
                {
                    if (body[i] == 0x48 && body[i + 1] == 0x8B && body[i + 2] == 0x4B && body[i + 3] == 0x48 && body[i + 4] == 0xE8
                        && body[i + 9] == 0x48 && body[i + 10] == 0x8B && body[i + 11] == 0x4B && body[i + 12] == 0x48 && body[i + 13] == 0xE8)
                    {
                        orig_FxRelease1 = reinterpret_cast<funcFxRelease_t>(rel(reinterpret_cast<uintptr_t>(body + i + 4)));
                        orig_FxRelease2 = reinterpret_cast<funcFxRelease_t>(rel(reinterpret_cast<uintptr_t>(body + i + 13)));
                        break;
                    }
                }
            }
        }
        catch (...) {}

        // DANGER banner: the game's Negative Penalty check, which calls show(side, 2, 0) or hide(side).
        //   movsx eax,word[rdi+9CB6] ; test bpl,bpl ; jne ; imul edx,eax,64 ; cmp ecx,edx ; jle ;
        //   movzx ecx,[rdi+4C] ; xor r8d,r8d ; lea edx,[r8+2] ; call SHOW
        //   ... imul eax,eax,64 ; cmp ecx,eax ; jg ; movzx ecx,[rdi+4C] ; call HIDE
        // Optional: if either moves in a game update, F9 just skips the banner sync.
        try
        {
            const auto rel = [](uintptr_t call) { return call + 5 + *reinterpret_cast<int32_t*>(call + 1); };
            const uintptr_t show = sigscan::get().scan(
                "\x0F\xBF\x87\xB6\x9C\x00\x00\x40\x84\xED\x75\x00\x6B\xD0\x64\x3B\xCA\x7E\x00\x0F\xB6\x4F\x4C\x45\x33\xC0\x41\x8D\x50\x02\xE8",
                "xxxxxxxxxxx?xxxxxx?xxxxxxxxxxxx");
            const uintptr_t hide = sigscan::get().scan(
                "\x6B\xC0\x64\x3B\xC8\x7F\x00\x0F\xB6\x4F\x4C\xE8", "xxxxxx?xxxxx");
            orig_DangerShow = reinterpret_cast<funcDangerShow_t>(rel(show + 0x1E));
            orig_DangerHide = reinterpret_cast<funcDangerHide_t>(rel(hide + 0xB));
        }
        catch (...) {}

        const uintptr_t GetReplayManager_Addr = sigscan::get().scan(
            "\x48\x8D\x05\x00\x00\x00\x00\xC3\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\x48\x63\xC1",
            "xxx????xxxxxxxxxxxx");
        orig_GetReplayManager = reinterpret_cast<funcGetReplayManager_t>(GetReplayManager_Addr);

        // Resolve is_battle flag byte.  GGST's layout for this byte shifted
        // between game patches: hardcoded 0x4F39B73 (Apr/May 2025 builds),
        // hardcoded 0x4fa4713 (Sep 2025), and a sigscan-derived address
        // (Nov 2025 builds, where the offset was no longer stable).  Picking
        // the wrong one corrupts game state and crashes on replay takeover.
        //
        // Strategy: compute all candidates, then validate each by reading
        // (NOT writing) the byte.  The real is_battle is always 0 or 1.
        // Use the first candidate that survives.  If none do, leave
        // is_battle_ptr null; the hook skips the toggle gracefully.
        {
            const auto game_mod = GetModuleHandleA("GGST-Win64-Shipping.exe");
            const uintptr_t mod_base = game_mod
                ? reinterpret_cast<uintptr_t>(game_mod)
                : reinterpret_cast<uintptr_t>(GetModuleHandleA(NULL));

            struct Candidate { const char* name; uint8_t* ptr; };
            std::vector<Candidate> candidates;

            // Candidate 1: Nov-2025 upstream sigscan -> RIP-relative decode.
            try {
                const uintptr_t hit = sigscan::get().scan(
                    "\xE8\x00\x00\x00\x00\x84\xC0\x75\x00\xE8\x00\x00\x00\x00\x84\xC0\x75\x00\x48\x8B\xCF\x48\x8D\x14\x5B\x48\x03\x0D\x00\x00\x00\x00\x0F\xB6\x8C\xD1",
                    "x????xxx?x????xxx?xxxxxxxxxx????xxxx");
                const uintptr_t fn = *reinterpret_cast<uint32_t*>(hit + 0x1) + hit + 0x5;
                orig_BattleKeyCheck = fn;
                // Decode the first instruction's RIP-relative disp32 to reach is_battle.
                auto p = reinterpret_cast<uint8_t*>(
                    *reinterpret_cast<int32_t*>(fn + 0x3) + fn + 0x7);
                candidates.push_back({"sigscan(nov)", p});
            } catch (...) {
                orig_BattleKeyCheck = 0;
            }

            // Candidate 2: Sep-2025 upstream hardcoded offset.
            candidates.push_back({"hardcoded 0x4fa4713",
                reinterpret_cast<uint8_t*>(mod_base + 0x4fa4713)});

            // Candidate 3: Apr/May-2025 upstream hardcoded offset.
            candidates.push_back({"hardcoded 0x4F39B73",
                reinterpret_cast<uint8_t*>(mod_base + 0x4F39B73)});

            for (const auto& c : candidates) {
                if (!c.ptr) continue;
                if (!is_ptr_in_game_module(c.ptr)) continue;
                // Validate by READ only — never write until we've picked a winner.
                // A real boolean flag will be 0 or 1; anything else means the
                // address lands in .text/.rdata or an unrelated data byte.
                const uint8_t b = *c.ptr;
                if (b == 0 || b == 1) {
                    is_battle_ptr = c.ptr;
                    break;
                }
            }
        }

        replay_manager.setManager(reinterpret_cast<ReplayManager_Internal*>(orig_GetReplayManager() - 0x8));

        ASWInitFunctions();
        //    bbscript::BBSInitializeFunctions();
        keybindings.loadButtons();

        // Initialize logging
        auto working_dir = UE4SSProgram::get_program().get_working_directory();
        ComboLog::instance().init(working_dir);
        COMBO_LOG("StriveLabs loaded");
        crash_log::install(working_dir);  // first hard fault -> combo_log.txt + StriveLabs_crash.dmp, for user reports
        select_memory::init(working_dir);
        imgui_overlay::install();
        COMBO_LOG(orig_FxRelease1 ? "effect repair: found" : "effect repair: NOT found");
        COMBO_LOG(orig_DangerHide ? "danger sync: found" : "danger sync: NOT found");
        COMBO_LOG(orig_ActorReleaseB ? "actor repair: found" : "actor repair: NOT found");
    }
};

extern "C" {
STRIVEFRAMEDATA_API CppUserModBase* start_mod()
{
    return new StriveFrameData();
}

STRIVEFRAMEDATA_API void uninstall_mod(CppUserModBase* mod)
{
    delete mod;
}
}
