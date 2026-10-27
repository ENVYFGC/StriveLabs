#include <algorithm>
#include <polyhook2/Detour/x64Detour.hpp>

#include "arcsys.h"
#include "keybindings.h"
#include "draw_utils.h"
#include "framebar.h"
#include "hitboxes.h"
#include "keybinds.h"
#include "imgui_overlay.h"
namespace crash_log { void install(const std::filesystem::path& dir); }
namespace select_memory { void init(const std::filesystem::path& dir); void save(); }
#include "menu.h"
#include "recordings.h"
#include "replay_manager.h"
#include "sigscan.h"
#include "game_state.h"
#include "timeline.h"
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
#include <Xinput.h>
#pragma comment(lib, "xinput9_1_0.lib")

#define STRIVEFRAMEDATA_API __declspec(dllexport)


/* Definitions */

// Classes
class BattleReplayManager;

// Functions
using funcAHUDPostRender_t = void (*)(void*);
using funcACamUpdateCamera_t = void (*)(void*, float);
using funcMatchStart_t = void (*)(AREDGameState_Battle*);
using funcUpdateBattle_t = void (*)(AREDGameState_Battle*, float);
using funcUpdateBattleInput_t = void (*)(asw_inputs*, RECFLG_ENUM);
using funcUpdateSystem_t = void (*)(uintptr_t);
using funcUpdateReplay_t = void (*)(BattleReplayManager*);
using funcUpdateOnPreTick_t = void (*)(AREDGameState_Battle*, float);

// Hooks
void hook_AHUDPostRender(void*);
void hook_ACamUpdateCamera(void*, float);
void hook_MatchStart(AREDGameState_Battle*);
void hook_UpdateBattle(AREDGameState_Battle*, float);

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
funcAHUDPostRender_t orig_AHUDPostRender;
funcACamUpdateCamera_t orig_ACamUpdateCamera;
funcMatchStart_t orig_MatchStart;
funcUpdateBattle_t orig_UpdateBattle;
funcUpdateBattleInput_t orig_UpdateBattleInput;
funcUpdateSystem_t orig_UpdateSystem;
funcUpdateReplay_t orig_UpdateReplay;
funcUpdateOnPreTick_t orig_UpdateOnPreTick;

// State Data
UE4SSProgram* Program;

// Trackers
GameState game_state;

Keybinds keybindings;

static void pollGamepad();

// Training record / playback / reset from the mod's binds. The training mode checks
// these buttons itself: it asks the button config which buttons are set for them
// (entries 0xB playback, 0xC record, 0xD reset position) and then asks the pad's key controller
// whether one of those was just pressed. While a bind fires, the lookup also hands out
// a made-up button no pad has, and "just pressed" says yes for it for that one frame.
// Works with or without the game's own buttons set for them.
static uint32_t g_training_press = 0;    // made-up buttons pressed this frame
static uint32_t g_training_pending = 0;  // pressed while the fight is held (mod pause, timeline pause)
static bool g_battle_ran = false;        // a battle update ran since the last system update
constexpr uint32_t kFakeReset = 0x1000000, kFakeRecord = 0x2000000, kFakePlayback = 0x4000000;

using funcKeyConfigLookup_t = uint32_t (*)(int pad, int entry);
funcKeyConfigLookup_t orig_KeyConfigLookup = nullptr;
uint32_t hook_KeyConfigLookup(int pad, int entry)
{
    uint32_t mask = orig_KeyConfigLookup(pad, entry);
    if (g_training_press && pad >= 0 && pad < PAD_ID_MAX)
    {
        if (entry == 0xB) mask |= g_training_press & kFakePlayback;
        else if (entry == 0xC) mask |= g_training_press & kFakeRecord;
        else if (entry == 0xD) mask |= g_training_press & kFakeReset;
    }
    return mask;
}

