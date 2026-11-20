#include "combo_trials.h"
#include "combo_online.h"
#include "combo_log.h"
#include "handshake.h"
#include "sigscan.h"
#include "bake_core.h"
#include "pak_mount.h"

#include <Unreal/FMemory.hpp>
#include <polyhook2/Detour/x64Detour.hpp>
#include "arcsys.h"  // after polyhook: the game structs' macros upset it
#include <atomic>
#include <fstream>
#include <memory>
#include <algorithm>
#include <vector>
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <cctype>
#include <cwchar>
#include <map>
#include <mutex>
#include <string>

// Found in the running game (training mode, Combo Maker and their save data):
//  - Training's Combo Recipe is one native object. It holds the steps the game
//    checks your moves against: per step a move name (32 chars) and a condition
//    (performed, hit, ...), plus a name and condition for the row's text. As moves
//    match, a progress list grows; a drop clears it.
//  - Combo Maker keeps its combos in the save data, each in a 0x5C30-byte block
//    with the very same per-step fields, which is what the game's own Load copies.
//  - Refreshing the recipe redraws the trial panel and resets progress.
namespace combo_trials {
void placeAsSet();
namespace {
  struct TArr { void* data; int32_t num; int32_t max; };

  uintptr_t g_recipe = 0;            // the recipe object
  uintptr_t* g_save_root = nullptr;  // points to the save data
  uintptr_t* g_chara_owner = nullptr;
  using GetBase_t = uintptr_t (*)();
  GetBase_t g_chara_base = nullptr;
  using Refresh_t = void (*)(void* recipe);
  Refresh_t g_refresh = nullptr;
  // Puts Starting Position, Player Side and Starting Range from the settings into
  // training's state (a reset alone doesn't redo Player Side). Same object.
  Refresh_t g_pos_side = nullptr;
  constexpr size_t tResetRequest = 0x1EBA;  // byte: training resets positions next frame

  // Combo Maker lists in the save data: 10 of your own, 100 kept from searches.
  struct ListDef { uintptr_t offset; int slots; };
  constexpr ListDef kLists[] = {{0x1758CE0, 10}, {0x17926C0, 100}, {0x1C12C40, 500}};
  constexpr int kCreatedList = 2;
  constexpr size_t kBlock = 0x5C30;
  constexpr int kMaxSteps = 64, kPanelRows = 32;

  // In a Combo Maker block
  constexpr size_t kNumber = 0x08, kCreator = 0x2A, kTitle = 0x86, kVersion = 0xF0, kChara = 0xF4,
                   kMatchCond = 0x148, kMatchName = 0x248, kShowName = 0xA48, kShowCond = 0x1288;
  // In the recipe object
  constexpr size_t rMatchName = 0x2558, rMatchCond = 0x2568, rShowName = 0x2578, rProgress = 0x2588,
                   rFlag = 0x2598, rShowCond = 0x25A8, rDone = 0x25B8, rStamp = 0x25C8, rSeparator = 0x25D0,
                   rVersion = 0x25D8;

  template <class T> T& at(uintptr_t a) { return *reinterpret_cast<T*>(a); }

  // Training's settings: 128 ints by id in the save data, the same layout a combo
  // keeps its start settings in. These stay the player's own while a combo is tried:
  // menu and display choices, the recording slot, and the two that turn the Combo
  // Recipe display and its checking on.
  constexpr size_t kSettings = 0x282D4, kComboSettings = 0x12C8;
  // The dummy's per-character block: its counter-attack settings and recordings. A
  // combo keeps the one for its dummy character; training keeps one per character.
  constexpr size_t kDummyBlocks = 0x66D930, kComboDummy = 0x29F0, kDummySize = 0x3240, kDummyChara = 0xF8;
  constexpr int kKeepIds[] = {0x17, 0x18, 0x19, 0x1C, 0x1D, 0x1E, 0x23, 0x24, 0x2F, 0x30, 0x3B, 0x56, 0x57};
  bool kept(int id) { return std::find(std::begin(kKeepIds), std::end(kKeepIds), id) != std::end(kKeepIds); }

  // The player's own values, and what the trial put in their place. Both also go to a
  // file: the game may write the trial's values to disk and not save again before a
  // quit, so at the next start the file tells them apart and the player's come back.
  // The file stays until a start finds the trial's values gone from the save.
  struct Backup {
    uint32_t magic = 0x31544C53;  // "SLT1"
    int32_t settings[128];
    int32_t dummy_chara = -1;
    uint8_t dummy[kDummySize];
    int32_t applied_settings[128];
    uint8_t applied_dummy[kDummySize];
  };
  Backup g_bk;
  bool g_applied = false, g_checked_file = false;
  // The loaded combo's recording, at block +0x14C8 like a training slot: chara, facing
  // (1 = left), frame count, start, then one input word per frame from the reset on,
  // screen directions. The system keys (training's reset/record/playback) are left out.
  constexpr size_t kRecDir = 0x14CC, kRecFrames = 0x14D0, kRecFlags = 0x14D8;
  constexpr int kRecMax = 2700;
  std::vector<uint16_t> g_demo;
  bool g_demo_left = false, g_watch_flip = false;
  int g_trial_version = 0;
  std::wstring g_trial_name;
  enum class Watch { Off, Armed, On } g_watch = Watch::Off;
  size_t g_watch_pos = 0;
  std::filesystem::path g_backup_file;

  void saveBackupFile() {
    std::ofstream f(g_backup_file, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(&g_bk), sizeof(g_bk));
  }
  constexpr int kMaxChara = 64;  // well past the roster; only guards a damaged file
  uintptr_t dummyBlock(int chara) { return *g_save_root + kDummyBlocks + static_cast<size_t>(chara) * kDummySize; }
  bool dummyValid(int chara) { return chara >= 0 && chara < kMaxChara; }

  constexpr int kStreamIds[2] = {0x20, 0x21};
  struct StreamBackup {
    uint32_t magic = 0x31535453;
    int32_t own[2] = {};
    int32_t set[2] = {};
  };
  StreamBackup g_stream_bk;
  int32_t g_stream_prev[2] = {};
  bool g_stream_setup = false, g_stream_pending = false;
  std::filesystem::path g_stream_file;
  bool streamId(int id) { return id == kStreamIds[0] || id == kStreamIds[1]; }

  void saveStreamFile() {
    std::ofstream f(g_stream_file, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(&g_stream_bk), sizeof(g_stream_bk));
  }
  int putBackStream(const StreamBackup& b) {
    const uintptr_t settings = *g_save_root + kSettings;
    int n = 0;
    for (int k = 0; k < 2; k++) {
      int32_t& now = at<int32_t>(settings + kStreamIds[k] * 4);
      if (b.own[k] == b.set[k] || now != b.set[k]) continue;
      now = b.own[k];
      n++;
    }
    return n;
  }
  void watchStreamFile() {
    if (!g_stream_pending || g_stream_setup || !g_save_root || !*g_save_root) return;
    const uintptr_t settings = *g_save_root + kSettings;
    bool changed = false, left = false;
    for (int k = 0; k < 2; k++) {
      if (g_stream_bk.own[k] == g_stream_bk.set[k]) continue;
      if (at<int32_t>(settings + kStreamIds[k] * 4) == g_stream_bk.set[k]) { g_stream_bk.own[k] = g_stream_bk.set[k]; changed = true; }
      else left = true;
    }
    if (!left) {
      g_stream_pending = false;
      std::error_code ec;
      std::filesystem::remove(g_stream_file, ec);
    } else if (changed) {
      saveStreamFile();
    }
  }

  // Puts back the player's value for each setting the trial changed, if it still holds
  // the trial's value (one changed since is the player's own choice and stays), and the
  // same for the dummy's block. Returns how many were put back.
  int putBack(const Backup& b) {
    const uintptr_t settings = *g_save_root + kSettings;
    int n = 0;
    for (int id = 0; id < 128; id++) {
      if (kept(id) || b.applied_settings[id] == b.settings[id]) continue;
      if (g_stream_setup && streamId(id)) continue;
      if (at<int32_t>(settings + id * 4) != b.applied_settings[id]) continue;
      at<int32_t>(settings + id * 4) = b.settings[id];
      n++;
    }
    if (dummyValid(b.dummy_chara) && std::memcmp(b.applied_dummy, b.dummy, kDummySize) != 0 &&
        std::memcmp(reinterpret_cast<void*>(dummyBlock(b.dummy_chara)), b.applied_dummy, kDummySize) == 0) {
      std::memcpy(reinterpret_cast<void*>(dummyBlock(b.dummy_chara)), b.dummy, kDummySize);
      n++;
    }
    return n;
  }
  void restoreSettings() {
    if (!g_save_root || !*g_save_root) return;
    putBack(g_bk);
    g_applied = false;
    g_watch = Watch::Off;
    COMBO_LOG("combo trials: training settings put back");
  }

  // The game asks one function which battle version it runs on: the current one,
  // except in replays and Combo Maker. Everything versioned follows its answer:
  // character and common scripts, hitboxes, data, and the rules in the game's code.
  using GetVersion_t = int (*)();
  GetVersion_t orig_GetVersion = nullptr;
  uintptr_t* g_common = nullptr;  // the game's common object; its +0x2F0 is the game mode
  // Picked in the menu, and the one in use: it only changes while no battle runs, so a
  // fight never mixes one version's loaded scripts with another's rules.
  std::atomic<int> g_training_version{0}, g_version_in_use{0};
  std::atomic<bool> g_stock{false};
  // Online lobbies, as the game's common object has them: its lobby field is 3 in the Park,
  // 2 in a private room and 5 out of them (the main menu, where ranked is queued), and the
  // online lobby flag next to it is set in both lobbies (not while ranked is queued).
  std::atomic<int> g_online_version{0}, g_online_in_use{0};
  bool g_was_in_lobby = false;
  unsigned long long g_lobby_since = 0;  // when the lobby was first seen, 0 = not in one
  uint8_t gameMode() { return g_common && *g_common ? at<uint8_t>(*g_common + 0x2F0) : 0; }
  int lobbyField() { return g_common && *g_common ? at<int32_t>(*g_common + 0xAD8) : 0; }
  bool inOnlineLobby() {
    const int t = lobbyField();
    return (t == 2 || t == 3) && at<int32_t>(*g_common + 0xB68) == 1;
  }

