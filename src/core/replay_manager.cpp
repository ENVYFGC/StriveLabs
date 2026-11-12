#include "replay_manager.h"

#include "arcsys.h"
#include "combo_log.h"
#include "object_scan.h"
#include "keybinds.h"
#include "bind_watcher.h"
#include "keybindings.h"
#include "recordings.h"
#include "sigscan.h"
#include "game_state.h"
#include "timeline.h"
#include "menu.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <atomic>

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Psapi.h>
#include <Xinput.h>
#pragma comment(lib, "Psapi.lib")

using namespace RC;
using namespace RC::Unreal;

namespace {
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

class GAME_KeyControler
{
public:
    FIELD(0x78, uint32_t, m_PureKey);
};

class AASystemRED
{
public:
    ARRAY_FIELD(0x1F8, GAME_KeyControler*[4], m_BattleKey);
};

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

}

struct ReplayManager::Data
{
    GameState* gameState = nullptr;
    Keybinds* keybindings = nullptr;
    Timeline* timeline = nullptr;
    static constexpr int kRewindFramesPerSecond = 60;
    std::atomic<int> rewindFramesRemaining{0};
    int rewindFramesTotal = 0; // duration captured when the countdown starts
    bool isWaitingForFrame = false;
    int lastGameFrame = -1;

    void resetRewindDelay()
    {
        if (rewindFramesRemaining > 0) timeline->setPaused(false);
        rewindFramesRemaining = 0;
        rewindFramesTotal = 0;
        isWaitingForFrame = false;
    }

    void beginRewindDelay()
    {
        const bool wasPaused = timeline->isPaused();
        resetRewindDelay();
        rewindFramesTotal = static_cast<int>(ModMenu::instance().rewindDelaySeconds() * kRewindFramesPerSecond + 0.5);
        rewindFramesRemaining = rewindFramesTotal;
        if (rewindFramesRemaining <= 0 && !wasPaused) return;

        // setPaused(false) clears queued manual steps, preventing extra frames after
        // rewind. Pausing again below happens before any game update can run.
        timeline->setPaused(false);
        // Freezing immediately after rollback leaves visual artifacts. Let the
        // restored fight process one frame before staying paused, even with no delay.
        lastGameFrame = asw_engine::get()->GameFrame;
        isWaitingForFrame = true;
        timeline->setPaused(true);
    }

    void onBattleFrameProcessed()
    {
        trialBattleCalls++;
        if (!isWaitingForFrame) return;
        const auto* engine = asw_engine::get();
        if (!engine) return;

        // Native replay pause can call UpdateBattle without advancing the fight.
        const int gameFrame = engine->GameFrame;
        if (gameFrame == lastGameFrame) return;
        lastGameFrame = gameFrame;
        isWaitingForFrame = false;
    }

    using funcSaveBackup_t = void (*)(asw_rollback*);
    using funcRollback_t = void (*)(asw_rollback*);
    funcSaveBackup_t orig_SaveBackup{};
    funcRollback_t orig_Rollback{};
    // The game's rollback is also its effect manager. Rollback() moves every effect
    // spawned at or after the restored frame into a "waiting to be reused" list; the
    // netplay loop then re-simulates and finally destroys whatever was not reused,
    // with these two calls (one per effect kind). Our F9 load does not re-simulate,
    // so without them the leftovers live on and the restored timeline spawns new
    // ones on top: the HC reticle / FD effect stacking.
    using funcEffectCleanup_t = void (*)(asw_rollback*);
    funcEffectCleanup_t orig_EffectCleanupA = nullptr;
    funcEffectCleanup_t orig_EffectCleanupB = nullptr;
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
    AASystemRED* system_red = nullptr;
    using funcGameKeyToRecFlag_t = RECFLG_ENUM (*)(PAD_ID, GAMEKEY_FLAG, bool);
    funcGameKeyToRecFlag_t orig_GameKeyToRecFlag{};
    using funcGetReplayManager_t = uintptr_t (*)();
    funcGetReplayManager_t orig_GetReplayManager{};
    uintptr_t orig_BattleKeyCheck{};

    // ---- Replay capture: up to 40 seconds of one side's inputs ------------------
    // Start remembers the side's replay read position; stop collects everything from
    // there to the current position. Frames are counted as the replay plays, and the
    // menu stops the capture itself once 2400 frames (one training slot) are in.
    bool g_cap_active = false;
    int g_cap_peer = 1;           // replay peer 0 = P1 seat, 1 = P2 seat
    ReplayHead g_cap_start{};
    uint32_t g_cap_frames = 0;

    asw_inputs* analyzer{};
    ReplayHead replayHead[6]{};
    ReplayManager_Internal* manager{};

    // ---- Replay Assist ------------------------------------------------------------
    // While a replay plays by itself, keep a ring of saves in the game's backup slots
    // 1-16 (offline nothing else uses them, F6 uses 18-19). F6 also remembers the
    // newest ring save that is at least the assist length older. F9 then loads that
    // one, lets the replay play both sides up to the F6 moment (standby), and hands
    // the chosen side over to the player right there.
    static constexpr int kRingSlots = 16;
    static constexpr int kRingFirstSlot = 1;  // ring entry i lives in game slot 1 + i
    struct RingSave
    {
        bool valid = false;
        uint32_t position = 0;  // peer 0 replay frame position
        int shift = 0;
        ReplayHead heads[6]{};
    };
    RingSave ring[kRingSlots]{};
    int ringNext = 0;
    uint32_t ringLast = 0;
    bool haveRingLast = false;
    int assistSlot = -1;          // ring slot picked by F6, -1 = plain rewind
    uint32_t assistTarget = 0;    // replay position at F6
    uint32_t assistFrom = 0;      // replay position of the picked save
    bool assistStandby = false;
    asw_inputs* assistSide = nullptr;
    ULONGLONG assistActiveAt = 0;
    // Set once takeover changes the fight: from then on it no longer matches the
    // replay's read position, so saves taken in that state would be wrong.
    bool diverged = false;
    bool pointDiverged = false;

    int assistFrames() const
    {
        return static_cast<int>(ModMenu::instance().replayAssistSeconds() * 60.0 + 0.5);
    }

    uint32_t replayPosition() const
    {
        return manager ? manager->m_ReplayBuffer[0].m_ReadHead.m_SolidPosition : 0;
    }

    void clearRingOnly()
    {
        for (auto& r : ring) r.valid = false;
        ringNext = 0;
        ringLast = 0;
        haveRingLast = false;
        assistSlot = -1;
        assistStandby = false;
    }

    void clearRing()
    {
        clearRingOnly();
        clearSeekSaves();
    }

    void clearAtStageChange()
    {
        if (seekEnabled && stageCanReset) clearRingOnly();
        else clearRing();
    }

    // The ring lives in the game's backup slots 0-15 and F6 uses 18-19, so all 20 must exist.
    static bool backupSlotsOk(asw_rollback* rollback)
    {
        return rollback && rollback->MaxBackupFrame >= 20 && rollback->BackupData.Num() >= 20;
    }

    void ringSave()
    {
        const int want = assistFrames();
        auto rollback = asw_rollback::get();
        const bool standIn = seekEnabled && seekSlots(rollback) == 0;
        if ((want <= 0 && !standIn) || !backupSlotsOk(rollback) || !manager) return;
        const uint32_t pos = replayPosition();
        // Moved back with the replay's own controls: the fight follows the recording
        // again from there, and saves ahead of it are no use.
        if (haveRingLast && pos < ringLast)
        {
            clearRing();
            diverged = false;
        }
        // Only while the round is being fought: no saves across a round change.
        if (analyzer || diverged || assistStandby || seekCatching || !gameState->roundActive) return;
        // Spacing so the ring always reaches back far enough, with one slot locked by F6.
        const uint32_t every = want > 0 ? (std::max)(6, (want + kRingSlots - 3) / (kRingSlots - 2)) : 60;
        if (haveRingLast && pos < ringLast + every) return;
        if (ringNext == assistSlot) ringNext = (ringNext + 1) % kRingSlots;
        const int slot = ringNext;
        ringNext = (ringNext + 1) % kRingSlots;

        int savedIndex = rollback->CurrentBackupDataIndex;
        rollback->CurrentBackupDataIndex = kRingFirstSlot + slot - 1;  // the save goes to the next slot
        orig_SaveBackup(rollback);
        rollback->CurrentBackupDataIndex = savedIndex;

        ring[slot].valid = true;
        ring[slot].position = pos;
        ring[slot].shift = stageAt;
        for (int i = 0; i < 6; i++) ring[slot].heads[i] = manager->m_ReplayBuffer[i].m_ReadHead;
        ringLast = pos;
        haveRingLast = true;
    }

    void pickAssistSave()
    {
        assistSlot = -1;
        assistStandby = false;
        pointDiverged = diverged;
        const int want = assistFrames();
        if (want <= 0) return;
        if (diverged)
        {
            COMBO_LOG("assist: F6 while the fight differs from the replay, plain rewind point");
            return;
        }
        assistTarget = replayPosition();
        for (int i = 0; i < kRingSlots; i++)
            if (ring[i].valid && ring[i].position + want <= assistTarget
                && (assistSlot < 0 || ring[i].position > ring[assistSlot].position))
                assistSlot = i;
        COMBO_LOG("assist: F6 at " + std::to_string(assistTarget) + ", save " + std::to_string(assistSlot)
                  + (assistSlot >= 0 ? " at " + std::to_string(ring[assistSlot].position) : std::string()));
    }

    void updateAssist()
    {
        if (!assistStandby) return;
        const uint32_t pos = replayPosition();
        // Moved with the replay's own controls during standby: hand nothing over.
        if (pos < assistFrom || pos > assistTarget + 12)
        {
            assistStandby = false;
            return;
        }
        if (pos < assistTarget) return;
        assistStandby = false;
        if (!assistSide) return;
        analyzer = assistSide;
        diverged = true;
        assistActiveAt = GetTickCount64();
        COMBO_LOG("assist: active at " + std::to_string(pos));
    }

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
        if (!gameState->checkModeReplay()) return;

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
    std::set<std::wstring> g_fx_classes;

    // Second kind of linked actor: fight object +0x43A0 <-> actor +0x29C98, with an
    // "unlinked" flag at actor +0x2A094. The game's second rollback cleanup retires
    // them per class: clear the link, set the flag, call that class's release. A
    // rewind across a round change left one linked to a fight slot that no longer
    // owned it, and the actor's update then crashed reading through that link.
    using funcActorClass_t = Unreal::UClass* (*)();
    using funcActorRelease_t = void (*)(void*);
    funcActorClass_t orig_ActorClassA = nullptr, orig_ActorClassB = nullptr;
    funcActorRelease_t orig_ActorReleaseA = nullptr, orig_ActorReleaseB = nullptr;
    std::set<std::wstring> g_actor_classes;

