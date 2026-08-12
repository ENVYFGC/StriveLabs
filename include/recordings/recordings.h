#pragma once

// Load/save Eddienput-style .txt recordings via GGST's training slots.
// Layout verified against battle version 5.00 and RED source (game_Save.h).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace recordings {

// RECFLG_ENUM from obj_ScriptInc.h. RECFLG is U16.
enum InputFlag : uint16_t {
  IF_Up      = 0x0001, IF_Down = 0x0002, IF_Left = 0x0004, IF_Right = 0x0008,
  IF_P       = 0x0010, IF_K    = 0x0020, IF_S    = 0x0040, IF_H     = 0x0080,
  IF_Dust    = 0x0100,
  IF_Respect = 0x0200,
  IF_Dash    = 0x0400,   // dash macro
  IF_RC      = 0x0800,   // roman cancel macro
  IF_FD      = 0x8000,   // faultless defense

  // RECFLG_TOREMO: Play|Memory|Select. Training menu keys, not player input.
  IF_System  = 0x1000 | 0x2000 | 0x4000,
  IF_Any     = static_cast<uint16_t>(~IF_System),
};

// CMemorySlot, mirroring game_Save.h.
// MAX_MEMORY_TIME 2400 + TR_REC_START_TIME_MAX 300 = 2700 buffer.
constexpr uint32_t kMaxFrames   = 2400;
constexpr uint32_t kFlagBuffer  = 2700;
constexpr int      kSlotCount   = 5;      // user-visible slots; 8 exist (5 + 3 buffer)
constexpr int32_t  kDirRight    = 0;      // DIR_RIGHT
constexpr int32_t  kDirLeft     = 1;      // DIR_LEFT
constexpr int32_t  kEmptyMaxTime = -1;    // what CMemorySlotInit writes

#pragma pack(push, 1)
struct MemorySlot {
  uint8_t  chara_id;
  uint8_t  _pad[3];
  int32_t  direction;    // m_MemoryDirection
  int32_t  max_time;     // m_MemoryMaxTime;  -1 = empty, <=4 also treated as empty
  int32_t  start_time;   // m_MemoryStartTime; TR_REC_START_TIME_QUICK = 0
  uint16_t flags[kFlagBuffer];
};
#pragma pack(pop)
static_assert(sizeof(MemorySlot) == 5416, "MemorySlot size mismatch");

// offsetof(SSaveData, m_TrainingMemorySlot). The same offset the slots occupy
// in the decrypted SYSTEM.sav, so the on-disk layout mirrors the struct.
constexpr uint32_t kTrainingSlotOffset = 0x284D4;

// Live slot array, resolved through the game's accessor. nullptr if the
// signature failed (e.g. after a patch moves the getter).
MemorySlot* findSlots();

// Eddienput notation: W<n> wait, X tap, [X] hold, ]X[ release, + same frame,
// # comment. Loops expand; mixups are rejected (a slot holds one sequence).
struct ParseResult {
  std::vector<uint16_t> frames;
  std::wstring          error;      // empty on success
  bool ok() const { return error.empty(); }
};

// Slots store screen direction, so `side` decides how 4/6 map: P1 faces right.
ParseResult parseText(const std::string& text, int32_t side);
ParseResult parseFile(const std::filesystem::path& path, int32_t side);

// kKeepCharaId leaves the slot's existing m_CharaID alone.
constexpr uint8_t kKeepCharaId = 0xFF;

bool writeSlot(int slot_index, const std::vector<uint16_t>& frames,
               int32_t direction, uint8_t chara_id, std::wstring& error);

// Slot -> notation. Deliberately independent of GGST's save system, which only
// flushes slots on game exit; going through it would force a restart between
// recording and sharing. The .txt is the persistence layer instead.
bool exportSlot(int slot_index, const std::filesystem::path& path,
                std::wstring& error);

// Short slot summary for the menu.
std::wstring describeSlot(int slot_index);

// Self-describing filename stem, e.g. "110f_6_6_2S_2H". Empty if slot is empty.
std::string suggestName(int slot_index);

// ---- files -----------------------------------------------------------------
std::vector<std::filesystem::path> scanDir(const std::filesystem::path& dir);

}  // namespace recordings