  int versionAsked() {
    const int v = orig_GetVersion();
    const int o = g_version_in_use.load(std::memory_order_relaxed);
    if (o > 0 && g_common && *g_common && at<uint8_t>(*g_common + 0x2F0) == 8) {  // training
      static int logged = 0;
      if (logged != o) { logged = o; COMBO_LOG("combo trials: training runs on battle version " + std::to_string(o)); }
      return o;
    }
    // Online: what was taken up in a Park or room lobby between fights. Never in training
    // (mode 8) or replays (23), which have their own.
    if (const int on = g_online_in_use.load(std::memory_order_relaxed); on > 0) {
      const uint8_t m = gameMode();
      if (m != 8 && m != 23 && m != 0x1A) return on;
    }
    return v;
  }
  struct GameStr { const wchar_t* data; int32_t num, max; };
  thread_local int t_today = 0;
  std::atomic<bool> g_keep_today{true};
  std::mutex g_today_m;
  std::vector<std::string> g_today_chars, g_today_served, g_today_asked;
  bool g_today_hooks = false;
  std::string chrCode(const GameStr* s) {
    std::string c;
    if (!s || !s->data || s->num <= 0 || s->num > 64) return c;
    for (int i = 0; i < s->num && s->data[i]; i++) {
      const wchar_t w = s->data[i];
      if (w > 127) return {};
      c += static_cast<char>(std::toupper(static_cast<unsigned char>(w)));
    }
    return c;
  }
  constexpr uint8_t kFirst[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // SOL .. GIO
                                2, 2,                                     // ANJ, INO: from 1.01
                                4, 5, 7, 8, 10,                           // GLD, JKO, COS, BKN, TST
                                13, 14, 16, 17,                           // BGT, SIN, BED, ASK
                                18, 20, 22, 23,                           // JHN, ELP, ABA, SLY
                                26, 29, 31, 33,                           // DZY, VEN, UNI, LUC
                                36,                                       // USG
                                38};                                      // ABS
  constexpr const char* kFirstCodes[] = {"SOL", "KYK", "MAY", "AXL", "CHP", "POT", "FAU", "MLL", "ZAT", "RAM", "LEO", "NAG", "GIO",
                                         "ANJ", "INO", "GLD", "JKO", "COS", "BKN", "TST", "BGT", "SIN", "BED", "ASK",
                                         "JHN", "ELP", "ABA", "SLY", "DZY", "VEN", "UNI", "LUC", "USG", "ABS"};
  static_assert(std::size(kFirst) == std::size(kFirstCodes));
  int firstVersionOf(const std::string& code) {
    for (size_t i = 0; i < std::size(kFirstCodes); i++)
      if (code == kFirstCodes[i]) return kFirst[i];
    return 0;
  }
  bool keepToday(const GameStr* name) {
    const int in_use = g_version_in_use.load(std::memory_order_relaxed);
    if (in_use <= 0 || gameMode() != 8) return false;
    const std::string c = chrCode(name);
    if (c.empty()) return false;
    if (firstVersionOf(c) > in_use) {
      bool first = false;
      {
        std::lock_guard<std::mutex> lk(g_today_m);
        if (g_today_asked.size() < 48 && std::find(g_today_asked.begin(), g_today_asked.end(), c) == g_today_asked.end()) { g_today_asked.push_back(c); first = true; }
      }
      if (first) COMBO_LOG("battle version: " + c + " wasn't in the game on version " + std::to_string(in_use) + ", so it runs on today's script and box files");
      return true;
    }
    if (!g_keep_today.load(std::memory_order_relaxed) || g_stock.load(std::memory_order_relaxed)) return false;
    bool first = false, fresh = false;
    {
      std::lock_guard<std::mutex> lk(g_today_m);
      if (g_today_asked.size() < 48 && std::find(g_today_asked.begin(), g_today_asked.end(), c) == g_today_asked.end()) { g_today_asked.push_back(c); fresh = true; }
    }
    if (fresh) COMBO_LOG("battle version: the game asked for the files of " + c + " on an old version");
    {
      std::lock_guard<std::mutex> lk(g_today_m);
      if (std::find(g_today_chars.begin(), g_today_chars.end(), c) == g_today_chars.end()) return false;
      if (std::find(g_today_served.begin(), g_today_served.end(), c) == g_today_served.end()) { g_today_served.push_back(c); first = true; }
    }
    if (first) COMBO_LOG("battle version: " + c + " has edits loaded, so it keeps today's script and box files; everything else runs on version " + std::to_string(g_version_in_use.load()));
    return true;
  }
  struct Today {
    bool on;
    explicit Today(const GameStr* name) : on(keepToday(name)) { if (on) t_today++; }
    ~Today() { if (on) t_today--; }
  };
  std::atomic<unsigned> g_load_no{0};
  struct FilesSeen { unsigned load = 0; int in_use = 0; int state = -1; };
  std::map<std::string, FilesSeen> g_files_seen;
  void noteFiles(const GameStr* name) {
    if (gameMode() != 8) return;
    const std::string c = chrCode(name);
    if (c.empty()) return;
    const int in_use = g_version_in_use.load(std::memory_order_relaxed);
    const int state = t_today > 0 || in_use <= 0 ? 1 : 0;
    const unsigned load = g_load_no.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_today_m);
    const auto it = g_files_seen.find(c);
    if (it == g_files_seen.end() || it->second.load != load || it->second.in_use != in_use) { g_files_seen[c] = FilesSeen{load, in_use, state}; return; }
    if (it->second.state != state) it->second.state = -1;
  }
  using Path2_t = void* (*)(void*, const GameStr*);
  using Path4_t = void* (*)(void*, const GameStr*, bool, int);
  using PathList_t = void* (*)(void*, void*, void*, void*);
  Path2_t orig_PathAdv = nullptr, orig_PathCol = nullptr;
  Path4_t orig_PathBbs = nullptr;
  PathList_t orig_PathList = nullptr;
  void* hook_PathAdv(void* out, const GameStr* name) { Today t(name); return orig_PathAdv(out, name); }
  void* hook_PathCol(void* out, const GameStr* name) { Today t(name); return orig_PathCol(out, name); }
  void* hook_PathBbs(void* out, const GameStr* name, bool ef, int kind) {
    Today t(name);
    noteFiles(name);
    return orig_PathBbs(out, name, ef, kind);
  }
  void* hook_PathList(void* self, void* a, void* b, void* c) {
    Today t(self ? reinterpret_cast<const GameStr*>(static_cast<const uint8_t*>(self) + 8) : nullptr);
    return orig_PathList(self, a, b, c);
  }
  bool startsWith(uintptr_t at, const char* bytes, const char* mask) {
    const auto* p = reinterpret_cast<const uint8_t*>(at);
    for (size_t i = 0; mask[i]; i++)
      if (mask[i] == 'x' && p[i] != static_cast<uint8_t>(bytes[i])) return false;
    return true;
  }
  bool findPathBuilders(const uint8_t* base, uintptr_t& adv, uintptr_t& col, uintptr_t& bbs, uintptr_t& list) {
    static const wchar_t* const kNames[4] = {L"%s/%s/Common/Data/%sADV_%s%s.ADV_%s%s", L"%s/%s/Common/Data/%sCOL_%s%s.COL_%s%s", L"%s/%s/Common/Data/%sBBS_%s%s%s.BBS_%s%s%s",
                                             L"%s/%s/Common/Data/%sBBS_%sEF%s%s.BBS_%sEF%s%s"};
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    const uint8_t* text[4] = {};
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
      if ((sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) || !(sec[i].Characteristics & IMAGE_SCN_MEM_READ) || (sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
      const uint8_t* lo = base + sec[i].VirtualAddress;
      const uint8_t* hi = lo + sec[i].Misc.VirtualSize;
      for (int k = 0; k < 4; k++) {
        if (text[k]) continue;
        const auto* w = reinterpret_cast<const uint8_t*>(kNames[k]);
        const size_t n = (std::wcslen(kNames[k]) + 1) * sizeof(wchar_t);
        const uint8_t* at = std::search(lo, hi, w, w + n);
        if (at != hi) text[k] = at;
      }
    }
    for (const uint8_t* t : text) if (!t) return false;
    std::vector<uintptr_t> in[4];
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
      if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) || sec[i].Misc.VirtualSize < 64) continue;
      const uint8_t* lo = base + sec[i].VirtualAddress + 16;
      const uint8_t* hi = base + sec[i].VirtualAddress + sec[i].Misc.VirtualSize - 16;
      for (const uint8_t* p = lo; p < hi; p++) {
        p = static_cast<const uint8_t*>(std::memchr(p, 0x8D, static_cast<size_t>(hi - p)));
        if (!p) break;
        if ((p[1] & 0xC7) != 0x05 || (p[-1] != 0x48 && p[-1] != 0x4C)) continue;
        int32_t rel = 0;
        std::memcpy(&rel, p + 2, 4);
        const uint8_t* to = p + 6 + rel;
        for (int k = 0; k < 4; k++) {
          if (to != text[k]) continue;
          const uint8_t* f = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(p) & ~static_cast<uintptr_t>(15));
          int steps = 0;
          while (f > lo && steps < 0x400 && !(f[-2] == 0xCC && f[-1] == 0xCC && f[0] != 0xCC)) { f -= 16; steps++; }
          if (f <= lo || steps >= 0x400) return false;
          const uintptr_t fn = reinterpret_cast<uintptr_t>(f);
          if (std::find(in[k].begin(), in[k].end(), fn) == in[k].end()) in[k].push_back(fn);
        }
      }
    }
    for (const auto& v : in) if (v.size() != 2) return false;
    list = 0;
    for (uintptr_t f : in[0])
      if (std::find(in[1].begin(), in[1].end(), f) != in[1].end() && std::find(in[2].begin(), in[2].end(), f) != in[2].end() && std::find(in[3].begin(), in[3].end(), f) != in[3].end()) list = f;
    if (!list) return false;
    const auto other = [&](const std::vector<uintptr_t>& v) { return v[0] == list ? v[1] : v[0]; };
    adv = other(in[0]);
    col = other(in[1]);
    bbs = other(in[2]);
    if (bbs != other(in[3]) || adv == col || adv == bbs || col == bbs || adv == list || col == list || bbs == list) return false;
    static const char kTwo[] = "\x48\x89\x5C\x24\x08\x48\x89\x74\x24\x10\x57\x48\x83\xEC\x70\x48\x8B\x1D\x00\x00\x00\x00\x48\x8B\xFA\x48\x8B\xF1";
    static const char kTwoMask[] = "xxxxxxxxxxxxxxxxxx????xxxxxx";
    static const char kFour[] = "\x48\x89\x5C\x24\x08\x48\x89\x6C\x24\x10\x48\x89\x74\x24\x18\x48\x89\x7C\x24\x20\x41\x56\x48\x81\xEC\x80\x00\x00\x00\x48\x8B\x1D\x00\x00\x00\x00\x48\x8B\xF2\x48\x8B\xE9\x4D\x63\xF1";
    static const char kFourMask[] = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxx";
    static const char kList[] = "\x48\x89\x5C\x24\x18\x48\x89\x4C\x24\x08\x55\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x8D\x6C\x24\xD9\x48\x81\xEC\xE0\x00\x00\x00\x4C\x8B\xE2\x48\x8B\xF9\x48\x8D\x55\xB7\xE8\x00\x00\x00\x00\x48\x63\x77\x10\x45\x33\xD2\x48\x8B\x5F\x08";
    static const char kListMask[] = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxx";
    return startsWith(adv, kTwo, kTwoMask) && startsWith(col, kTwo, kTwoMask) && startsWith(bbs, kFour, kFourMask) && startsWith(list, kList, kListMask);
  }
  constexpr int kLastSeason4 = 35, kBeforeRiscRework = 32;
  constexpr size_t kRiscChecks = 8;
  std::atomic<int> g_old_risc{0};
  const void* g_risc_from[2] = {};
  const void* g_risc407_from[kRiscChecks] = {};
  bool g_risc407 = false;
  uintptr_t g_version_fn = 0;
  int hook_GetVersion() {
    if (t_today > 0) return orig_GetVersion();
    const int v = versionAsked();
    if (v > kLastSeason4) {
      const int mode = g_old_risc.load(std::memory_order_relaxed);
      if (mode && gameMode() == 8) {
        const void* from = _ReturnAddress();
        if (from && (from == g_risc_from[0] || from == g_risc_from[1])) {
          static bool logged = false;
          if (!logged) { logged = true; COMBO_LOG("battle rules: counter hit and max RISC damage as in 4.09 (training)"); }
          return kLastSeason4;
        }
        if (from && mode == 2)
          for (const void* f : g_risc407_from)
            if (from == f) {
              static bool logged = false;
              if (!logged) { logged = true; COMBO_LOG("battle rules: RISC gauge rules as in 4.06 (training)"); }
              return kBeforeRiscRework;
            }
      }
    }
    return v;
  }
  size_t findRiscChecks(const uint8_t* base, uintptr_t fn, const void** out, size_t room) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    static const uint8_t cmp33[3] = {0x83, 0xF8, 0x21};
    static const uint8_t gauge[4] = {0xA4, 0xCA, 0x00, 0x00}, scale[4] = {0xA8, 0xCA, 0x00, 0x00};
    const auto has = [](const uint8_t* p, size_t n, const uint8_t* what, size_t m) { return std::search(p, p + n, what, what + m) != p + n; };
    size_t found = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
      if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) || sec[i].Misc.VirtualSize < 64) continue;
      const uint8_t* lo = base + sec[i].VirtualAddress + 8;
      const uint8_t* hi = base + sec[i].VirtualAddress + sec[i].Misc.VirtualSize - 48;
      for (const uint8_t* p = lo; p < hi; p++) {
        p = static_cast<const uint8_t*>(std::memchr(p, 0xE8, static_cast<size_t>(hi - p)));
        if (!p) break;
        int32_t rel = 0;
        std::memcpy(&rel, p + 1, 4);
        if (reinterpret_cast<uintptr_t>(p) + 5 + rel != fn) continue;
        if (!has(p + 5, 14, cmp33, 3)) continue;
        if (!has(p - 8, 8, gauge, 4) && !has(p - 8, 8, scale, 4) && !has(p + 5, 40, gauge, 4) && !has(p + 5, 40, scale, 4)) continue;
        if (found < room) out[found] = p + 5;
        found++;
      }
    }
    return found;
  }

  // Scripts ask the game for spots by number. Up to 2.04, 163-165 were a spot in front
  // of the character, one behind it, and the screen's center; since 2.05 those are
  // 166-168, and 163-165 are points on the character's skeleton. The game doesn't map
  // the old numbers, so 2.04 and older scripts (Nagoriyuki, Leo, a common effect) would
  // get the wrong spot, in training, replays and Combo Maker alike.
  using Position_t = int (*)(void*, int, void*, void*, void*);
  Position_t orig_Position = nullptr;
  constexpr int kLastOldPositions = 15;  // battle version 2.04
  int hook_Position(void* self, int id, void* a, void* b, void* c) {
    if (id >= 163 && id <= 165 && orig_GetVersion) {
      const int v = versionAsked();
      if (v > 0 && v <= kLastOldPositions) id += 3;
    }
    return orig_Position(self, id, a, b, c);
  }

  std::wstring wide(uintptr_t p, size_t max_chars) {
    std::wstring s;
    for (size_t i = 0; i < max_chars; i++) {
      const wchar_t c = at<wchar_t>(p + i * 2);
      if (!c) break;
      s += c;
    }
    return s;
  }

  // The player's character, or the other side's (the dummy in training).
  int charaAt(bool other_side) {
    // The fight's engine. The game's own getter (g_chara_base) is the same lookup but
    // reads through null when no fight is there, so it's done here with the check.
    if (!g_chara_owner || !*g_chara_owner || !g_chara_base) return -1;
    const uint8_t side = at<uint8_t>(*g_chara_owner + 0x2F4) ^ (other_side ? 1 : 0);
    const uintptr_t b = reinterpret_cast<uintptr_t>(asw_engine::get());
    if (!b) return -1;
    return at<uint8_t>(b + side * 0xADB80 + 0x274DDE);
  }
  int charaPlayed() { return charaAt(false); }

  int stepsIn(uintptr_t block) {
    int n = 0;
    while (n < kMaxSteps && at<char>(block + kMatchName + n * 32)) n++;
    return n;
  }

  // Room for n elements, with the game's own allocator, then n in use.
  void resize(uintptr_t arr, int n, size_t elem) {
    auto& a = at<TArr>(arr);
    if (a.max < n) {
      const int cap = (std::max)(n, 64);
      a.data = (*RC::Unreal::GMalloc)->Realloc(a.data, cap * elem);
      a.max = cap;
    }
    a.num = n;
  }
}