    // Active objects own their links. whole_pool adds the inactive slots too: F9
    // restores their links as well, and the next reset releases every slot's link.
    void collectFightObjects(std::vector<uint8_t*>& out, bool whole_pool = false)
    {
        auto* eng = asw_engine::get();
        if (!eng) return;
        const int n = (std::min)(whole_pool ? eng->pool_count : eng->entity_count, 107);
        for (int i = 0; i < n; i++)
            if (auto* e = reinterpret_cast<uint8_t*>(whole_pool ? eng->pool[i] : eng->entities[i])) out.push_back(e);
    }

    // Each battle frame: remember the class of every linked effect actor, so F9 can list all of them.
    void trackEffectClasses()
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

    // ---- Effects and models after a load -------------------------------------------
    // The game's own rollback is built to go back a few frames. After loading it keeps
    // every fight object's effect as it was saved, and gives each model that is out
    // right now back to whatever object sits in the same slot. F9 goes back seconds,
    // and by then that effect may have been reused for something else and that slot
    // may hold a different object: effects in the wrong place or at the wrong size,
    // models on the wrong object.
    // Each fight object also saves which effect and model it should have: the name,
    // the move it came from and the frame it started. The rule below compares that with
    // what the object had just before the load. If it is the same effect or model
    // still running, the link is right and is kept. If not, the wrong one is let go and
    // the right one is made again by name with the game's own link functions, starting
    // partway through the way the game does it. This does not depend on the character.
    static constexpr int kFxLink = 0x43C8, kModelLink = 0x43A0;
    static constexpr int kFxName = 0x515C, kFxSource = 0x517C, kFxWithArgs = 0x5180, kFxArgs = 0x5184,
                         kFxSaved = 0x51F0, kFxStarted = 0x51F4;          // started frame + move name: 36 bytes
    static constexpr int kModelName = 0x521C, kModelSaved = 0x523C, kModelShared = 0x523D,
                         kModelCommonFx = 0x523E, kModelTeam = 0x523F, kModelBlueprint = 0x5240,
                         kModelStarted = 0x5244;                          // started frame + move name: 36 bytes
    static constexpr int kSpawnArgs = 0x3DA8, kSpawnArgsSize = 0x6C, kFxStartTime = 0x54C0, kFxPlayed = 0x54C4;

    using funcLinkFx_t = void (*)(void* obj, const char* name, int source);
    using funcLinkByName_t = void (*)(void* obj, const char* name);
    funcLinkFx_t orig_LinkFx = nullptr;
    funcLinkByName_t orig_LinkFxWithArgs = nullptr;
    funcLinkByName_t orig_LinkModel = nullptr;
    funcLinkByName_t orig_LinkCommonFxModel = nullptr;
    funcLinkByName_t orig_LinkBlueprintModel = nullptr;

    struct BeforeLoad
    {
        void* fx = nullptr;
        void* model = nullptr;
        uint8_t fxName[32]{}, fxStarted[36]{};
        uint8_t modelName[32]{}, modelStarted[36]{};
    };
    std::vector<std::pair<uint8_t*, BeforeLoad>> beforeLoad;

    void rememberLinksBeforeLoad()
    {
        beforeLoad.clear();
        std::vector<uint8_t*> objs;
        collectFightObjects(objs);
        for (auto* e : objs)
        {
            BeforeLoad b;
            b.fx = *reinterpret_cast<void**>(e + kFxLink);
            b.model = *reinterpret_cast<void**>(e + kModelLink);
            memcpy(b.fxName, e + kFxName, 32);
            memcpy(b.fxStarted, e + kFxStarted, 36);
            memcpy(b.modelName, e + kModelName, 32);
            memcpy(b.modelStarted, e + kModelStarted, 36);
            beforeLoad.emplace_back(e, b);
        }
    }

    void rebuildLinksAfterLoad()
    {
        if (!orig_LinkFx || !orig_LinkFxWithArgs || !orig_LinkModel || !orig_LinkCommonFxModel || !orig_LinkBlueprintModel)
            return;
        int fxRebuilt = 0, modelsRebuilt = 0, modelsLetGo = 0;
        std::vector<uint8_t*> objs;
        collectFightObjects(objs);
        for (auto* e : objs)
        {
            if (e[0x20]) continue;  // players keep their own model and have no linked effect
            const BeforeLoad* b = nullptr;
            for (const auto& [obj, state] : beforeLoad)
                if (obj == e) { b = &state; break; }

            // Effect
            const int played = *reinterpret_cast<int*>(e + kFxPlayed);
            if (e[kFxSaved] && played > 0)
            {
                auto*& fx = *reinterpret_cast<uint8_t**>(e + kFxLink);
                const bool same = b && fx && b->fx == fx && !(fx[0x432] & 0x80)
                    && *reinterpret_cast<void**>(fx + 0x668) == e
                    && !memcmp(b->fxName, e + kFxName, 32) && !memcmp(b->fxStarted, e + kFxStarted, 36);
                if (!same)
                {
                    char name[32];
                    uint8_t started[36];
                    memcpy(name, e + kFxName, 32);
                    memcpy(started, e + kFxStarted, 36);
                    fx = nullptr;  // not ours any more: linking anew must not delete it
                    *reinterpret_cast<float*>(e + kFxStartTime) = static_cast<float>(played) / 60.0f;
                    if (e[kFxWithArgs])
                    {
                        memcpy(e + kSpawnArgs, e + kFxArgs, kSpawnArgsSize);
                        orig_LinkFxWithArgs(e, name);
                    }
                    else
                    {
                        orig_LinkFx(e, name, *reinterpret_cast<int*>(e + kFxSource));
                    }
                    *reinterpret_cast<float*>(e + kFxStartTime) = 0.0f;
                    memcpy(e + kFxStarted, started, 36);  // keep the original start, not the load frame
                    fxRebuilt++;
                }
            }

            // Model. Shared models are put back by the game itself; team models only
            // exist in 3v3 and have no link function here, so both are left alone.
            if (e[kModelShared] || e[kModelTeam]) continue;
            auto*& model = *reinterpret_cast<uint8_t**>(e + kModelLink);
            const bool want = e[kModelSaved] != 0;
            // Only a model that was out just before the load is known to be alive.
            bool wasOut = false;
            for (const auto& [obj, state] : beforeLoad)
                if (state.model == model) { wasOut = true; break; }
            const bool linkedBack = model && wasOut && *reinterpret_cast<void**>(model + 0x29C98) == e;
            const bool same = b && linkedBack && b->model == model
                && !memcmp(b->modelName, e + kModelName, 32) && !memcmp(b->modelStarted, e + kModelStarted, 36);
            if (model && (!want || !same))
            {
                using vfn_t = void (*)(void*);
                auto unlink = *reinterpret_cast<vfn_t*>(*reinterpret_cast<uintptr_t*>(e) + 0xB8);  // the object's own unlink
                // Only a model that points back at this object is ours to unlink;
                // otherwise just drop the link and leave the model to its owner.
                if (linkedBack && is_ptr_in_game_module(reinterpret_cast<void*>(unlink))) unlink(e);
                else model = nullptr;
                modelsLetGo++;
            }
            if (want && !same)
            {
                char name[32];
                uint8_t started[36];
                memcpy(name, e + kModelName, 32);
                memcpy(started, e + kModelStarted, 36);
                if (e[kModelBlueprint]) orig_LinkBlueprintModel(e, name);
                else if (e[kModelCommonFx]) orig_LinkCommonFxModel(e, name);
                else orig_LinkModel(e, name);
                memcpy(e + kModelStarted, started, 36);
                modelsRebuilt++;
            }
        }
        beforeLoad.clear();
        if (fxRebuilt || modelsRebuilt || modelsLetGo)
            COMBO_LOG("F9 links: effects rebuilt " + std::to_string(fxRebuilt) + ", models rebuilt "
                      + std::to_string(modelsRebuilt) + ", models let go " + std::to_string(modelsLetGo));
    }

    void repairEffectsAfterLoad()
    {
        if (!orig_FxRelease1 || !orig_FxRelease2 || g_fx_classes.empty()) return;
        std::vector<uint8_t*> objs;
        collectFightObjects(objs);
        std::set<void*> linked;
        for (auto* e : objs)
            if (auto* a = *reinterpret_cast<void**>(e + 0x43C8)) linked.insert(a);

        std::vector<Unreal::UObject*> actors;
        {
            std::vector<const wchar_t*> names;
            for (const auto& name : g_fx_classes) names.push_back(name.c_str());
            object_scan::allOf(names, actors);
        }

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
        std::string unreleased;
        if (orig_ActorClassA && orig_ActorClassB && orig_ActorReleaseA && orig_ActorReleaseB && !g_actor_classes.empty())
        {
            std::set<void*> linked_b;
            for (auto* e : objs)
                if (auto* a = *reinterpret_cast<void**>(e + 0x43A0)) linked_b.insert(a);
            std::vector<Unreal::UObject*> bs;
            {
                std::vector<const wchar_t*> names;
                for (const auto& name : g_actor_classes) names.push_back(name.c_str());
                object_scan::allOf(names, bs);
            }
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
                else if (unreleased.size() < 240)
                {
                    if (auto* cls = obj->GetClassPrivate())
                        for (const auto ch : cls->GetName()) unreleased += ch < 128 ? static_cast<char>(ch) : '?';
                    unreleased += ' ';
                }
                released_b++;
            }
        }
        COMBO_LOG("F9 effects: released " + std::to_string(released) + ", cleared " + std::to_string(cleared)
                  + ", actors released " + std::to_string(released_b)
                  + ", tracked classes " + std::to_string(g_fx_classes.size()) + "+" + std::to_string(g_actor_classes.size())
                  + (unreleased.empty() ? std::string() : ", unlinked with no release for: " + unreleased));
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

    void syncDangerBanner()
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

    bool seekEnabled = false;
    bool seekVerbose = false;
    uint64_t* frameCounter = nullptr;
    bool seekDue = false, seekCatching = false, seekStarted = false;
    bool haveSeekTop = false;
    static constexpr int kSeekFirstSlot = 20, kSeekMaxSaves = 400;
    static constexpr uint32_t kSeekEvery = 60;
    struct SeekSave
    {
        bool valid = false;
        bool calm = false;
        uint32_t position = 0;
        int gameFrame = 0;
        int shift = 0;
        ReplayHead heads[6]{};
    };
    bool stageCanReset = false;
    static constexpr uint32_t kSeekCalmEvery = 45, kSeekRunUp = 600, kSeekQuickRunUp = 120;
    uint32_t seekOrigin = 0;
    bool haveSeekOrigin = false, seekQuick = false, seekRough = false;

    int seekBusy() const
    {
        auto* eng = asw_engine::get();
        if (!eng) return 1;
        int busy = 0;
        const int n = (std::min)(static_cast<int>(eng->entity_count), 107);
        for (int i = 0; i < n; i++)
        {
            auto* e = reinterpret_cast<uint8_t*>(eng->entities[i]);
            if (!e || e[0x20]) continue;
            if (*reinterpret_cast<void**>(e + kFxLink) != nullptr || (e[kFxSaved] && *reinterpret_cast<int*>(e + kFxPlayed) > 0)) busy++;
        }
        return busy;
    }

