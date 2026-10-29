#include "combo_trials.h"
#include "combo_log.h"
#include "sigscan.h"

#include <Unreal/FMemory.hpp>
#include <polyhook2/Detour/x64Detour.hpp>
#include <atomic>
#include <fstream>
#include <memory>
#include <algorithm>
#include <cstdint>
#include <cstring>

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
  constexpr ListDef kLists[] = {{0x1758CE0, 10}, {0x17926C0, 100}};
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
  std::filesystem::path g_backup_file;

  void saveBackupFile() {
    std::ofstream f(g_backup_file, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(&g_bk), sizeof(g_bk));
  }
  constexpr int kMaxChara = 64;  // well past the roster; only guards a damaged file
  uintptr_t dummyBlock(int chara) { return *g_save_root + kDummyBlocks + static_cast<size_t>(chara) * kDummySize; }
  bool dummyValid(int chara) { return chara >= 0 && chara < kMaxChara; }

  // Puts back the player's value for each setting the trial changed, if it still holds
  // the trial's value (one changed since is the player's own choice and stays), and the
  // same for the dummy's block. Returns how many were put back.
  int putBack(const Backup& b) {
    const uintptr_t settings = *g_save_root + kSettings;
    int n = 0;
    for (int id = 0; id < 128; id++) {
      if (kept(id) || b.applied_settings[id] == b.settings[id]) continue;
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
  int hook_GetVersion() {
    const int v = orig_GetVersion();
    const int o = g_version_in_use.load(std::memory_order_relaxed);
    if (o > 0 && g_common && *g_common && at<uint8_t>(*g_common + 0x2F0) == 8) {  // training
      static int logged = 0;
      if (logged != o) { logged = o; COMBO_LOG("combo trials: training runs on battle version " + std::to_string(o)); }
      return o;
    }
    return v;
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
    if (!g_chara_owner || !*g_chara_owner || !g_chara_base) return -1;
    const uint8_t side = at<uint8_t>(*g_chara_owner + 0x2F4) ^ (other_side ? 1 : 0);
    const uintptr_t b = g_chara_base();
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

void init(const std::filesystem::path& dir) {
  g_backup_file = dir / "StriveLabs_trial_settings.bin";
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
    auto* d = new PLH::x64Detour(v, reinterpret_cast<uint64_t>(&hook_GetVersion), reinterpret_cast<uint64_t*>(&orig_GetVersion));
    if (!d->hook()) orig_GetVersion = nullptr;
  } catch (...) {
    orig_GetVersion = nullptr;
  }
  if (!orig_GetVersion) COMBO_LOG("combo trials: battle version hook not found, older versions off");
}

void setTrainingVersion(int version) { g_training_version = orig_GetVersion ? version : 0; }
void latchVersion(bool battle_running) { if (!battle_running) g_version_in_use = g_training_version.load(); }
int trainingVersion() { return g_training_version; }

void stop() {
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
}

bool active() { return g_applied; }

// Takes Starting Position, Player Side and Starting Range from the settings as they
// are now, and resets positions next frame, as the game does when Combo Maker loads a
// combo. Training only, in battle.
void placeAsSet() {
  if (!g_pos_side) return;
  g_pos_side(reinterpret_cast<void*>(g_recipe));
  at<uint8_t>(g_recipe + tResetRequest) = 1;
}

void update(bool in_training) {
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
  }
  if (!in_training) idle();
}

// Out of training (main menu, character select, another mode): the trial ends, its
// settings go and the recipe is emptied, so nothing carries into the next session.
void idle() {
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
      if (at<char>(b + kMatchName)) newest = (std::max)(newest, static_cast<int>(at<uint32_t>(b + kVersion)));
    }
  for (int l = static_cast<int>(std::size(kLists)) - 1; l >= 0; l--) {
    for (int slot = 0; slot < kLists[l].slots; slot++) {
      const uintptr_t b = *g_save_root + kLists[l].offset + slot * kBlock;
      const int steps = stepsIn(b);
      if (!steps || at<uint32_t>(b + kChara) != static_cast<uint32_t>(chara)) continue;
      Entry e;
      e.list = l; e.slot = slot; e.steps = steps;
      e.version = static_cast<int>(at<uint32_t>(b + kVersion));
      e.number = at<uint32_t>(b + kNumber);
      e.creator = wide(b + kCreator, 22);
      e.title = wide(b + kTitle, 53);
      e.older = e.version < newest;
      out.push_back(std::move(e));
    }
  }
  return out;
}

std::wstring load(const Entry& e) {
  if (!available() || !*g_save_root) return L"Combo trials aren't available in this game version";
  if (e.list < 0 || e.list >= static_cast<int>(std::size(kLists)) || e.slot < 0 || e.slot >= kLists[e.list].slots)
    return L"That combo is gone, refresh the list";
  const uintptr_t b = *g_save_root + kLists[e.list].offset + e.slot * kBlock;
  const int n = stepsIn(b);
  if (!n) return L"That combo is gone, refresh the list";
  if (at<uint32_t>(b + kChara) != static_cast<uint32_t>(charaPlayed())) return L"That combo is for another character";
  if (n > kPanelRows) return L"Training's recipe panel only has " + std::to_wstring(kPanelRows) + L" rows";

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
  saveBackupFile();
  placeAsSet();
  COMBO_LOG("combo trials: loaded " + std::to_string(e.number) + ", " + std::to_string(n) + " steps");
  return {};
}
}  // namespace combo_trials