namespace {
  using ReqCtor_t = void* (*)(void*);
  using SideFill_t = void (*)(void*, int, int);
  using Submit_t = bool (*)(void*);
  ReqCtor_t g_req_ctor = nullptr;
  SideFill_t g_side_fill = nullptr;
  Submit_t g_submit = nullptr;
  std::vector<uint8_t> g_pending;
  unsigned long long g_pending_at = 0;
  std::filesystem::path g_stock_dir;
  int g_stock_order = 500000;
  std::vector<bake::PakFile> g_shadowed;
  struct Before { bool valid = false; int opponent = -1, version = 0; uint8_t stage = 0; uint32_t bgm = 0; uint8_t color[2] = {}; } g_before;
  using Getter_t = void* (*)();
  using Select_t = void (*)(void*, int, int, bool);
  using Bgm_t = int (*)(int, int);
  using Owns_t = bool (*)(int);
  Getter_t g_cm_state = nullptr, g_cm_temp = nullptr;
  Select_t g_cm_select = nullptr;
  Bgm_t g_cm_bgm = nullptr;
  Owns_t g_owns = nullptr;
  bool g_native = false, g_native_seen = false, g_create = false;
  unsigned long long g_native_at = 0;
  uintptr_t g_cm_st = 0;
  int g_mode_want = -1;
  unsigned long long g_mode_at = 0;
  void modeLater(int mode) { g_mode_want = mode; g_mode_at = GetTickCount64(); }
  int stockOn(const std::vector<std::string>& chrs);
  bool stockOff();

  void dropFallback() {
    if (g_native) return;
    g_pending.clear();
    g_stock = false;
    stockOff();
    if (g_before.valid && orig_GetVersion) g_training_version = g_before.version;
    g_before = Before{};
  }
  void describe(uintptr_t b, int n);

  Before nowSetup() {
    Before s;
    const uintptr_t gc = *g_common;
    s.valid = true;
    s.opponent = charaAt(true);
    s.version = g_training_version.load();
    s.stage = at<uint8_t>(gc + 0x370);
    s.bgm = at<uint32_t>(gc + 0x37C);
    for (int i = 0; i < 2; i++)
      if (const uintptr_t e = at<uintptr_t>(gc + 0x4D0 + static_cast<size_t>(i) * 8)) s.color[i] = at<uint8_t>(e + 0x2A);
    return s;
  }