    using StagePath = std::vector<std::vector<asw_stage_call>>;
    StagePath stageJournal;
    int stageAt = 0;

    static bool stageStepKnown(const std::vector<asw_stage_call>& step)
    {
        for (const asw_stage_call& call : step)
            if (call.event == kStageBgChange) return true;
        return false;
    }

    bool stageCovers(const StagePath& path, int from, int to) const
    {
        if (to > static_cast<int>(path.size())) return false;
        for (int k = from; k < to; k++)
            if (!stageStepKnown(path[static_cast<size_t>(k)])) return false;
        return true;
    }

    bool seekUsable(int breaks, int breaksNow) const
    {
        if (breaks == breaksNow) return true;
        if (!stageCanReset) return false;
        return breaks > breaksNow ? stageCovers(stageJournal, breaksNow, breaks) : stageCovers(stageJournal, 0, breaks);
    }

    bool stageSettled() const
    {
        return stageAt == 0 || (stageAt <= static_cast<int>(stageJournal.size()) && stageStepKnown(stageJournal[static_cast<size_t>(stageAt - 1)]));
    }

    void stagePut(int breaks, const StagePath* own, int areaBefore)
    {
        const int area = asw_stage_area();
        const StagePath& path = own ? *own : stageJournal;
        const bool settled = stageSettled();
        if (breaks != stageAt || area != areaBefore || !settled)
        {
            const bool forward = !own && breaks > stageAt && settled;
            const int from = forward ? stageAt : 0;
            const std::string what = "load: stage went from " + std::to_string(stageAt) + " wall break(s), area " + std::to_string(areaBefore) + (settled ? "" : " (still changing)")
                                     + ", to " + std::to_string(breaks) + ", area " + std::to_string(area) + ": ";
            if (stageCanReset && stageCovers(path, from, breaks))
            {
                const int reset = forward ? -1 : asw_stage_round_reset();
                int sent = 0;
                for (int k = from; k < breaks; k++)
                    for (const asw_stage_call& call : path[static_cast<size_t>(k)])
                        if (call.event == kStageBgChange || call.event == kStageBoxMoveEnd) sent += asw_stage_fire(call) > 0 ? 1 : 0;
                COMBO_LOG(what + (reset >= 0 ? "round reset on " + std::to_string(reset) + " listener(s), " : "") + std::to_string(sent) + " change event(s) sent");
            }
            else
                COMBO_LOG(what + "can't be put back, the stage is left as it is");
            if (area != areaBefore)
            {
                asw_stage_record_refresh();
                stageCanReset = asw_stage_can_reset();
            }
        }
        stageAt = breaks;
        lastStage = area;
    }

    bool seekOldest(uint32_t& oldest) const
    {
        bool any = false;
        const int shiftNow = stageAt;
        for (const auto& r : ring)
            if (r.valid && seekUsable(r.shift, shiftNow) && (!any || r.position < oldest))
            {
                oldest = r.position;
                any = true;
            }
        const int slots = (std::min)(seekSlots(asw_rollback::get()), static_cast<int>(seekSaves.size()));
        for (int i = 0; i < slots; i++)
            if (seekSaves[static_cast<size_t>(i)].valid && seekUsable(seekSaves[static_cast<size_t>(i)].shift, shiftNow)
                && (!any || seekSaves[static_cast<size_t>(i)].position < oldest))
            {
                oldest = seekSaves[static_cast<size_t>(i)].position;
                any = true;
            }
        return any;
    }

    void seekPoints(int& origin, std::vector<int>& calm) const
    {
        calm.clear();
        uint32_t oldest = 0;
        const bool any = seekOldest(oldest);
        origin = static_cast<int>(haveSeekOrigin ? seekOrigin : any ? oldest : 0);
        const int slots = (std::min)(seekSlots(asw_rollback::get()), static_cast<int>(seekSaves.size()));
        const int shiftNow = stageAt;
        for (int i = 0; i < slots; i++)
            if (seekSaves[static_cast<size_t>(i)].valid && seekSaves[static_cast<size_t>(i)].calm && seekUsable(seekSaves[static_cast<size_t>(i)].shift, shiftNow))
                calm.push_back(static_cast<int>(seekSaves[static_cast<size_t>(i)].position));
    }
    std::vector<SeekSave> seekSaves;
    int seekNext = 0;
    bool haveSeekWatch = false, seekOwnJump = false;
    bool haveWorld = false, worldAway = false, worldChanging = false;
    int worldWall[2]{};
    int worldX[2]{};
    uint32_t seekWatch = 0;

    static int seekSlots(asw_rollback* rollback)
    {
        if (!rollback) return 0;
        const int have = (std::min)(static_cast<int>(rollback->MaxBackupFrame), static_cast<int>(rollback->BackupData.Num()));
        return std::clamp(have - kSeekFirstSlot, 0, kSeekMaxSaves);
    }

    void clearSeekSaves()
    {
        for (auto& s : seekSaves) s.valid = false;
        seekNext = 0;
        haveSeekTop = false;
        haveSeekWatch = false;
        haveSeekOrigin = false;
        seekRough = false;
        if (!gameState || !gameState->roundActive) worldAway = false;
    }

