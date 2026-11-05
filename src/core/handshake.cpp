#include "handshake.h"

#include <polyhook2/Detour/x64Detour.hpp>  // before the game structs: their macros upset it
#include "arcsys.h"
#include "combo_log.h"
#include "combo_trials.h"
#include "recolor_share.h"
#include "sigscan.h"

#include <Windows.h>
#include <intrin.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

// How it fits the game's netcode: each tick the game reads the local pad, turns it into
// an input word (the converter hooked here, at the three places that read the local
// pad) and sends that word; both games then simulate from the same stored words. So a
// pattern written into the word there is what the opponent's game gets, and what our
// own game simulates too. Opponent words are read where each analyzer takes its input.
namespace handshake {
namespace {
  using ToRecFlag_t = uint32_t (*)(int, uint32_t, bool);
  ToRecFlag_t orig_ToRecFlag = nullptr;
  using Abort_t = void (*)(int);
  Abort_t g_abort = nullptr;  // the game's own "abort this online battle, for this reason"
  uintptr_t g_local_sites[3] = {};  // return addresses of the game's local pad reads

  constexpr int kHold = 4;          // frames per symbol
  constexpr int kSymbols = 10;      // preamble 4, version 3, check 3
  constexpr int kStartFrame = 90;   // well into the fight's intro, far from the round start
  constexpr int kFinalAge = 20;     // older opponent inputs can't be a rollback guess any more
  constexpr int kSearchBack = 300;  // the opponent's pattern starts near ours
  constexpr int kInvalidMatch = 9;  // abort reason: the game's "invalid match" popup
  // Recolor sharing's pattern (any online fight, with the setting on): preamble 0 3 0 3, our Steam account's
  // 32 bits (16 symbols, high first), then its CRC-8 (4 symbols). It goes after the version's pattern.
  constexpr int kShareSymbols = 24;

  // Engine: GameFrame +0x3E7C; bit 0 of +0x330 set once players can act; inputs at
  // +0x3868, 0x7E apart, P1's at [0], P2's at [3]. Battle state (+0xC50 of the game
  // state): round number at +0x118.
  int frameOf(uintptr_t e) { return *reinterpret_cast<int*>(e + 0x3E7C); }

  struct Fight {
    uintptr_t engine = 0;
    int last_frame = -1;
    int send_from = -1;  // first frame the pattern goes out on, -1 = not yet
    int symbols[kSymbols] = {};
    int side = 0;        // ours, 0 = P1
    int version = 0;
    bool decided = false;
    int share_from = -1;  // -1 = not sending
    int share[kShareSymbols] = {};
    bool share_done = false;  // read (or given up on) this fight
  } g;
  uint8_t g_opp[4096];  // the opponent's direction symbol per frame (by frame & 4095)
  int g_opp_frame[4096];

  // Direction symbol: bit 0 up, bit 1 left or right (only forward is ever sent).
  int sym(uint32_t recflag) { return ((recflag & 0x1) ? 1 : 0) | ((recflag & 0xC) ? 2 : 0); }

  void encode(int v, int out[kSymbols]) {
    static const int pre[4] = {3, 0, 3, 0};
    for (int i = 0; i < 4; i++) out[i] = pre[i];
    const int d[3] = {(v >> 4) & 3, (v >> 2) & 3, v & 3};
    for (int i = 0; i < 3; i++) { out[4 + i] = d[i]; out[7 + i] = 3 - d[i]; }
  }

  // The opponent's version from their final inputs in [from, to], or -1 if no pattern.
  // Read by content, a symbol from the middle of each hold, wherever it starts: input
  // delay moves it by a few frames.
  int decode(int from, int to) {
    auto at = [](int x) { return g_opp_frame[x & 4095] == x ? static_cast<int>(g_opp[x & 4095]) : -1; };
    for (int f = from; f + kSymbols * kHold <= to; f++) {
      int s[kSymbols];
      bool ok = true;
      for (int k = 0; k < kSymbols && ok; k++) ok = (s[k] = at(f + k * kHold + kHold / 2)) >= 0;
      if (!ok || s[0] != 3 || s[1] != 0 || s[2] != 3 || s[3] != 0) continue;
      if (s[7] != 3 - s[4] || s[8] != 3 - s[5] || s[9] != 3 - s[6]) continue;
      return (s[4] << 4) | (s[5] << 2) | s[6];
    }
    return -1;
  }