using funcKeyJustPressed_t = bool (*)(void* controller, uint32_t mask);
funcKeyJustPressed_t orig_KeyJustPressed = nullptr;
bool hook_KeyJustPressed(void* controller, uint32_t mask)
{
    if (mask & g_training_press) return true;
    return orig_KeyJustPressed(controller, mask);
}

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
    ReplayManager::instance().onMatchStart();
    select_memory::save();  // remember this character select's picks for the next restart
    Timeline::instance().setPaused(false);
    g_overlays_visible = false;
    game_state.matchStarted = true;
    game_state.roundActive = false;
    // A training position reset also lands here. Keep the hold frame on
    // through it, so the user stays paused at the new position.
    if (game_state.checkModeTraining()) pause_manager.stopAdvance();
    else pause_manager.reset();
    tracker.reset();
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
    // One frame per press: last frame's is over, also when this mode isn't handled.
    g_training_press = 0;
    g_battle_ran = true;
    if (!game_state.checkMode())
    {
        g_training_pending = 0;
        orig_UpdateBattle(GameState, DeltaTime);
        return;
    }
    g_last_battle_update = GetTickCount64();
    if (!game_state.mode_battle_logged) { game_state.mode_battle_logged = true; COMBO_LOG("battle code running in this mode"); }

    std::this_thread::sleep_for(std::chrono::milliseconds(ModMenu::instance().delayAmount()));

    pollGamepad();  // before anything reads button states this frame
    ReplayManager::instance().onBattleUpdate();
    pause_manager.checkPause();
    g_last_battle_update = GetTickCount64();  // the pause can hold this call for a long time

    // After the pause, which reads binds while it holds: a press made then counts too.
    // Held until the fight actually updates, so a press during a pause isn't lost.
    uint32_t pressed = 0;
    if (keybindings.takeAction(binds::TRAINING_RECORD)) pressed |= kFakeRecord;
    if (keybindings.takeAction(binds::TRAINING_PLAYBACK)) pressed |= kFakePlayback;
    if (keybindings.takeAction(binds::TRAINING_RESET)) pressed |= kFakeReset;
    if (game_state.checkModeTraining() && orig_KeyJustPressed) g_training_pending |= pressed;
    else g_training_pending = 0;

    if (game_state.checkModeReplay()) ReplayManager::instance().update();

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

    if (game_state.roundJustStarted)
    {
        game_state.roundJustStarted = false;
        g_overlays_visible = true;
        // Round call is the sync point: an armed stream begins here, and
        // re-begins on every reset, so the dummy's inputs line up with the
        // round rather than with when the user opened the menu.
        recordings::streamOnRoundStart();
        ReplayManager::instance().onRoundStart();
    }
    if (game_state.resetting)
    {
        the_bar.reset();
        tension_overlay.reset();
    }

    if (!Timeline::instance().canAdvanceFrame()) return;  // timeline pause: no fight update this frame
    if (g_training_pending)
    {
        g_training_press = g_training_pending;
        g_training_pending = 0;
        COMBO_LOG(std::string("training: ") + (g_training_press & kFakeRecord ? "record " : "") + (g_training_press & kFakePlayback ? "playback " : "")
                  + (g_training_press & kFakeReset ? "reset " : "") + "bind");
    }
    orig_UpdateBattle(GameState, DeltaTime);
    ReplayManager::instance().onBattleFrameProcessed();

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

void hook_UpdateBattleInput(asw_inputs* Analyzer, RECFLG_ENUM recFlag)
{
    // Menu open: swallow pad/keyboard so the D-pad drives the menu, not the character.
    // In a replay only the side being taken over is the player's; every other side
    // is fed from the replay itself, and zeroing it froze both characters and
    // desynced the replay.
    if (ModMenu::instance().isShowing() &&
        (!game_state.checkModeReplay() || (!ReplayManager::instance().playbackEnabled()
            && ReplayManager::instance().isTakeoverInput(Analyzer))))
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

    if (!game_state.checkModeReplay() || !ReplayManager::instance().isTakeoverInput(Analyzer))
    {
        orig_UpdateBattleInput(Analyzer, recFlag);
        return;
    }

    orig_UpdateBattleInput(Analyzer, ReplayManager::instance().takeoverInput(recFlag, ModMenu::instance().isShowing()));
}

