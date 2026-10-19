#include "recordings.h"
#include "sigscan.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

namespace recordings {
namespace {

// ---- notation --------------------------------------------------------------

// Slots store screen direction, so forward depends on the side.
uint16_t dirBits(char digit, int32_t side) {
  const bool faces_right = (side == kDirRight);
  const uint16_t fwd  = faces_right ? IF_Right : IF_Left;
  const uint16_t back = faces_right ? IF_Left  : IF_Right;
  switch (digit) {
    case '1': return IF_Down | back;
    case '2': return IF_Down;
    case '3': return IF_Down | fwd;
    case '4': return back;
    case '5': return 0;
    case '6': return fwd;
    case '7': return IF_Up | back;
    case '8': return IF_Up;
    case '9': return IF_Up | fwd;
    default:  return 0;
  }
}

// Screen bits -> numpad digit.
char dirDigit(uint16_t bits, int32_t side) {
  const bool faces_right = (side == kDirRight);
  const bool fwd  = (bits & (faces_right ? IF_Right : IF_Left))  != 0;
  const bool back = (bits & (faces_right ? IF_Left  : IF_Right)) != 0;
  const bool up   = (bits & IF_Up)   != 0;
  const bool down = (bits & IF_Down) != 0;
  if (up   && back) return '7';
  if (up   && fwd)  return '9';
  if (down && back) return '1';
  if (down && fwd)  return '3';
  if (up)           return '8';
  if (down)         return '2';
  if (back)         return '4';
  if (fwd)          return '6';
  return '5';
}

struct Btn { uint16_t bit; const char* name; };
constexpr Btn kButtons[] = {
  {IF_P, "P"}, {IF_K, "K"}, {IF_S, "S"}, {IF_H, "H"}, {IF_Dust, "D"},
  {IF_Dash, "Dash"}, {IF_RC, "RC"}, {IF_FD, "FD"}, {IF_Respect, "Respect"},
};

bool symbolBits(const std::string& sym, int32_t side, uint16_t& out, bool& is_dir) {
  if (sym.size() == 1 && sym[0] >= '1' && sym[0] <= '9') {
    out = dirBits(sym[0], side);
    is_dir = true;
    return true;
  }
  is_dir = false;
  std::string up;
  for (char c : sym) up += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (up == "P") { out = IF_P;    return true; }
  if (up == "K") { out = IF_K;    return true; }
  if (up == "S") { out = IF_S;    return true; }
  if (up == "H") { out = IF_H;    return true; }
  if (up == "D") { out = IF_Dust; return true; }
  if (up == "DASH")    { out = IF_Dash;    return true; }
  if (up == "RC")      { out = IF_RC;      return true; }
  if (up == "FD")      { out = IF_FD;      return true; }
  if (up == "RESPECT") { out = IF_Respect; return true; }
  return false;
}

std::string stripComment(const std::string& line) {
  const auto hash = line.find('#');
  std::string s = (hash == std::string::npos) ? line : line.substr(0, hash);
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Run of identical frames -> notation, as a delta from the previous frame.
// A transition token costs one frame, hence the (run_len - 1) wait.
std::string emitRun(uint16_t prev, uint16_t cur, size_t run_len, int32_t side) {
  std::vector<std::string> parts;

  const char pd = dirDigit(prev, side);
  const char cd = dirDigit(cur, side);
  if (pd != cd) {
    // A new held direction replaces the old, so only neutral needs a release.
    if (cd == '5') parts.push_back(std::string("]") + pd + "[");
    else           parts.push_back(std::string("[") + cd + "]");
  }
  for (const auto& b : kButtons) {
    const bool was = (prev & b.bit) != 0;
    const bool now = (cur  & b.bit) != 0;
    if (now && !was) parts.push_back(std::string("[") + b.name + "]");
    if (was && !now) parts.push_back(std::string("]") + b.name + "[");
  }

  std::string out;
  if (parts.empty()) {
    out = "W" + std::to_string(run_len);
  } else {
    for (size_t i = 0; i < parts.size(); i++) {
      if (i) out += "+";
      out += parts[i];
    }
    if (run_len > 1) out += " W" + std::to_string(run_len - 1);
  }
  return out;
}

std::string framesToNotation(const uint16_t* flags, int32_t n, int32_t side) {
  if (n <= 0) return {};
  std::string out;
  uint16_t prev = 0;
  int32_t i = 0, tokens_on_line = 0;
  while (i < n) {
    const uint16_t v = flags[i] & IF_Any;
    int32_t j = i + 1;
    while (j < n && (flags[j] & IF_Any) == v) j++;
    out += emitRun(prev, v, static_cast<size_t>(j - i), side);
    if (++tokens_on_line >= 6) { out += "\n"; tokens_on_line = 0; } else out += " ";
    prev = v; i = j;
  }
  if (!out.empty() && out.back() == ' ') out.pop_back();
  out += "\n";
  return out;
}

std::string slotToNotation(const MemorySlot& slot) {
  const int32_t n = slot.max_time;
  if (n <= 4) return {};

  std::string out;
  uint16_t prev = 0;
  int32_t i = 0;
  int tokens_on_line = 0;
  while (i < n) {
    // Mask off the training menu keys (Play/Memory/Select) that get recorded
    // alongside real inputs; they are not part of the sequence.
    const uint16_t v = slot.flags[i] & IF_Any;
    int32_t j = i + 1;
    while (j < n && (slot.flags[j] & IF_Any) == v) j++;

    out += emitRun(prev, v, static_cast<size_t>(j - i), slot.direction);
    if (++tokens_on_line >= 6) { out += "\n"; tokens_on_line = 0; }
    else                        out += " ";

    prev = v;
    i = j;
  }
  if (!out.empty() && out.back() == ' ') out.pop_back();
  out += "\n";
  return out;
}

}  // namespace

// The slot array is found by resolving the game's own accessor, not by
// scanning memory for the struct shape -- scanning cannot tell the live array
// apart from the ~11 identical spare copies of the save data the game keeps.
// Tail of GGST's slot getter:
//   48 8B 05 ?? ?? ?? ??   mov rax,[rip+disp]   ; save data
//   48 05 D4 84 02 00      add rax,0x284D4      ; training slots
// The "add rax,284D4" half is what makes the pattern unique.

MemorySlot* findSlots() {
  static uintptr_t* s_static = nullptr;
  static bool s_tried = false;

  if (!s_static && !s_tried) {
    s_tried = true;
    const uintptr_t hit = sigscan::get().scan(
        "\x48\x8B\x05\x00\x00\x00\x00\x48\x05\xD4\x84\x02\x00",
        "xxx????xxxxxx");
    if (hit) {
      const int32_t disp = *reinterpret_cast<const int32_t*>(hit + 3);
      s_static = reinterpret_cast<uintptr_t*>(hit + 7 + disp);
    }
  }
  if (!s_static) return nullptr;

  // Re-read each call; the save data moves between sessions.
  const uintptr_t save_data = *s_static;
  if (!save_data) return nullptr;
  return reinterpret_cast<MemorySlot*>(save_data + kTrainingSlotOffset);
}

// ---- parsing ---------------------------------------------------------------

ParseResult parseText(const std::string& text, int32_t side) {
  ParseResult r;

  std::vector<std::string> lines;
  {
    std::istringstream in(text);
    std::string raw;
    while (std::getline(in, raw)) {
      auto s = stripComment(raw);
      if (!s.empty()) lines.push_back(s);
    }
  }
  if (!lines.empty()) {
    const auto l = lower(lines.front());
    if (l.size() > 5 && l.compare(l.size() - 5, 5, ".json") == 0)
      lines.erase(lines.begin());
  }

  // Expand loops. Mixups are refused rather than collapsed to one option: a
  // slot holds a single fixed sequence, so picking one would silently
  // misrepresent the file.
  std::vector<std::string> flat;
  for (size_t i = 0; i < lines.size(); i++) {
    const auto l = lower(lines[i]);
    if (l.rfind("startmix", 0) == 0) {
      r.error = L"mixups are not supported - a slot holds one fixed sequence";
      return r;
    }
    if (l.rfind("startloop", 0) == 0) {
      int reps = 0;
      std::istringstream ls(lines[i].substr(9));
      ls >> reps;
      if (reps <= 0) { r.error = L"startloop needs a positive count"; return r; }

      std::vector<std::string> body;
      int depth = 1;
      for (i++; i < lines.size(); i++) {
        const auto b = lower(lines[i]);
        if (b.rfind("startloop", 0) == 0) depth++;
        else if (b.rfind("endloop", 0) == 0 && --depth == 0) break;
        body.push_back(lines[i]);
      }
      if (depth != 0) { r.error = L"startloop without endloop"; return r; }
      for (int n = 0; n < reps; n++)
        flat.insert(flat.end(), body.begin(), body.end());
      continue;
    }
    if (l.rfind("endloop", 0) == 0) { r.error = L"endloop without startloop"; return r; }
    flat.push_back(lines[i]);
  }

  // Tokenise. Every bare token costs one frame; W<n> costs n.
  uint16_t held = 0;
  for (const auto& line : flat) {
    std::istringstream ts(line);
    std::string tok;
    while (ts >> tok) {
      if ((tok[0] == 'W' || tok[0] == 'w') && tok.size() > 1 &&
          std::all_of(tok.begin() + 1, tok.end(),
                      [](unsigned char c) { return std::isdigit(c) != 0; })) {
        const int n = std::atoi(tok.c_str() + 1);
        for (int k = 0; k < n; k++) r.frames.push_back(held);
        continue;
      }

      uint16_t frame = held;
      size_t start = 0;
      while (start <= tok.size()) {
        const auto plus = tok.find('+', start);
        std::string part = tok.substr(start, plus == std::string::npos
                                                 ? std::string::npos
                                                 : plus - start);
        if (!part.empty()) {
          uint16_t bits = 0;
          bool is_dir = false;
          if (part.size() >= 3 && part.front() == '[' && part.back() == ']') {
            if (!symbolBits(part.substr(1, part.size() - 2), side, bits, is_dir)) {
              r.error = L"unknown symbol in hold"; return r;
            }
            if (is_dir) held &= ~static_cast<uint16_t>(0xF);
            held |= bits;
            frame = held;
          } else if (part.size() >= 3 && part.front() == ']' && part.back() == '[') {
            if (!symbolBits(part.substr(1, part.size() - 2), side, bits, is_dir)) {
              r.error = L"unknown symbol in release"; return r;
            }
            held &= ~bits;
            frame = held;
          } else {
            if (!symbolBits(part, side, bits, is_dir)) {
              r.error = L"unknown symbol"; return r;
            }
            frame |= bits;                  // tap: this frame only
          }
        }
        if (plus == std::string::npos) break;
        start = plus + 1;
      }
      r.frames.push_back(frame);
    }
  }

  if (r.frames.size() <= 4)
    r.error = L"too short - the game treats <=4 frames as an empty slot";
  else if (r.frames.size() > kMaxFrames)
    r.error = L"too long - max 2400 frames (40s)";
  return r;
}

ParseResult parseFile(const std::filesystem::path& path, int32_t side) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    ParseResult r;
    r.error = L"could not open file";
    return r;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return parseText(ss.str(), side);
}

// ---- writing ---------------------------------------------------------------

bool writeSlot(int slot_index, const std::vector<uint16_t>& frames,
               int32_t direction, uint8_t chara_id, std::wstring& error) {
  if (slot_index < 0 || slot_index >= kSlotCount) {
    error = L"slot index out of range";
    return false;
  }
  if (frames.size() <= 4 || frames.size() > kMaxFrames) {
    error = L"frame count out of range";
    return false;
  }
  auto* slots = findSlots();
  if (!slots) {
    error = L"could not locate the training slot array";
    return false;
  }

  MemorySlot& slot = slots[slot_index];
  // Real recordings stamp the character that made them. The field's exact
  // role is unclear, so preserve it rather than overwrite it.
  if (chara_id != kKeepCharaId) slot.chara_id = chara_id;
  slot._pad[0] = slot._pad[1] = slot._pad[2] = 0;
  slot.direction  = direction;
  slot.start_time = 0;                       // start right away
  std::memset(slot.flags, 0, sizeof(slot.flags));
  std::memcpy(slot.flags, frames.data(), frames.size() * sizeof(uint16_t));
  // Written last: the game may read this slot from another thread, and a
  // length set before the data would expose frames that are not there yet.
  slot.max_time = static_cast<int32_t>(frames.size());
  return true;
}

// ---- export ----------------------------------------------------------------

bool exportSlot(int slot_index, const std::filesystem::path& path,
                std::wstring& error) {
  if (slot_index < 0 || slot_index >= kSlotCount) {
    error = L"slot index out of range";
    return false;
  }
  auto* slots = findSlots();
  if (!slots) { error = L"could not locate the training slot array"; return false; }

  const MemorySlot& slot = slots[slot_index];
  if (slot.max_time <= 4) { error = L"slot is empty"; return false; }

  const std::string body = slotToNotation(slot);
  if (body.empty()) { error = L"nothing to export"; return false; }

  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) { error = L"could not write file"; return false; }