  bool mountTrialPak(const std::vector<bake::PakFile>& files, const char* what) {
    std::error_code ec;
    std::filesystem::create_directories(g_stock_dir, ec);
    const int order = ++g_stock_order;
    const std::filesystem::path pak = g_stock_dir / ("trial_" + std::to_string(order) + ".pak");
    std::string why;
    if (!bake::writePak(pak.wstring(), files, why) || !bake::checkPak(pak.wstring(), files, why)) {
      std::filesystem::remove(pak, ec);
      COMBO_LOG(std::string("combo trials: ") + what + ": the pak couldn't be written: " + why);
      return false;
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::filesystem::copy_file(std::filesystem::path(exe).parent_path().parent_path().parent_path() / "Content" / "Paks" / "pakchunk0-WindowsNoEditor.sig",
                               std::filesystem::path(pak).replace_extension(".sig"), std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      COMBO_LOG(std::string("combo trials: ") + what + ": the game's .sig couldn't be copied");
      return false;
    }
    std::wstring w = pak.wstring();
    for (wchar_t& c : w) if (c == L'\\') c = L'/';
    const bool ok = pak_mount::mount(w, order);
    std::string names;
    for (const bake::PakFile& f : files) names += (names.empty() ? "" : ", ") + f.name.substr(f.name.rfind('/') + 1);
    COMBO_LOG(std::string("combo trials: ") + what + (ok ? ": mounted at order " : ": NOT mounted, order ") + std::to_string(order) + ": " + names);
    return ok;
  }

  int stockOn(const std::vector<std::string>& chrs) {
    if (!pak_mount::found()) return 0;
    std::vector<bake::PakFile> stock, modded;
    int unread = 0;
    for (const std::string& c : chrs) {
      if (c.empty() || std::count(chrs.begin(), chrs.end(), c) > 1 && &c != &*std::find(chrs.begin(), chrs.end(), c)) continue;
      const std::string rel = "RED/Content/Chara/" + c + "/Common/Data";
      const std::wstring dir = L"../../../" + std::wstring(rel.begin(), rel.end());
      std::vector<std::wstring> files;
      if (!pak_mount::listDir(dir, files)) continue;
      for (std::wstring f : files) {
        for (wchar_t& ch : f) if (ch == L'\\') ch = L'/';
        const std::wstring leaf = f.substr(f.rfind(L'/') == std::wstring::npos ? 0 : f.rfind(L'/') + 1);
        const size_t dot = leaf.rfind(L'.');
        if (dot == std::wstring::npos) continue;
        const std::wstring ext = leaf.substr(dot);
        if (ext != L".uasset" && ext != L".uexp") continue;
        const std::wstring full = dir + L"/" + leaf;
        std::vector<uint8_t> all, own;
        if (!pak_mount::readFile(full, all)) continue;
        if (!pak_mount::readGameFile(full, own)) { unread++; continue; }
        if (all == own) continue;
        std::string name = rel + "/";
        for (wchar_t ch : leaf) name += static_cast<char>(ch);
        stock.push_back({name, std::move(own)});
        modded.push_back({name, std::move(all)});
      }
    }
    if (unread) COMBO_LOG("combo trials: " + std::to_string(unread) + " files couldn't be read from the game's own paks, so they weren't compared");
    if (stock.empty()) return 0;
    if (!mountTrialPak(stock, "the game's own files over a mod pak's")) return -1;
    for (bake::PakFile& m : modded)
      if (std::find_if(g_shadowed.begin(), g_shadowed.end(), [&](const bake::PakFile& s) { return s.name == m.name; }) == g_shadowed.end()) g_shadowed.push_back(std::move(m));
    return static_cast<int>(stock.size());
  }

  bool stockOff() {
    if (g_shadowed.empty()) return false;
    const bool ok = mountTrialPak(g_shadowed, "the mod pak's files back");
    if (ok) g_shadowed.clear();
    return ok;
  }

  bool reloadWith(int opponent) {
    if (!g_req_ctor || !g_side_fill || !g_submit || !g_common || !*g_common || gameMode() != 8) return false;
    alignas(16) uint8_t req[0x100] = {};
    g_req_ctor(req);
    uintptr_t gc = *g_common;
    struct { uintptr_t* gc; void* req; } ctx{&gc, req};
    const int me = at<uint8_t>(gc + 0x2F4) & 1;
    const uintptr_t entry = at<uintptr_t>(gc + 0x4D0 + static_cast<size_t>(me) * 8);
    if (!entry) return false;
    const int mine = at<uint8_t>(entry + 0x29);
    g_side_fill(&ctx, me, mine);
    g_side_fill(&ctx, me ^ 1, opponent);
    const uintptr_t r = reinterpret_cast<uintptr_t>(req);
    int32_t& my_color = at<int32_t>(r + 0xC + static_cast<size_t>(me) * 0x48 + 4);
    int32_t& their_color = at<int32_t>(r + 0xC + static_cast<size_t>(me ^ 1) * 0x48 + 4);
    if (opponent == mine && their_color == my_color) their_color = my_color == 0 ? 1 : 0;
    at<int32_t>(r + 8) = 0x25;
    const bool queued = g_submit(req);
    if (void* text = at<void*>(r + 0xA8)) (*RC::Unreal::GMalloc)->Free(text);
    COMBO_LOG("combo trials: training reload asked with opponent " + std::to_string(opponent) + (queued ? "" : ": the game refused it"));
    return queued;
  }

  bool nativeReady() { return g_cm_state && g_cm_temp && g_cm_select && g_cm_bgm && g_req_ctor && g_side_fill && g_submit && g_common && *g_common; }

  std::wstring nativeStart(uintptr_t b, int list, int idx, bool& reloading) {
    if (g_native && !g_native_seen && GetTickCount64() - g_native_at < 20000) return L"The fight is still loading";
    const int n = stepsIn(b);
    const int mine = static_cast<int>(at<uint32_t>(b + kChara)), opp = static_cast<int>(at<uint32_t>(b + kDummyChara));
    const int made_on = static_cast<int>(at<uint32_t>(b + kVersion));
    if (!dummyValid(mine) || !dummyValid(opp)) return L"That combo's data doesn't look right";
    if (orig_GetVersion && gameMode() != 0x1A && made_on > orig_GetVersion()) return L"That combo was made on a newer version of the game";
    if (g_owns && (!g_owns(mine) || !g_owns(opp))) return L"That combo needs a character you don't own";
    const uintptr_t gc = *g_common;
    const int me = at<uint8_t>(gc + 0x2F4) & 1;
    const uintptr_t slots[2] = {at<uintptr_t>(gc + 0x4D0), at<uintptr_t>(gc + 0x4D8)};
    if (!slots[0] || !slots[1]) return L"The fight isn't ready, press Try again";
    const Before now = nowSetup();
    g_stock = true;
    const int shadowed = stockOn({charaCode(mine), charaCode(opp), "CMN"});
    void* st = g_cm_state();
    void* temp = list < 0 ? g_cm_temp() : nullptr;
    if (!st || (list < 0 && !temp)) {
      if (!g_native) { g_stock = false; stockOff(); }
      return L"The game's Combo Recipe state wasn't found";
    }
    if (list < 0) {
      std::memcpy(temp, reinterpret_cast<const void*>(b), kBlock);
      *static_cast<uint8_t*>(temp) = 1;
      g_cm_select(st, 0, 0, true);
    } else {
      g_cm_select(st, list, idx, false);
    }
    alignas(16) uint8_t req[0x100] = {};
    g_req_ctor(req);
    const uintptr_t r = reinterpret_cast<uintptr_t>(req);
    at<int32_t>(r + 8) = 0x25;
    at<int32_t>(r + 0xA0) = at<int32_t>(b + 0xFC);
    at<int32_t>(r + 0x9C) = g_cm_bgm(mine, opp);
    const int my_color = now.color[me];
    for (int side = 0; side < 2; side++) {
      const uintptr_t slot = r + 0xC + static_cast<size_t>(side) * 0x48;
      const bool player = side == me;
      at<int32_t>(slot) = player ? mine : opp;
      at<int32_t>(slot + 4) = player ? my_color : (mine == opp && my_color == 0 ? 1 : 0);
      at<int32_t>(slot + 8) = 0;
      at<int32_t>(slot + 0xC) = 0;
      at<int32_t>(slot + 0x10) = 2;
      at<uint8_t>(slot + 0x14) = 0;
      for (int extra = 1; extra < 3; extra++) {
        const uintptr_t x = slot + static_cast<size_t>(extra) * 0x18;
        at<int32_t>(x) = 0x26;
        at<int32_t>(x + 4) = 0;
        at<int32_t>(x + 8) = 0;
        at<int32_t>(x + 0xC) = 0;
        at<int32_t>(x + 0x10) = 2;
        at<uint8_t>(x + 0x14) = 0;
      }
      for (size_t off : {0x2F, 0x3F, 0x4F}) at<uint8_t>(slots[side] + off) = 0;
    }
    const bool queued = g_submit(req);
    if (void* text = at<void*>(r + 0xA8)) (*RC::Unreal::GMalloc)->Free(text);
    COMBO_LOG("combo trials: try: the game's own Combo Recipe fight asked for No." + std::to_string(at<int32_t>(b + kNumber)) + ": " + charaCode(mine) + " against " + charaCode(opp) +
              ", made on battle version " + std::to_string(made_on) + ", stage " + std::to_string(at<int32_t>(b + 0xFC)) + ", " + (list < 0 ? std::string("from the temp block") : "list " + std::to_string(list) + " slot " + std::to_string(idx)) +
              ", player side " + std::to_string(me) + ", " + std::to_string(shadowed) + " mod pak files set aside, game mode was " + std::to_string(gameMode()) + (queued ? "" : ": the game refused it"));
    if (!queued) {
      if (!g_native) { g_stock = false; stockOff(); }
      return L"The game didn't take it, press Try again";
    }
    g_trial_version = made_on;
    if (!g_before.valid && gameMode() == 8) g_before = now;
    describe(b, n);
    g_create = false;
    g_native = true;
    g_native_seen = false;
    g_native_at = GetTickCount64();
    g_cm_st = reinterpret_cast<uintptr_t>(st);
    modeLater(0x1A);
    reloading = true;
    return {};
  }

  bool backToTraining(const Before& was) {
    if (!g_req_ctor || !g_side_fill || !g_submit || !g_common || !*g_common) return false;
    alignas(16) uint8_t req[0x100] = {};
    g_req_ctor(req);
    uintptr_t gc = *g_common;
    struct { uintptr_t* gc; void* req; } ctx{&gc, req};
    const int me = at<uint8_t>(gc + 0x2F4) & 1;
    const uintptr_t entry = at<uintptr_t>(gc + 0x4D0 + static_cast<size_t>(me) * 8);
    if (!entry) return false;
    const int mine = at<uint8_t>(entry + 0x29);
    const int opponent = was.valid && dummyValid(was.opponent) ? was.opponent : charaAt(true);
    g_side_fill(&ctx, me, mine);
    g_side_fill(&ctx, me ^ 1, opponent);
    const uintptr_t r = reinterpret_cast<uintptr_t>(req);
    {
      const int32_t my_color = at<int32_t>(r + 0xC + static_cast<size_t>(me) * 0x48 + 4);
      int32_t& their_color = at<int32_t>(r + 0xC + static_cast<size_t>(me ^ 1) * 0x48 + 4);
      if (opponent == mine && their_color == my_color) their_color = my_color == 0 ? 1 : 0;
    }
    if (was.valid && !(opponent == mine && was.color[0] == was.color[1])) {
      at<int32_t>(r + 0xC + static_cast<size_t>(me) * 0x48 + 4) = was.color[me];
      at<int32_t>(r + 0xC + static_cast<size_t>(me ^ 1) * 0x48 + 4) = was.color[me ^ 1];
      at<int32_t>(r + 0xA0) = was.stage;
      at<int32_t>(r + 0x9C) = static_cast<int32_t>(was.bgm);
    }
    at<int32_t>(r + 8) = 0x25;
    const bool queued = g_submit(req);
    if (void* text = at<void*>(r + 0xA8)) (*RC::Unreal::GMalloc)->Free(text);
    COMBO_LOG("combo trials: back to training asked with opponent " + std::to_string(opponent) + (queued ? "" : ": the game refused it"));
    if (queued) modeLater(8);
    return queued;
  }

  std::wstring loadBlock(uintptr_t b);
  std::wstring startBlock(uintptr_t b, bool& reloading, int list = -1, int idx = 0) {
    reloading = false;
    if (!stepsIn(b)) return L"That combo is gone, refresh the list";
    if (at<uint32_t>(b + kChara) != static_cast<uint32_t>(charaPlayed())) return L"That combo is for another character";
    if (nativeReady() && (gameMode() == 8 || gameMode() == 0x1A)) return nativeStart(b, list, idx, reloading);
    if (gameMode() != 8) return L"Open this in training mode";
    if (!g_pending.empty() && GetTickCount64() - g_pending_at < 20000) return L"The fight is still loading";
    const int opponent = static_cast<int>(at<uint32_t>(b + kDummyChara));
    const int made_on = static_cast<int>(at<uint32_t>(b + kVersion));
    const int today = orig_GetVersion ? orig_GetVersion() : 0;
    const int want = !orig_GetVersion || made_on <= 0 || made_on >= today ? 0 : made_on;
    const bool same_dummy = opponent == charaAt(true) || !dummyValid(opponent);
    const bool same_version = !orig_GetVersion || g_version_in_use.load() == want;
    bool edited_today = false;
    {
      std::lock_guard<std::mutex> lk(g_today_m);
      edited_today = g_version_in_use.load() > 0 && g_keep_today.load() && !g_stock.load() && !g_today_chars.empty();
    }
    g_stock = true;
    const int shadowed = g_submit ? stockOn({charaCode(charaPlayed()), charaCode(dummyValid(opponent) ? opponent : charaAt(true)), "CMN"}) : 0;
    COMBO_LOG("combo trials: try: made against " + std::to_string(opponent) + " on version " + std::to_string(made_on) + "; the dummy is " + std::to_string(charaAt(true)) +
              ", version in use " + std::to_string(g_version_in_use.load()) + "; " + std::to_string(shadowed) + " mod pak files set aside" + (edited_today ? "; today's script was kept for edits" : ""));
    if ((same_dummy && same_version && shadowed <= 0 && !edited_today) || !g_submit) {
      const std::wstring err = loadBlock(b);
      if (!err.empty() && !g_applied) dropFallback();
      return err;
    }
    if (!g_before.valid) g_before = Before{true, charaAt(true), g_training_version.load()};
    g_pending.assign(reinterpret_cast<const uint8_t*>(b), reinterpret_cast<const uint8_t*>(b) + kBlock);
    g_pending_at = GetTickCount64();
    if (orig_GetVersion) g_training_version = want;
    if (!reloadWith(same_dummy ? charaAt(true) : opponent)) {
      dropFallback();
      return L"The game didn't take the reload, press Try again";
    }
    reloading = true;
    return {};
  }
}

bool startPending() {
  if (g_pending.size() != kBlock) return false;
  static std::vector<uint8_t> held;
  held.swap(g_pending);
  g_pending.clear();
  if (GetTickCount64() - g_pending_at > 120000 || !available() || !inTraining()) { dropFallback(); return false; }
  const std::wstring err = loadBlock(reinterpret_cast<uintptr_t>(held.data()));
  if (!err.empty() && !g_applied) dropFallback();
  std::string why;
  for (wchar_t c : err) why += c < 128 ? static_cast<char>(c) : '?';
  COMBO_LOG("combo trials: the trial waiting for the reload " + (err.empty() ? std::string("is loaded") : "was not loaded: " + why));
  return err.empty();
}

std::wstring start(const Entry& e, bool& reloading) {
  reloading = false;
  if (!available() || !*g_save_root) return L"Combo trials aren't available in this game version";
  if (e.list < 0 || e.list >= static_cast<int>(std::size(kLists)) || e.slot < 0 || e.slot >= kLists[e.list].slots)
    return L"That combo is gone, refresh the list";
  return startBlock(*g_save_root + kLists[e.list].offset + e.slot * kBlock, reloading, e.list == kCreatedList ? -1 : e.list, e.slot);
}

std::wstring record(int& version) {
  version = 0;
  if (!nativeReady()) return L"The game's Combo Maker wasn't found in this game version";
  if (gameMode() != 8) return L"Open this in training mode";
  if (g_native && !g_native_seen && GetTickCount64() - g_native_at < 20000) return L"The fight is still loading";
  const int today = orig_GetVersion ? orig_GetVersion() : 0;
  if (today <= 0) return L"The game's battle version wasn't found";
  int v = g_training_version.load();
  if (v <= 0 || v > today) v = today;
  const int mine = charaAt(false), opp = charaAt(true);
  if (!dummyValid(mine) || !dummyValid(opp)) return L"The fight isn't ready, press Record again";
  if (const int oldest = oldestVersion(); oldest > 0 && v < oldest) return L"One of the two fighters wasn't in the game on that version";
  uintptr_t gc = *g_common;
  const int me = at<uint8_t>(gc + 0x2F4) & 1;
  const uintptr_t entry = at<uintptr_t>(gc + 0x4D0 + static_cast<size_t>(me) * 8);
  if (!entry || !at<uintptr_t>(gc + 0x4D0 + static_cast<size_t>(me ^ 1) * 8)) return L"The fight isn't ready, press Record again";
  void* st = g_cm_state();
  if (!st) return L"The game's Combo Maker state wasn't found";
  const Before now = nowSetup();
  g_stock = true;
  const int shadowed = stockOn({charaCode(mine), charaCode(opp), "CMN"});
  const uintptr_t s = reinterpret_cast<uintptr_t>(st);
  at<uint32_t>(s) &= ~0x1A0u;
  at<int32_t>(s + 4) = 0;
  at<int32_t>(s + 0x14) = v;
  alignas(16) uint8_t req[0x100] = {};
  g_req_ctor(req);
  struct { uintptr_t* gc; void* req; } ctx{&gc, req};
  g_side_fill(&ctx, me, at<uint8_t>(entry + 0x29));
  g_side_fill(&ctx, me ^ 1, opp);
  const uintptr_t r = reinterpret_cast<uintptr_t>(req);
  {
    const int32_t my_color = at<int32_t>(r + 0xC + static_cast<size_t>(me) * 0x48 + 4);
    int32_t& their_color = at<int32_t>(r + 0xC + static_cast<size_t>(me ^ 1) * 0x48 + 4);
    if (opp == mine && their_color == my_color) their_color = my_color == 0 ? 1 : 0;
  }
  at<int32_t>(r + 8) = 0x25;
  const bool queued = g_submit(req);
  if (void* text = at<void*>(r + 0xA8)) (*RC::Unreal::GMalloc)->Free(text);
  COMBO_LOG("combo trials: record: the game's own Combo Maker recording asked for " + charaCode(mine) + " against " + charaCode(opp) + " on battle version " + std::to_string(v) +
            " (the game's current is " + std::to_string(today) + "), " + std::to_string(shadowed) + " mod pak files set aside" + (queued ? "" : ": the game refused it"));
  if (!queued) {
    if (!g_native) { g_stock = false; stockOff(); }
    return L"The game didn't take it, press Record again";
  }
  if (!g_before.valid) g_before = now;
  g_trial_name = L"Recording a combo";
  g_trial_version = v;
  g_create = true;
  g_native = true;
  g_native_seen = false;
  g_native_at = GetTickCount64();
  g_cm_st = s;
  modeLater(0x1A);
  version = v;
  return {};
}

bool recording() { return g_create && nativeFight(); }

bool nativeFight() { return g_native && g_common && *g_common && gameMode() == 0x1A && asw_engine::get(); }

void nativeTick() {
  if (g_mode_want >= 0 && g_common && *g_common) {
    const uintptr_t gc = *g_common;
    if (!asw_engine::get()) {
      const uint8_t now = at<uint8_t>(gc + 0x2F0);
      if (now != g_mode_want) {
        at<uint8_t>(gc + 0x2F2) = now;
        at<uint8_t>(gc + 0x2F0) = static_cast<uint8_t>(g_mode_want);
      }
      COMBO_LOG("combo trials: between the fights, game mode " + std::to_string(now) + " -> " + std::to_string(g_mode_want) + ", " + std::to_string(GetTickCount64() - g_mode_at) + " ms after the request");
      g_mode_want = -1;
    } else if (GetTickCount64() - g_mode_at > 30000) {
      COMBO_LOG("combo trials: the running fight never ended, game mode left at " + std::to_string(at<uint8_t>(gc + 0x2F0)));
      g_mode_want = -1;
    }
  }
  if (!g_native || !g_common || !*g_common) return;
  if (gameMode() == 0x1A) {
    static unsigned long long gone_at = 0;
    if (asw_engine::get()) {
      gone_at = 0;
      if (g_create && g_recipe && g_cm_st && g_trial_version > 0 && at<int32_t>(g_cm_st + 4) == 0 && at<int32_t>(g_cm_st + 0x14) == g_trial_version && at<int32_t>(g_recipe + rVersion) != g_trial_version) {
        static unsigned long long noted_for = 0;
        if (noted_for != g_native_at) {
          noted_for = g_native_at;
          COMBO_LOG("combo trials: recording: the recipe's step rules set to battle version " + std::to_string(g_trial_version) + " (they were on " + std::to_string(at<int32_t>(g_recipe + rVersion)) + "), again after every reset of this recording");
        }
        at<int32_t>(g_recipe + rVersion) = g_trial_version;
      }
      if (!g_native_seen) {
        g_native_seen = true;
        COMBO_LOG(std::string("combo trials: the game's own Combo ") + (g_create ? "Maker recording" : "Recipe fight") + " is on, the game's battle version answer is " + std::to_string(orig_GetVersion ? orig_GetVersion() : -1) +
                  (g_create && g_cm_st ? ", state: play/create " + std::to_string(at<int32_t>(g_cm_st + 4)) + ", version " + std::to_string(at<int32_t>(g_cm_st + 0x14)) + ", flags " + std::to_string(at<uint32_t>(g_cm_st)) : std::string()));
      }
      return;
    }
    if (!g_native_seen) {
      if (GetTickCount64() - g_native_at < 60000) return;
    } else {
      if (!gone_at) gone_at = GetTickCount64();
      if (GetTickCount64() - gone_at < 30000) return;
      gone_at = 0;
    }
  }
  if (!g_native_seen && (gameMode() == 8 || g_mode_want >= 0) && GetTickCount64() - g_native_at < 40000) return;
  g_native = false;
  g_create = false;
  g_stock = false;
  g_before = Before{};
  const bool back = stockOff();
  COMBO_LOG(std::string("combo trials: the Combo Recipe fight is over") + (back ? ", the mod pak's files are back from the next fight" : ""));
}

std::wstring startData(const void* block, size_t size, bool& reloading) {
  reloading = false;
  if (!available() || !*g_save_root) return L"Combo trials aren't available in this game version";
  if (size < kBlock) return L"That combo's data doesn't look right";
  static std::vector<uint8_t> held;
  held.assign(static_cast<const uint8_t*>(block), static_cast<const uint8_t*>(block) + kBlock);
  return startBlock(reinterpret_cast<uintptr_t>(held.data()), reloading);
}

void init(const std::filesystem::path& dir) {
  g_backup_file = dir / "StriveLabs_trial_settings.bin";
  g_stock_dir = dir / "StriveLabs_trial_paks";
  {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(g_stock_dir, ec)) {
      const std::string n = e.path().filename().string();
      if (n.rfind("trial_", 0) == 0 && (e.path().extension() == ".pak" || e.path().extension() == ".sig")) std::filesystem::remove(e.path(), ec);
    }
  }
  try {
    auto& sc = sigscan::get();
    g_req_ctor = reinterpret_cast<ReqCtor_t>(sc.scan("\x48\x89\x5C\x24\x08\x57\x48\x83\xEC\x20\x48\x8D\x41\x0C\x33\xFF\xC6\x40\x14\x00\x48\x8D\x50\x48", "xxxxxxxxxxxxxxxxxxxxxxxx"));
    g_side_fill = reinterpret_cast<SideFill_t>(sc.scan("\x48\x89\x5C\x24\x08\x48\x89\x6C\x24\x18\x48\x89\x74\x24\x20\x57\x41\x56\x41\x57\x48\x83\xEC\x20\x4C\x8B\xF1\x48", "xxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
    g_submit = reinterpret_cast<Submit_t>(sc.scan(
        "\x40\x53\x48\x83\xEC\x20\x80\x3D\x00\x00\x00\x00\x00\x48\x8B\xD9\x74\x3D\xE8\x00\x00\x00\x00\x84\xC0\x75\x34\x48\x8B\xD3\x48\x8D\x0D\x00\x00\x00\x00\xE8\x00\x00\x00\x00\x33\xC0\x33\xD2\xB1\x01\x89\x05\x00\x00\x00\x00\x89\x05\x00\x00\x00\x00",
        "xxxxxxxx????xxxxxxx????xxxxxxxxxx????x????xxxxxxxx????xx????"));
  } catch (...) {
    g_req_ctor = nullptr;
    g_side_fill = nullptr;
    g_submit = nullptr;
  }
  try {
    auto& sc = sigscan::get();
    const uintptr_t a = sc.scan("\x48\x8B\x1D\x00\x00\x00\x00\xE8\x00\x00\x00\x00\x48\x8B\xC8\x45\x33\xC9\x45\x33\xC0\x33\xD2\xE8\x00\x00\x00\x00\x48\x8D\x93\xE0\x8C\x75\x01", "xxx????x????xxxxxxxxxxxx????xxxxxxx");
    const uintptr_t t = sc.scan("\xE8\x00\x00\x00\x00\x48\x8B\xC8\xE8\x00\x00\x00\x00\x48\x8B\xC8\x48\x8B\xF8\xE8\x00\x00\x00\x00\xC6\x07\x01", "x????xxxx????xxxxxxx????xxx");
    if (a && t) {
      g_cm_state = reinterpret_cast<Getter_t>(reinterpret_cast<uintptr_t>(get_rip_relative(a + 8)));
      g_cm_select = reinterpret_cast<Select_t>(reinterpret_cast<uintptr_t>(get_rip_relative(a + 0x18)));
      g_cm_temp = reinterpret_cast<Getter_t>(reinterpret_cast<uintptr_t>(get_rip_relative(t + 9)));
    }
    g_cm_bgm = reinterpret_cast<Bgm_t>(sc.scan("\x40\x53\x48\x83\xEC\x30\x8B\xD9\x83\xF9\x01\x75\x7C\x83\xFA\x15\x0F\x84\x00\x00\x00\x00\x48\x8B", "xxxxxxxxxxxxxxxxxx????xx"));
  } catch (...) {
    g_cm_state = g_cm_temp = nullptr;
    g_cm_select = nullptr;
    g_cm_bgm = nullptr;
  }
  try {
    g_owns = reinterpret_cast<Owns_t>(sigscan::get().scan("\x40\x53\x48\x83\xEC\x20\x8B\xD9\x83\xF9\x26\x75\x08\x32\xC0\x48\x83\xC4\x20\x5B\xC3", "xxxxxxxxxxxxxxxxxxxxx"));
  } catch (...) {
    g_owns = nullptr;
  }
  COMBO_LOG(std::string("combo trials: the game's own Combo Recipe fight ") + (g_cm_state && g_cm_temp && g_cm_select && g_cm_bgm ? "can be started from Try" : "wasn't found: Try reloads training instead") +
            (g_owns ? "" : " (no ownership check)"));
  if (!g_req_ctor || !g_side_fill || !g_submit) {
    g_submit = nullptr;
    COMBO_LOG("combo trials: the game's opponent change wasn't found: Try loads the combo against the dummy you have");
  } else {
    COMBO_LOG("combo trials: opponent change ready");
  }
  g_stream_file = dir / "StriveLabs_stream_settings.bin";
  try {
    const uintptr_t m = sigscan::get().scan(
        "\x0F\x85\x00\x00\x00\x00\x0F\x10\x03\x48\x8D\x54\x24\x30\x0F\x10\x4B\x10\x48\x8D\x0D\x00\x00\x00\x00\x0F\x29\x44\x24\x30",
        "xx????xxxxxxxxxxxxxxx????xxxxx");
    g_recipe = reinterpret_cast<uintptr_t>(get_rip_relative(m + 21));
    const uintptr_t s = sigscan::get().scan("\x48\x8B\x3D\x00\x00\x00\x00\x48\x81\xC7\xE0\x8C\x75\x01", "xxx????xxxxxxx");
    g_save_root = static_cast<uintptr_t*>(get_rip_relative(s + 3));
    const uintptr_t c = sigscan::get().scan(
        "\x48\x8B\x05\x00\x00\x00\x00\x0F\xB6\x98\xF4\x02\x00\x00\xE8\x00\x00\x00\x00\x48\x69\xD3\x80\xDB\x0A\x00\x0F\xB6\x84\x02\xDE\x4D\x27\x00\x3B\x87\xF4\x00\x00\x00",
        "xxx????xxxxxxxx????xxxxxxxxxxxxxxxxxxxxx");
    g_chara_owner = static_cast<uintptr_t*>(get_rip_relative(c + 3));
    g_chara_base = reinterpret_cast<GetBase_t>(reinterpret_cast<uintptr_t>(get_rip_relative(c + 15)));
    g_refresh = reinterpret_cast<Refresh_t>(sigscan::get().scan(
        "\x40\x55\x53\x48\x8D\xAC\x24\x78\xFC\xFF\xFF\x48\x81\xEC\x88\x04\x00\x00\x48\x8B\x05\x00\x00\x00\x00\x48\x33\xC4\x48\x89\x85\x70\x03\x00\x00\x48\x8B\xD9\xE8\x00\x00\x00\x00",
        "xxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxx????"));
    g_pos_side = reinterpret_cast<Refresh_t>(sigscan::get().scan(
        "\x48\x89\x5C\x24\x10\x48\x89\x6C\x24\x18\x48\x89\x74\x24\x20\x57\x48\x81\xEC\x30\x04\x00\x00\x48\x8B\x05\x00\x00\x00\x00\x48\x33\xC4\x48\x89\x84\x24\x20\x04\x00\x00\x48\x8B\xD9\x40\x32\xFF\x48\x8D\x0D\x00\x00\x00\x00",
        "xxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxx????"));
  } catch (...) {
    g_refresh = nullptr;
  }
  COMBO_LOG(available() ? "combo trials: ready" : "combo trials: not found, page disabled");
  try {
    const uintptr_t v = sigscan::get().scan(
        "\x40\x53\x48\x83\xEC\x20\x48\x83\x3D\x00\x00\x00\x00\x00\xBB\x27\x00\x00\x00\x74\x20",
        "xxxxxxxxx????xxxxxxxx");
    g_common = static_cast<uintptr_t*>(get_rip_relative(v + 0x18));  // mov rax, [common] at +0x15
    g_version_fn = v;
    auto* d = new PLH::x64Detour(v, reinterpret_cast<uint64_t>(&hook_GetVersion), reinterpret_cast<uint64_t*>(&orig_GetVersion));
    if (!d->hook()) orig_GetVersion = nullptr;
  } catch (...) {
    orig_GetVersion = nullptr;
  }
  if (!orig_GetVersion) COMBO_LOG("combo trials: battle version hook not found, older versions off");
  try {
    const uintptr_t a = sigscan::get().scan("\x40\xF6\xC7\x04\x74\x00\x8B\x3D\x00\x00\x00\x00\xE8\x00\x00\x00\x00\xB9\x78\x00\x00\x00\x83\xF8\x24\x0F\x42\xF9",
                                            "xxxxx?xx????x????xxxxxxxxxxx");
    const uintptr_t b = sigscan::get().scan("\x40\xF6\xC7\x01\x74\x00\x8B\x3D\x00\x00\x00\x00\xE8\x00\x00\x00\x00\xB9\x6E\x00\x00\x00\xEB",
                                            "xxxxx?xx????x????xxxxxx");
    const auto called = [](uintptr_t call) {
      int32_t rel = 0;
      std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), 4);
      return call + 5 + rel;
    };
    const bool ok = g_version_fn && called(a + 12) == g_version_fn && called(b + 12) == g_version_fn;
    g_risc_from[0] = ok ? reinterpret_cast<const void*>(a + 17) : nullptr;
    g_risc_from[1] = ok ? reinterpret_cast<const void*>(b + 17) : nullptr;
  } catch (...) {
    g_risc_from[0] = g_risc_from[1] = nullptr;
  }
  COMBO_LOG(orig_GetVersion && g_risc_from[0] && g_risc_from[1] ? "battle rules: RISC damage checks found" : "battle rules: RISC damage checks not found, the 4.09 RISC damage option does nothing");
  try {
    uintptr_t adv = 0, col = 0, bbs = 0, list = 0;
    if (orig_GetVersion && findPathBuilders(reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr)), adv, col, bbs, list)) {
      auto* d1 = new PLH::x64Detour(adv, reinterpret_cast<uint64_t>(&hook_PathAdv), reinterpret_cast<uint64_t*>(&orig_PathAdv));
      auto* d2 = new PLH::x64Detour(col, reinterpret_cast<uint64_t>(&hook_PathCol), reinterpret_cast<uint64_t*>(&orig_PathCol));
      auto* d3 = new PLH::x64Detour(bbs, reinterpret_cast<uint64_t>(&hook_PathBbs), reinterpret_cast<uint64_t*>(&orig_PathBbs));
      auto* d4 = new PLH::x64Detour(list, reinterpret_cast<uint64_t>(&hook_PathList), reinterpret_cast<uint64_t*>(&orig_PathList));
      const bool a = d1->hook(), b = d2->hook(), c = d3->hook(), d = d4->hook();
      g_today_hooks = a && b && c && d;
      if (!g_today_hooks) {
        if (a) d1->unHook();
        if (b) d2->unHook();
        if (c) d3->unHook();
        if (d) d4->unHook();
      }
    }
  } catch (...) {
    g_today_hooks = false;
  }
  COMBO_LOG(g_today_hooks ? "battle version: file name builders found (4): fighters with edits can keep today's files on an old version"
                          : "battle version: file name builders not found: on an old version every fighter runs that version's own files");
  try {
    const size_t n = orig_GetVersion && g_risc_from[0] && g_risc_from[1] ? findRiscChecks(reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr)), g_version_fn, g_risc407_from, kRiscChecks) : 0;
    g_risc407 = n == kRiscChecks;
    if (!g_risc407) std::fill(std::begin(g_risc407_from), std::end(g_risc407_from), nullptr);
    COMBO_LOG(g_risc407 ? "battle rules: RISC gauge checks found (8)" : "battle rules: RISC gauge checks not found (" + std::to_string(n) + " of 8), the 4.06 RISC option does nothing");
  } catch (...) {
    g_risc407 = false;
    std::fill(std::begin(g_risc407_from), std::end(g_risc407_from), nullptr);
  }
  try {
    const uintptr_t p = sigscan::get().scan(
        "\x48\x8B\xC4\x55\x53\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x8D\x68\xB8\x48\x81\xEC\x08\x01\x00\x00\x0F\x29\x70\xA8\x0F\x29\x78\x98\x44\x0F\x29\x40\x88\x48\x8B\x05\x00\x00\x00\x00\x48\x33\xC4\x48\x89\x45\xC8\x48\x8B\x45\x70\x4C\x8D\x6C\x24\x78",
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxx");
    auto* d = new PLH::x64Detour(p, reinterpret_cast<uint64_t>(&hook_Position), reinterpret_cast<uint64_t*>(&orig_Position));
    if (!d->hook()) orig_Position = nullptr;
  } catch (...) {
    orig_Position = nullptr;
  }
  if (!orig_Position) COMBO_LOG("combo trials: position lookup not found, old versions keep the game's own numbering");
}

