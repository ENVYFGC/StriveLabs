#include "script_pak.h"
#include "bake_pkg.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace script_pak {
namespace {
  const uint8_t kFoot[4] = {0xC1, 0x83, 0x2A, 0x9E};
  constexpr size_t kHead = 0x34;

  uint32_t u32(const Bytes& b, size_t at) { uint32_t v = 0; std::memcpy(&v, b.data() + at, 4); return v; }
  int32_t i32(const Bytes& b, size_t at) { int32_t v = 0; std::memcpy(&v, b.data() + at, 4); return v; }
  std::string cstr(const Bytes& b, size_t at, size_t n) {
    size_t len = 0;
    while (len < n && b[at + len]) len++;
    return std::string(reinterpret_cast<const char*>(b.data() + at), len);
  }
  bool routineOp(int op) { return op == 8 || op == 15; }
  int endOf(int kind) { return kind == 0 ? 1 : routineOp(kind) ? 16 : -1; }

  struct Sizes {
    const short* s = nullptr;
    int max = 0;
    int of(int op) const { return op >= 0 && op < max && s[op] >= 4 ? s[op] : 0; }
  };

  bool payloadOf(const Bytes& u, Bytes& p, std::string& err) {
    if (u.size() < kHead + 8 || std::memcmp(u.data() + u.size() - 4, kFoot, 4) != 0) { err = "the script file doesn't end as expected"; return false; }
    const uint32_t n = u32(u, 0x24);
    if (n != u.size() - 0x38 || u32(u, 0x28) != n) { err = "the script file's size fields don't match its length"; return false; }
    p.assign(u.begin() + kHead, u.end() - 4);
    return true;
  }

  size_t blockEnd(const Bytes& b, size_t start, const Sizes& sz) {
    if (start + 36 > b.size()) return 0;
    const int end = endOf(i32(b, start));
    if (end < 0) return 0;
    for (size_t p = start; p + 4 <= b.size();) {
      const int op = i32(b, p), n = sz.of(op);
      if (!n || p + static_cast<size_t>(n) > b.size()) return 0;
      p += static_cast<size_t>(n);
      if (op == end) return p;
    }
    return 0;
  }
  bool formed(const Bytes& b, int kind, const std::string& name, const Sizes& sz, std::string& err) {
    if (b.size() < 40 || endOf(kind) < 0 || (kind == 0 ? i32(b, 0) != 0 : !routineOp(i32(b, 0))) || cstr(b, 4, 32) != name) { err = name + ": doesn't start as that block"; return false; }
    if (blockEnd(b, 0, sz) != b.size()) { err = name + ": doesn't end on its end line"; return false; }
    return true;
  }

  struct Piece { size_t end = 0; int kind = 0; std::string name; Bytes code; };

  bool wrap(const Bytes& u, const Bytes& a, const Bytes& P, Bytes& U, Bytes& A, std::string& err) {
    U.assign(u.begin(), u.begin() + kHead);
    U.insert(U.end(), P.begin(), P.end());
    U.insert(U.end(), kFoot, kFoot + 4);
    const uint32_t n = static_cast<uint32_t>(P.size());
    std::memcpy(U.data() + 0x24, &n, 4);
    std::memcpy(U.data() + 0x28, &n, 4);
    bake::Package pkg;
    std::string why;
    bake::Bytes outE;
    if (pkg.parse(a, u, why) && pkg.exports.size() == 1 && pkg.exportSize(0) == static_cast<int64_t>(u.size()) - 4 &&
        pkg.build(U, {static_cast<int64_t>(U.size()) - 4}, A, outE, why))
      return true;
    A = a;
    const int64_t pairs[2][2] = {{static_cast<int64_t>(u.size()) - 4, static_cast<int64_t>(U.size()) - 4},
                                 {static_cast<int64_t>(a.size() + u.size()) - 4, static_cast<int64_t>(a.size() + U.size()) - 4}};
    for (const auto& pr : pairs) {
      size_t found = 0, at = 0;
      for (size_t i = 0; i + 8 <= a.size(); i++) {
        int64_t v = 0;
        std::memcpy(&v, a.data() + i, 8);
        if (v == pr[0]) { found++; at = i; }
      }
      if (found != 1) { err = "the script's .uasset holds its size " + std::to_string(found) + " times, not once"; return false; }
      std::memcpy(A.data() + at, &pr[1], 8);
    }
    return true;
  }
}  // namespace