  out << "# exported from StriveLabs -- training slot " << (slot_index + 1) << "\n";
  out << "# " << slot.max_time << " frames, side "
      << (slot.direction == kDirLeft ? "2P" : "1P") << "\n";
  out << body;
  return out.good();
}

bool writeFramesToFile(const std::vector<uint16_t>& frames,
                       const std::filesystem::path& path,
                       int32_t direction, std::wstring& error) {
  if (frames.empty()) { error = L"nothing to write"; return false; }
  const std::string body = framesToNotation(frames.data(),
                                            static_cast<int32_t>(frames.size()), direction);
  if (body.empty()) { error = L"nothing to write"; return false; }
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) { error = L"could not write file"; return false; }
  out << "# captured from replay\n";
  out << "# " << frames.size() << " frames, side "
      << (direction == kDirLeft ? "2P" : "1P") << "\n";
  out << body;
  return out.good();
}

std::string suggestName(int slot_index) {
  if (slot_index < 0 || slot_index >= kSlotCount) return {};
  auto* slots = findSlots();
  if (!slots) return {};
  const MemorySlot& slot = slots[slot_index];
  if (slot.max_time <= 4) return {};

  // Frame count, then the first few distinct inputs. Filename-safe only.
  constexpr int kMaxTokens = 4;
  std::string name = std::to_string(slot.max_time) + "f";
  std::string prev_tok;
  int taken = 0;
  const int32_t n = slot.max_time;
  for (int32_t i = 0; i < n && taken < kMaxTokens; ) {
    const uint16_t v = slot.flags[i] & IF_Any;
    int32_t j = i + 1;
    while (j < n && (slot.flags[j] & IF_Any) == v) j++;
    i = j;
    if (v == 0) continue;

    std::string tok;
    const char d = dirDigit(v, slot.direction);
    if (d != '5') tok += d;
    for (const auto& b : kButtons)
      if (v & b.bit) tok += b.name;
    if (tok.empty() || tok == prev_tok) continue;

    name += "_" + tok;
    prev_tok = tok;
    taken++;
  }
  return name;
}

