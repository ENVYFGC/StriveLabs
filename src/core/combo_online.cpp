#include "combo_online.h"
#include "combo_log.h"
#include "combo_trials.h"
#include "sigscan.h"

#include <Unreal/FMemory.hpp>

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <iterator>
#include <map>
#include <string>

// The game's network manager (one static object) sends every online request: it
// serializes, encrypts and posts it on its own HTTP worker, and its ticker (game thread)
// finishes it into a response slot. The mod fills the same parameters the Combo Maker
// search screen does and calls the same two requests; then, for a download, the game's
// own "downloaded" step stores the combo in the kept list, and its save routine writes it.
namespace combo_online {
namespace {
  template <typename T> T& at(uintptr_t a) { return *reinterpret_cast<T*>(a); }

  using GetMgr_t = uintptr_t (*)();
  using Request_t = bool (*)(uintptr_t mgr, void* param, void* delegate);
  using RootInt_t = int (*)(uintptr_t root);
  using RootBool_t = bool (*)(uintptr_t root);
  using Loaded_t = void (*)(void* request, uintptr_t response, bool ok);
  GetMgr_t g_mgr = nullptr;
  Request_t g_get_combo = nullptr, g_load = nullptr;
  RootInt_t g_kept_count = nullptr, g_kept_slot = nullptr;
  RootBool_t g_request_save = nullptr;
  Loaded_t g_on_loaded = nullptr;
  // The game's texts: a key's FText (namespace "Game"), and an FText's string.
  using GameText_t = void* (*)(void* out, const wchar_t* key, bool title_namespace);
  using ToString_t = const void* (*)(const void* text);
  GameText_t g_text = nullptr;
  ToString_t g_to_string = nullptr;
  // Game keys built as FStrings (freed with the game's allocator): a stage's name key,
  // and a recipe step's input text key (by move name and version, from training's list).
  struct GameFString { wchar_t* data = nullptr; int32_t num = 0, max = 0; };
  using StageKey_t = GameFString* (*)(GameFString* out, int stage);
  using InputKey_t = GameFString* (*)(uintptr_t training, GameFString* out, const char* name32, int version);
  StageKey_t g_stage_key = nullptr;
  InputKey_t g_input_key = nullptr;
  const int* g_input_first = nullptr;  // non-zero: the input goes before the move's name

  // Network manager: request in progress (API id, 0 = none) +0x2FF1CC; has a session
  // +0x2F4184; responses: combo search +0x2FC8C8, combo load +0x2FC538 (each: status
  // +0xA8, 0 = ok).
  constexpr uintptr_t kCurrentApi = 0x2FF1CC, kHasSession = 0x2F4184, kSearchResp = 0x2FC8C8, kLoadResp = 0x2FC538;
  constexpr int kApiGetCombo = 0x60, kApiLoad = 0x5E;
  // Save data: saving enabled +0x28229AC; a save job queued +0x2822190 (0 = none).
  constexpr uintptr_t kSaveEnabled = 0x28229AC, kSaveJob = 0x2822190;
  constexpr int kKeptMax = 100, kPerPage = 20;

  enum class Job { None, Search, Download, Try, Save } g_job = Job::None;
  unsigned long long g_started = 0;
  std::vector<Result> g_results;
  Result g_downloading;

  // An MSVC std::string in the game: inline below 16 characters, else a pointer.
  std::string gameString(uintptr_t s, size_t max = 4096) {
    const size_t size = at<size_t>(s + 0x10), cap = at<size_t>(s + 0x18);
    if (size > max) return {};
    const char* p = cap >= 16 ? at<const char*>(s) : reinterpret_cast<const char*>(s);
    return p ? std::string(p, size) : std::string();
  }