bool trainingMode() { return gameMode() == 8; }
void setOldRiscDamage(int mode) {
  const bool found = orig_GetVersion && g_risc_from[0] && g_risc_from[1];
  g_old_risc = !found ? 0 : mode == 1 ? 1 : mode == 2 && g_risc407 ? 2 : 0;
}
void setKeepToday(bool on) { g_keep_today = on && g_today_hooks; }
bool keepTodayOn() { return g_keep_today.load(std::memory_order_relaxed) && g_today_hooks; }
bool trainingOld() { return g_version_in_use.load(std::memory_order_relaxed) > 0 && gameMode() == 8; }
bool runsToday(const std::string& chr) {
  const int in_use = g_version_in_use.load(std::memory_order_relaxed);
  return in_use > 0 && gameMode() == 8 && g_today_hooks && firstVersionOf(chr) > in_use;
}
void setEditedChars(const std::vector<std::string>& chars) {
  std::lock_guard<std::mutex> lk(g_today_m);
  g_today_chars = chars;
}
std::vector<std::string> takeServedToday() {
  std::lock_guard<std::mutex> lk(g_today_m);
  std::vector<std::string> out;
  out.swap(g_today_served);
  return out;
}
std::map<std::string, int> filesThisLoad() {
  std::map<std::string, int> out;
  const unsigned load = g_load_no.load(std::memory_order_relaxed);
  const int in_use = g_version_in_use.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lk(g_today_m);
  for (const auto& [c, f] : g_files_seen)
    if (f.load == load && f.in_use == in_use) out[c] = f.state;
  return out;
}
unsigned loadNumber() { return g_load_no.load(std::memory_order_relaxed); }
bool fileNamesSeen() { return g_today_hooks; }
bool oldRiscRules() { return g_old_risc.load(std::memory_order_relaxed) == 2 && gameMode() == 8 && versionAsked() > kLastSeason4; }
void setTrainingVersion(int version) { g_training_version = orig_GetVersion ? version : 0; }
void latchVersion(bool battle_running) {
  static bool was_running = false;
  if (was_running && !battle_running) g_load_no.fetch_add(1, std::memory_order_relaxed);
  was_running = battle_running;
  if (battle_running) return;
  int v = g_training_version.load();
  if (v > 0 && !g_today_hooks) {
    const int oldest = oldestVersion();
    if (oldest > 0 && v < oldest) v = 0;
  }
  if (g_version_in_use.exchange(v) != v && v == 0 && g_training_version.load() > 0)
    COMBO_LOG("combo trials: a character wasn't in the game on the picked version, training runs on the current one");
}
int trainingVersion() { return g_training_version; }