std::wstring describeSlot(int slot_index) {
  if (slot_index < 0 || slot_index >= kSlotCount) return L"";
  auto* slots = findSlots();
  if (!slots) return L"slots not found";
  const MemorySlot& slot = slots[slot_index];
  if (slot.max_time <= 4) return L"empty";
  return std::to_wstring(slot.max_time) + L"f  " +
         (slot.direction == kDirLeft ? L"2P" : L"1P");
}

// ---- live stream -----------------------------------------------------------

namespace {
  std::vector<uint16_t> g_stream;
  size_t                g_stream_pos = 0;
  bool                  g_stream_on  = false;
  bool                  g_stream_armed = false;
}

void streamArm(std::vector<uint16_t> frames) {
  g_stream = std::move(frames);
  g_stream_pos = 0;
  g_stream_on = false;
  g_stream_armed = !g_stream.empty();
}

void streamOnRoundStart() {
  if (!g_stream_armed) return;
  g_stream_pos = 0;
  g_stream_on = true;      // stays armed so it re-syncs on every round reset
}

bool streamArmed() { return g_stream_armed; }

void streamStart(std::vector<uint16_t> frames) {
  g_stream = std::move(frames);
  g_stream_pos = 0;
  g_stream_on = !g_stream.empty();
}

void streamStop() { g_stream_on = false; g_stream_armed = false; g_stream_pos = 0; g_stream.clear(); }
bool streamActive() { return g_stream_on; }
size_t streamPos() { return g_stream_pos; }
size_t streamLen() { return g_stream.size(); }

uint16_t streamNext() {
  if (!g_stream_on || g_stream_pos >= g_stream.size()) { g_stream_on = false; return 0; }
  return g_stream[g_stream_pos++];
}

// ---- files -----------------------------------------------------------------

std::vector<std::filesystem::path> scanDir(const std::filesystem::path& dir) {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) return out;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) break;
    if (!e.is_regular_file(ec)) continue;
    auto ext = lower(e.path().extension().string());
    if (ext == ".txt") out.push_back(e.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace recordings