// Keys are only read during fights; drop the ones pressed outside one (character
// select, loading), or they would all fire on the first fight frame.
static void dropKeysOutsideFight()
{
    if (GetTickCount64() - g_last_battle_update > 250)
    {
        keybindings.checkBinds(false);
        keybindings.resetButtons();
    }
}

void hook_UpdateSystem(uintptr_t System)
{
    orig_UpdateSystem(System);
    ReplayManager::instance().setInputSystem(System);
    // Out of battle (the fight ended on the frame a bind fired), nothing clears the
    // press at the start of the next battle update, so clear it here, and drop one
    // still held from a pause.
    if (!g_battle_ran) g_training_press = g_training_pending = 0;
    g_battle_ran = false;
    dropKeysOutsideFight();
    // The pads' key controllers share one vtable; its "just pressed" check is the
    // function at +0x60. Hooked once, as soon as the first controller exists.
    if (!orig_KeyJustPressed && orig_KeyConfigLookup && System)
    {
        if (auto* controller = *reinterpret_cast<void**>(System + 0x1F8))
        {
            auto** vtable = *reinterpret_cast<const void***>(controller);
            orig_KeyJustPressed = reinterpret_cast<funcKeyJustPressed_t>(vtable_hook(vtable, 0x60 / 8, reinterpret_cast<const void*>(&hook_KeyJustPressed)));
        }
    }
}


void hook_UpdateReplay(BattleReplayManager* Manager)
{
    if (!Timeline::instance().canAdvanceFrame()) return;  // timeline pause: the replay waits too
    if (ReplayManager::instance().playbackEnabled())
    {
        orig_UpdateReplay(Manager);
        ReplayManager::instance().onReplayFrame();
    }
}

void hook_UpdateOnPreTick(AREDGameState_Battle* pThis, float DeltaSeconds)
{
    dropKeysOutsideFight();  // first tick after a load: what was pressed during it
    keybindings.checkBinds(false);
    // Rewind needs the queued Timeline hotkeys and can request Pause/Step.
    // Run it between input processing and the final decision for this tick.
    Timeline::instance().onPreTick([] {
        ReplayManager::instance().onPreTick();
    });

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
    PLH::x64Detour* KeyConfigLookup_Detour = nullptr;

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

        game_state.initialize();
        Timeline::instance().initialize(game_state, keybindings);

        ReplayManager::instance().initialize(game_state, keybindings, Timeline::instance());

        // Optional: without it the training record / playback / reset binds do nothing.
        bool training_hook = false;
        try
        {
            const uint64_t KeyConfigLookup_Addr = sigscan::get().scan(
                "\x48\x89\x5C\x24\x10\x57\x48\x83\xEC\x50\x48\x8B\x05\x00\x00\x00\x00\x48\x33\xC4\x48\x89\x44\x24\x40\x8B\x05\x00\x00\x00\x00\x8B\xD9\x48\x63\xFA\x85\xC9\x74\x06",
                "xxxxxxxxxxxxx????xxxxxxxxxx????xxxxxxxxx");
            KeyConfigLookup_Detour = new PLH::x64Detour(KeyConfigLookup_Addr, reinterpret_cast<uint64_t>(&hook_KeyConfigLookup),
                                                        reinterpret_cast<uint64_t*>(&orig_KeyConfigLookup));
            training_hook = KeyConfigLookup_Detour->hook();
        }
        catch (...) {}

        ASWInitFunctions();
        //    bbscript::BBSInitializeFunctions();
        keybindings.loadButtons();

        // Initialize logging
        auto working_dir = UE4SSProgram::get_program().get_working_directory();
        ComboLog::instance().init(working_dir);
        COMBO_LOG("StriveLabs loaded");
        if (!training_hook) COMBO_LOG("training binds: button config lookup not found");
        crash_log::install(working_dir);  // first hard fault -> combo_log.txt + StriveLabs_crash.dmp, for user reports
        select_memory::init(working_dir);
        imgui_overlay::install();
        ReplayManager::instance().logStatus();
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
