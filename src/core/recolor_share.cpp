#include "recolor_share.h"

#include "combo_log.h"
#include "combo_trials.h"
#include "recolor_test.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

// Steam's flat API, from the game's own steam_api64.dll (Steamworks v157): ISteamUser v023
// for our id, ISteamNetworkingMessages v002 for the channel. Layouts from Valve's
// steamnetworkingtypes.h: SteamNetworkingIdentity is 136 bytes, packed (type 16 = SteamID,
// the 64-bit id right after type and size); a received message has the sender's identity at
// +16, so its SteamID at +24. The game pumps Steam's callbacks itself; we only call in.
namespace recolor_share {
namespace {
  using Clock = std::chrono::steady_clock;
  using GetIface_t = void* (*)();
  using GetSteamID_t = uint64_t (*)(void*);
  using Send_t = int (*)(void*, const void* identity, const void* data, uint32_t size, int flags, int channel);
  using Receive_t = int (*)(void*, int channel, void** out, int max);
  using Session_t = bool (*)(void*, const void* identity);
  using SetId_t = void (*)(void* identity, uint64_t id);
  using Release_t = void (*)(void* msg);
  GetIface_t g_user_iface = nullptr, g_msgs_iface = nullptr;
  GetSteamID_t g_get_id = nullptr;
  Send_t g_send = nullptr;
  Receive_t g_receive = nullptr;
  Session_t g_accept = nullptr, g_close = nullptr;
  SetId_t g_set_id = nullptr;
  Release_t g_release = nullptr;

  constexpr int kChannel = 0x53544C42;     // 'STLB', since the game uses no networking-messages channel
  constexpr int kReliable = 8;             // k_nSteamNetworkingSend_Reliable
  constexpr uint64_t kIndividual = 0x0110000100000000ull;  // the top half of a public individual account's id
  constexpr uint8_t kHello = 1, kChunk = 2, kEnd = 3, kNone = 4;
  constexpr uint32_t kMaxPayload = 48u << 20;  // theirs is dropped if bigger than this
  constexpr size_t kChunkSize = 128 * 1024;    // Steam's send buffer is 512 KB, so pieces go out as it takes them
  enum Tx { kTxWait, kTxSending, kTxDone };

  struct Identity { alignas(8) uint8_t b[136] = {}; };
  std::filesystem::path g_file;
  std::atomic<bool> g_on{false};
  uint32_t g_me = 0;
  void* g_msgs = nullptr;

  // The match's peer (game thread).
  uint32_t g_opp = 0;
  Identity g_opp_id;
  Clock::time_point g_last_send, g_last_battle;
  int g_sends = 0;
  bool g_heard = false;
  // Our outgoing data. It's made when the textures are ready and sent in pieces as Steam takes them.
  Tx g_tx = kTxWait;
  std::vector<uint8_t> g_out;
  size_t g_out_at = 0;
  int g_tries = 0;
  Clock::time_point g_last_try;
  // The incoming data. These are the opponent's bytes, so sizes are checked before anything is copied.
  std::vector<uint8_t> g_in;
  uint32_t g_in_total = 0;
  size_t g_in_got = 0;
  uint32_t crc32(const uint8_t* d, size_t n) {  // by table, eight times quicker than bit by bit, since it runs on the game thread
    static const auto table = [] {
      std::array<uint32_t, 256> t{};
      for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
      }
      return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return ~c;
  }
  uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
  void le32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; i++) o.push_back(static_cast<uint8_t>(v >> (8 * i))); }
  int sendTyped(uint8_t type, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> m{'S', 'T', 'L', 'B', 1, type};
    m.insert(m.end(), body.begin(), body.end());
    return g_send(g_msgs, &g_opp_id, m.data(), static_cast<uint32_t>(m.size()), kReliable, kChannel);
  }
  void resetTransfer() {
    g_tx = kTxWait; g_out.clear(); g_out_at = 0; g_tries = 0;
    g_in.clear(); g_in_total = 0; g_in_got = 0;
  }
  std::mutex g_status_m;
  std::string g_status = "Off";

  void say(const std::string& s) { std::lock_guard<std::mutex> lk(g_status_m); g_status = s; }
  void saveSetting() {
    std::error_code ec;
    std::filesystem::create_directories(g_file.parent_path(), ec);  // a fresh install has no folder yet
    std::ofstream(g_file, std::ios::trunc) << (g_on ? "on" : "off") << "\n";
  }

  void sendHello() {
    const char text[] = "StriveLabs";
    uint8_t m[6 + sizeof(text) - 1];
    std::memcpy(m, "STLB", 4);
    m[4] = 1;  // protocol
    m[5] = kHello;
    std::memcpy(m + 6, text, sizeof(text) - 1);
    const int r = g_send(g_msgs, &g_opp_id, m, sizeof(m), kReliable, kChannel);
    g_sends++;
    g_last_send = Clock::now();
    if (g_sends == 1 || r != 1) COMBO_LOG("share: hello sent (try " + std::to_string(g_sends) + ", result " + std::to_string(r) + ")");  // 1 = OK
  }
  void closePeer(const char* why) {
    if (!g_opp) return;
    g_close(g_msgs, &g_opp_id);
    COMBO_LOG(std::string("share: session closed (") + why + ")");
    g_opp = 0;
    resetTransfer();  // let go of whatever was being sent or received
    say(g_on ? "Waiting for a match" : "Off");
  }
}