bool rebuild(const Bytes& uexp, const Bytes& uasset, const short* sizes, int max_op, const std::vector<Block>& swaps, const std::vector<LineEdit>& edits,
             Result& out, std::string& err) {
  out = Result{};
  const Sizes sz{sizes, max_op};
  if (!sizes) { err = "the game's instruction sizes aren't known"; return false; }
  Bytes P;
  if (!payloadOf(uexp, P, err)) return false;
  if (P.size() < 4) { err = "empty script"; return false; }
  const uint32_t count = u32(P, 0);
  const size_t code0 = 4 + static_cast<size_t>(count) * 36;
  if (count == 0 || count > 8000 || code0 > P.size()) { err = "the move table doesn't fit the file"; return false; }
  std::vector<std::string> names;
  std::vector<size_t> at;
  for (uint32_t i = 0; i < count; i++) {
    names.push_back(cstr(P, 4 + i * 36, 32));
    at.push_back(code0 + u32(P, 4 + i * 36 + 32));
    if (at.back() + 36 > P.size()) { err = "the move table's entry for " + names.back() + " points past the file"; return false; }
  }
  std::map<std::string, std::vector<size_t>> routines;
  bool routines_read = false;
  auto readRoutines = [&]() {
    if (routines_read) return;
    routines_read = true;
    for (size_t p = code0; p + 4 <= P.size();) {
      const int op = i32(P, p), n = sz.of(op);
      if (!n || p + static_cast<size_t>(n) > P.size()) break;
      if (routineOp(op) && n >= 36) routines[cstr(P, p + 4, 32)].push_back(p);
      p += static_cast<size_t>(n);
    }
  };
  auto locate = [&](const std::string& name, int kind, size_t& start, std::string& why) -> bool {
    if (kind <= 0) {
      for (uint32_t i = 0; i < count; i++) {
        if (names[i] != name) continue;
        if (i32(P, at[i]) != 0 || cstr(P, at[i] + 4, 32) != name) { why = name + ": its table entry doesn't point at it"; return false; }
        start = at[i];
        return true;
      }
      if (kind == 0) { why = name + ": not a move of this script file"; return false; }
    }
    readRoutines();
    auto r = routines.find(name);
    if (r == routines.end()) { why = name + ": not in this script file"; return false; }
    if (r->second.size() != 1) { why = name + ": this script file has " + std::to_string(r->second.size()) + " routines of that name"; return false; }
    start = r->second[0];
    return true;
  };
  std::map<size_t, Piece> pieces;
  auto touch = [&](const std::string& name, int kind, std::string& why) -> Piece* {
    size_t start = 0;
    if (!locate(name, kind, start, why)) return nullptr;
    auto it = pieces.find(start);
    if (it != pieces.end()) return &it->second;
    const size_t end = blockEnd(P, start, sz);
    if (!end) { why = name + ": its end wasn't found"; return nullptr; }
    for (const auto& [s, pc] : pieces)
      if (s < end && start < pc.end) { why = name + ": overlaps " + pc.name; return nullptr; }
    Piece pc;
    pc.end = end;
    pc.kind = i32(P, start);
    pc.name = name;
    pc.code.assign(P.begin() + static_cast<ptrdiff_t>(start), P.begin() + static_cast<ptrdiff_t>(end));
    return &pieces.emplace(start, std::move(pc)).first->second;
  };
  for (const Block& s : swaps) {
    std::string why;
    size_t start = 0;
    if (!locate(s.name, s.kind == 0 ? 0 : 15, start, why)) { out.left_out.push_back(why); continue; }
    if (!formed(s.code, i32(P, start), s.name, sz, why)) { out.left_out.push_back(why); continue; }
    const bool had = pieces.count(start) != 0;
    Piece* pc = touch(s.name, s.kind == 0 ? 0 : 15, why);
    if (!pc) { out.left_out.push_back(why); continue; }
    bool inside = false;
    for (uint32_t i = 0; i < count; i++) inside |= at[i] > start && at[i] < pc->end;
    if (inside && s.code.size() != pc->end - start) {
      if (!had) pieces.erase(start);
      out.left_out.push_back(s.name + ": another move starts inside it, so its length can't change");
      continue;
    }
    pc->code = s.code;
    out.blocks++;
  }
  for (const LineEdit& e : edits) {
    const std::string where = e.name + " +" + std::to_string(e.offset);
    std::string why;
    Piece* pc = touch(e.name, -1, why);
    if (!pc) { out.left_out.push_back(why); continue; }
    Bytes& c = pc->code;
    bool line = false;
    for (size_t p = 0; p + 4 <= c.size();) {
      const int n = sz.of(i32(c, p));
      if (!n) break;
      if (p == static_cast<size_t>(e.offset)) { line = true; break; }
      p += static_cast<size_t>(n);
    }
    if (e.offset < 0 || e.field < 0 || !line) { out.left_out.push_back(where + ": no line starts there"); continue; }
    const size_t off = static_cast<size_t>(e.offset), field = static_cast<size_t>(e.field);
    const int op = i32(c, off);
    const bool replaced = e.field == 0 && e.now.size() >= 4 && op == i32(e.now, 0);
    if (op != e.op && !replaced) { out.left_out.push_back(where + ": the line is another instruction"); continue; }
    if (e.orig.size() != e.now.size() || e.orig.empty() || field + e.orig.size() > static_cast<size_t>(sz.of(op)) || off + field + e.orig.size() > c.size()) {
      out.left_out.push_back(where + ": the edit doesn't fit the line");
      continue;
    }
    if (!e.sig.empty()) {
      const bool a = off + 4 + e.sig.size() <= c.size() && std::memcmp(c.data() + off + 4, e.sig.data(), e.sig.size()) == 0;
      const bool alt = e.sig_alt.size() == e.sig.size() && off + 4 + e.sig_alt.size() <= c.size() && std::memcmp(c.data() + off + 4, e.sig_alt.data(), e.sig_alt.size()) == 0;
      if (!a && !alt) { out.left_out.push_back(where + ": the sprite there is " + (off + 36 <= c.size() ? cstr(c, off + 4, 32) : std::string("another one"))); continue; }
    }
    const bool is_orig = std::memcmp(c.data() + off + field, e.orig.data(), e.orig.size()) == 0;
    const bool is_now = std::memcmp(c.data() + off + field, e.now.data(), e.now.size()) == 0;
    if (!is_orig && !is_now) out.notes.push_back(where + ": the file held another value there, the edit's value went in");
    const Bytes before(c.begin() + static_cast<ptrdiff_t>(off + field), c.begin() + static_cast<ptrdiff_t>(off + field + e.now.size()));
    std::memcpy(c.data() + off + field, e.now.data(), e.now.size());
    if (blockEnd(c, 0, sz) != c.size()) {
      std::memcpy(c.data() + off + field, before.data(), before.size());
      out.left_out.push_back(where + ": the block wouldn't end where it does");
      continue;
    }
    out.lines++;
  }
  Bytes N(P.begin(), P.begin() + static_cast<ptrdiff_t>(code0));
  size_t pos = code0;
  std::vector<std::pair<size_t, ptrdiff_t>> shifts;
  for (const auto& [start, pc] : pieces) {
    N.insert(N.end(), P.begin() + static_cast<ptrdiff_t>(pos), P.begin() + static_cast<ptrdiff_t>(start));
    N.insert(N.end(), pc.code.begin(), pc.code.end());
    shifts.push_back({start, static_cast<ptrdiff_t>(pc.code.size()) - static_cast<ptrdiff_t>(pc.end - start)});
    pos = pc.end;
  }
  N.insert(N.end(), P.begin() + static_cast<ptrdiff_t>(pos), P.end());
  for (uint32_t i = 0; i < count; i++) {
    ptrdiff_t move = 0;
    for (const auto& [start, d] : shifts) if (start < at[i]) move += d;
    const size_t now = static_cast<size_t>(static_cast<ptrdiff_t>(at[i]) + move);
    if (now + 36 > N.size() || i32(N, now) != i32(P, at[i]) || std::memcmp(N.data() + now + 4, P.data() + at[i] + 4, 32) != 0) {
      err = "the rebuilt script's table entry for " + names[i] + " doesn't point at it";
      return false;
    }
    const uint32_t rel = static_cast<uint32_t>(now - code0);
    std::memcpy(N.data() + 4 + i * 36 + 32, &rel, 4);
  }
  return wrap(uexp, uasset, N, out.uexp, out.uasset, err);
}
}  // namespace script_pak