  // A game text by key, in the game's language; empty if the game has none. Each key is
  // looked up once.
  std::wstring gameText(const std::wstring& key) {
    static std::map<std::wstring, std::wstring> cache;
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    std::wstring s;
    if (g_text && g_to_string) {
      // ponytail: the FText is never released (each key once, cached): a few hundred
      // bytes kept for good; release it if this ever runs per frame.
      alignas(8) uint8_t t[0x18] = {};
      g_text(t, key.c_str(), false);
      if (const auto* fs = static_cast<const uint8_t*>(g_to_string(t))) {  // FString: data, count with the NUL
        const wchar_t* d = *reinterpret_cast<wchar_t* const*>(fs);
        const int n = *reinterpret_cast<const int*>(fs + 8);
        if (d && n > 1 && n < 4096) s.assign(d, static_cast<size_t>(n - 1));
      }
    }
    if (!s.empty() && s.front() == L'*') s.clear();  // a missing key comes back as *key*
    return cache[key] = s;
  }
  std::wstring widen(const std::string& u) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), static_cast<int>(u.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, u.c_str(), static_cast<int>(u.size()), w.data(), n);
    return w;
  }
  // Title: ids joined by "_", each a ComboMakerTitle word ("[Wall]").
  std::wstring titleText(const std::string& codes) {
    std::wstring t;
    size_t i = 0;
    while (i < codes.size()) {
      const size_t j = codes.find('_', i);
      const std::string id = codes.substr(i, j == std::string::npos ? std::string::npos : j - i);
      if (!id.empty() && id.find_first_not_of("0123456789") == std::string::npos && id != "0")
        t += gameText(L"ComboMakerTitle" + widen(id));
      if (j == std::string::npos) break;
      i = j + 1;
    }
    return t.empty() ? widen(codes) : t;
  }
  // A key the game handed back as an FString: copied, the game's copy freed.
  std::wstring takeKey(GameFString& s) {
    std::wstring k;
    if (s.data && s.num > 1 && s.num < 512) k.assign(s.data, static_cast<size_t>(s.num - 1));
    if (s.data) (*RC::Unreal::GMalloc)->Free(s.data);
    s = GameFString{};
    return k;
  }
  std::wstring stageText(int stage) {
    if (stage <= 0 || !g_stage_key) return {};
    GameFString k;
    g_stage_key(&k, stage);
    const std::wstring key = takeKey(k);
    return key.empty() ? std::wstring() : gameText(key);
  }
  std::wstring iconsAsText(const std::wstring& s);
  // A step's input ("HS", "236 HS"), as training's recipe shows it; empty if none.
  std::wstring inputText(const std::string& move, int version) {
    const uintptr_t training = combo_trials::trainingManager();
    if (!g_input_key || !training || move.empty() || move.size() >= 32) return {};
    char name[32] = {};
    std::memcpy(name, move.data(), move.size());
    GameFString k;
    g_input_key(training, &k, name, version);
    const std::wstring key = takeKey(k);
    return key.empty() ? std::wstring() : iconsAsText(gameText(key));
  }

  // The game's texts carry button icons as "^mAtkHS;": written as the button ("HS").
  std::wstring iconsAsText(const std::wstring& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size();) {
      if (s[i] == L'^' && i + 1 < s.size() && s[i + 1] == L'm') {
        const size_t end = s.find(L';', i);
        if (end != std::wstring::npos && end - i < 24) {
          std::wstring code = s.substr(i + 2, end - i - 2);
          if (code.rfind(L"Atk", 0) == 0) code = code.substr(3);
          out += code;
          i = end + 1;
          continue;
        }
      }
      out += s[i++];
    }
    return out;
  }
  // Steps: "kind,name,show input,hit type@..." -> "Fafnir - 236 HS [Consecutive Hit]".
  std::vector<std::wstring> stepTexts(const std::string& recipe, int version) {
    static const wchar_t* kHit[] = {L"", L"", L"TRHT_Hit", L"TRHT_ChainHit", L"TRHT_Guard", L"TRHT_ChainGuard"};
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i < recipe.size() && out.size() < 64) {
      const size_t j = recipe.find('@', i);
      const std::string step = recipe.substr(i, j == std::string::npos ? std::string::npos : j - i);
      std::string f[4];
      size_t a = 0;
      for (int k = 0; k < 4; k++) {
        const size_t b = step.find(',', a);
        f[k] = step.substr(a, b == std::string::npos ? std::string::npos : b - a);
        if (b == std::string::npos) break;
        a = b + 1;
      }
      if (!f[1].empty()) {
        std::wstring name = gameText(L"CMCR_" + widen(f[1]));
        if (name.empty()) name = widen(f[1]);
        if (f[2] == "1")
          if (const std::wstring in = inputText(f[1], version); !in.empty())
            name = g_input_first && *g_input_first ? in + L" - " + name : name + L" - " + in;
        const int hit = std::atoi(f[3].c_str());
        if (hit >= 2 && hit <= 5) {
          const std::wstring h = gameText(kHit[hit]);
          if (!h.empty()) name += L" " + h;
        }
        out.push_back(iconsAsText(name));  // the move names carry button icons too
      }
      if (j == std::string::npos) break;
      i = j + 1;
    }
    return out;
  }
  std::wstring tagTexts(const std::vector<int>& tags) {
    static const wchar_t* kTag[] = {L"", L"Official", L"ScreenEdge", L"NotScreenEdge", L"Tension_0", L"Tension_50",
                                    L"Tension_100", L"RiscMax", L"Counter", L"Crouch", L"Air", L"Random"};
    std::wstring s;
    for (int t : tags)
      if (t >= 1 && t <= 11) {
        const std::wstring w = gameText(std::wstring(L"CM_FilterMenu_RecipeTag_") + kTag[t]);
        if (w.empty()) continue;
        if (!s.empty()) s += L", ";
        s += w;
      }
    return s;
  }
  // Base64url, as the download carries the combo.
  std::vector<uint8_t> unbase64url(const std::string& in) {
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int bits = 0;
    for (const char c : in) {
      int v;
      if (c >= 'A' && c <= 'Z') v = c - 'A';
      else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
      else if (c >= '0' && c <= '9') v = c - '0' + 52;
      else if (c == '-' || c == '+') v = 62;
      else if (c == '_' || c == '/') v = 63;
      else continue;  // padding, line breaks
      acc = (acc << 6) | static_cast<uint32_t>(v);
      bits += 6;
      if (bits >= 8) { bits -= 8; out.push_back(static_cast<uint8_t>(acc >> bits)); }
    }
    return out;
  }

  std::wstring notReady(uintptr_t mgr) {
    if (!at<uint8_t>(mgr + kHasSession)) return L"Not connected to the game's network: open an online menu once (like Combo Maker), then try again";
    if (at<int>(mgr + kCurrentApi)) return L"The game is busy with another online request, try again in a moment";
    return {};
  }

  std::wstring failed(int status) {
    if (status == 2 || status == 10) return L"The game's network session ran out: open an online menu once, then try again";
    if (status < 0) return L"No answer from the server (offline, or maintenance)";
    return L"The server said no (error " + std::to_wstring(status) + L")";
  }
}