void init(const std::filesystem::path& ue4ss_dir) {
  g_file = ue4ss_dir / "StriveLabs_colors" / "share.txt";
  {
    std::ifstream f(g_file);
    std::string v;
    g_on = (f >> v) && v == "on";
  }
  HMODULE m = GetModuleHandleA("steam_api64.dll");  // Game Pass has none, so the feature stays off
  auto get = [&](const char* name) { return m ? GetProcAddress(m, name) : nullptr; };
  g_user_iface = reinterpret_cast<GetIface_t>(get("SteamAPI_SteamUser_v023"));
  g_get_id = reinterpret_cast<GetSteamID_t>(get("SteamAPI_ISteamUser_GetSteamID"));
  g_msgs_iface = reinterpret_cast<GetIface_t>(get("SteamAPI_SteamNetworkingMessages_SteamAPI_v002"));
  g_send = reinterpret_cast<Send_t>(get("SteamAPI_ISteamNetworkingMessages_SendMessageToUser"));
  g_receive = reinterpret_cast<Receive_t>(get("SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel"));
  g_accept = reinterpret_cast<Session_t>(get("SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser"));
  g_close = reinterpret_cast<Session_t>(get("SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser"));
  g_set_id = reinterpret_cast<SetId_t>(get("SteamAPI_SteamNetworkingIdentity_SetSteamID64"));
  g_release = reinterpret_cast<Release_t>(get("SteamAPI_SteamNetworkingMessage_t_Release"));
  const bool all = g_user_iface && g_get_id && g_msgs_iface && g_send && g_receive && g_accept && g_close && g_set_id && g_release;
  COMBO_LOG(all ? "share: Steam networking found" : "share: Steam networking not found, recolor sharing stays off");
  if (!all) { g_msgs_iface = nullptr; say("Not available without Steam"); return; }
  say(g_on ? "Waiting for a match" : "Off");
}

bool enabled() { return g_on; }
void setEnabled(bool on) {
  g_on = on;
  saveSetting();
  if (g_msgs_iface) say(on ? "Waiting for a match" : "Off");
  COMBO_LOG(std::string("share: turned ") + (on ? "on" : "off"));
}

uint32_t myAccount() {
  if (!g_msgs_iface) return 0;
  if (!g_me) {  // Steam is up by the time a fight runs, so it's asked then, on the game thread
    void* user = g_user_iface();
    const uint64_t id = user ? g_get_id(user) : 0;
    if ((id & 0xFFFFFFFF00000000ull) == kIndividual) g_me = static_cast<uint32_t>(id);
  }
  return g_me;
}
bool sharing() { return g_on && myAccount() != 0; }

extern bool g_taken;
void opponentFound(uint32_t account) {
  if (!sharing() || !account) return;
  if (account == g_me) { COMBO_LOG("share: the pattern read back is our own (a local match), no peer"); return; }
  if (!g_msgs) g_msgs = g_msgs_iface();
  if (!g_msgs) { COMBO_LOG("share: no networking messages interface"); return; }
  if (g_opp && g_opp != account) closePeer("another opponent");
  recolor_test::shareNoteSides(combo_trials::localSide());  // which fighter is ours, noted as round 1 starts
  if (g_opp == account) { g_tx = kTxWait; g_out.clear(); g_out_at = 0; g_tries = 0; g_taken = false; return; }  // a rematch: ours is sent again, and theirs may already be coming
  resetTransfer();
  g_taken = false;
  g_opp = account;
  g_opp_id = Identity{};
  g_set_id(&g_opp_id, kIndividual | account);
  g_accept(g_msgs, &g_opp_id);  // only them, so a stranger's session request just times out
  g_sends = 0;
  g_heard = false;
  g_last_battle = Clock::now();
  COMBO_LOG("share: the opponent runs StriveLabs with sharing on, connecting");
  say("Connecting...");
  sendHello();
}