void setOnlineVersion(int version) {
  g_online_version = orig_GetVersion && handshake::ready() ? version : 0;
  COMBO_LOG("online version: " + (g_online_version ? std::to_string(g_online_version.load())
                                                    : std::string(!version ? "off" : !orig_GetVersion ? "off (the game's version lookup wasn't found)" : "off (the version check wasn't found)")));
}
int onlineVersion() { return g_online_version; }
void stepOnlineVersion(int dir) {
  // Off, then 38 (5.02) down to 1 (1.00); the current version needs no pick.
  const int v = g_online_version;
  int idx = v == 0 ? 0 : 39 - v;  // position in that list
  idx = ((idx + (dir > 0 ? 1 : -1)) % 39 + 39) % 39;
  setOnlineVersion(idx == 0 ? 0 : 39 - idx);
}
int onlineVersionUsed() { return g_online_in_use; }
uintptr_t saveRoot() { return g_save_root && *g_save_root ? *g_save_root : 0; }
uintptr_t trainingManager() { return g_recipe; }
int playerChara() { return charaPlayed(); }
std::string charaCode(int chara) { return chara >= 0 && chara < static_cast<int>(std::size(kFirstCodes)) ? kFirstCodes[chara] : ""; }
bool inTraining() { return gameMode() == 8; }
int gameModeNow() { return g_common && *g_common ? gameMode() : -1; }
bool onlineFight() { return gameMode() == 22; }
int opponentChara() { return charaAt(true); }
int localSide() { return g_chara_owner && *g_chara_owner ? at<uint8_t>(*g_chara_owner + 0x2F4) & 1 : 0; }
bool onlineVersionInUse() {
  const uint8_t m = gameMode();
  return orig_GetVersion && g_online_in_use > 0 && m != 8 && m != 23 && m != 0x1A;
}

