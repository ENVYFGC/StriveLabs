#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace matpics {
  struct Entry { std::string color, value; };
  struct Row { std::string path_key, param; std::vector<Entry> by_color; };
  struct Option { int color = 0; std::vector<int> members; std::string path; bool known = true; };
  struct Pick {
    enum Kind { Own, Texture, Png, Unknown, Unresolved } kind = Unknown;
    std::string path, hash, why;
    int color = 0;
  };

  inline std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  }
  inline int colorNum(const std::string& folder) {
    return folder.size() > 5 && folder.rfind("Color", 0) == 0 && std::isdigit(static_cast<unsigned char>(folder[5])) ? std::atoi(folder.c_str() + 5) : 0;
  }
  inline bool parseLine(std::string line, Row& r) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::vector<std::string> c;
    for (size_t a = 0;;) {
      const size_t b = line.find('\t', a);
      c.push_back(line.substr(a, b == std::string::npos ? b : b - a));
      if (b == std::string::npos) break;
      a = b + 1;
    }
    if (c.size() < 4 || c[0].empty() || c[1].empty()) return false;
    r = Row{c[0], c[1], {}};
    for (size_t k = 2; k < c.size(); k++) {
      const size_t eq = c[k].find('=');
      if (eq == std::string::npos || eq == 0 || eq + 1 >= c[k].size() || !colorNum(c[k].substr(0, eq))) continue;
      r.by_color.push_back({c[k].substr(0, eq), c[k].substr(eq + 1)});
    }
    return r.by_color.size() >= 2;
  }
  inline std::string masterOf(const std::string& value) { return value.rfind("default@", 0) == 0 ? value.substr(8) : std::string(); }
  inline std::string masterKey(const std::string& master, const std::string& param) { return master + "|" + lower(param); }
  inline std::string keyOfPackage(const std::string& pkg) {
    const size_t c = pkg.find("/Chara/"), m = pkg.find("/Material/"), slash = pkg.rfind('/');
    if (c == std::string::npos || m == std::string::npos || slash == std::string::npos || m <= c + 7 || slash < m + 10) return {};
    return lower(pkg.substr(c + 7, m - c - 7) + "/" + pkg.substr(slash + 1));
  }
  inline std::string folderOfPackage(const std::string& pkg) {
    const size_t m = pkg.find("/Material/"), slash = pkg.rfind('/');
    if (m == std::string::npos || slash == std::string::npos || slash < m + 10) return {};
    return pkg.substr(m + 10, slash - m - 10);
  }
  inline std::string chrOfPackage(const std::string& pkg) {
    const size_t c = pkg.find("/Chara/"), m = pkg.find("/Material/");
    if (c == std::string::npos || m == std::string::npos || m <= c + 7) return {};
    const std::string rest = pkg.substr(c + 7, m - c - 7);
    const size_t s = rest.find('/');
    if (s == std::string::npos) return {};
    const std::string code = rest.substr(0, s), costume = rest.substr(s + 1);
    return costume.size() == 9 && costume.rfind("Costume", 0) == 0 && costume != "Costume01" ? code + "_C" + costume.substr(7) : code;
  }
  inline std::string valueOf(const Entry& e, const std::string& param, const std::map<std::string, std::string>& masters, bool& known) {
    known = true;
    const std::string m = masterOf(e.value);
    if (m.empty() && e.value != "default") return e.value;
    auto it = m.empty() ? masters.end() : masters.find(masterKey(m, param));
    if (it != masters.end() && !it->second.empty()) return it->second;
    known = false;
    return e.value;
  }
  inline std::vector<Option> options(const Row& r, const std::map<std::string, std::string>& masters) {
    std::vector<Option> out;
    for (const Entry& e : r.by_color) {
      bool known = true;
      const std::string v = valueOf(e, r.param, masters, known);
      auto o = std::find_if(out.begin(), out.end(), [&](const Option& x) { return x.path == v && x.known == known; });
      if (o == out.end()) { out.push_back(Option{0, {}, v, known}); o = out.end() - 1; }
      o->members.push_back(colorNum(e.color));
    }
    for (Option& o : out) {
      std::sort(o.members.begin(), o.members.end());
      o.color = o.members.front();
    }
    std::sort(out.begin(), out.end(), [](const Option& a, const Option& b) { return a.color < b.color; });
    return out;
  }
  inline const Option* optionWith(const std::vector<Option>& opts, int color) {
    for (const Option& o : opts)
      if (std::find(o.members.begin(), o.members.end(), color) != o.members.end()) return &o;
    return nullptr;
  }
  inline bool isPiece(float f) { return std::isfinite(f) && f >= 0.0f && f < 16777216.0f && f == std::floor(f); }
  inline std::string hashOf(const float v[4]) {
    if (!isPiece(v[1]) || !isPiece(v[2]) || !isPiece(v[3])) return {};
    char b[32];
    std::snprintf(b, sizeof b, "%06x%06x%06x", static_cast<unsigned>(v[1]), static_cast<unsigned>(v[2]), static_cast<unsigned>(v[3]));
    return b;
  }
  inline bool hashToValue(const std::string& hex, float v[4]) {
    if (hex.size() < 18) return false;
    for (int k = 0; k < 3; k++) {
      const std::string part = hex.substr(static_cast<size_t>(k) * 6, 6);
      if (part.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
      v[k + 1] = static_cast<float>(std::strtoul(part.c_str(), nullptr, 16));
    }
    v[0] = 0.0f;
    return true;
  }
  inline std::string num(float v) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e8f) return std::to_string(static_cast<long long>(v));
    std::ostringstream o;
    o << v;
    return o.str();
  }
  inline Pick resolve(const Row& r, const std::map<std::string, std::string>& masters, int slot_color, const float v[4]) {
    Pick p;
    if (!std::isfinite(v[0])) { p.why = "no stored value"; return p; }
    const int n = static_cast<int>(std::lround(v[0]));
    p.color = n;
    if (n == 0) {
      p.hash = hashOf(v);
      if (p.hash.empty()) { p.why = "the stored PNG id is broken"; return p; }
      p.kind = Pick::Png;
      return p;
    }
    const std::vector<Option> opts = options(r, masters);
    const Option* want = optionWith(opts, n);
    if (!want) { p.why = "color " + std::to_string(n) + " isn't one of this material's colors"; return p; }
    const Option* own = optionWith(opts, slot_color);
    p.path = want->known ? want->path : std::string();
    if (own == want) { p.kind = Pick::Own; return p; }
    if (!want->known) { p.kind = Pick::Unresolved; p.why = "color " + std::to_string(n) + "'s picture comes from " + masterOf(want->path) + ", which wasn't read"; return p; }
    p.kind = Pick::Texture;
    return p;
  }
  enum class Act { Nothing, Put, Restore };
  inline Act act(const Pick& p, bool changed_by_us) {
    if (p.kind == Pick::Own) return changed_by_us ? Act::Restore : Act::Nothing;
    if (p.kind == Pick::Texture || p.kind == Pick::Png) return Act::Put;
    return Act::Nothing;
  }
  inline bool importLine(const std::string& values, const std::string& rest, float out[4]) {
    if (rest.find("(unchanged)") != std::string::npos) return false;
    float v[4] = {};
    std::istringstream in(values);
    if (!(in >> v[0] >> v[1] >> v[2] >> v[3])) return false;
    if (!std::isfinite(v[0]) || v[0] < 0.0f || v[0] != std::floor(v[0])) return false;
    if (v[0] == 0.0f && hashOf(v).empty()) return false;
    if (v[0] != 0.0f) v[1] = v[2] = v[3] = 0.0f;
    for (int k = 0; k < 4; k++) out[k] = v[k];
    return true;
  }
  inline std::string ownPath(const Row& r, const std::map<std::string, std::string>& masters, int slot_color) {
    const std::vector<Option> opts = options(r, masters);
    const Option* own = optionWith(opts, slot_color);
    return own && own->known ? own->path : std::string();
  }
  inline std::string pngCopyPath(const std::string& mic_pkg, const std::string& src_tex_pkg, const std::string& param, const std::string& hash) {
    const size_t a = mic_pkg.rfind('/'), b = src_tex_pkg.rfind('/');
    if (a == std::string::npos || b == std::string::npos || hash.size() < 8) return {};
    std::string safe = param;
    for (char& c : safe)
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) c = '_';
    return mic_pkg.substr(0, a) + "/SL_baked/pic_" + mic_pkg.substr(a + 1) + "_" + safe + "_" + hash.substr(0, 8) + src_tex_pkg.substr(b);
  }
}