std::atomic<bool> g_fighting{false};
bool g_taken = false;  // the opponent's recolor came this match, so another send of theirs is dropped
bool fightRunning() { return g_fighting; }
void update(bool battle_running) {
  g_fighting = battle_running;
  if (!g_msgs || !g_opp) return;
  const auto now = Clock::now();
  if (battle_running) g_last_battle = now;
  else if (now - g_last_battle > std::chrono::seconds(20)) { closePeer("the match is over"); return; }  // it stays between rounds and rematches
  if (!g_on) { closePeer("turned off"); return; }
  if (!g_heard && g_sends < 15 && now - g_last_send > std::chrono::seconds(1)) sendHello();
  void* got[8];
  const int n = g_receive(g_msgs, kChannel, got, 8);
  for (int i = 0; i < n && i < 8; i++) {
    const uint8_t* msg = static_cast<const uint8_t*>(got[i]);
    const uint8_t* data = *reinterpret_cast<const uint8_t* const*>(msg);
    const int size = *reinterpret_cast<const int*>(msg + 8);
    const uint64_t from = *reinterpret_cast<const uint64_t*>(msg + 24);  // the identity is at +16: type, size, then the SteamID
    if (from == (kIndividual | g_opp) && data && size >= 6 && size <= 512 * 1024 && std::memcmp(data, "STLB", 4) == 0 && data[4] == 1) {
      if (data[5] == kChunk && size >= 14) {
        const uint32_t total = le32(data + 6), at = le32(data + 10);
        const size_t n = static_cast<size_t>(size) - 14;
        if (total == 0 || total > kMaxPayload || g_taken || (g_in_total && g_in_total != total && at != 0)) { /* (too big, already taken, or not the send under way: dropped) */ }
        else {
          if (g_in_total != total) { g_in.assign(total, 0); g_in_total = total; g_in_got = 0; }
          else if (at == 0) g_in_got = 0;  // a new send of the same size is counted afresh
          if (at <= total && n <= total - at) { std::memcpy(g_in.data() + at, data + 14, n); g_in_got += n; }
        }
      } else if (data[5] == kEnd && size >= 14) {
        const uint32_t total = le32(data + 6), crc = le32(data + 10);
        std::string why;
        if (g_taken) why = "already taken this match";
        else if (total != g_in_total || g_in_got != total) why = "incomplete (" + std::to_string(g_in_got) + " of " + std::to_string(total) + " bytes)";
        else if (crc32(g_in.data(), g_in.size()) != crc) why = "damaged (checksum)";
        else if (recolor_test::shareTake(g_in, why)) { g_taken = true; COMBO_LOG("share: the opponent's recolor came: " + why); { std::string info = recolor_test::shareInfo(); const size_t c = info.find("Color"); if (c != std::string::npos) info = info.substr(0, c) + recolor_test::slotLabel(info.substr(c)); say("Showing their recolor: " + info); } why.clear(); }
        if (!why.empty()) { COMBO_LOG("share: the opponent's recolor not taken: " + why); say("Their recolor couldn't be used"); }
        g_in.clear(); g_in_total = 0; g_in_got = 0;
      } else if (data[5] == kNone) {
        COMBO_LOG("share: the opponent has no recolor for their slot");
        say("Connected. They have no recolor on.");
      } else if (data[5] == kHello && !g_heard) {
        g_heard = true;
        std::string text;
        for (int k = 6; k < size && k < 6 + 32; k++) text += (data[k] >= 32 && data[k] < 127) ? static_cast<char>(data[k]) : '?';  // theirs, printable characters only
        COMBO_LOG("share: hello from the opponent (" + text + ")");
        say("Connected");
        if (g_sends == 0) sendHello();
      }
    }
    g_release(got[i]);
  }
  if (g_heard && battle_running && g_tx == kTxWait && now - g_last_try > std::chrono::seconds(1)) {  // ours, once its textures are made
    g_last_try = now;
    std::string why;
    const int r = recolor_test::sharePayload(g_out, why);
    if (r == 0) { g_tx = kTxSending; g_out_at = 0; COMBO_LOG("share: sending our recolor (" + why + ")"); }
    else if (r == 2 || ++g_tries > 60) {
      g_tx = kTxDone;
      sendTyped(kNone, {});
      COMBO_LOG("share: nothing of ours sent: " + why);
    }
  }
  for (int k = 0; g_tx == kTxSending && k < 4; k++) {  // a few pieces per update, as Steam takes them
    if (g_out_at < g_out.size()) {
      const size_t n = (std::min)(kChunkSize, g_out.size() - g_out_at);
      std::vector<uint8_t> body;
      le32(body, static_cast<uint32_t>(g_out.size()));
      le32(body, static_cast<uint32_t>(g_out_at));
      body.insert(body.end(), g_out.begin() + g_out_at, g_out.begin() + g_out_at + n);
      const int r = sendTyped(kChunk, body);
      if (r != 1) { if (r != 25) COMBO_LOG("share: a piece didn't go (result " + std::to_string(r) + "), again next update"); break; }  // 25 means Steam's buffer is full
      g_out_at += n;
    } else {
      std::vector<uint8_t> body;
      le32(body, static_cast<uint32_t>(g_out.size()));
      le32(body, crc32(g_out.data(), g_out.size()));
      if (sendTyped(kEnd, body) != 1) break;
      g_tx = kTxDone;
      COMBO_LOG("share: our recolor sent (" + std::to_string(g_out.size() / 1024) + " KB)");
      g_out.clear();
    }
  }
  if (!g_heard && g_sends >= 15) {
    static uint32_t said = 0;
    if (said != g_opp) { said = g_opp; COMBO_LOG("share: no hello back after 15 tries"); say("No answer from the opponent"); }
  }
}

std::string status() { std::lock_guard<std::mutex> lk(g_status_m); return g_status; }
}  // namespace recolor_share