void init() {
  try {
    auto& s = sigscan::get();
    g_get_combo = reinterpret_cast<Request_t>(s.scan(
        "\x40\x55\x53\x56\x41\x54\x41\x55\x48\x8D\x6C\x24\xC9\x48\x81\xEC\xB0\x00\x00\x00\x83\xB9\xCC\xF1\x2F\x00\x00\x4D\x8B\xE8\x4C\x8B\xE2\x48\x89\x55\xCF\x48\x8B\xD9\x74\x23\x48\x8D\x91\xC8\xC8\x2F\x00",
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
    g_load = reinterpret_cast<Request_t>(s.scan(
        "\x40\x55\x53\x41\x54\x41\x56\x41\x57\x48\x8D\x6C\x24\xC9\x48\x81\xEC\xB0\x00\x00\x00\x83\xB9\xCC\xF1\x2F\x00\x00\x4C\x8D\xA1\x38\xC5\x2F\x00\x4D\x8B\xF0\x48\x89\x55\xCF\x4C\x8B\xFA\x48\x8B\xD9",
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
    g_kept_count = reinterpret_cast<RootInt_t>(s.scan(
        "\x33\xD2\x48\x81\xC1\x20\xDF\x79\x01\x44\x8D\x4A\x02\x0F\x1F\x00\x80\xB9\xA0\x47\xFF\xFF\x00\x8D\x42\x01",
        "xxxxxxxxxxxxxxxxxxxxxxxxxx"));
    g_kept_slot = reinterpret_cast<RootInt_t>(s.scan(
        "\x45\x33\xC0\x4C\x8B\xD1\x45\x8B\xC8\x41\x8B\xD0\x48\x81\xC1\xC0\x26\x79\x01\xB8\xFF\xFF\xFF\xFF",
        "xxxxxxxxxxxxxxxxxxxxxxxx"));
    g_request_save = reinterpret_cast<RootBool_t>(s.scan(
        "\x48\x89\x5C\x24\x10\x57\x48\x83\xEC\x20\x0F\xB6\xB9\xAC\x29\x82\x02\x48\x8B\xD9\x40\x84\xFF\x74\x3C",
        "xxxxxxxxxxxxxxxxxxxxxxxxx"));
    // In the game's own "download and save": a call to the network manager getter, and
    // the address of its "downloaded" step.
    const uintptr_t m = s.scan("\xE8\x00\x00\x00\x00\x8B\x53\x6C\x33\xED\x8B\x8B\x34\x0A\x00\x00\x83\xFA\xFF\x48\x8B\xF0",
                               "x????xxxxxxxxxxxxxxxxx");
    g_mgr = reinterpret_cast<GetMgr_t>(m + 5 + at<int32_t>(m + 1));
    const uintptr_t l = s.scan("\x8B\x83\x34\x0A\x00\x00\x89\x87\x94\x00\x00\x00\x48\x8D\x05\x00\x00\x00\x00",
                               "xxxxxxxxxxxxxxx????");
    g_on_loaded = reinterpret_cast<Loaded_t>(l + 19 + at<int32_t>(l + 15));
    g_text = reinterpret_cast<GameText_t>(s.scan(
        "\x48\x89\x5C\x24\x08\x48\x89\x74\x24\x10\x48\x89\x7C\x24\x18\x4C\x89\x74\x24\x20\x55\x48\x8D\x6C\x24\xA9\x48\x81\xEC\x90\x00\x00\x00\x45\x84\xC0",
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
    g_to_string = reinterpret_cast<ToString_t>(s.scan(
        "\x40\x53\x48\x83\xEC\x20\x48\x8B\xD9\xE8\x00\x00\x00\x00\x48\x8B\x0B\x48\x8B\x01\x48\x83\xC4\x20\x5B\x48\xFF\x60\x10\xCC\xCC\xCC",
        "xxxxxxxxxx????xxxxxxxxxxxxxxxxxx"));
    g_stage_key = reinterpret_cast<StageKey_t>(s.scan(
        "\x48\x89\x5C\x24\x18\x48\x89\x6C\x24\x20\x56\x48\x83\xEC\x20\x33\xDB\x8B\xEA\x48\x8B\xF1\x83\xFA\x05\x75\x4D\x48\x89\x19\x8D\x55\x0B",
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
    g_input_key = reinterpret_cast<InputKey_t>(s.scan(
        "\x40\x55\x53\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x8D\xAC\x24\xF8\xFE\xFF\xFF\x48\x81\xEC\x08\x02\x00\x00\x48\x8B\x05\x00\x00\x00\x00\x48\x33\xC4\x48\x89\x85\xF0\x00\x00\x00\x49\x8B\xF8",
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxx"));
    {  // in the game's recipe rows: cmp dword [order flag], 0
      const uintptr_t o = s.scan("\x83\x3D\x00\x00\x00\x00\x00\x75\x4D\x44\x8D\x43\xFF\x85\xDB\x75", "xx????xxxxxxxxxx");
      g_input_first = reinterpret_cast<const int*>(o + 7 + at<int32_t>(o + 2));
    }
  } catch (...) {
    g_get_combo = nullptr;
  }
  COMBO_LOG(available() ? "combo online: ready" : "combo online: not found, the online search is off");
}

bool available() {
  return g_get_combo && g_load && g_kept_count && g_kept_slot && g_request_save && g_mgr && g_on_loaded &&
         combo_trials::saveRoot();
}

bool busy() { return g_job != Job::None; }
const std::vector<Result>& results() { return g_results; }

std::wstring search(const Filter& f, int sort, int page) {
  if (!available()) return L"The online search isn't available in this game version";
  if (busy()) return L"Still waiting for the last request";
  const uintptr_t mgr = g_mgr();
  if (auto why = notReady(mgr); !why.empty()) return why;
  // The search screen's parameters (0x70 bytes): page, sort and order, count, then the
  // filter: player, user ids (empty), character, vs character, cleared, version, type,
  // number, tags (empty), and the platform (6: every platform, like the game sends).
  alignas(8) uint8_t p[0x70] = {};
  static const int kSort[4] = {4, 3, 1, 5};  // downloads, likes, date, clear rate
  at<int>(reinterpret_cast<uintptr_t>(p) + 0x08) = page;
  at<int>(reinterpret_cast<uintptr_t>(p) + 0x0C) = kSort[sort < 0 || sort > 3 ? 0 : sort];
  at<int>(reinterpret_cast<uintptr_t>(p) + 0x10) = 2;  // descending
  at<int>(reinterpret_cast<uintptr_t>(p) + 0x14) = kPerPage;
  at<int>(reinterpret_cast<uintptr_t>(p) + 0x18) = -1;
  at<int>(reinterpret_cast<uintptr_t>(p) + 0x38) = f.chara;
  for (uintptr_t o : {0x3C, 0x40, 0x44, 0x48, 0x4C}) at<int>(reinterpret_cast<uintptr_t>(p) + o) = -1;
  at<int>(reinterpret_cast<uintptr_t>(p) + 0x3C) = f.vs;
  if (f.current) at<int>(reinterpret_cast<uintptr_t>(p) + 0x44) = 39;
  // Tags: a std::vector<int> (first, last, end of storage); the game copies it.
  static int tags[8];
  const size_t n = (std::min)(f.tags.size(), std::size(tags));
  for (size_t i = 0; i < n; i++) tags[i] = f.tags[i];
  if (n) {
    at<int*>(reinterpret_cast<uintptr_t>(p) + 0x50) = tags;
    at<int*>(reinterpret_cast<uintptr_t>(p) + 0x58) = tags + n;
    at<int*>(reinterpret_cast<uintptr_t>(p) + 0x60) = tags + n;
  }
  static std::string creator;
  creator = f.creator_id;
  if (!creator.empty()) {
    at<std::string*>(reinterpret_cast<uintptr_t>(p) + 0x20) = &creator;
    at<std::string*>(reinterpret_cast<uintptr_t>(p) + 0x28) = &creator + 1;
    at<std::string*>(reinterpret_cast<uintptr_t>(p) + 0x30) = &creator + 1;
  }
  at<int>(reinterpret_cast<uintptr_t>(p) + 0x68) = 6;
  alignas(8) uint8_t delegate[16] = {};  // no callback: the reply is polled
  if (!g_get_combo(mgr, p, delegate)) return L"The game didn't take the request (busy or offline)";
  g_job = Job::Search;
  g_started = GetTickCount64();
  COMBO_LOG("combo online: search, character " + std::to_string(f.chara) + " vs " + std::to_string(f.vs) + ", " +
            std::to_string(n) + " tags" + (creator.empty() ? "" : ", one creator") + ", sort " + std::to_string(sort) +
            ", page " + std::to_string(page));
  return {};
}

namespace { std::wstring fetch(const Result& r, Job job); }

std::wstring download(const Result& r) {
  if (!available()) return L"The online search isn't available in this game version";
  if (busy()) return L"Still waiting for the last request";
  const uintptr_t root = combo_trials::saveRoot();
  if (!at<uint8_t>(root + kSaveEnabled)) return L"Saving is off in the game right now";
  if (g_kept_count(root) >= kKeptMax) return L"Your kept Combo Maker list is full (100). Delete some in Combo Maker first";
  return fetch(r, Job::Download);
}

std::wstring tryIt(const Result& r) {
  if (!available()) return L"The online search isn't available in this game version";
  if (busy()) return L"Still waiting for the last request";
  if (r.chara != combo_trials::playerChara()) return L"That combo is for another character";
  return fetch(r, Job::Try);
}

namespace {
std::wstring fetch(const Result& r, Job job) {
  const uintptr_t mgr = g_mgr();
  if (auto why = notReady(mgr); !why.empty()) return why;
  alignas(8) uint8_t p[0x10] = {};  // number, creator's platform
  at<uint32_t>(reinterpret_cast<uintptr_t>(p) + 0x08) = r.number;
  at<int>(reinterpret_cast<uintptr_t>(p) + 0x0C) = r.platform;
  alignas(8) uint8_t delegate[16] = {};
  if (!g_load(mgr, p, delegate)) return L"The game didn't take the request (busy or offline)";
  g_job = job;
  g_downloading = r;
  g_started = GetTickCount64();
  COMBO_LOG(std::string(job == Job::Try ? "combo online: fetch to try No." : "combo online: download No.") + std::to_string(r.number));
  return {};
}
}  // namespace

bool update(std::wstring& message) {
  if (g_job == Job::None || !available()) return false;
  // Waiting since before training was left: whatever is in the reply now may be another
  // request's (the game's own Combo Maker), so it's dropped.
  if (g_job != Job::Save && GetTickCount64() - g_started > 60000) {
    g_job = Job::None;
    message = L"No answer from the server";
    return true;
  }
  const uintptr_t mgr = g_mgr();
  const uintptr_t root = combo_trials::saveRoot();
  if (g_job == Job::Save) {  // the game's save routine, once no other save is queued
    if (at<uintptr_t>(root + kSaveJob)) {
      if (GetTickCount64() - g_started < 10000) return false;
      g_job = Job::None;
      message = L"Downloaded, but the game was busy saving: it's in the list, and saved with your next save";
      return true;
    }
    g_request_save(root);
    g_job = Job::None;
    COMBO_LOG("combo online: saved");
    return false;
  }
  const int api = g_job == Job::Search ? kApiGetCombo : kApiLoad;
  if (at<int>(mgr + kCurrentApi) == api) {
    if (GetTickCount64() - g_started > 30000) {  // let the game finish it, just stop waiting
      g_job = Job::None;
      message = L"No answer from the server";
      return true;
    }
    return false;
  }
  if (g_job == Job::Search) {
    g_job = Job::None;
    const uintptr_t resp = mgr + kSearchResp;
    const int status = at<int>(resp + 0xA8);
    if (status != 0) { message = failed(status); COMBO_LOG("combo online: search failed " + std::to_string(status)); return true; }
    g_results.clear();
    // Each result is 0x148 bytes: number, title, characters, the creator (name at +0x50),
    // platform, date, downloads, likes, clear rate, cleared, comment, recipe, tags.
    const uintptr_t first = at<uintptr_t>(resp + 0xB0), last = at<uintptr_t>(resp + 0xB8);
    for (uintptr_t e = first; e && e < last && g_results.size() < 50; e += 0x148) {
      Result r;
      r.number = at<uint32_t>(e + 0x00);
      r.chara = at<int>(e + 0x28);
      r.vs_chara = at<int>(e + 0x2C);
      r.author = gameString(e + 0x50);
      r.platform = at<int>(e + 0xB0);
      r.downloads = at<int>(e + 0xD8);
      r.likes = at<int>(e + 0xDC);
      r.clear_rate = at<int>(e + 0xE0);
      r.cleared = at<int>(e + 0xE4) != 0;
      r.comment = gameString(e + 0xF0);
      r.version = at<int>(e + 0xEC);
      std::vector<int> tags;
      for (uintptr_t t = at<uintptr_t>(e + 0x130); t && t < at<uintptr_t>(e + 0x138) && tags.size() < 16; t += 4) {
        tags.push_back(at<int>(t));
        if (at<int>(t) == 1) r.official = true;
      }
      r.title = titleText(gameString(e + 0x08));
      r.tags = tagTexts(tags);
      r.steps = stepTexts(gameString(e + 0x110), r.version);
      r.stage = stageText(at<int>(e + 0xE8));
      r.creator_id = gameString(e + 0x30);
      g_results.push_back(std::move(r));
    }
    message = g_results.empty() ? L"No combos found" : std::to_wstring(g_results.size()) + L" combos found";
    COMBO_LOG("combo online: " + std::to_string(g_results.size()) + " results");
    return true;
  }
  // A download: hand it to the game's own "downloaded" step, which stores it in the kept
  // list. Only with a free slot: the step writes wherever the slot finder points, and -1
  // would land in your own list.
  const Job job = g_job;
  g_job = Job::None;
  const uintptr_t resp = mgr + kLoadResp;
  const int status = at<int>(resp + 0xA8);
  if (status != 0 || at<size_t>(resp + 0xB0 + 0x10) == 0) {
    message = failed(status);
    COMBO_LOG("combo online: download failed " + std::to_string(status));
    return true;
  }
  if (job == Job::Try) {  // decode it ourselves and load it into training, nothing kept
    if (!combo_trials::inTraining() || combo_trials::playerChara() != g_downloading.chara) {
      message = combo_trials::inTraining() ? L"That combo is for another character" : L"Open this in training mode";
      return true;
    }
    const std::vector<uint8_t> block = unbase64url(gameString(resp + 0xB0, 1 << 20));
    message = combo_trials::loadData(block.data(), block.size());
    if (message.empty()) message = L"Trying No." + std::to_wstring(g_downloading.number) + L" in training (not kept). Combo Trials can play it for you (Watch it) or set its version.";
    COMBO_LOG("combo online: try, " + std::to_string(block.size()) + " bytes");
    return true;
  }
  if (g_kept_count(root) >= kKeptMax || g_kept_slot(root) < 0) {
    message = L"Your kept Combo Maker list is full (100). Delete some in Combo Maker first";
    return true;
  }
  // What the game's download request carries into that step: number, official, cleared,
  // creator's platform; it reports back at +0x88 (1 = stored).
  alignas(8) uint8_t req[0x98] = {};
  const uintptr_t q = reinterpret_cast<uintptr_t>(req);
  at<uint32_t>(q + 0x8C) = g_downloading.number;
  at<uint8_t>(q + 0x90) = g_downloading.official ? 1 : 0;
  at<uint8_t>(q + 0x91) = g_downloading.cleared ? 1 : 0;
  at<int>(q + 0x94) = g_downloading.platform;
  g_on_loaded(req, resp, true);
  if (at<int>(q + 0x88) != 1) {
    message = L"The game couldn't store the combo";
    COMBO_LOG("combo online: store failed " + std::to_string(at<int>(q + 0x88)));
    return true;
  }
  COMBO_LOG("combo online: stored No." + std::to_string(g_downloading.number));
  g_job = Job::Save;
  g_started = GetTickCount64();
  message = L"Downloaded No." + std::to_wstring(g_downloading.number) + L": it's in Combo Trials now";
  return true;
}
}  // namespace combo_online