    void seekSaveTick()
    {
        if (!seekEnabled || !manager) return;
        const uint32_t pos = replayPosition();
        if (auto* eng = asw_engine::get())
        {
            const bool quiet = seekOwnJump || seekCatching || trial != Trial::Off || !haveWorld || !seekVerbose;
            for (int i = 0; i < 2; i++)
            {
                auto* e = eng->players[i].entity;
                if (!e) continue;
                const int wall = *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(e) + 0xD860);
                const int x = e->pos_x;
                if (!quiet && ((wall < worldWall[i] - 500) || (wall >= 10000) != (worldWall[i] >= 10000)))
                    COMBO_LOG("watch: P" + std::to_string(i + 1) + " wall damage " + std::to_string(worldWall[i]) + " -> " + std::to_string(wall) + " at " + std::to_string(pos));
                if (!quiet && std::abs(static_cast<long long>(x) - worldX[i]) > 400000)
                    COMBO_LOG("watch: P" + std::to_string(i + 1) + " moved from " + std::to_string(worldX[i]) + " to " + std::to_string(x) + " in one frame at " + std::to_string(pos));
                worldWall[i] = wall;
                worldX[i] = x;
            }
            haveWorld = true;
            if (const bool changing = asw_stage_changing(); changing != worldChanging)
            {
                if (!quiet) COMBO_LOG(std::string("watch: the engine's flag at 0x156 went ") + (changing ? "on" : "off") + " at " + std::to_string(pos));
                worldChanging = changing;
            }
            bool outside = false, settled = true;
            int wallLeft = 0, wallRight = 0;
            const bool haveWalls = asw_stage_walls(wallLeft, wallRight);
            for (int i = 0; i < 2; i++)
            {
                const long long x = worldX[i];
                const long long left = haveWalls ? wallLeft * 1000LL : -1600000LL;
                const long long right = haveWalls ? wallRight * 1000LL : 1600000LL;
                if (x < left - 500000 || x > right + 500000) outside = true;
                if (x < left - 200000 || x > right + 200000 || worldWall[i] >= 10000) settled = false;
            }
            if (!worldAway && outside)
            {
                worldAway = true;
                clearAtStageChange();
                if (!quiet) COMBO_LOG("seek: a wall break started at " + std::to_string(pos) + ", no jumps until it is over");
            }
            else if (worldAway && settled)
            {
                worldAway = false;
                clearAtStageChange();
                if (!quiet) COMBO_LOG("seek: the wall break is over at " + std::to_string(pos) + ", saves start again from here");
            }
        }
        if (haveSeekWatch && pos < seekWatch && !seekOwnJump)
        {
            clearRing();
            diverged = false;
        }
        seekOwnJump = false;
        seekWatch = pos;
        haveSeekWatch = true;
        if (analyzer || diverged || assistStandby || seekCatching || trial != Trial::Off || worldAway || !gameState->roundActive) return;
        if (!haveSeekTop || pos > seekTop)
        {
            seekTop = pos;
            haveSeekTop = true;
        }
        auto rollback = asw_rollback::get();
        const int slots = seekSlots(rollback);
        if (slots <= 0 || !backupSlotsOk(rollback)) return;
        if (static_cast<int>(seekSaves.size()) != slots)
        {
            seekSaves.assign(static_cast<size_t>(slots), SeekSave{});
            seekNext = 0;
        }
        const bool calm = seekBusy() == 0;
        bool nearAny = false, nearCalm = false;
        for (const auto& s : seekSaves)
        {
            if (!s.valid || s.position > pos) continue;
            if (pos - s.position < kSeekEvery) nearAny = true;
            if (s.calm && pos - s.position < kSeekCalmEvery) nearCalm = true;
        }
        if (calm ? nearCalm : nearAny) return;
        SeekSave& s = seekSaves[static_cast<size_t>(seekNext)];
        trialSave(rollback, kSeekFirstSlot + seekNext);
        if (!haveSeekOrigin)
        {
            seekOrigin = pos;
            haveSeekOrigin = true;
        }
        s.valid = true;
        s.calm = calm;
        s.position = pos;
        s.gameFrame = asw_engine::get() ? static_cast<int>(asw_engine::get()->GameFrame) : -1;
        s.shift = stageAt;
        for (int i = 0; i < 6; i++) s.heads[i] = manager->m_ReplayBuffer[i].m_ReadHead;
        seekNext = (seekNext + 1) % slots;
    }
    uint32_t seekTarget = 0, seekTop = 0, seekLastPos = 0, seekFrom = 0;
    ULONGLONG seekBegan = 0;

    bool seekRange(int& first, int& last, int& now) const
    {
        if (!seekEnabled || !gameState->checkModeReplay() || !manager || !haveSeekTop) return false;
        if (analyzer || diverged || assistStandby || worldAway) return false;
        uint32_t oldest = 0;
        if (!seekOldest(oldest) || seekTop <= oldest + 1) return false;
        first = static_cast<int>(oldest + 1);
        last = static_cast<int>(seekTop);
        now = static_cast<int>(replayPosition());
        return true;
    }

    static constexpr int kSeekUndoSlot = 17;
    uint32_t seekWanted = 0;
    bool seekWantedQuick = false, seekWantedBeyond = false;
    bool seekUndo = false, seekUndoDue = false, seekUndoRough = false;
    int seekUndoBreaks = 0;
    int seekStill = 0;
    ReplayHead seekUndoHeads[6]{};

    void seekFinish(const char* why, bool stalled = false)
    {
        seekCatching = false;
        timeline->setForced(false);
        timeline->setPaused(false);
        timeline->setPaused(true);
        const uint32_t pos = manager ? replayPosition() : 0;
        COMBO_LOG(std::string("seek: ") + (why ? why : "there") + ", at " + std::to_string(pos) + " (asked " + std::to_string(seekTarget) + ", from "
                  + std::to_string(seekFrom) + "), " + std::to_string(GetTickCount64() - seekBegan) + " ms");
        if (stalled && seekUndo)
        {
            seekUndoDue = true;
            seekDue = false;
        }
        seekUndo = false;
    }

    void seekPutBack()
    {
        seekUndoDue = false;
        auto rollback = asw_rollback::get();
        if (!backupSlotsOk(rollback) || !manager) return;
        int savedIndex = rollback->CurrentBackupDataIndex;
        const bool ok = restoreRewindPoint(kSeekUndoSlot, seekUndoBreaks);
        rollback->CurrentBackupDataIndex = savedIndex;
        if (ok)
        {
            for (int i = 0; i < 6; i++) manager->m_ReplayBuffer[i].m_ReadHead = seekUndoHeads[i];
            seekRough = seekUndoRough;
            ringLast = replayPosition();
            haveRingLast = true;
            seekStarted = true;
        }
        timeline->setPaused(false);
        timeline->setPaused(true);
        COMBO_LOG(std::string("seek: ") + (ok ? "put back where it was, at " + std::to_string(replayPosition()) : "couldn't be put back where it was"));
    }

    void seekPreTick()
    {
        if (seekUndoDue && !seekCatching) seekPutBack();
        if (!seekDue || seekCatching) return;
        seekDue = false;
        seekUndo = false;
        seekStill = 0;
        seekQuick = seekWantedQuick;
        auto rollback = asw_rollback::get();
        int first = 0, last = 0, now = 0;
        const char* why = trial != Trial::Off ? "a catch-up test is running"
            : !backupSlotsOk(rollback) ? "the game's save slots aren't there"
            : !gameState->roundActive ? "no round running"
            : g_cap_active ? "a capture is running"
            : promptSide ? "the takeover question is open"
            : !seekRange(first, last, now) ? "nothing saved to jump to"
            : nullptr;
        if (why)
        {
            COMBO_LOG(std::string("seek: not done, ") + why);
            return;
        }
        const int furthest = seekWantedBeyond ? (std::max)(last, static_cast<int>(seekWanted)) : last;
        const uint32_t target = static_cast<uint32_t>(std::clamp(static_cast<int>(seekWanted), first, furthest));
        const uint32_t pos = replayPosition();
        if (target == pos)
        {
            timeline->setPaused(false);
            timeline->setPaused(true);
            return;
        }
        seekFrom = pos;
        const uint32_t runUp = seekQuick ? kSeekQuickRunUp : kSeekRunUp;
        int bestSlot = -1;
        int bestFrame = -1;
        uint32_t bestPos = 0;
        bool bestCalm = false;
        bool bestSame = false;
        const ReplayHead* bestHeads = nullptr;
        int calmKept = 0, kept = 0;
        const int shiftNow = stageAt;
        int bestShift = stageAt;
        auto consider = [&](int slot, uint32_t position, const ReplayHead* heads, int gameFrame, bool calm, int shift) {
            kept++;
            if (calm) calmKept++;
            if (position >= target) return;
            const bool same = shift == shiftNow;
            if (!seekUsable(shift, shiftNow)) return;
            const bool clean = calm && target - position <= runUp;
            const bool better = bestSlot < 0 || (same != bestSame ? same : (clean != bestCalm ? clean : position > bestPos));
            if (better)
            {
                bestSlot = slot;
                bestPos = position;
                bestHeads = heads;
                bestFrame = gameFrame;
                bestCalm = clean;
                bestSame = same;
                bestShift = shift;
            }
        };
        for (int i = 0; i < kRingSlots; i++)
            if (ring[i].valid) consider(kRingFirstSlot + i, ring[i].position, ring[i].heads, -1, false, ring[i].shift);
        const int slots = (std::min)(seekSlots(rollback), static_cast<int>(seekSaves.size()));
        for (int i = 0; i < slots; i++)
        {
            const SeekSave& s = seekSaves[static_cast<size_t>(i)];
            if (s.valid) consider(kSeekFirstSlot + i, s.position, s.heads, s.gameFrame, s.calm, s.shift);
        }
        if (target < pos && bestSlot < 0)
        {
            COMBO_LOG("seek: not done, no save before that frame");
            return;
        }
        const bool ahead = bestSlot >= 0 && bestPos > pos;
        if (bestSlot >= 0 && (target < pos || (ahead && (bestCalm || target - pos > runUp)) || (seekRough && bestCalm)))
        {
            ReplayHead before[6];
            for (int i = 0; i < 6; i++) before[i] = manager->m_ReplayBuffer[i].m_ReadHead;
            int savedIndex = rollback->CurrentBackupDataIndex;
            trialSave(rollback, kSeekUndoSlot);
            seekUndo = true;
            seekUndoBreaks = stageAt;
            seekUndoRough = seekRough;
            for (int i = 0; i < 6; i++) seekUndoHeads[i] = before[i];
            const bool ok = restoreRewindPoint(bestSlot, bestShift);
            rollback->CurrentBackupDataIndex = savedIndex;
            if (!ok)
            {
                seekUndo = false;
                if (bestSlot >= kSeekFirstSlot) seekSaves[static_cast<size_t>(bestSlot - kSeekFirstSlot)].valid = false;
                else ring[bestSlot - kRingFirstSlot].valid = false;
                COMBO_LOG("seek: not done, the load was refused");
                return;
            }
            const int loadedFrame = asw_engine::get() ? static_cast<int>(asw_engine::get()->GameFrame) : -1;
            if (bestFrame >= 0 && loadedFrame != bestFrame)
            {
                COMBO_LOG("seek: stopped, the load gave frame " + std::to_string(loadedFrame) + " and the save was made at " + std::to_string(bestFrame));
                for (int i = 0; i < 6; i++) manager->m_ReplayBuffer[i].m_ReadHead = before[i];
                diverged = true;
                timeline->setPaused(false);
                timeline->setPaused(true);
                return;
            }
            for (int i = 0; i < 6; i++) manager->m_ReplayBuffer[i].m_ReadHead = bestHeads[i];
            seekRough = !bestCalm;
            roundCount = asw_state::get()->RoundCount;
            ringLast = bestPos;
            haveRingLast = true;
            seekStarted = true;
            COMBO_LOG(std::string("seek: loaded the ") + (bestCalm ? "calm" : "rough") + " save at " + std::to_string(bestPos) + " (slot " + std::to_string(bestSlot)
                      + ") for " + std::to_string(target) + ", " + std::to_string(calmKept) + " of " + std::to_string(kept) + " saves are calm");
        }
        seekTarget = target;
        seekLastPos = replayPosition();
        seekBegan = GetTickCount64();
        seekCatching = true;
        resetRewindDelay();
        timeline->setForced(true);
    }

    int seekAfterTick()
    {
        if (!seekCatching) return 0;
        if (!gameState->checkModeReplay() || !manager || !asw_engine::get() || !gameState->roundActive || analyzer)
        {
            seekFinish("stopped, the round or the replay changed");
            return 0;
        }
        const uint32_t pos = replayPosition();
        if (pos >= seekTarget)
        {
            seekFinish(nullptr);
            return 0;
        }
        if (pos == seekLastPos)
        {
            if (++seekStill >= 12)
            {
                seekFinish("stopped, the replay isn't moving (paused in the game or frozen)", true);
                return 0;
            }
        }
        else seekStill = 0;
        seekLastPos = pos;
        return static_cast<int>((std::min)(seekTarget - pos, static_cast<uint32_t>(60)));
    }

    void seekBatchDone()
    {
        if (!seekCatching || !manager) return;
        const uint32_t pos = replayPosition();
        if (pos >= seekTarget) seekFinish(nullptr);
        else if (pos != seekLastPos)
        {
            seekLastPos = pos;
            seekStill = 0;
        }
    }

    void seekGuard()
    {
        if (!seekCatching) return;
        keybindings->resetButton(keybindings->TOGGLE_SAVE_BUTTON);
        keybindings->resetButton(keybindings->TOGGLE_LOAD_BUTTON);
        keybindings->resetButton(keybindings->TAKEOVER_P1_BUTTON);
        keybindings->resetButton(keybindings->TAKEOVER_P2_BUTTON);
        loadState = false;
        rewindRequested = false;
        if (analyzer || !gameState->roundActive || !gameState->checkModeReplay()) seekFinish("stopped, the round or the replay changed");
    }

    struct TrialProbe
    {
        int gameFrame = 0;
        ReplayHead heads[2]{};
        int x[2]{}, y[2]{}, hp[2]{}, action[2]{};
        char sprite[2][33]{};
    };
    enum class Trial { Off, SaveDue, Playing, RestoreDue, CatchingUp };
    Trial trial = Trial::Off;
    int trialFrames = 0;
    int trialTicks = 0;
    int trialBattleCalls = 0;
    int trialCallsSeen = 0;
    int trialLastFrame = 0;
    uint32_t trialLastPos = 0;
    bool trialControl = false;
    bool trialSameCounter = false;
    uint64_t trialCounterAtSave = 0;
    ReplayHead trialHeads[6]{};
    TrialProbe trialStart{}, trialNormal{}, trialRestored{};
    static constexpr int kTrialSlot = 17, kTrialNormalSlot = 0, kTrialCaughtSlot = 18;

    TrialProbe trialProbe() const
    {
        TrialProbe p;
        auto* eng = asw_engine::get();
        if (!eng || !manager) return p;
        p.gameFrame = eng->GameFrame;
        for (int i = 0; i < 2; i++)
        {
            p.heads[i] = manager->m_ReplayBuffer[i].m_ReadHead;
            auto* e = eng->players[i].entity;
            if (!e) continue;
            p.x[i] = e->pos_x;
            p.y[i] = e->pos_y;
            p.action[i] = e->action_time;
            p.hp[i] = *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(e) + 0x1254);
            const char* s = e->get_sprite_name();
            if (s) memcpy(p.sprite[i], s, strnlen(s, 32));
        }
        return p;
    }

    static std::string trialDiff(const TrialProbe& a, const TrialProbe& b, bool fightOnly)
    {
        std::string d;
        if (!fightOnly && a.gameFrame != b.gameFrame) d += "frame ";
        for (int i = 0; i < 2; i++)
        {
            const std::string who = i == 0 ? "P1 " : "P2 ";
            if (!fightOnly && (a.heads[i].m_Cursor != b.heads[i].m_Cursor || a.heads[i].m_Count != b.heads[i].m_Count
                               || a.heads[i].m_SolidPosition != b.heads[i].m_SolidPosition)) d += who + "read, ";
            if (a.x[i] != b.x[i]) d += who + "x, ";
            if (a.y[i] != b.y[i]) d += who + "y, ";
            if (a.hp[i] != b.hp[i]) d += who + "hp, ";
            if (strcmp(a.sprite[i], b.sprite[i]) != 0) d += who + "sprite, ";
            if (a.action[i] != b.action[i]) d += who + "action time, ";
        }
        return d;
    }

    static std::string trialText(const TrialProbe& p)
    {
        char b[420];
        snprintf(b, sizeof(b), "frame %d | P1 read %u/%u/%u at %d,%d hp %d %s +%d | P2 read %u/%u/%u at %d,%d hp %d %s +%d",
                 p.gameFrame,
                 p.heads[0].m_Cursor, p.heads[0].m_Count, p.heads[0].m_SolidPosition, p.x[0], p.y[0], p.hp[0], p.sprite[0], p.action[0],
                 p.heads[1].m_Cursor, p.heads[1].m_Count, p.heads[1].m_SolidPosition, p.x[1], p.y[1], p.hp[1], p.sprite[1], p.action[1]);
        return b;
    }

    void trialSave(asw_rollback* rollback, int slot)
    {
        int savedIndex = rollback->CurrentBackupDataIndex;
        const int max = rollback->MaxBackupFrame;
        rollback->CurrentBackupDataIndex = (slot + max - 1) % max;
        orig_SaveBackup(rollback);
        rollback->CurrentBackupDataIndex = savedIndex;
    }

    static std::string trialBytes(const uint8_t* x, const uint8_t* y, size_t n, const char* name, bool inlinePart)
    {
        if (!x || !y || n == 0 || n > (size_t(256) << 20)) return std::string(name) + " not compared";
        size_t differ = 0;
        std::string where;
        int listed = 0;
        for (size_t i = 0; i < n; i++)
        {
            if (inlinePart && ((i >= 0x3EA0 && i < 0x3EB0) || (i >= 0x6E10 && i < 0x6E18))) continue;
            if (x[i] == y[i]) continue;
            differ++;
            if (listed < 8)
            {
                char b[24];
                snprintf(b, sizeof(b), "%s0x%zX", listed ? " " : "", i);
                where += b;
                listed++;
            }
        }
        return std::string(name) + " " + std::to_string(differ) + " of " + std::to_string(n) + " bytes differ" + (differ ? " (first at " + where + ")" : std::string());
    }

    std::string trialStateDiff(asw_rollback* rollback, int a, int b)
    {
        const auto* sa = reinterpret_cast<const uint8_t*>(&rollback->BackupData[a]);
        const auto* sb = reinterpret_cast<const uint8_t*>(&rollback->BackupData[b]);
        const size_t objSize = static_cast<size_t>(rollback->ObjVectorBufferSize);
        const size_t charSize = static_cast<size_t>(rollback->CharVectorBufferSize);
        const auto* oa = *reinterpret_cast<const uint8_t* const*>(sa + 0x3EA0);
        const auto* ob = *reinterpret_cast<const uint8_t* const*>(sb + 0x3EA0);
        const auto* ca = *reinterpret_cast<const uint8_t* const*>(sa + 0x3EA8);
        const auto* cb = *reinterpret_cast<const uint8_t* const*>(sb + 0x3EA8);
        return trialBytes(sa, sb, 0x8750, "slot", true) + "; " + trialBytes(oa, ob, objSize, "objects", false) + "; "
               + trialBytes(ca, cb, charSize, "characters", false);
    }

    void trialKey(int frames, int flags)
    {
        if (trial != Trial::Off)
        {
            COMBO_LOG("catch-up test: one is already running");
            return;
        }
        const char* why = !gameState->checkModeReplay() ? "not in a replay"
            : seekDue || seekCatching ? "a jump is going"
            : !manager ? "no replay data yet"
            : !backupSlotsOk(asw_rollback::get()) ? "the game's save slots aren't there"
            : analyzer ? "a side is taken over"
            : diverged ? "the fight differs from the replay"
            : assistStandby ? "Replay Assist is counting down"
            : g_cap_active ? "a capture is running"
            : !gameState->roundActive ? "no round running"
            : timeline->isPaused() ? "the timeline is paused"
            : nullptr;
        if (why)
        {
            COMBO_LOG(std::string("catch-up test: not started, ") + why);
            return;
        }
        trialFrames = std::clamp(frames, 2, 120);
        trialControl = (flags & 1) != 0;
        trialSameCounter = (flags & 2) != 0;
        trial = Trial::SaveDue;
    }

    void trialPreTick()
    {
        if (trial == Trial::Off) return;
        auto rollback = asw_rollback::get();
        if (!gameState->checkModeReplay() || !manager || !backupSlotsOk(rollback))
        {
            COMBO_LOG("catch-up test: stopped, the replay is gone");
            trial = Trial::Off;
            return;
        }
        if ((trial == Trial::SaveDue || trial == Trial::Playing) && (!gameState->roundActive || timeline->isPaused() || analyzer))
        {
            COMBO_LOG("catch-up test: stopped, the round ended or a pause or takeover came in");
            trial = Trial::Off;
            return;
        }
        if (trial == Trial::SaveDue)
        {
            trialSave(rollback, kTrialSlot);
            for (int i = 0; i < 6; i++) trialHeads[i] = manager->m_ReplayBuffer[i].m_ReadHead;
            trialStart = trialProbe();
            trialCounterAtSave = trialSameCounter && frameCounter ? *frameCounter : 0;
            trialTicks = 0;
            trialCallsSeen = trialBattleCalls;
            trialLastFrame = trialStart.gameFrame;
            trialLastPos = replayPosition();
            trial = Trial::Playing;
            COMBO_LOG("catch-up test: saved, " + std::to_string(trialFrames) + " frames" + (trialControl ? " (control: one frame short on purpose)" : "")
                      + (trialCounterAtSave ? " (same engine frame numbers as the normal part)" : "")
                      + ", " + std::to_string(static_cast<int>(rollback->MaxBackupFrame)) + " slots, buffers "
                      + std::to_string(static_cast<unsigned long long>(rollback->ObjVectorBufferSize)) + " + "
                      + std::to_string(static_cast<unsigned long long>(rollback->CharVectorBufferSize)) + " bytes");
            COMBO_LOG("catch-up test: start   " + trialText(trialStart));
        }
        else if (trial == Trial::RestoreDue)
        {
            int savedIndex = rollback->CurrentBackupDataIndex;
            const bool ok = restoreRewindPoint(kTrialSlot);
            rollback->CurrentBackupDataIndex = savedIndex;
            if (!ok)
            {
                COMBO_LOG("catch-up test: INCONCLUSIVE, the load was refused");
                trial = Trial::Off;
                return;
            }
            for (int i = 0; i < 6; i++) manager->m_ReplayBuffer[i].m_ReadHead = trialHeads[i];
            ringLast = replayPosition();
            haveRingLast = true;
            trialRestored = trialProbe();
            trialCallsSeen = trialBattleCalls;
            trial = Trial::CatchingUp;
            COMBO_LOG(std::string("catch-up test: loaded  ") + trialText(trialRestored) + (ok ? "" : " (the load was refused)"));
        }
    }

    int trialAfterTick()
    {
        if (trial != Trial::Playing) return 0;
        auto* eng = asw_engine::get();
        auto rollback = asw_rollback::get();
        if (!gameState->checkModeReplay() || !eng || !manager || !backupSlotsOk(rollback))
        {
            COMBO_LOG("catch-up test: stopped, the replay is gone");
            trial = Trial::Off;
            return 0;
        }
        const int calls = trialBattleCalls - trialCallsSeen;
        trialCallsSeen = trialBattleCalls;
        const int frame = eng->GameFrame;
        const uint32_t pos = replayPosition();
        const bool stood = frame == trialLastFrame && pos == trialLastPos;
        trialLastFrame = frame;
        trialLastPos = pos;
        if (calls != 1 || stood)
        {
            COMBO_LOG("catch-up test: INCONCLUSIVE, a tick passed without the fight moving (a pause or a slowed replay): try again, " + std::to_string(calls)
                      + " fight updates in that tick");
            trial = Trial::Off;
            return 0;
        }
        if (++trialTicks < trialFrames) return 0;
        trialNormal = trialProbe();
        trialSave(rollback, kTrialNormalSlot);
        trialSave(rollback, kTrialCaughtSlot);
        COMBO_LOG("catch-up test: normal  " + trialText(trialNormal));
        COMBO_LOG("catch-up test: whole state, the same instant saved twice: " + trialStateDiff(rollback, kTrialNormalSlot, kTrialCaughtSlot));
        trial = Trial::RestoreDue;
        return trialControl ? trialFrames - 1 : trialFrames;
    }

    void trialCatchUpDone(double ms)
    {
        if (trial != Trial::CatchingUp)
        {
            COMBO_LOG("catch-up test: INCONCLUSIVE, the load never ran during the catch-up");
            trial = Trial::Off;
            return;
        }
        trial = Trial::Off;
        const int wanted = trialControl ? trialFrames - 1 : trialFrames;
        const int calls = trialBattleCalls - trialCallsSeen;
        const TrialProbe fast = trialProbe();
        COMBO_LOG("catch-up test: caught  " + trialText(fast));
        auto rollback = asw_rollback::get();
        if (!trialControl && backupSlotsOk(rollback))
        {
            trialSave(rollback, kTrialCaughtSlot);
            COMBO_LOG("catch-up test: whole state, normal against caught: " + trialStateDiff(rollback, kTrialNormalSlot, kTrialCaughtSlot));
        }
        const std::string loadDiff = trialDiff(trialRestored, trialStart, false);
        const std::string diff = trialDiff(fast, trialNormal, false);
        const std::string fightDiff = trialDiff(fast, trialNormal, true);
        const bool still = trialDiff(trialNormal, trialStart, true).empty();
        std::string verdict;
        if (!loadDiff.empty()) verdict = "INCONCLUSIVE, the load didn't put the start back (" + loadDiff + ")";
        else if (calls != wanted) verdict = "INCONCLUSIVE, the fight updated " + std::to_string(calls) + " times in " + std::to_string(wanted) + " catch-up ticks";
        else if (trialNormal.gameFrame == trialStart.gameFrame) verdict = "INCONCLUSIVE, the fight's frame didn't move during the normal part";
        else if (trialControl) verdict = fightDiff.empty() ? "CONTROL WEAK, one frame short looks the same in the fighters here: try again while they move"
                                                           : "control ok, one frame short is told apart by " + fightDiff;
        else if (diff.empty()) verdict = std::string("MATCH") + (still ? " (weak: the fighters didn't move during the test)" : "");
        else verdict = "MISMATCH in " + diff;
        if (!trialControl && !diff.empty() && loadDiff.empty()) diverged = true;
        char b[200];
        snprintf(b, sizeof(b), "; %d frames caught up in %.1f ms; frame went %d normally, %d caught up", wanted, ms,
                 trialNormal.gameFrame - trialStart.gameFrame, fast.gameFrame - trialRestored.gameFrame);
        COMBO_LOG("catch-up test: " + verdict + b);
    }

    void placeRewindPoint()
    {
        auto rollback = asw_rollback::get();
        if (!backupSlotsOk(rollback)) return;
        rollback->CurrentBackupDataIndex = 18;
        orig_SaveBackup(rollback);
        rewindPointSet = true;
        rewindCharacters = currentCharacters();
        rewindStage = asw_stage_area();
        rewindShift = stageAt;
        rewindPath.assign(stageJournal.begin(), stageJournal.begin() + (std::min)(static_cast<size_t>((std::max)(stageAt, 0)), stageJournal.size()));
        rewindPathOpen = !stageSettled();
        rewindIdentity = asw_stage_identity();
        rewindRound = asw_state::get() ? static_cast<int>(asw_state::get()->RoundCount) : -1;
        stageCanReset = asw_stage_can_reset();
        rewindStale = false;
        COMBO_LOG("rewind point: set in stage area " + std::to_string(rewindStage) + " after " + std::to_string(rewindShift) + " wall break(s), the stage "
                  + (rewindCanPutBack() ? "can be put back" : "can't be put back from another area"));
        if (gameState->checkModeReplay())
        {
            pickAssistSave();
            saveBackup();
        }
        ModMenu::instance().notifyRewindPointCreated();
    }

    bool restoreRewindPoint(int slot = 19, int breaks = -1, const StagePath* own = nullptr)
    {
        auto rollback = asw_rollback::get();
        if (!backupSlotsOk(rollback)) return false;
        const int max = rollback->MaxBackupFrame;
        if (slot < 0 || slot >= max || !rollback->BackupData[slot].bSet)
        {
            COMBO_LOG("load: nothing saved in slot " + std::to_string(slot));
            return false;
        }
        seekOwnJump = true;
        const int areaBefore = asw_stage_area();
        int after = (slot + 1) % max;
        bool afterSet = rollback->BackupData[after].bSet;
        rollback->CurrentBackupDataIndex = after;
        rollback->RollbackFrame = 1;
        rollback->TotalRollbackFrames = 0;
        auto backupData = rollback->BackupData[slot];
        rememberLinksBeforeLoad();
        orig_Rollback(rollback);
        rollback->BackupData[after].bSet = afterSet;
        if (orig_EffectCleanupA && orig_EffectCleanupB)
        {
            orig_EffectCleanupA(rollback);
            orig_EffectCleanupB(rollback);
        }
        rebuildLinksAfterLoad();
        repairEffectsAfterLoad();
        syncDangerBanner();
        rollback->BackupData[slot] = backupData;
        rollback->RollbackFrame = 0;
        rollback->TotalRollbackFrames = 0;
        restoreReplayHead();
        if (breaks >= 0) stagePut(breaks, own, areaBefore);
        else if (const int area = asw_stage_area(); area != areaBefore && asw_stage_untouched())
        {
            const int called = asw_stage_round_reset();
            COMBO_LOG("load: went from stage area " + std::to_string(areaBefore) + " to " + std::to_string(area)
                      + ", the stage's round reset ran on " + std::to_string(called) + " listener(s)");
            stageAt = 0;
            lastStage = area;
        }
        else lastStage = asw_stage_area();
        return true;
    }

    bool restoreAssist()
    {
        if (!gameState->checkModeReplay() || assistFrames() <= 0 || !assistSide
            || assistSlot < 0 || !ring[assistSlot].valid) return false;
        const RingSave& save = ring[assistSlot];
        if (!restoreRewindPoint(kRingFirstSlot + assistSlot, save.shift)) return false;
        for (int i = 0; i < 6; i++) manager->m_ReplayBuffer[i].m_ReadHead = save.heads[i];
        analyzer = nullptr;
        diverged = false;
        ringLast = save.position;
        haveRingLast = true;
        assistFrom = save.position;
        assistStandby = true;
        resetRewindDelay();
        timeline->setPaused(false);
        return true;
    }

    void onPreTick()
    {
        if (!gameState->checkMode())
        {
            closePrompt("left the fight");
            resetRewindDelay();
            return;
        }
        if (promptSide && !gameState->checkModeReplay()) closePrompt("left the replay");
        if (promptSide) BindWatcherI::setEscapeCapture(true);  // keep Esc for the prompt a little longer
        seekGuard();
        const int stage = asw_stage_area();
        asw_stage_record_start();
        if (stage != lastStage)
        {
            stageCanReset = stage != kNoStageArea && asw_stage_can_reset();
            asw_stage_record_refresh();
            if (lastStage != kNoStageArea && stage != kNoStageArea)
            {
                const bool reset = asw_stage_untouched();
                rewindPathOpen = false;
                if (!reset)
                {
                    stageAt++;
                    if (static_cast<int>(stageJournal.size()) < stageAt) stageJournal.resize(static_cast<size_t>(stageAt));
                    stageJournal[static_cast<size_t>(stageAt - 1)].clear();
                }
                clearAtStageChange();
                COMBO_LOG("stage: area changed from " + std::to_string(lastStage) + " to " + std::to_string(stage)
                          + (reset ? " (a reset)" : ", wall break " + std::to_string(stageAt))
                          + (seekEnabled && stageCanReset ? ", Timeline saves are kept" : ", saves from before it are dropped"));
            }
            lastStage = stage;
        }
        if (stage != kNoStageArea && asw_stage_untouched()) stageAt = 0;
        for (const asw_stage_call& call : asw_stage_recorded())
        {
            if (call.event != kStageRoundReset && stageAt >= 1 && static_cast<int>(stageJournal.size()) >= stageAt)
            {
                auto& step = stageJournal[static_cast<size_t>(stageAt - 1)];
                const bool have = std::any_of(step.begin(), step.end(), [&](const asw_stage_call& c) {
                    return c.event == call.event && c.size == call.size && memcmp(c.params, call.params, sizeof(c.params)) == 0;
                });
                if (!have) step.push_back(call);
                if (rewindPathOpen && rewindShift == stageAt && static_cast<int>(rewindPath.size()) == stageAt) rewindPath[static_cast<size_t>(stageAt - 1)] = step;
            }
            if (seekVerbose)
            {
                std::string bytes;
                char hex[4];
                for (int b = 0; b < call.size; b++)
                {
                    snprintf(hex, sizeof(hex), " %02X", call.params[b]);
                    bytes += hex;
                }
                COMBO_LOG(std::string("stage event: ") + asw_stage_event_name(call.event) + " with" + bytes + " (area " + std::to_string(stage) + ", wall break "
                          + std::to_string(stageAt) + ")");
            }
        }
        rewindStale = rewindPointSet && stage != kNoStageArea && (rewindStage != stage || rewindShift != stageAt) && !rewindCanPutBack();

        if (!isWaitingForFrame && rewindFramesRemaining > 0)
        {
            // With the prompt up the fight stays paused until it is answered.
            if (--rewindFramesRemaining == 0)
            {
                if (promptSide) promptWasPaused = false;
                else timeline->setPaused(false);
            }
        }

        // No automatic rewind point. A timer that re-saved every N seconds
        // destroyed the user's manual F6 point before they could return to it
        // (F9 would only go back as far as the last automatic save), and in a
        // replay it re-snapshotted all six read heads and reset takeover state
        // on that same timer.

        if (keybindings->getButtonState(keybindings->TOGGLE_SAVE_BUTTON))
        {
            keybindings->resetButton(keybindings->TOGGLE_SAVE_BUTTON);
            placeRewindPoint();
        }

        if (loadState || (!gameState->checkModeReplay()
            && keybindings->getButtonState(keybindings->TOGGLE_LOAD_BUTTON)))
        {
            keybindings->resetButton(keybindings->TOGGLE_LOAD_BUTTON);
            if (trial != Trial::Off)
            {
                COMBO_LOG("catch-up test: stopped, a rewind came in");
                trial = Trial::Off;
            }
            const bool explicitRewind = rewindRequested || !gameState->checkModeReplay();
            // Assist only for F9 with a side picked. Turning takeover off goes back to
            // the F6 point itself, like before.
            const bool assist = rewindRequested;
            loadState = false;
            rewindRequested = false;
            COMBO_LOG(std::string("load: ") + (assist ? "F9" : "takeover off") + ", side "
                      + (assistSide ? "set" : "none") + ", analyzer " + (analyzer ? "set" : "none"));
            const bool otherStage = rewindPointSet && (rewindStage != asw_stage_area() || rewindShift != stageAt);
            if (!rewindPointSet || rewindCharacters != currentCharacters())
            {
                // A new opponent or character starts a fresh fight, and the old save can't
                // be loaded into it: do nothing at all rather than half a load.
                COMBO_LOG(rewindPointSet ? "load: the rewind point was for other characters, press F6 again"
                                         : "load: no rewind point yet");
                rewindPointSet = false;
            }
            else if (otherStage && explicitRewind && !rewindCanPutBack())
            {
                COMBO_LOG("load: the rewind point is from stage area " + std::to_string(rewindStage) + ", this is area "
                          + std::to_string(asw_stage_area()) + ", nothing loaded");
            }
            else if ((!assist || !restoreAssist()) && restoreRewindPoint(19, rewindShift, &rewindPath))
            {
                if (otherStage && !rewindCanPutBack())
                    COMBO_LOG("load: the stage changed since the rewind point (a wall break), it can look wrong until the round ends");
                diverged = pointDiverged;
                // Our own jump back: keep the ring, only restart its spacing from here.
                ringLast = replayPosition();
                haveRingLast = gameState->checkModeReplay();
                if (explicitRewind) beginRewindDelay();
            }
        }

        seekPreTick();
        trialPreTick();

        if (gameState->checkModeReplay())
        {
            updateAssist();
            ringSave();
            seekSaveTick();
        }

        // A tick can consume Step without reaching UpdateBattle. Keep requesting
        // it until the restored fight has actually processed its first frame.
        if (isWaitingForFrame) timeline->advanceFrame();
    }

    void update()
    {
        if (asw_state::get()->RoundCount != roundCount)
        {
            roundCount = asw_state::get()->RoundCount;
            asw_engine::get()->GameFrame = 0;
        }

        seekGuard();
        if (seekCatching) return;

        if (keybindings->getButtonState(keybindings->TOGGLE_SAVE_BUTTON))
        {
            keybindings->resetButton(keybindings->TOGGLE_SAVE_BUTTON);
            placeRewindPoint();
        }

        if (keybindings->getButtonState(keybindings->TAKEOVER_P1_BUTTON))
        {
            keybindings->resetButton(keybindings->TAKEOVER_P1_BUTTON);
            takeoverKey(&asw_engine::get()->inputs[0]);
        }

        if (keybindings->getButtonState(keybindings->TAKEOVER_P2_BUTTON))
        {
            keybindings->resetButton(keybindings->TAKEOVER_P2_BUTTON);
            takeoverKey(&asw_engine::get()->inputs[3]);
        }

        // Prompt answers from the pad: d-pad Right is yes, Left is no. Only the real
        // d-pad counts (not the stick or the arrow keys, which may be moving a
        // character), and only once the prompt has been up for a moment.
        const WORD dpad = promptSide ? dpadPresses() : 0;
        if (promptSide && !ModMenu::instance().takesInput() && GetTickCount64() - promptOpenedAt > 250)
        {
            if (dpad & XINPUT_GAMEPAD_DPAD_RIGHT) takeoverKey(promptSide);
            else if (dpad & XINPUT_GAMEPAD_DPAD_LEFT) closePrompt("no");
        }
        if (promptSide && BindWatcherI::takeEscape()) closePrompt("no");

        if (keybindings->getButtonState(keybindings->TOGGLE_LOAD_BUTTON))
        {
            keybindings->resetButton(keybindings->TOGGLE_LOAD_BUTTON);
            if (promptSide) closePrompt("F9");
            if (isTakeover)
            {
                loadState = true;
                rewindRequested = true;
                if (analyzer) assistSide = analyzer;
            }
        }
    }

    // ---- Takeover prompt -------------------------------------------------------------
    // F7/F8 pause on the current frame and ask first. The same key again says yes,
    // Esc says no. Starting and stopping both ask, so the jump back to the rewind
    // point that stopping does is never a surprise.
    std::string rewindCharacters;  // who was fighting when F6 was pressed
    int rewindStage = kNoStageArea;
    int rewindShift = 0;
    StagePath rewindPath;
    bool rewindPathOpen = false;
    const void* rewindIdentity = nullptr;
    int rewindRound = -1;
    bool rewindCanPutBack() const
    {
        return stageCanReset && stageCovers(rewindPath, 0, rewindShift) && rewindIdentity && rewindIdentity == asw_stage_identity()
               && asw_state::get() && static_cast<int>(asw_state::get()->RoundCount) == rewindRound;
    }
    int lastStage = kNoStageArea;
    std::atomic<bool> rewindStale{false};

    static std::string currentCharacters()
    {
        const auto* eng = asw_engine::get();
        if (!eng) return {};
        std::string who;
        for (int p = 0; p < 2; p++)
        {
            const auto* e = eng->players[p].entity;
            const char* sprite = e ? e->get_sprite_name() : nullptr;
            who += sprite ? std::string(sprite, strnlen(sprite, 3)) : std::string("?");  // sprites start with the character code
            who += '/';
        }
        return who;
    }

    asw_inputs* promptSide = nullptr;
    bool promptStart = false;      // what the prompt asked: start (true) or stop
    bool promptWasPaused = false;
    ULONGLONG promptOpenedAt = 0;
    WORD dpadHeld = 0;
    bool rewindPointSet = false;

    WORD dpadPresses()
    {
        WORD now = 0;
        for (DWORD i = 0; i < 4; i++)
        {
            XINPUT_STATE st{};
            if (XInputGetState(i, &st) == ERROR_SUCCESS)
                now |= st.Gamepad.wButtons & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT);
        }
        const WORD pressed = now & ~dpadHeld;
        dpadHeld = now;
        return pressed;
    }

    void takeoverKey(asw_inputs* side)
    {
        const auto* eng = asw_engine::get();
        const char* name = eng && side == &eng->inputs[0] ? "P1" : "P2";
        if (!promptSide)
        {
            promptSide = side;
            promptStart = analyzer != side;
            promptOpenedAt = GetTickCount64();
            dpadPresses();  // a d-pad already held now doesn't count as an answer
            promptWasPaused = timeline->isPaused();
            timeline->setPaused(true);
            BindWatcherI::setEscapeCapture(true);
            COMBO_LOG(std::string("takeover: asking ") + (analyzer == side ? "to stop " : "to start ") + name);
            return;
        }
        if (promptSide != side) return;  // the other side's key while asking: ignored
        // Do what was asked, even if the state changed in the frame before the pause.
        const bool start = promptStart;
        closePrompt("yes");
        if (start == (analyzer != side)) toggleTakeover(side);
        else COMBO_LOG("takeover: already so, nothing to do");
    }

    void closePrompt(const char* answer)
    {
        if (!promptSide) return;
        promptSide = nullptr;
        BindWatcherI::setEscapeCapture(false);
        if (!promptWasPaused) timeline->setPaused(false);
        COMBO_LOG(std::string("takeover: prompt answered ") + answer);
    }

    void reset()
    {
        closePrompt("closed");
        isTakeover = false;
        loadState = false;
        rewindRequested = false;
        analyzer = nullptr;
        assistStandby = false;
    }

    // Same side's key again = stop taking over. The replay then plays on by itself.
    // With a rewind point, jump back to it first: from where takeover left off the
    // fight no longer matches the recording, so the replay inputs would make no sense.
    void toggleTakeover(asw_inputs* side)
    {
        const auto* eng = asw_engine::get();
        const char* name = eng && side == &eng->inputs[0] ? "P1" : "P2";
        if (analyzer != side)
        {
            analyzer = side;
            assistSide = side;
            assistStandby = false;
            diverged = true;
            COMBO_LOG(std::string("takeover: ") + name + " on at " + std::to_string(replayPosition()));
            return;
        }
        analyzer = nullptr;
        assistSide = nullptr;
        if (isTakeover) loadState = true;
        COMBO_LOG(std::string("takeover: ") + name + " off at " + std::to_string(replayPosition())
                  + (isTakeover ? ", back to the rewind point" : ""));
    }

    bool isTakeover = false;
    bool loadState = false;
    bool rewindRequested = false;
    int roundCount = -1;

    void initialize(GameState& state, Keybinds& input, Timeline& playback)
    {
        gameState = &state;
        keybindings = &input;
        timeline = &playback;
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

        // Link-by-name functions for effects and models (see rebuildLinksAfterLoad).
        // Optional: if any moves in a game update, F9 just skips the rebuild.
        try
        {
            orig_LinkFx = reinterpret_cast<funcLinkFx_t>(sigscan::get().scan(
                "\x40\x55\x53\x56\x41\x54\x41\x56\x48\x8D\x6C\x24\xC0\x48\x81\xEC\x40\x01\x00\x00\x33\xDB\xC6\x81\xA8\x3D\x00\x00\x00\x45\x33\xC9\x48\x89\x99\xC8\x3D\x00\x00\x48\x89\x99\xD0\x3D\x00\x00",
                "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
            orig_LinkFxWithArgs = reinterpret_cast<funcLinkByName_t>(sigscan::get().scan(
                "\x40\x55\x56\x41\x56\x41\x57\x48\x8D\x6C\x24\x88\x48\x81\xEC\x78\x01\x00\x00\x41\xB1\x01\x41\xB8\x17\x00\x00\x00\x4C\x8B\xF2\x48\x8B\xF1\xE8\x00\x00\x00\x00\x45\x33\xFF\x84\xC0",
                "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxx"));
            orig_LinkModel = reinterpret_cast<funcLinkByName_t>(sigscan::get().scan(
                "\x48\x89\x5C\x24\x18\x55\x56\x57\x48\x81\xEC\x70\x01\x00\x00\x48\x8B\x05\x00\x00\x00\x00\x48\x33\xC4\x48\x89\x84\x24\x60\x01\x00\x00\x48\x8B\x05\x00\x00\x00\x00\x48\x8B\xF2\x48\x8B\xF9",
                "xxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxx????xxxxxx"));
            orig_LinkCommonFxModel = reinterpret_cast<funcLinkByName_t>(sigscan::get().scan(
                "\x40\x56\x57\x48\x81\xEC\x78\x01\x00\x00\x48\x8B\x05\x00\x00\x00\x00\x48\x33\xC4\x48\x89\x84\x24\x60\x01\x00\x00\xC6\x44\x24\x28\x00\x41\xB1\x01\x45\x33\xC0\xC6\x44\x24\x20\x00",
                "xxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxxxxx"));
            orig_LinkBlueprintModel = reinterpret_cast<funcLinkByName_t>(sigscan::get().scan(
                "\x48\x89\x74\x24\x20\x57\x48\x81\xEC\x70\x01\x00\x00\x48\x8B\x05\x00\x00\x00\x00\x48\x33\xC4\x48\x89\x84\x24\x60\x01\x00\x00\xC6\x44\x24\x28\x01\x45\x33\xC9\x45\x33\xC0\xC6\x44\x24\x20\x00",
                "xxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxxxxx"));
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

        const uintptr_t GameKeyToRecFlag_Addr = sigscan::get().scan(
            "\x89\x54\x24\x00\x89\x4C\x24\x00\x53\x55\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x83\xEC\x00\x8B\xFA",
            "xxx?xxx?xxxxxxxxxxxxxxx?xx");
        orig_GameKeyToRecFlag = reinterpret_cast<funcGameKeyToRecFlag_t>(GameKeyToRecFlag_Addr);

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

        setManager(reinterpret_cast<ReplayManager_Internal*>(orig_GetReplayManager() - 0x8));

    }

    // Replay input buffers are one continuous ~36000-frame stream per side for
    // the WHOLE match, including intro/menu frames (an intro skip is recorded
    // like any other input), so index 0 is not round start. Capture the read
    // cursors at BATTLE_START -- that is the per-round anchor needed to slice a
    // single round's inputs out of the stream.
    void onRoundStart()
    {
        // Replay peers are one per player seat: 0 = P1, 1 = P2. (The 0-2 / 3-5
        // grouping is asw_inputs' live/committed/backup triplets and does not
        // apply here -- peers 2-5 stay empty in a 1v1 replay, which is why
        // capturing from peer 3 produced nothing.)
        ReplayHead p1{}, p2{};
        if (gameState->checkModeReplay()
            && peerReadHead(0, p1) && peerReadHead(1, p2))
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

    RECFLG_ENUM takeoverInput(RECFLG_ENUM recFlag, bool menuOpen) const
    {
        // is_battle_ptr is pre-resolved at init from multiple candidates and only
        // set if it actually points to a byte valued 0 or 1 (i.e., a real boolean
        // flag).  If probing failed on this game build, or system state isn't
        // ready, fall through with the original recFlag — no crash, just no
        // live key-mapping override.
        const auto pad = gameState->getMainQuadrant();
        if (!binds::use_mod_binds && (is_battle_ptr == nullptr || system_red == nullptr
            || system_red->m_BattleKey[pad] == nullptr))
        {
            return recFlag;
        }

        if (binds::use_mod_binds)
        {
            // Mod's own takeover binds: keyboard + XInput, independent of the game's key config.
            // Menu open: the inputs drive the menu instead.
            return static_cast<RECFLG_ENUM>(menuOpen ? 0u : binds::takeoverRecFlag());
        }

        const auto bak = *is_battle_ptr;
        *is_battle_ptr = 1;

        const auto gameKeyFlag = static_cast<GAMEKEY_FLAG>(system_red->m_BattleKey[pad]->m_PureKey);
        const auto newRecflag = orig_GameKeyToRecFlag(pad, gameKeyFlag, false);

        *is_battle_ptr = bak;
        return menuOpen ? static_cast<RECFLG_ENUM>(0) : newRecflag;
    }

    std::vector<uint16_t> captureReplayInputs(int peer, uint32_t fromEntry,
                                              uint32_t entryCount, uint32_t tailFrames) const
    {
        std::vector<uint16_t> out;
        const uint32_t* buf = peerBuffer(peer);
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
    std::vector<uint16_t> captureCurrentRound(int peer) const
    {
        std::vector<uint16_t> out;
        if (!gameState->checkModeReplay()) { COMBO_LOG("capture: not a replay"); return out; }
        ReplayHead head{};
        if (!peerReadHead(peer, head)) { COMBO_LOG("capture: no head"); return out; }

        const uint32_t from = g_round_anchor[peer];
        if (head.m_Cursor < from) { COMBO_LOG("capture: cursor behind anchor"); return out; }
        if (head.m_Cursor == from && head.m_Count == 0)
        {
            COMBO_LOG("capture: nothing since round start");
            return out;
        }
        return captureReplayInputs(peer, from, head.m_Cursor - from, head.m_Count);
    }

    void cancelCapture()
    {
        g_cap_active = false;
    }

    bool captureActive() const
    {
        return g_cap_active;
    }

    uint32_t captureFrames() const
    {
        return g_cap_frames;
    }

    bool captureFull() const
    {
        return g_cap_active && g_cap_frames >= recordings::kMaxFrames;
    }

    bool startCapture(int peer, std::wstring& err)
    {
        if (!gameState->checkModeReplay()) { err = L"Open a replay first"; return false; }
        ReplayHead h{};
        if (!peerReadHead(peer, h)) { err = L"The replay isn't ready yet"; return false; }
        g_cap_active = true;
        g_cap_peer = peer;
        g_cap_start = h;
        g_cap_frames = 0;
        return true;
    }

    std::vector<uint16_t> stopCapture(std::wstring& err)
    {
        std::vector<uint16_t> out;
        if (!g_cap_active) return out;
        g_cap_active = false;
        ReplayHead cur{};
        if (!peerReadHead(g_cap_peer, cur)) { err = L"The replay isn't ready"; return out; }
        if (cur.m_Cursor < g_cap_start.m_Cursor ||
            (cur.m_Cursor == g_cap_start.m_Cursor && cur.m_Count < g_cap_start.m_Count))
        {
            err = L"Replay rewound, capture stopped";
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
};

ReplayManager::ReplayManager()
    : data(std::make_unique<Data>())
{
}

ReplayManager::~ReplayManager() = default;

ReplayManager& ReplayManager::instance()
{
    static ReplayManager manager;
    return manager;
}

void ReplayManager::initialize(GameState& gameState, Keybinds& keybindings, Timeline& timeline)
{
    data->initialize(gameState, keybindings, timeline);
}

void ReplayManager::onMatchStart()
{
    data->seekEnabled = data->frameCounter != nullptr && ModMenu::instance().replayTimeline();
    ModMenu::instance().replayStarted();
    data->resetRewindDelay();
    data->cancelCapture();  // a new match or replay: the old capture start means nothing
    // New match (or a freshly loaded replay) -- let the next round call
    // re-anchor the capture range.
    data->g_anchor_set = false;
    data->reset();
    data->clearRing();
    data->diverged = false;
    data->assistSide = nullptr;
    if (!data->gameState->checkModeTraining()) data->rewindPointSet = false;
}

void ReplayManager::onRoundStart()
{
    data->stageCanReset = asw_stage_can_reset();
    data->lastStage = kNoStageArea;
    if (data->isAnalyzerSame(nullptr)) data->diverged = false;  // not taking over: the round starts as recorded
    data->clearRing();
    data->onRoundStart();
}

void ReplayManager::update()
{
    data->update();
}

void ReplayManager::onPreTick()
{
    data->onPreTick();
}

void ReplayManager::trialKey(int frames, int flags)
{
    data->trialKey(frames, flags);
}

uint64_t ReplayManager::trialCounterStart() const
{
    return data->trialCounterAtSave;
}

void ReplayManager::enableSeek(uint64_t* frameCounter)
{
    data->frameCounter = frameCounter;
    data->seekEnabled = frameCounter != nullptr && ModMenu::instance().replayTimeline();
}

void ReplayManager::setFrameCounter(uint64_t* frameCounter)
{
    data->frameCounter = frameCounter;
}

void ReplayManager::enableSeekLogs()
{
    data->seekVerbose = true;
}

bool ReplayManager::seekAvailable() const
{
    return data->seekEnabled;
}

bool ReplayManager::seekRange(int& first, int& last, int& now) const
{
    return data->seekRange(first, last, now);
}

void ReplayManager::seekTo(int position, bool quick)
{
    if (!data->seekEnabled || position < 0) return;
    data->seekWanted = static_cast<uint32_t>(position);
    data->seekWantedQuick = quick;
    data->seekWantedBeyond = false;
    data->seekDue = true;
}

void ReplayManager::stepBack()
{
    int first = 0, last = 0, now = 0;
    if (!data->seekRange(first, last, now)) return;
    const int from = data->seekDue ? static_cast<int>(data->seekWanted) : data->seekCatching ? static_cast<int>(data->seekTarget) : now;
    if (from > first) seekTo(from - 1, true);
}

void ReplayManager::stepTick(int direction)
{
    int first = 0, last = 0, now = 0;
    if (!data->seekRange(first, last, now)) return;
    uint32_t oldest = 0;
    const bool any = data->seekOldest(oldest);
    const int origin = static_cast<int>(data->haveSeekOrigin ? data->seekOrigin : any ? oldest : 0);
    const int from = data->seekDue ? static_cast<int>(data->seekWanted) : data->seekCatching ? static_cast<int>(data->seekTarget) : now;
    const int rel = from - origin;
    int to = -1;
    if (direction < 0)
    {
        if (rel > 0) to = (std::max)(origin + ((rel - 1) / 60) * 60, first);
        else if (from > first) to = first;
        if (to >= from) to = -1;
    }
    else to = origin + ((std::max)(rel, 0) / 60 + 1) * 60;
    if (to < 0) return;
    seekTo(to, false);
    data->seekWantedBeyond = direction > 0;
}

void ReplayManager::seekPoints(int& origin, std::vector<int>& calm) const
{
    data->seekPoints(origin, calm);
}

bool ReplayManager::takeSeekStarted()
{
    const bool started = data->seekStarted;
    data->seekStarted = false;
    return started;
}

int ReplayManager::seekAfterTick()
{
    return data->seekAfterTick();
}

void ReplayManager::seekBatchDone()
{
    data->seekBatchDone();
}

bool ReplayManager::seekGoing() const
{
    return data->seekCatching;
}

int ReplayManager::trialAfterTick()
{
    return data->trialAfterTick();
}

void ReplayManager::trialCatchUpDone(double ms)
{
    data->trialCatchUpDone(ms);
}

bool ReplayManager::isTakeoverInput(const asw_inputs* input) const
{
    return data->isAnalyzerSame(input);
}

std::optional<int> ReplayManager::takeoverPlayer() const
{
    if (!data->gameState || !data->gameState->checkModeReplay() || data->isAnalyzerSame(nullptr))
        return std::nullopt;

    auto* engine = asw_engine::get();
    if (!engine) return std::nullopt;
    if (data->isAnalyzerSame(&engine->inputs[0])) return 1;
    if (data->isAnalyzerSame(&engine->inputs[3])) return 2;
    return std::nullopt;
}

bool ReplayManager::playbackEnabled() const
{
    return data->isAnalyzerSame(nullptr);
}

RECFLG_ENUM ReplayManager::takeoverInput(RECFLG_ENUM input, bool menuOpen) const
{
    return data->takeoverInput(input, menuOpen);
}

void ReplayManager::setInputSystem(uintptr_t system)
{
    data->system_red = reinterpret_cast<AASystemRED*>(system);
}

void ReplayManager::onReplayFrame()
{
    if (data->g_cap_active)
        data->g_cap_frames++;  // one replay frame played
}

std::vector<uint16_t> ReplayManager::captureCurrentRound(int peer) const
{
    return data->captureCurrentRound(peer);
}

bool ReplayManager::startCapture(int peer, std::wstring& error)
{
    return data->startCapture(peer, error);
}

std::vector<uint16_t> ReplayManager::stopCapture(std::wstring& error)
{
    return data->stopCapture(error);
}

bool ReplayManager::captureActive() const
{
    return data->captureActive();
}

bool ReplayManager::captureFull() const
{
    return data->captureFull();
}

uint32_t ReplayManager::captureFrames() const
{
    return data->captureFrames();
}

void ReplayManager::onBattleUpdate()
{
    data->trackEffectClasses();
}

void ReplayManager::onBattleFrameProcessed()
{
    data->onBattleFrameProcessed();
}

int ReplayManager::assistState(float& remaining) const
{
    if (data->assistStandby)
    {
        const float total = static_cast<float>(data->assistTarget - data->assistFrom);
        const float left = static_cast<float>(data->assistTarget) - static_cast<float>(data->replayPosition());
        remaining = total > 0 ? (std::clamp)(left / total, 0.0f, 1.0f) : 0.0f;
        return 1;
    }
    if (data->assistActiveAt && GetTickCount64() - data->assistActiveAt < 1000 && !data->isAnalyzerSame(nullptr))
        return 2;
    return 0;
}

ReplayManager::Status ReplayManager::status() const
{
    Status s;
    if (!data->gameState) return s;
    s.replay = data->gameState->checkModeReplay();
    s.training = data->gameState->checkModeTraining();
    s.rewindPoint = data->rewindPointSet;
    s.rewindStale = data->rewindStale.load();
    const int rewindFrames = data->rewindFramesRemaining.load();
    // Round up so the banner never shows 0.0 before playback actually resumes.
    constexpr int framesPerTenth = Data::kRewindFramesPerSecond / 10;
    s.rewindLeftSeconds = ((rewindFrames + framesPerTenth - 1) / framesPerTenth) / 10.0f;
    if (data->rewindFramesTotal > 0)
        s.rewindFraction = (std::clamp)(static_cast<float>(rewindFrames) / data->rewindFramesTotal, 0.0f, 1.0f);
    if (const auto p = takeoverPlayer()) s.takeover = *p;
    s.assistSeconds = s.replay ? ModMenu::instance().replayAssistSeconds() : 0.0;
    float left = 0.0f;
    s.assist = s.replay ? assistState(left) : 0;
    s.assistFraction = left;
    if (s.assist == 1)
        s.assistLeftSeconds = static_cast<float>(static_cast<int>(data->assistTarget) - static_cast<int>(data->replayPosition())) / 60.0f;
    if (const auto* side = data->promptSide)
    {
        const auto* eng = asw_engine::get();
        s.prompt = eng && side == &eng->inputs[0] ? 1 : 2;
        s.promptStop = !data->promptStart;
        s.promptRewinds = s.promptStop && data->isTakeover;
    }
    return s;
}

bool ReplayManager::promptOpen() const
{
    return data->promptSide != nullptr;
}

bool ReplayManager::isRewindDelayActive() const
{
    return data->rewindFramesRemaining.load() > 0;
}

void ReplayManager::logStatus() const
{
    COMBO_LOG(data->orig_FxRelease1 ? "effect repair: found" : "effect repair: NOT found");
    COMBO_LOG(data->orig_DangerHide ? "danger sync: found" : "danger sync: NOT found");
    COMBO_LOG(data->orig_ActorReleaseB ? "actor repair: found" : "actor repair: NOT found");
    COMBO_LOG(data->orig_LinkBlueprintModel && data->orig_LinkCommonFxModel && data->orig_LinkModel
              && data->orig_LinkFxWithArgs && data->orig_LinkFx ? "link rebuild: found" : "link rebuild: NOT found");
}