void updateOnline(bool any_battle_running) {
  if (!orig_GetVersion) return;
  auto off = [](const char* why) {
    if (g_online_version) { g_online_version = 0; COMBO_LOG(std::string("online version off: ") + why); }
  };
  if (any_battle_running) return;  // never changes mid-fight
  auto use = [](int v) {
    if (g_online_in_use.exchange(v) != v)
      COMBO_LOG(v ? "online version taken up: " + std::to_string(v) + " (lobby " + std::to_string(lobbyField()) + ")"
                  : std::string("online version dropped"));
  };
  if (inOnlineLobby()) {
    // In a lobby only after two seconds there, so a passing state on the way somewhere
    // else never counts.
    if (!g_lobby_since) g_lobby_since = GetTickCount64();
    if (GetTickCount64() - g_lobby_since >= 2000) g_was_in_lobby = true;
  } else {
    g_lobby_since = 0;
    // The main menu: clearly out of a lobby. Not training: the Park has its own, to wait
    // in (the online version never applies in training anyway).
    if (lobbyField() == 5) {
      if (g_was_in_lobby) { g_was_in_lobby = false; off("back from the lobby"); }
      use(0);
      return;
    }
  }
  // From the lobby on (rematches at the station too), a new pick counts from the next fight.
  if (g_was_in_lobby) use(g_online_version.load());
}
int battleVersion() { return orig_GetVersion ? versionAsked() : 0; }

int oldestVersion() {
  // Each character's first battle version, by the game's character number (its own
  // list runs SOL, KYK, MAY, AXL, ... USG): the first version folder in that
  // character's Chara/<code>/Common/Data. Before it, the game has no data for them.
  int v = 1;
  for (const bool other : {false, true}) {
    const int c = charaAt(other);
    if (c < 0 || c >= static_cast<int>(std::size(kFirst))) return 0;
    v = (std::max)(v, static_cast<int>(kFirst[c]));
  }
  return v;
}

void stop() {
  if (g_native) {
    if (!g_native_seen && GetTickCount64() - g_native_at < 20000) {
      COMBO_LOG("combo trials: stop asked while the fight is still loading, left for when it is on");
      return;
    }
    const Before was = g_before;
    g_native = false;
    g_create = false;
    g_stock = false;
    g_before = Before{};
    stockOff();
    if (!backToTraining(was)) g_mode_want = -1;
    return;
  }
  if (available()) {
    const uintptr_t r = g_recipe;
    for (size_t off : {rMatchName, rMatchCond, rShowName, rProgress, rFlag, rShowCond}) at<TArr>(r + off).num = 0;
    at<uint8_t>(r + rDone) = 0;
    g_refresh(reinterpret_cast<void*>(r));
  }
  if (g_applied) {
    restoreSettings();
    placeAsSet();  // back to the player's own start
  }
  g_stock = false;
  const bool paks_back = stockOff();
  if (g_before.valid || paks_back) {
    const Before was = g_before;
    g_before = Before{};
    if (was.valid && orig_GetVersion) g_training_version = was.version;
    const int opponent = was.valid && dummyValid(was.opponent) ? was.opponent : charaAt(true);
    if (g_submit && (paks_back || was.valid)) reloadWith(opponent);
  }
}

bool stockFight() { return g_stock.load(std::memory_order_relaxed); }

bool active() { return g_applied || nativeFight(); }
int trialVersion() { return active() ? g_trial_version : 0; }
std::wstring trialName() { return active() ? g_trial_name : std::wstring(); }
bool watch() {
  if (!g_applied || g_demo.empty()) return false;
  g_watch = Watch::Armed;
  placeAsSet();  // it plays from the reset, where its maker started
  return true;
}
void stopWatching() { g_watch = Watch::Off; }
bool watching() { return g_watch != Watch::Off; }

void onRoundStart() {  // every reset starts it over, like a stream to the dummy
  startPending();
  if (g_watch == Watch::Off) return;
  g_watch = Watch::On;
  g_watch_pos = 0;
}

bool watchNext(bool facing_left, uint16_t& flags) {
  if (g_watch != Watch::On) return false;
  if (g_watch_pos >= g_demo.size()) { g_watch = Watch::Off; return false; }
  if (g_watch_pos == 0) {
    g_watch_flip = facing_left != g_demo_left;
    COMBO_LOG(std::string("combo trials: demo starts: you face ") + (facing_left ? "left" : "right") + ", it was recorded facing " + (g_demo_left ? "left" : "right") +
              ", inputs " + (g_watch_flip ? "mirrored" : "as recorded") + ", " + std::to_string(g_demo.size()) + " frames");
  }
  flags = g_demo[g_watch_pos++] & 0x0FFF;
  if (g_watch_flip) flags = static_cast<uint16_t>((flags & ~0xC) | ((flags & 0x4) << 1) | ((flags & 0x8) >> 1));
  return true;
}

// Takes Starting Position, Player Side and Starting Range from the settings as they
// are now, and resets positions next frame, as the game does when Combo Maker loads a
// combo. Training only, in battle.
void placeAsSet() {
  if (!g_pos_side) return;
  g_pos_side(reinterpret_cast<void*>(g_recipe));
  at<uint8_t>(g_recipe + tResetRequest) = 1;
}

static void recipeSlots(bool in_training) {
  constexpr size_t kSlots = 0x7B7E10, kSlot = 0x888;
  constexpr int kCount = 30;
  static uint64_t seen[kCount] = {};
  static bool have = false;
  static int wait = 0, rules_seen = -1, count_seen = -1;
  static bool ours = false;
  static uint64_t steps_seen = 0;
  if (!in_training || g_native || !orig_GetVersion || !g_save_root || !*g_save_root || gameMode() != 8) { have = false; rules_seen = -1; count_seen = -1; ours = false; return; }
  const int latest = orig_GetVersion(), v = versionAsked();
  if (g_recipe) {
    const TArr& names = at<TArr>(g_recipe + rMatchName);
    const int count = names.data && names.num > 0 && names.num <= kMaxSteps ? names.num : 0;
    uint64_t steps = 1469598103934665603ull;
    for (int i = 0; i < count * 32; i++) steps = (steps ^ at<uint8_t>(reinterpret_cast<uintptr_t>(names.data) + static_cast<size_t>(i))) * 1099511628211ull;
    int rules = at<int32_t>(g_recipe + rVersion);
    const auto say = [&](const std::string& what) {
      COMBO_LOG("combo trials: recipe slots: " + what + " (step rules " + std::to_string(rules) + ", " + std::to_string(count) + " steps, training runs on " + std::to_string(v) + ", the game's own is " + std::to_string(latest) + ")");
    };
    if (rules != rules_seen) say("the game set the recipe's step rules");
    if (count != count_seen && (count == 0 || count_seen <= 0)) say(count == 0 ? "the recipe is empty" : "the recipe has steps");
    const bool old = v > 0 && v <= 7 && v < latest;
    if (!old) {
      ours = false;
    } else {
      if (count == 0 && !ours) { ours = true; say("an empty recipe on an old version: what is recorded into it uses that version's step rules"); }
      else if (ours && rules != v && count > 0 && (rules != latest || steps != steps_seen)) { ours = false; say("another recipe was loaded: its own step rules are left alone"); }
      if (ours && rules != v) {
        at<int32_t>(g_recipe + rVersion) = v;
        say("step rules set to battle version " + std::to_string(v));
        rules = v;
      }
    }
    rules_seen = rules;
    count_seen = count;
    steps_seen = steps;
  }
  if (have && ++wait < 30) return;
  wait = 0;
  const auto hash = [](uintptr_t p) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < kSlot; i++) h = (h ^ at<uint8_t>(p + i)) * 1099511628211ull;
    return h;
  };
  for (int i = 0; i < kCount; i++) {
    const uintptr_t slot = *g_save_root + kSlots + static_cast<size_t>(i) * kSlot;
    uint64_t h = hash(slot);
    if (have && h != seen[i]) {
      const int stamp = at<int32_t>(slot), chara = at<int32_t>(slot + 4);
      const bool write = v > 0 && v < latest && stamp == latest && chara >= 0 && chara < 64 && (v > 7 || ours);
      COMBO_LOG("combo trials: recipe slots: slot " + std::to_string(i + 1) + " changed, version " + std::to_string(stamp) + ", character " + std::to_string(chara) +
                (write ? ", version set to " + std::to_string(v) + " (the one training runs on)"
                       : v > 0 && v <= 7 && v < latest && stamp == latest ? std::string(", left as it is: it wasn't recorded from empty on this version, so its steps use today's rules")
                                                                        : std::string(", left as it is")));
      if (write) { at<int32_t>(slot) = v; h = hash(slot); }
    }
    seen[i] = h;
  }
  have = true;
}

