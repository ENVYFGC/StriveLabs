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
namespace recolor_test { void init(const std::filesystem::path& dir); void update(bool round_started); void afterBattleUpdate(); void battleGone(); void menuUpdate(bool in_fight); void menuRelease(); bool isOpen(); bool holdsFight(); bool setupRunning();
                         void toggleCamera(); bool soloCamera(); void soloCameraOff(); void toggle(); void outsideTraining(); }
#include "box_editor.h"
#include "menu.h"
#include "recordings.h"
#include "replay_manager.h"
#include "sigscan.h"
#include "pak_mount.h"
#include "game_state.h"
#include "timeline.h"
#include "tension_overlay.h"
#include "combo_log.h"
#include "combo_trials.h"
#include "damage_panel.h"
#include "game_menu.h"
#include "handshake.h"
#include "recolor_share.h"
#include "combo_online.h"
#include "perf_log.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
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
static bool g_mode_battle_ran = false;   // one this mod handles (training, replay...) ran since the last system update
// The system updates in a row with no battle update (any, or a handled one). With the time since one, they say
// whether a fight is over: a single slow frame is never the end of one.
static int g_no_battle_updates = 2, g_no_mode_battle_updates = 2;  // (none yet: no fight at startup)
static bool g_any_fight = false, g_mode_fight = false;  // set at the start of each system update
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