  uint8_t crc8(uint32_t v) {
    uint8_t c = 0;
    for (int i = 3; i >= 0; i--) {
      c ^= static_cast<uint8_t>(v >> (i * 8));
      for (int b = 0; b < 8; b++) c = static_cast<uint8_t>((c & 0x80) ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
  }
  void encodeShare(uint32_t acct, int out[kShareSymbols]) {
    static const int pre[4] = {0, 3, 0, 3};
    for (int i = 0; i < 4; i++) out[i] = pre[i];
    for (int i = 0; i < 16; i++) out[4 + i] = static_cast<int>(acct >> (30 - 2 * i)) & 3;
    const uint8_t c = crc8(acct);
    for (int i = 0; i < 4; i++) out[20 + i] = (c >> (6 - 2 * i)) & 3;
  }
  // The opponent's account from their final inputs in [from, to], 0 if no pattern.
  uint32_t decodeShare(int from, int to) {
    auto at = [](int x) { return g_opp_frame[x & 4095] == x ? static_cast<int>(g_opp[x & 4095]) : -1; };
    for (int f = from; f + kShareSymbols * kHold <= to; f++) {
      int s[kShareSymbols];
      bool ok = true;
      for (int k = 0; k < kShareSymbols && ok; k++) ok = (s[k] = at(f + k * kHold + kHold / 2)) >= 0;
      if (!ok || s[0] != 0 || s[1] != 3 || s[2] != 0 || s[3] != 3) continue;
      uint32_t v = 0;
      for (int k = 0; k < 16; k++) v = (v << 2) | static_cast<uint32_t>(s[4 + k]);
      uint8_t c = 0;
      for (int k = 0; k < 4; k++) c = static_cast<uint8_t>((c << 2) | s[20 + k]);
      if (c == crc8(v) && v) return v;
    }
    return 0;
  }

  uint32_t hook_ToRecFlag(int pad, uint32_t key, bool flag) {
    uint32_t r = orig_ToRecFlag(pad, key, flag);
    if ((g.send_from < 0 && g.share_from < 0) || !g.engine) return r;
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (ret != g_local_sites[0] && ret != g_local_sites[1] && ret != g_local_sites[2]) return r;
    if (*reinterpret_cast<uint32_t*>(g.engine + 0x330) & 1) return r;  // once players can act there's never a pattern, whatever's left of it
    const int now = frameOf(g.engine);
    if (g.share_from >= 0 && now >= g.share_from && (now - g.share_from) / kHold < kShareSymbols) {  // the share pattern uses the same directions
      const int s = g.share[(now - g.share_from) / kHold];
      r &= ~0xFu;
      if (s & 1) r |= 0x1;
      if (s & 2) r |= g.side == 0 ? 0x8 : 0x4;
      return r;
    }
    if (g.send_from < 0) return r;
    const int k = (frameOf(g.engine) - g.send_from) / kHold;
    if (frameOf(g.engine) < g.send_from || k >= kSymbols) return r;
    // The directions only, replaced (a real press can't spoil the pattern); never back,
    // which the round start reads, and never down.
    r &= ~0xFu;
    if (g.symbols[k] & 1) r |= 0x1;
    if (g.symbols[k] & 2) r |= g.side == 0 ? 0x8 : 0x4;
    return r;
  }
}

void install() {
  try {
    auto& s = sigscan::get();
    const uintptr_t a = s.scan("\xE8\x00\x00\x00\x00\x0F\xB7\xD0\x49\x63\x47\x60\x48\x83\xC0\x04\x49\x8D\x0C\x87",
                               "x????xxxxxxxxxxxxxxx");
    const uintptr_t b = s.scan("\xE8\x00\x00\x00\x00\x0F\xB7\xD0\xEB\x19\x0F\xB6\x89\xF3\x02\x00\x00\x45\x33\xC0",
                               "x????xxxxxxxxxxxxxxx");
    const uintptr_t c = s.scan("\xE8\x00\x00\x00\x00\x0F\xB7\xD0\xEB\x03\x41\x8B\xD5\x48\x8D\x4D\x40\xE8",
                               "x????xxxxxxxxxxxxx");
    g_local_sites[0] = a + 5; g_local_sites[1] = b + 5; g_local_sites[2] = c + 5;
    g_abort = reinterpret_cast<Abort_t>(s.scan(
        "\x48\x89\x5C\x24\x08\x57\x48\x83\xEC\x20\x8B\xF9\xE8\x00\x00\x00\x00\x48\x8D\x4C\x24\x38",
        "xxxxxxxxxxxxx????xxxxx"));
    const uintptr_t conv = s.scan(
        "\x89\x54\x24\x00\x89\x4C\x24\x00\x53\x55\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x83\xEC\x00\x8B\xFA",
        "xxx?xxx?xxxxxxxxxxxxxxx?xx");
    auto* d = new PLH::x64Detour(conv, reinterpret_cast<uint64_t>(&hook_ToRecFlag), reinterpret_cast<uint64_t*>(&orig_ToRecFlag));
    if (!d->hook()) orig_ToRecFlag = nullptr;
  } catch (...) {
    orig_ToRecFlag = nullptr;
  }
  COMBO_LOG(orig_ToRecFlag && g_abort ? "handshake: ready" : "handshake: not found, the online version stays off");
}

namespace {
  // Recolor sharing: our account goes out in round 1's intro (after the version pattern, if any), and the
  // opponent's is read when the round starts. Nothing is ever aborted. No pattern just means no sharing.
  void shareUpdate(void* game_state, uintptr_t e, int frame) {
    if (g.share_done || !orig_ToRecFlag || !combo_trials::onlineFight() || !recolor_share::sharing()) return;
    const uintptr_t state = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(game_state) + 0xC50);
    if (!state || *reinterpret_cast<int*>(state + 0x118) != 1) return;  // round 1 only
    const bool can_act = (*reinterpret_cast<uint32_t*>(e + 0x330) & 1) != 0;
    if (g.share_from < 0) {
      if (can_act) { g.share_done = true; COMBO_LOG("share: the round started before our pattern went out (intro skipped?)"); return; }
      int start = kStartFrame;
      if (combo_trials::onlineVersionInUse()) {  // after the version's pattern, as it really went out
        if (g.send_from < 0) return;
        start = g.send_from + kSymbols * kHold + 8;
      }
      if (frame < start) return;
      g.side = combo_trials::localSide();
      encodeShare(recolor_share::myAccount(), g.share);
      g.share_from = frame + 2;
      COMBO_LOG("share: sending our pattern from frame " + std::to_string(g.share_from) + " as P" + std::to_string(g.side + 1));
      return;
    }
    if (!can_act) return;
    const int to = frame - kFinalAge;
    if (to < g.share_from + kShareSymbols * kHold + 8) {
      if (frame > g.share_from + kShareSymbols * kHold + 8 + kFinalAge + 120) { g.share_done = true; COMBO_LOG("share: no time to read the opponent's pattern"); }
      return;  // wait a few frames more
    }
    g.share_done = true;
    const uint32_t theirs = decodeShare((std::max)(0, g.share_from - kSearchBack), to);
    COMBO_LOG(theirs ? "share: the opponent's pattern read" : "share: no pattern from the opponent (no StriveLabs, sharing off, or an older one)");
    if (theirs) recolor_share::opponentFound(theirs);
  }
}

void update(void* game_state) {
  auto* eng = asw_engine::get();
  if (!eng || !game_state) return;
  const uintptr_t e = reinterpret_cast<uintptr_t>(eng);
  const int frame = frameOf(e);
  if (e != g.engine || frame < g.last_frame - 60) {  // a new fight
    g = Fight{};
    g.engine = e;
    std::memset(g_opp_frame, 0xFF, sizeof(g_opp_frame));
  }
  g.last_frame = frame;
  shareUpdate(game_state, e, frame);
  if (!combo_trials::onlineVersionInUse() || g.decided) return;
  if (!orig_ToRecFlag || !g_abort) {  // can't check: never play on an unchecked version
    g.decided = true;
    COMBO_LOG("handshake: unavailable, leaving the match");
    if (g_abort) g_abort(kInvalidMatch);
    return;
  }
  const uintptr_t state = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(game_state) + 0xC50);
  if (!state || *reinterpret_cast<int*>(state + 0x118) != 1) return;  // round 1 only
  const bool can_act = (*reinterpret_cast<uint32_t*>(e + 0x330) & 1) != 0;
  if (g.send_from < 0) {
    if (can_act) {  // the intro ended before the pattern went out: never play unchecked
      g.decided = true;
      COMBO_LOG("handshake: the round started before the version check, leaving the match");
      g_abort(kInvalidMatch);
      return;
    }
    if (frame < kStartFrame) return;
    g.side = combo_trials::localSide();
    g.version = combo_trials::onlineVersionUsed();
    encode(g.version, g.symbols);
    g.send_from = frame + 2;
    COMBO_LOG("handshake: sending version " + std::to_string(g.version) + " from frame " + std::to_string(g.send_from) +
              " as P" + std::to_string(g.side + 1));
    return;
  }
  if (!can_act) return;  // decided when the round starts
  const int to = frame - kFinalAge;
  if (to < g.send_from + kSymbols * kHold + 8) return;  // the intro was short: a few frames more
  g.decided = true;
  const int theirs = decode((std::max)(0, g.send_from - kSearchBack), to);
  if (theirs == g.version) {
    COMBO_LOG("handshake: opponent on the same version, playing on");
    return;
  }
  COMBO_LOG(theirs < 0 ? "handshake: no answer from the opponent, leaving the match"
                       : "handshake: opponent on version " + std::to_string(theirs) + ", leaving the match");
  g_abort(kInvalidMatch);
}

void onBattleInput(const void* analyzer, uint32_t recflag) {
  const bool version = !g.decided && combo_trials::onlineVersionInUse();
  const bool share = !g.share_done && g.share_from >= 0;
  if (!g.engine || (!version && !share)) return;
  const int opp = 1 - combo_trials::localSide();
  if (reinterpret_cast<uintptr_t>(analyzer) != g.engine + 0x3868 + static_cast<uintptr_t>(opp) * 3 * 0x7E) return;
  const int frame = frameOf(g.engine);
  if (frame < 0) return;
  g_opp[frame & 4095] = static_cast<uint8_t>(sym(recflag));
  g_opp_frame[frame & 4095] = frame;
}
}  // namespace handshake