void update(bool in_training) {
  recipeSlots(in_training);
  if (!g_checked_file && g_save_root && *g_save_root) {
    // A file from an earlier session: put the player's values back wherever the save
    // still holds the trial's, then drop the file once none are left.
    g_checked_file = true;
    std::ifstream f(g_backup_file, std::ios::binary);
    auto b = std::make_unique<Backup>();
    if (f && f.read(reinterpret_cast<char*>(b.get()), sizeof(Backup)) && b->magic == Backup{}.magic) {
      if (!dummyValid(b->dummy_chara)) b->dummy_chara = -1;
      if (putBack(*b)) COMBO_LOG("combo trials: training settings from the last session put back");
      else { f.close(); std::error_code ec; std::filesystem::remove(g_backup_file, ec); }
    }
    std::ifstream sf(g_stream_file, std::ios::binary);
    StreamBackup sb;
    if (sf && sf.read(reinterpret_cast<char*>(&sb), sizeof(sb)) && sb.magic == StreamBackup{}.magic) {
      if (putBackStream(sb)) {
        g_stream_bk = sb;
        g_stream_pending = true;
        COMBO_LOG("stream to dummy: Round Call and Player Side from the last session put back");
      }
      else { sf.close(); std::error_code ec; std::filesystem::remove(g_stream_file, ec); }
    }
  }
  watchStreamFile();
  if (!in_training) idle();
}

bool streamSetup(bool dummy_on_left) {
  if (!available() || !*g_save_root || !inTraining()) return false;
  streamRestore(false);
  const uintptr_t settings = *g_save_root + kSettings;
  const int32_t want[2] = {dummy_on_left ? 1 : 0, 1};
  for (int k = 0; k < 2; k++) {
    const int id = kStreamIds[k];
    int32_t& now = at<int32_t>(settings + id * 4);
    g_stream_prev[k] = now;
    g_stream_bk.own[k] = g_applied && now == g_bk.applied_settings[id] ? g_bk.settings[id] : now;
    g_stream_bk.set[k] = want[k];
    now = want[k];
  }
  g_stream_setup = true;
  g_stream_pending = false;
  saveStreamFile();
  placeAsSet();
  COMBO_LOG(std::string("stream to dummy: Round Call on (was ") + std::to_string(g_stream_prev[1]) + "), Player Side " +
            std::to_string(want[0]) + " (was " + std::to_string(g_stream_prev[0]) + "), position reset asked");
  return true;
}

void streamRestore(bool reset) {
  if (!g_stream_setup) return;
  g_stream_setup = false;
  if (!g_save_root || !*g_save_root) return;
  const uintptr_t settings = *g_save_root + kSettings;
  for (int k = 0; k < 2; k++) {
    const int id = kStreamIds[k];
    int32_t& now = at<int32_t>(settings + id * 4);
    if (now != g_stream_bk.set[k]) continue;
    now = g_applied ? g_stream_prev[k] : g_stream_bk.own[k];
  }
  g_stream_pending = true;
  if (reset && available() && inTraining()) placeAsSet();
  COMBO_LOG("stream to dummy: Round Call and Player Side put back");
}

// Out of training (main menu, character select, another mode): the trial ends, its
// settings go and the recipe is emptied, so nothing carries into the next session.
void idle() {
  if (g_stock.load() && !g_native && g_pending.empty()) dropFallback();
  if (!g_applied || !g_save_root || !*g_save_root) return;
  restoreSettings();
  const uintptr_t r = g_recipe;
  for (size_t off : {rMatchName, rMatchCond, rShowName, rProgress, rFlag, rShowCond}) at<TArr>(r + off).num = 0;
  at<uint8_t>(r + rDone) = 0;
}

bool available() { return g_recipe && g_save_root && g_chara_owner && g_chara_base && g_refresh && g_pos_side; }

std::vector<Entry> list() {
  std::vector<Entry> out;
  if (!available() || !*g_save_root) return out;
  const int chara = charaPlayed();
  if (chara < 0) return out;
  int newest = 0;  // the newest version among all saved combos stands in for the current one
  for (const auto& def : kLists)
    for (int slot = 0; slot < def.slots; slot++) {
      const uintptr_t b = *g_save_root + def.offset + slot * kBlock;
      if (!at<uint8_t>(b)) continue;
      if (at<char>(b + kMatchName)) newest = (std::max)(newest, static_cast<int>(at<uint32_t>(b + kVersion)));
    }
  for (int l = static_cast<int>(std::size(kLists)) - 1; l >= 0; l--) {
    for (int slot = 0; slot < kLists[l].slots; slot++) {
      const uintptr_t b = *g_save_root + kLists[l].offset + slot * kBlock;
      if (!at<uint8_t>(b)) continue;
      const int steps = stepsIn(b);
      if (!steps || at<uint32_t>(b + kChara) != static_cast<uint32_t>(chara)) continue;
      Entry e;
      e.list = l; e.slot = slot; e.steps = steps;
      e.version = static_cast<int>(at<uint32_t>(b + kVersion));
      e.number = at<int32_t>(b + kNumber) > 0 ? at<uint32_t>(b + kNumber) : 0;
      e.creator = wide(b + kCreator, 22);
      e.title = wide(b + kTitle, 53);
      e.opponent = static_cast<int>(at<uint32_t>(b + kDummyChara));
      e.older = e.version < newest;
      out.push_back(std::move(e));
    }
  }
  out.erase(std::remove_if(out.begin(), out.end(), [&out](const Entry& e) {
    if (e.list != kCreatedList || !e.number) return false;
    for (const Entry& o : out) if (o.list != kCreatedList && o.number == e.number) return true;
    return false;
  }), out.end());
  return out;
}

namespace {
void describe(uintptr_t b, int) {
  g_trial_name = wide(b + kTitle, 53);
  if (g_trial_name.empty()) g_trial_name = at<int32_t>(b + kNumber) > 0 ? L"No." + std::to_wstring(at<uint32_t>(b + kNumber)) : L"Your combo";
}

std::wstring loadBlock(uintptr_t b) {
  const int n = stepsIn(b);
  if (!n) return L"That combo is gone, refresh the list";
  if (at<uint32_t>(b + kChara) != static_cast<uint32_t>(charaPlayed())) return L"That combo is for another character";
  if (n > kPanelRows) return L"Too many steps (max " + std::to_wstring(kPanelRows) + L")";

  const uintptr_t r = g_recipe;
  resize(r + rMatchName, n, 32);
  resize(r + rMatchCond, n, 4);
  resize(r + rShowName, n, 32);
  resize(r + rFlag, n, 1);
  resize(r + rShowCond, n, 4);
  auto data = [&](size_t off) { return reinterpret_cast<uintptr_t>(at<TArr>(r + off).data); };
  for (int i = 0; i < n; i++) {
    std::memcpy(reinterpret_cast<void*>(data(rMatchName) + i * 32), reinterpret_cast<void*>(b + kMatchName + i * 32), 32);
    std::memcpy(reinterpret_cast<void*>(data(rShowName) + i * 32), reinterpret_cast<void*>(b + kShowName + i * 32), 32);
    at<int32_t>(data(rMatchCond) + i * 4) = at<int32_t>(b + kMatchCond + i * 4);
    at<int32_t>(data(rShowCond) + i * 4) = at<uint8_t>(b + kShowCond + i);
    at<uint8_t>(data(rFlag) + i) = 1;
  }
  at<TArr>(r + rProgress).num = 0;
  at<uint8_t>(r + rDone) = 0;
  at<int32_t>(r + rStamp) = 0;
  at<uint8_t>(r + rSeparator) = 0;
  at<int32_t>(r + rVersion) = static_cast<int32_t>(at<uint32_t>(b + kVersion));
  g_refresh(reinterpret_cast<void*>(r));

  // Start settings: the player's are kept aside once, then the combo's go in.
  streamRestore(false);
  const uintptr_t settings = *g_save_root + kSettings;
  if (!g_applied) {
    std::memcpy(g_bk.settings, reinterpret_cast<void*>(settings), sizeof(g_bk.settings));
    g_bk.dummy_chara = -1;
    g_applied = true;
  }
  for (int id = 0; id < 128; id++)
    if (!kept(id)) at<int32_t>(settings + id * 4) = at<int32_t>(b + kComboSettings + id * 4);
  std::memcpy(g_bk.applied_settings, reinterpret_cast<void*>(settings), sizeof(g_bk.applied_settings));
  // The dummy's counter-attack setup, into training's block for the character the dummy
  // really is. A combo made against someone else leaves the dummy as it is.
  const int dummy = static_cast<int>(at<uint32_t>(b + kDummyChara));
  if (dummy == charaAt(true)) {
    if (g_bk.dummy_chara != dummy) {
      if (g_bk.dummy_chara >= 0) std::memcpy(reinterpret_cast<void*>(dummyBlock(g_bk.dummy_chara)), g_bk.dummy, kDummySize);
      g_bk.dummy_chara = dummy;
      std::memcpy(g_bk.dummy, reinterpret_cast<void*>(dummyBlock(dummy)), kDummySize);
    }
    std::memcpy(reinterpret_cast<void*>(dummyBlock(dummy)), reinterpret_cast<void*>(b + kComboDummy), kDummySize);
    std::memcpy(g_bk.applied_dummy, reinterpret_cast<void*>(b + kComboDummy), kDummySize);
  } else if (g_bk.dummy_chara >= 0) {
    std::memcpy(reinterpret_cast<void*>(dummyBlock(g_bk.dummy_chara)), g_bk.dummy, kDummySize);
    g_bk.dummy_chara = -1;
  }
  const int frames = std::clamp(at<int32_t>(b + kRecFrames), 0, kRecMax);
  const auto* rec = reinterpret_cast<const uint16_t*>(b + kRecFlags);
  g_demo.assign(rec, rec + frames);
  g_demo_left = at<int32_t>(b + kRecDir) == 1;
  g_trial_version = static_cast<int>(at<uint32_t>(b + kVersion));
  COMBO_LOG("combo trials: start settings: player side " + std::to_string(at<int32_t>(b + kComboSettings + 0x20 * 4)) + " (0 left, else right), starting position " +
            std::to_string(at<int32_t>(b + kComboSettings + 0x1F * 4)) + ", recording direction " + std::to_string(at<int32_t>(b + kRecDir)) + ", " + std::to_string(frames) +
            " recorded frames, made on battle version " + std::to_string(g_trial_version) + ", training runs on " + std::to_string(g_version_in_use.load()) +
            " (0 current), made against character " + std::to_string(dummy) + ", the dummy is character " + std::to_string(charaAt(true)));
  describe(b, n);
  g_watch = Watch::Off;
  saveBackupFile();
  placeAsSet();
  COMBO_LOG("combo trials: loaded " + std::to_string(at<uint32_t>(b + kNumber)) + ", " + std::to_string(n) + " steps");
  return {};
}
}  // namespace
}  // namespace combo_trials