// The pause's style is the mod's setting, but with the color editor open the fight stops while the game
// keeps running (style 1), so the editor's free camera and picking work on the frozen frame.
static int pauseStyle() { return (recolor_test::isOpen() || recolor_test::soloCamera()) && game_state.checkModeTraining() ? 1 : ModMenu::instance().pauseType(); }  // training only, since a replay keeps feeding inputs (the free camera needs the game drawing too)

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

        if (pauseStyle() == 0)
        {
            while (isPaused && !shouldAdvance)
            {
                keybindings.checkBinds(false);
                pollGamepad();
                // Under Proton, key presses are only read in when the window's thread checks its
                // queue, and this loop holds that thread. This lets them in without handling any.
                // (Wine only: on Windows input arrives anyway, so nothing changes there.)
                static const bool wine = GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version") != nullptr;
                if (wine) MsgWaitForMultipleObjectsEx(0, nullptr, 0, QS_ALLINPUT, 0);
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
        try { hookFuncsBody(); }
        catch (...) { COMBO_LOG("hud and camera hooks: the game's code wasn't found, so they aren't in"); }
    }

    void hookFuncsBody()
    {

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

namespace perf {
std::atomic<bool> on{false};
namespace {
    double g_sum[BUCKETS]{}, g_tick[BUCKETS]{}, g_mod = 0, g_worst = 0;
    int g_ticks = 0, g_huds = 0, g_slow = 0;
    const char* const g_names[] = {"replay", "menu", "pause menu", "trials", "damage panel", "colors", "frame bar"};
    const char* g_worst_part = "rest";
    double g_worst_part_ms = 0;
    std::mutex g_overlay_m;
    double g_copy = 0, g_draw = 0;
    int g_frames = 0;
}

void add(int bucket, double ms)
{
    if (bucket == HUD) g_sum[HUD] += ms;
    else g_tick[bucket] += ms;
}

void hudDone() { g_huds++; }

void overlayFrame(double copy_ms, double draw_ms)
{
    std::lock_guard<std::mutex> lock(g_overlay_m);
    g_copy += copy_ms;
    g_draw += draw_ms;
    g_frames++;
}

void tickDone(double whole_ms, bool counted)
{
    const double mod = (std::max)(whole_ms - g_tick[GAME] - g_tick[HELD], 0.0);
    if (counted)
    {
        for (int b = 0; b < BUCKETS; b++)
            if (b != HUD) g_sum[b] += g_tick[b];
        g_mod += mod;
        if (mod > 4.0) g_slow++;
        if (mod > g_worst)
        {
            g_worst = mod;
            g_worst_part = "rest";
            g_worst_part_ms = 0;
            double known = 0;
            for (int b = REPLAY; b <= BAR; b++)
            {
                known += g_tick[b];
                if (g_tick[b] > g_worst_part_ms) { g_worst_part_ms = g_tick[b]; g_worst_part = g_names[b]; }
            }
            if (mod - known > g_worst_part_ms) { g_worst_part_ms = mod - known; g_worst_part = "rest"; }
        }
        g_ticks++;
    }
    for (double& t : g_tick) t = 0;
    if (g_ticks < 600) return;
    const double n = g_ticks;
    double named = 0;
    for (int b = REPLAY; b <= BAR; b++) named += g_sum[b];
    int frames = 0;
    double copy = 0, draw = 0;
    {
        std::lock_guard<std::mutex> lock(g_overlay_m);
        frames = g_frames;
        copy = g_copy;
        draw = g_draw;
        g_frames = 0;
        g_copy = g_draw = 0;
    }
    char line[540];
    snprintf(line, sizeof(line),
             "perf: mod %.3f ms per tick (replay %.3f, menu %.3f, pause menu %.3f, trials %.3f, damage panel %.3f, colors %.3f, frame bar %.3f, rest %.3f), "
             "worst tick %.2f ms (%s %.2f), %d ticks over 4 ms, hud draw %.3f ms per frame, overlay windows %.3f ms per frame (menu data copy %.4f), %d ticks without catch-up, %d frames",
             g_mod / n, g_sum[REPLAY] / n, g_sum[MENU] / n, g_sum[PAUSE_MENU] / n, g_sum[TRIALS] / n, g_sum[DAMAGE] / n, g_sum[COLORS] / n, g_sum[BAR] / n,
             (std::max)(g_mod - named, 0.0) / n, g_worst, g_worst_part, g_worst_part_ms, g_slow, g_huds ? g_sum[HUD] / g_huds : 0.0, frames ? draw / frames : 0.0, frames ? copy / frames : 0.0,
             g_ticks, frames);
    COMBO_LOG(line);
    for (double& t : g_sum) t = 0;
    g_mod = g_worst = g_worst_part_ms = 0;
    g_worst_part = "rest";
    g_ticks = g_huds = g_slow = 0;
}
}

FrameBar the_bar;
bool g_catching_up = false;
extern bool g_timeline_hooks_tried;
void installTimelineHooks(bool at_startup);
TensionOverlay tension_overlay;

// Overlays (frame bar, hitboxes, tension) stay hidden from a new match load until its
// first round starts, so they do not draw over the versus splash with the previous
// fight's numbers. Training resets and wall breaks do not load a match, so they keep showing.
static bool g_overlays_visible = true;
// Loading screens (e.g. changing character from the training menu) keep the mode at
// training but stop the battle update, so also require a battle update in the last 300 ms.
static std::atomic<unsigned long long> g_last_battle_update{0};
static std::atomic<unsigned long long> g_any_battle_update{0};  // any fight, also in modes the mod leaves alone
// An online fight on the online version: only the battle version window shows.
bool onlineVersionScreen() { return GetTickCount64() - g_any_battle_update < 300 && combo_trials::onlineVersionInUse(); }

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
    perf::Scope hud_time(perf::HUD);
    if (perf::on) perf::hudDone();
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
    g_any_battle_update = GetTickCount64();
    struct WholeTick
    {
        const bool timed = perf::on;
        const std::chrono::steady_clock::time_point t0 = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        ~WholeTick() { if (timed) perf::tickDone(perf::since(t0), !g_catching_up); }
    } whole_tick;
    if (!g_timeline_hooks_tried && ModMenu::instance().replayTimeline()) installTimelineHooks(false);
    handshake::update(GameState);
    if (const int bv = combo_trials::battleVersion(); bv > 0)  // paused and online too
      imgui_overlay::setBattleVersion(battleVersionName(bv) + (combo_trials::onlineVersionInUse() ? " (online)" : ""));
    const bool mode_handled = game_state.checkMode();  // (also brings the mode up to date for the line below)
    if (!game_state.checkModeTraining()) { box_editor::outsideTraining(); recolor_test::soloCameraOff(); recolor_test::outsideTraining(); recordings::streamStop(); combo_trials::streamRestore(false); }  // (never Script & boxes' edits, or the free camera, outside training)
    if (!mode_handled)
    {
        g_training_pending = 0;
        combo_trials::update(false);  // a trial's settings never go into other modes
        perf::Scope game(perf::GAME);
        orig_UpdateBattle(GameState, DeltaTime);
        return;
    }
    g_last_battle_update = GetTickCount64();
    g_mode_battle_ran = true;
    // Stamped again on the way out: the work in here (the mod's and the game's) is no gap between battle updates
    struct Restamp { ~Restamp() { g_any_battle_update = g_last_battle_update = GetTickCount64(); } } restamp;
    if (!game_state.mode_battle_logged) { game_state.mode_battle_logged = true; COMBO_LOG("battle code running in this mode"); }

    {
        perf::Scope held(perf::HELD);
        const int delay = g_catching_up ? 0 : ModMenu::instance().delayAmount();
        if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }

    pollGamepad();  // before anything reads button states this frame
    {
        perf::Scope timed(perf::REPLAY);
        ReplayManager::instance().onBattleUpdate();
    }
    {
        perf::Scope held(perf::HELD);
        pause_manager.checkPause();
    }
    g_last_battle_update = GetTickCount64();  // the pause can hold this call for a long time

    // After the pause, which reads binds while it holds: a press made then counts too.
    // Held until the fight actually updates, so a press during a pause isn't lost.
    uint32_t pressed = 0;
    if (keybindings.takeAction(binds::TRAINING_RECORD)) pressed |= kFakeRecord;
    if (keybindings.takeAction(binds::TRAINING_PLAYBACK)) pressed |= kFakePlayback;
    if (keybindings.takeAction(binds::TRAINING_RESET)) pressed |= kFakeReset;
    // Freeze: the timeline's pause, which holds the fight and keeps the game drawing (F3's holds it all), in training
    const bool held_modes = game_state.checkModeTraining() || game_state.checkModeReplay();
    if (keybindings.takeAction(binds::BOX_PAUSE) && held_modes) ModMenu::instance().click(MenuClick{MenuClick::Type::TlPlay});
    if (keybindings.takeAction(binds::BOX_STEP) && held_modes) ModMenu::instance().click(MenuClick{MenuClick::Type::TlStep});
    if (keybindings.takeAction(binds::STEP_BACK) && game_state.checkModeReplay()) ReplayManager::instance().stepBack();
    if (keybindings.takeAction(binds::TICK_BACK) && game_state.checkModeReplay()) ReplayManager::instance().stepTick(-1);
    if (keybindings.takeAction(binds::TICK_FORWARD) && game_state.checkModeReplay()) ReplayManager::instance().stepTick(1);
    if (keybindings.takeAction(binds::FREE_CAMERA) && game_state.checkModeTraining()) recolor_test::toggleCamera();  // (with or without the color editor)
    if (keybindings.takeAction(binds::COLOR_EDITOR) && game_state.checkModeTraining()) recolor_test::toggle();
    if (keybindings.takeAction(binds::BOX_EDITOR) && game_state.checkModeTraining()) box_editor::toggle();
    if (keybindings.takeAction(binds::ONLINE_VERSION)) imgui_overlay::noteOnlineKey();
    {
        perf::Scope timed(perf::TRIALS);
        combo_trials::update(game_state.checkModeTraining());
    }
    {
        perf::Scope timed(perf::PAUSE_MENU);
        game_menu::update();
    }
    if (game_state.checkModeTraining() && orig_KeyJustPressed) g_training_pending |= pressed;
    else g_training_pending = 0;

    if (game_state.checkModeReplay())
    {
        perf::Scope timed(perf::REPLAY);
        ReplayManager::instance().update();
    }

    const int style = pauseStyle();
    const bool scan_hold = game_state.checkModeTraining() && recolor_test::holdsFight();  // the editor's scan: the fighter mustn't move
    if ((style == 0 && pause_manager.advancing()) ||
        (style == 1 && pause_manager.cinematicShouldAdvance()) || scan_hold)
    {
        perf::Scope timed(perf::COLORS);
        if (game_state.checkModeTraining()) { recolor_test::update(false); recolor_test::afterBattleUpdate(); box_editor::tick(); }  // paused here too
        return;
    }

    {
    perf::Scope timed(perf::MENU);
    ModMenu::instance().update(PressedKeys{
        keybindings.getButtonState(keybindings.TOGGLE_FRAMEBAR_BUTTON),
        keybindings.getButtonState(keybindings.TOGGLE_HITBOX_BUTTON),
        keybindings.getButtonState(keybindings.TOGGLE_MENU_BUTTON),
        keybindings.getButtonState(keybindings.MENU_UP_BUTTON),
        keybindings.getButtonState(keybindings.MENU_DOWN_BUTTON),
        keybindings.getButtonState(keybindings.MENU_RIGHT_BUTTON),
        keybindings.getButtonState(keybindings.MENU_LEFT_BUTTON)
    });
    }
    keybindings.resetButtons();

    game_state.checkRound();

    const bool round_started = game_state.roundJustStarted;
    if (game_state.roundJustStarted)
    {
        game_state.roundJustStarted = false;
        g_overlays_visible = true;
        // Round call is the sync point: an armed stream begins here, and
        // re-begins on every reset, so the dummy's inputs line up with the
        // round rather than with when the user opened the menu.
        recordings::streamOnRoundStart();
        combo_trials::onRoundStart();
        ReplayManager::instance().onRoundStart();
    }
    if (game_state.resetting)
    {
        the_bar.reset();
        tension_overlay.reset();
    }

    if (game_state.checkModeTraining())
    {
        perf::Scope timed(perf::DAMAGE);
        damage_panel::update(round_started);
    }
    if (game_state.checkModeTraining())
    {
        perf::Scope timed(perf::COLORS);
        recolor_test::update(round_started);
    }
    if (game_state.checkModeTraining() && !Timeline::instance().canAdvanceFrame())
    {
        perf::Scope timed(perf::COLORS);
        recolor_test::afterBattleUpdate();
        box_editor::tick();
    }  // the free camera, paused
    if (!Timeline::instance().canAdvanceFrame()) return;  // timeline pause: no fight update this frame
    if (g_training_pending)
    {
        g_training_press = g_training_pending;
        g_training_pending = 0;
        COMBO_LOG(std::string("training: ") + (g_training_press & kFakeRecord ? "record " : "") + (g_training_press & kFakePlayback ? "playback " : "")
                  + (g_training_press & kFakeReset ? "reset " : "") + "bind");
    }
    {
        perf::Scope game(perf::GAME);
        orig_UpdateBattle(GameState, DeltaTime);
    }
    if (game_state.checkModeTraining())
    {
        perf::Scope timed(perf::COLORS);
        recolor_test::afterBattleUpdate();
        box_editor::tick();
    }
    {
        perf::Scope timed(perf::REPLAY);
        ReplayManager::instance().onBattleFrameProcessed();
    }

    if (game_state.matchStarted)
    {
        game_state.matchStarted = false;
        tracker.setup();
    }

    if (!tracker.isUePaused() && game_state.roundActive)
    {
        perf::Scope timed(perf::BAR);
        the_bar.addFrame();
    }

}

void hook_UpdateBattleInput(asw_inputs* Analyzer, RECFLG_ENUM recFlag)
{
    handshake::onBattleInput(Analyzer, static_cast<uint32_t>(recFlag));
    if (game_state.checkModeTraining() && recolor_test::setupRunning()) recFlag = static_cast<RECFLG_ENUM>(0);  // The editor's first-time setup holds the pad too
    // Menu open: swallow pad/keyboard so the D-pad drives the menu, not the character.
    // In a replay only the side being taken over is the player's; every other side
    // is fed from the replay itself, and zeroing it froze both characters and
    // desynced the replay.
    if (ModMenu::instance().takesInput() && game_state.checkMode() &&
        (!game_state.checkModeReplay() || (!ReplayManager::instance().playbackEnabled()
            && ReplayManager::instance().isTakeoverInput(Analyzer))))
        recFlag = static_cast<RECFLG_ENUM>(0);

    // Combo Trials "Watch": the combo's own recording drives your character.
    if (combo_trials::watching())
    {
        if (auto* engine = asw_engine::get())
        {
            const int side = combo_trials::localSide();
            if (Analyzer == &engine->inputs[side * 3])
            {
                const auto* me = engine->players[side].entity;
                uint16_t flags = 0;
                if (me && combo_trials::watchNext(me->facing == direction::left, flags))
                {
                    orig_UpdateBattleInput(Analyzer, static_cast<RECFLG_ENUM>(flags));
                    return;
                }
            }
        }
    }

    // Live stream: feed our own per-frame flags to the P2 (dummy) analyzer.
    // asw_engine::inputs[0..2] belong to P1 and [3..5] to P2, so [3] is the
    // dummy. One word per engine frame keeps this frame-exact -- the reason
    // this exists rather than a virtual controller, which pushes on a wall
    // clock and drifts whenever the game hitches.
    if (recordings::streamActive() && game_state.checkModeTraining())
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

    orig_UpdateBattleInput(Analyzer, ReplayManager::instance().takeoverInput(recFlag, ModMenu::instance().takesInput()));
}

// Keys are only read during fights; drop the ones pressed outside one (character
// select, loading), or they would all fire on the first fight frame.
static void dropKeysOutsideFight()
{
    if (!g_mode_fight)
    {
        keybindings.checkBinds(false);
        // Outside every fight, the online version window takes its own key, and the
        // menu's Left / Right while it's open; the rest is dropped.
        if (!g_any_fight)
        {
            if (keybindings.takeAction(binds::ONLINE_VERSION))
                imgui_overlay::setOnlinePicker(!imgui_overlay::onlinePickerOpen());
            if (imgui_overlay::onlinePickerOpen())
            {
                if (keybindings.takeAction(binds::LEFT)) combo_trials::stepOnlineVersion(-1);
                if (keybindings.takeAction(binds::RIGHT)) combo_trials::stepOnlineVersion(1);
                if (const int step = imgui_overlay::takeOnlinePickStep()) combo_trials::stepOnlineVersion(step);
            }
        }
        else if (keybindings.takeAction(binds::ONLINE_VERSION))  // in an online fight it opens the recolor sharing panel, and the version stays put
            imgui_overlay::setOnlinePicker(!imgui_overlay::onlinePickerOpen());
        keybindings.resetButtons();
        // Only when the overlay draws out here (an online fight, the online version
        // window): never while the game is still starting up.
        if (g_any_fight || imgui_overlay::onlinePickerOpen())
            ModMenu::instance().refreshView();
    }
}

void hook_UpdateSystem(uintptr_t System)
{
    orig_UpdateSystem(System);
    {  // The overlay's Present hook comes 5 s after the game's first update, on its own thread.
       // It makes a throwaway DirectX device, so never while the game starts up, which OBS or a playing video
       // already makes slow.
        static ULONGLONG first = 0;
        static bool hooked = false;
        if (!hooked) {
            if (!first) first = GetTickCount64();
            else if (GetTickCount64() - first > 5000) { hooked = true; std::thread([] { imgui_overlay::install(); }).detach(); }
        }
    }
    ReplayManager::instance().setInputSystem(System);
    // Out of battle (the fight ended on the frame a bind fired), nothing clears the
    // press at the start of the next battle update, so clear it here, and drop one
    // still held from a pause.
    if (!g_battle_ran) g_training_press = g_training_pending = 0;
    g_no_battle_updates = g_battle_ran ? 0 : g_no_battle_updates + 1;
    g_no_mode_battle_updates = g_mode_battle_ran ? 0 : g_no_mode_battle_updates + 1;
    g_battle_ran = g_mode_battle_ran = false;
    {  // A slow frame isn't the end of a fight; a long gap always is, even with one system update in it (a blocking load)
      const ULONGLONG now = GetTickCount64(), any_gap = now - g_any_battle_update, mode_gap = now - g_last_battle_update;
      g_any_fight = any_gap < 250 || (g_no_battle_updates < 2 && any_gap < 1000);
      g_mode_fight = mode_gap < 250 || (g_no_mode_battle_updates < 2 && mode_gap < 1000);
    }
    const bool battle_running = g_mode_fight;
    combo_trials::latchVersion(battle_running);
    combo_trials::updateOnline(g_any_fight);
    recolor_share::update(g_any_fight);
    if (!battle_running) {
        combo_trials::idle();     // left training: the trial doesn't carry on
        damage_panel::release();  // the game's panel goes with the fight
        game_menu::release();
    }
    {  // Each time a fight counts as over, with how long there was no battle update (stalls show up here)
      static bool was_fight = false;
      if (was_fight && !g_any_fight)
        COMBO_LOG("fight over: " + std::to_string(GetTickCount64() - g_any_battle_update) + " ms and " + std::to_string(g_no_battle_updates) +
                  " system updates without a battle update");
      was_fight = g_any_fight;
    }
    if (!g_any_fight) recolor_test::battleGone();  // effect colors go back once there's no fight of any kind
    if (const int mode = game_state.currentMode(); mode >= 0 && mode != 8) { box_editor::outsideTraining(); recolor_test::soloCameraOff(); recolor_test::outsideTraining(); recordings::streamStop(); combo_trials::streamRestore(false); }  // (the menus too: never a Script & boxes edit while the next fight loads; 8 is training)
    // Saved recolors on the menus' models (character select), only with no fight at all.
    if (!g_any_fight) recolor_test::menuUpdate(false);
    else if (!game_state.checkModeTraining()) recolor_test::menuUpdate(true);  // (any other fight: the saved recolors show by their bakes)
    else recolor_test::menuRelease();
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

using funcWorldTick_t = void (*)(void*, int, float);
funcWorldTick_t orig_WorldTick = nullptr;
uint64_t* g_frame_counter = nullptr;
PLH::x64Detour* g_world_tick_detour = nullptr;
std::filesystem::path g_experiments_file;
bool g_experiments_on = false;

void hook_WorldTick(void* world, int type, float dt)
{
    orig_WorldTick(world, type, dt);
    static bool was_down = false;
    const bool down = g_experiments_on && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState('K') & 0x8000);
    if (down && !was_down)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid == GetCurrentProcessId())
        {
            int frames = 60;
            int flags = 0;
            try
            {
                std::ifstream f(g_experiments_file);
                const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                if (text.find("control") != std::string::npos) flags |= 1;
                if (text.find("samecounter") != std::string::npos) flags |= 2;
                const size_t at = text.find_first_of("0123456789");
                if (at != std::string::npos) frames = std::atoi(text.c_str() + at);
            }
            catch (...) {}
            ReplayManager::instance().trialKey(frames, flags);
        }
    }
    was_down = down;
    if (!g_frame_counter) return;
    ReplayManager& replay = ReplayManager::instance();
    if (replay.takeSeekStarted())
    {
        the_bar.reset();
        tension_overlay.reset();
    }
    if (const int jump = replay.seekAfterTick(); jump > 0)
    {
        g_catching_up = true;
        for (int i = 0; i < jump && replay.seekGoing(); i++)
        {
            ++*g_frame_counter;
            orig_WorldTick(world, type, 1.0f / 60.0f);
        }
        g_catching_up = false;
        replay.seekBatchDone();
        return;
    }
    const int n = replay.trialAfterTick();
    if (n <= 0) return;
    const uint64_t same = replay.trialCounterStart();
    const auto t0 = std::chrono::steady_clock::now();
    g_catching_up = true;
    for (int i = 0; i < n; i++)
    {
        if (same) *g_frame_counter = same + static_cast<uint64_t>(i);
        else ++*g_frame_counter;
        orig_WorldTick(world, type, 1.0f / 60.0f);
    }
    g_catching_up = false;
    replay.trialCatchUpDone(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

using funcRollbackCtor_t = void* (*)(void*, int);
funcRollbackCtor_t orig_RollbackCtor = nullptr;
PLH::x64Detour* g_rollback_ctor_detour = nullptr;

void* hook_RollbackCtor(void* self, int slots)
{
    const int mode = combo_trials::gameModeNow();
    const int want = mode == 23 && slots == 20 && ModMenu::instance().replayTimeline() ? 180 : slots;
    COMBO_LOG("timeline: the game made its save slots, " + std::to_string(slots)
              + (want != slots ? " raised to " + std::to_string(want) : std::string(" kept")) + " (mode " + std::to_string(mode)
              + ", the fight hooks say " + std::to_string(game_state.currentMode()) + ")");
    return orig_RollbackCtor(self, want);
}

bool g_timeline_hooks_tried = false;

void installTimelineHooks(bool at_startup)
{
    if (g_timeline_hooks_tried) return;
    g_timeline_hooks_tried = true;
    try
    {
        const uintptr_t counter = sigscan::get().scan(
            "\x48\x8B\x05\x00\x00\x00\x00\x48\xFF\xC0\x48\x89\x05\x00\x00\x00\x00\x48\x83\xF8\x06",
            "xxx????xxxxxx????xxxx");
        g_frame_counter = static_cast<uint64_t*>(get_rip_relative(counter + 3));
        if (get_rip_relative(counter + 13) != static_cast<void*>(g_frame_counter))
        {
            g_frame_counter = nullptr;
            COMBO_LOG("timeline: the frame counter doesn't look right, jumps are off");
            return;
        }
        const uint64_t tick = sigscan::get().scan(
            "\x48\x8B\xC4\x55\x53\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x8D\xA8\x38\xFE\xFF\xFF\x48\x81\xEC\x88\x02\x00\x00\x0F\x29\x70\xA8\x48\x8B\xF9",
            "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
        g_world_tick_detour = new PLH::x64Detour(tick, reinterpret_cast<uint64_t>(&hook_WorldTick), reinterpret_cast<uint64_t*>(&orig_WorldTick));
        if (!g_world_tick_detour->hook())
        {
            orig_WorldTick = nullptr;
            COMBO_LOG("timeline: the world tick hook didn't go in, jumps are off");
            return;
        }
        if (at_startup) ReplayManager::instance().enableSeek(g_frame_counter);
        else ReplayManager::instance().setFrameCounter(g_frame_counter);
        if (g_experiments_on)
        {
            game_state.logEvents = true;
            ReplayManager::instance().enableSeekLogs();
        }
        try
        {
            const uint64_t ctor = sigscan::get().scan(
                "\x48\x89\x5C\x24\x08\x48\x89\x6C\x24\x10\x48\x89\x74\x24\x18\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x83\xEC\x20\x45\x33\xF6\x48\x63\xEA\x4C\x8B\xF9\x4C\x8D\x61\x48",
                "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
            g_rollback_ctor_detour = new PLH::x64Detour(ctor, reinterpret_cast<uint64_t>(&hook_RollbackCtor), reinterpret_cast<uint64_t*>(&orig_RollbackCtor));
            if (!g_rollback_ctor_detour->hook())
            {
                orig_RollbackCtor = nullptr;
                COMBO_LOG("timeline: more save slots: the hook didn't go in, the Timeline reaches back 16 s");
            }
        }
        catch (...)
        {
            COMBO_LOG("timeline: more save slots: the game's code wasn't found, the Timeline reaches back 16 s");
        }
        COMBO_LOG(std::string("timeline: jumps ready for replays") + (g_experiments_on ? ", experiments on (Ctrl+Alt+K runs the catch-up test, extra log lines)" : ""));
    }
    catch (...)
    {
        COMBO_LOG("timeline: the game's code wasn't found, jumps are off");
    }
}

void installExperiments(const std::filesystem::path& dir)
{
    g_experiments_file = dir / "StriveLabs_experiments.txt";
    std::error_code ec;
    g_experiments_on = std::filesystem::exists(g_experiments_file, ec);
    perf::on = g_experiments_on;
    if (ModMenu::instance().replayTimeline()) installTimelineHooks(true);
}

void hook_UpdateOnPreTick(AREDGameState_Battle* pThis, float DeltaSeconds)
{
    {  // After a blocking load the last system update's flags are old, so a long gap is no fight here
      const ULONGLONG now = GetTickCount64();
      if (now - g_last_battle_update >= 1000) g_mode_fight = false;
      if (now - g_any_battle_update >= 1000) g_any_fight = false;
    }
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
        bbscript::BBSInitializeFunctions();  // frame bar countdown; turns itself off if not found
        keybindings.loadButtons();

        // Initialize logging
        auto working_dir = UE4SSProgram::get_program().get_working_directory();
        ComboLog::instance().init(working_dir);
        COMBO_LOG("StriveLabs loaded");
        if (!training_hook) COMBO_LOG("training binds: button config lookup not found");
        COMBO_LOG(bbscript::scriptReaderAvailable() ? "countdown: script reader ready" : "countdown: script reader not found, recovery countdown off");
        combo_trials::init(working_dir);
        handshake::install();
        combo_online::init();
        crash_log::install(working_dir);  // first hard fault -> combo_log.txt + StriveLabs_crash.dmp, for user reports
        select_memory::init(working_dir);
        recolor_test::init(working_dir);
        box_editor::init(working_dir);
        recolor_share::init(working_dir);
        installExperiments(working_dir);
        pak_mount::probe(working_dir);  // (finds and checks the engine's pak mount and logs it; mounts nothing)
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
