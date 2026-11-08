#include "bake_pkg.h"

#include <cstring>
#include <map>

namespace bake {
  namespace {
    constexpr uint32_t kTag = 0x9E2A83C1;

    struct Rd {
      const Bytes& b;
      size_t at;
      bool bad = false;
      bool need(size_t n) { if (at > b.size() || n > b.size() - at) { bad = true; return false; } return true; }  // (never wraps)
      int32_t i32() { if (!need(4)) return 0; int32_t v; std::memcpy(&v, b.data() + at, 4); at += 4; return v; }
      uint32_t u32() { return static_cast<uint32_t>(i32()); }
      int64_t i64() { if (!need(8)) return 0; int64_t v; std::memcpy(&v, b.data() + at, 8); at += 8; return v; }
      uint16_t u16() { if (!need(2)) return 0; uint16_t v; std::memcpy(&v, b.data() + at, 2); at += 2; return v; }
      uint8_t u8() { if (!need(1)) return 0; return b[at++]; }
      Bytes raw(size_t n) { if (!need(n)) return {}; Bytes v(b.begin() + static_cast<ptrdiff_t>(at), b.begin() + static_cast<ptrdiff_t>(at + n)); at += n; return v; }
      // an FString; `wide` set if it was UTF-16 (then only its ASCII survives in the result)
      std::string fstr(bool* wide = nullptr) {
        const int32_t n = i32();
        if (wide) *wide = n < 0;
        if (n == 0) return {};
        std::string s;
        if (n < 0) {
          if (!need(static_cast<size_t>(-n) * 2)) return {};
          for (int32_t k = 0; k < -n; k++) { const uint16_t c = static_cast<uint16_t>(b[at + 2 * k] | (b[at + 2 * k + 1] << 8)); if (c) s += c < 128 ? static_cast<char>(c) : '?'; }
          at += static_cast<size_t>(-n) * 2;
        } else {
          if (!need(static_cast<size_t>(n))) return {};
          s.assign(reinterpret_cast<const char*>(b.data() + at), strnlen(reinterpret_cast<const char*>(b.data() + at), static_cast<size_t>(n)));
          at += static_cast<size_t>(n);
        }
        return s;
      }
    };
    template <typename T> void put(Bytes& b, T v) { const auto* p = reinterpret_cast<const uint8_t*>(&v); b.insert(b.end(), p, p + sizeof v); }
    template <typename T> void putAt(Bytes& b, size_t at, T v) { std::memcpy(b.data() + at, &v, sizeof v); }
    Bytes fstrBytes(const std::string& s) {
      Bytes b;
      if (s.empty()) { put<int32_t>(b, 0); return b; }
      put<int32_t>(b, static_cast<int32_t>(s.size() + 1));
      b.insert(b.end(), s.begin(), s.end());
      b.push_back(0);
      return b;
    }

    // ---- the name hashes ----
    struct Tables {
      uint32_t crc[256], dep[256];
      Tables() {
        for (uint32_t i = 0; i < 256; i++) {
          uint32_t c = i;
          for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
          crc[i] = c;
          uint32_t d = i << 24;
          for (int k = 0; k < 8; k++) d = (d & 0x80000000u) ? (d << 1) ^ 0x04C11DB7u : d << 1;
          dep[i] = d;
        }
      }
    };
    const Tables& tables() { static const Tables t; return t; }
  }

  uint16_t hashNonCase(const std::string& s) {
    uint32_t h = 0;
    for (unsigned char ch : s) {
      if (ch >= 'a' && ch <= 'z') ch = static_cast<unsigned char>(ch - 32);
      h = ((h >> 8) & 0x00FFFFFFu) ^ tables().dep[(h ^ ch) & 0xFF];
    }
    return static_cast<uint16_t>(h & 0xFFFF);
  }

  uint16_t hashCase(const std::string& s) {
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char ch : s) {
      uint32_t c = ch;
      for (int k = 0; k < 4; k++) { crc = (crc >> 8) ^ tables().crc[(crc ^ c) & 0xFF]; c >>= 8; }
    }
    return static_cast<uint16_t>(~crc & 0xFFFF);
  }

  // ---- the package ----
  bool Package::parse(const Bytes& a, const Bytes& e, std::string& why) {
    uasset = a;
    uexp = e;
    names.clear(); imports.clear(); exports.clear(); offsets_.clear();
    Rd r{uasset, 0};
    if (r.u32() != kTag || r.i32() != -7) { why = "not a 4.25 package"; return false; }
    r.i32(); r.i32(); r.i32();
    if (r.i32() != 0) { why = "a versioned package"; return false; }
    p_total_ = r.at; total_ = r.i32();
    r.fstr();
    r.u32();
    p_names_ = r.at;
    const int32_t name_count = r.i32();
    name_off_ = r.i32();
    auto off = [&]() { const size_t at = r.at; const int32_t v = r.i32(); offsets_.emplace_back(at, v); };  // (where, then what)
    r.i32(); off();  // gatherable text
    const int32_t export_count = r.i32(); export_off_ = static_cast<int32_t>(0); off(); export_off_ = offsets_.back().second;
    const int32_t import_count = r.i32(); off(); import_off_ = offsets_.back().second;
    off();           // depends
    r.i32(); off();  // soft package references
    off();           // searchable names
    off();           // thumbnails
    r.raw(16);
    gens_ = r.i32();
    p_gens_ = r.at;
    r.raw(static_cast<size_t>(gens_) * 8);
    for (int k = 0; k < 2; k++) { r.u16(); r.u16(); r.u16(); r.u32(); r.fstr(); }
    r.u32();
    if (r.i32() != 0) { why = "compressed chunks"; return false; }
    r.u32();
    const int32_t extra = r.i32();
    for (int32_t k = 0; k < extra && !r.bad; k++) r.fstr();
    off();  // asset registry
    p_bulk_ = r.at; r.i64();
    off();  // world tile info
    const int32_t chunks = r.i32();
    r.raw(static_cast<size_t>(chunks) * 4);
    r.i32(); off();  // preload dependencies
    if (r.bad || static_cast<int32_t>(r.at) != name_off_) { why = "summary layout"; return false; }
    for (int32_t k = 0; k < name_count && !r.bad; k++) {
      bool wide = false;
      NameEntry n;
      n.s = r.fstr(&wide);
      n.h1 = r.u16();
      n.h2 = r.u16();
      if (wide) { why = "a UTF-16 name"; return false; }
      names.push_back(n);
    }
    names_end_ = static_cast<int32_t>(r.at);
    r.at = static_cast<size_t>(import_off_);
    for (int32_t k = 0; k < import_count && !r.bad; k++) {
      std::array<int32_t, 7> m{};
      for (int32_t& v : m) v = r.i32();
      imports.push_back(m);
    }
    for (int32_t k = 0; k < export_count; k++) {
      const size_t at = static_cast<size_t>(export_off_) + 104 * static_cast<size_t>(k);
      if (at + 104 > uasset.size()) { why = "exports"; return false; }
      std::array<uint8_t, 104> x{};
      std::memcpy(x.data(), uasset.data() + at, 104);
      exports.push_back(x);
    }
    if (r.bad) { why = "truncated"; return false; }
    if (uexp.size() < 4 || std::memcmp(uexp.data() + uexp.size() - 4, &kTag, 4) != 0) { why = ".uexp end tag"; return false; }
    for (const auto& imp : imports)
      for (int k : {0, 2, 5}) if (imp[k] < 0 || imp[k] >= static_cast<int32_t>(names.size())) { why = "import name index"; return false; }
    return true;
  }

  int32_t Package::nameIndex(const std::string& s, bool add) {
    for (size_t i = 0; i < names.size(); i++) if (names[i].s == s) return static_cast<int32_t>(i);
    if (!add) return -1;
    names.push_back({s, hashNonCase(s), hashCase(s)});
    return static_cast<int32_t>(names.size() - 1);
  }

  int64_t Package::exportSize(size_t i) const { int64_t v; std::memcpy(&v, exports[i].data() + 28, 8); return v; }
  int64_t Package::exportOffset(size_t i) const { int64_t v; std::memcpy(&v, exports[i].data() + 36, 8); return v; }

  bool Package::build(Bytes& outA, Bytes& outE, std::string& why) const {
    std::vector<int64_t> sizes;
    for (size_t i = 0; i < exports.size(); i++) sizes.push_back(exportSize(i));
    return build(uexp, sizes, outA, outE, why);
  }

  bool Package::build(const Bytes& newUexp, const std::vector<int64_t>& sizes, Bytes& outA, Bytes& outE, std::string& why) const {
    Bytes nm;
    for (const NameEntry& n : names) {
      const Bytes s = fstrBytes(n.s);
      nm.insert(nm.end(), s.begin(), s.end());
      put<uint16_t>(nm, n.h1);
      put<uint16_t>(nm, n.h2);
    }
    const int32_t delta = static_cast<int32_t>(nm.size()) - (names_end_ - name_off_);
    Bytes out(uasset.begin(), uasset.begin() + name_off_);
    out.insert(out.end(), nm.begin(), nm.end());
    out.insert(out.end(), uasset.begin() + names_end_, uasset.end());
    putAt<int32_t>(out, p_total_, total_ + delta);
    putAt<int32_t>(out, p_names_, static_cast<int32_t>(names.size()));
    for (const auto& [at, v] : offsets_) if (v > name_off_) putAt<int32_t>(out, at, v + delta);
    if (gens_ > 0) putAt<int32_t>(out, p_gens_ + 8 * static_cast<size_t>(gens_ - 1) + 4, static_cast<int32_t>(names.size()));
    if (sizes.size() != exports.size()) { why = "export sizes"; return false; }
    int64_t pos = total_ + delta;
    for (size_t i = 0; i < exports.size(); i++) {
      const size_t at = static_cast<size_t>(export_off_ + delta) + 104 * i;
      putAt<int64_t>(out, at + 28, sizes[i]);
      putAt<int64_t>(out, at + 36, pos);
      pos += sizes[i];
    }
    if (pos + 4 != total_ + delta + static_cast<int64_t>(newUexp.size())) { why = "export sizes don't add up to the .uexp"; return false; }
    putAt<int64_t>(out, p_bulk_, total_ + delta + static_cast<int64_t>(newUexp.size()) - 4);
    outA = std::move(out);
    outE = newUexp;
    return true;
  }

  // ---- properties ----
  namespace {
    const std::map<std::string, size_t>& binaryStructs() {
      static const std::map<std::string, size_t> m = {{"LinearColor", 16}, {"Guid", 16}, {"Vector", 12}, {"Color", 4}, {"Vector2D", 8},
                                                      {"Rotator", 12}, {"Quat", 16}, {"Vector4", 16}, {"IntPoint", 8}};
      return m;
    }
    bool rdName(Rd& r, const Package& p, std::string& s, int32_t& num) {
      const int32_t i = r.i32();
      num = r.i32();
      if (r.bad || i < 0 || i >= static_cast<int32_t>(p.names.size())) return false;
      s = p.name(i);
      return true;
    }
    void wrName(Package& p, Bytes& b, const std::string& s, int32_t num = 0) {
      put<int32_t>(b, p.nameIndex(s, true));
      put<int32_t>(b, num);
    }
    bool readValue(Package& p, Rd& r, Prop& pr, size_t end, std::string& why);
  }

  bool readProps(Package& p, const Bytes& b, size_t& at, size_t end, std::vector<Prop>& out, std::string& why) {
    Rd r{b, at};
    for (;;) {
      Prop pr;
      int32_t dummy = 0;
      if (!rdName(r, p, pr.name, pr.num)) { why = "property name"; return false; }
      if (pr.name == "None") { at = r.at; return true; }
      if (!rdName(r, p, pr.type, dummy)) { why = "property type"; return false; }
      const int32_t size = r.i32();
      pr.aidx = r.i32();
      if (pr.type == "StructProperty") { if (!rdName(r, p, pr.structName, dummy)) return false; pr.sguid = r.raw(16); }
      else if (pr.type == "BoolProperty") pr.boolv = r.u8();
      else if (pr.type == "ByteProperty" || pr.type == "EnumProperty") { if (!rdName(r, p, pr.enumName, dummy)) return false; }
      else if (pr.type == "ArrayProperty" || pr.type == "SetProperty") { if (!rdName(r, p, pr.inner, dummy)) return false; }
      else if (pr.type == "MapProperty") { if (!rdName(r, p, pr.inner, dummy) || !rdName(r, p, pr.inner2, dummy)) return false; }
      pr.hasGuid = r.u8();
      if (pr.hasGuid) pr.pguid = r.raw(16);
      if (r.bad || size < 0 || r.at + static_cast<size_t>(size) > end) { why = "property " + pr.name + " size"; return false; }
      const size_t vend = r.at + static_cast<size_t>(size);
      if (!readValue(p, r, pr, vend, why)) return false;
      if (r.at != vend) { why = "property " + pr.name + " length"; return false; }
      out.push_back(std::move(pr));
    }
  }

  namespace {
    bool readValue(Package& p, Rd& r, Prop& pr, size_t end, std::string& why) {
      Value& v = pr.value;
      if (pr.type == "StructProperty") {
        if (binaryStructs().count(pr.structName)) { v.kind = Value::Raw; v.raw = r.raw(end - r.at); return !r.bad; }
        v.kind = Value::Props;
        return readProps(p, r.b, r.at, end, v.props, why);
      }
      if (pr.type == "ArrayProperty") {
        v.count = r.i32();
        if (pr.inner == "StructProperty") {
          auto it = std::make_shared<Prop>();
          int32_t dummy = 0;
          if (!rdName(r, p, it->name, it->num) || !rdName(r, p, it->type, dummy)) { why = "array tag"; return false; }
          r.i32();
          it->aidx = r.i32();
          if (!rdName(r, p, it->structName, dummy)) { why = "array tag"; return false; }
          it->sguid = r.raw(16);
          it->hasGuid = r.u8();
          if (it->hasGuid) it->pguid = r.raw(16);
          v.kind = Value::StructArray;
          v.innerTag = it;
          const auto bs = binaryStructs().find(it->structName);
          for (int32_t k = 0; k < v.count; k++) {
            Value e;
            if (bs != binaryStructs().end()) { e.kind = Value::Raw; e.raw = r.raw(bs->second); }
            else { e.kind = Value::Props; if (!readProps(p, r.b, r.at, end, e.props, why)) return false; }
            v.elems.push_back(std::move(e));
          }
          return !r.bad;
        }
        v.kind = Value::Array;
        v.raw = r.raw(end - r.at);
        return !r.bad;
      }
      if (pr.type == "NameProperty") { v.kind = Value::Name; return rdName(r, p, v.name.first, v.name.second); }
      if ((pr.type == "ByteProperty" || pr.type == "EnumProperty") && pr.enumName != "None" && end - r.at == 8) {
        v.kind = Value::EnumName;
        return rdName(r, p, v.name.first, v.name.second);
      }
      v.kind = Value::Raw;
      v.raw = r.raw(end - r.at);
      return !r.bad;
    }

    Bytes writeValue(Package& p, const Value& v);
    Bytes writeProp(Package& p, const Prop& pr) {
      const Bytes val = writeValue(p, pr.value);
      Bytes h;
      wrName(p, h, pr.name, pr.num);
      wrName(p, h, pr.type);
      put<int32_t>(h, static_cast<int32_t>(val.size()));
      put<int32_t>(h, pr.aidx);
      if (pr.type == "StructProperty") { wrName(p, h, pr.structName); h.insert(h.end(), pr.sguid.begin(), pr.sguid.end()); }
      else if (pr.type == "BoolProperty") h.push_back(pr.boolv);
      else if (pr.type == "ByteProperty" || pr.type == "EnumProperty") wrName(p, h, pr.enumName);
      else if (pr.type == "ArrayProperty" || pr.type == "SetProperty") wrName(p, h, pr.inner);
      else if (pr.type == "MapProperty") { wrName(p, h, pr.inner); wrName(p, h, pr.inner2); }
      h.push_back(pr.hasGuid);
      if (pr.hasGuid) h.insert(h.end(), pr.pguid.begin(), pr.pguid.end());
      h.insert(h.end(), val.begin(), val.end());
      return h;
    }
    Bytes writeValue(Package& p, const Value& v) {
      Bytes b;
      switch (v.kind) {
        case Value::Raw: return v.raw;
        case Value::Props: return writeProps(p, v.props);
        case Value::Name:
        case Value::EnumName: wrName(p, b, v.name.first, v.name.second); return b;
        case Value::Array: put<int32_t>(b, v.count); b.insert(b.end(), v.raw.begin(), v.raw.end()); return b;
        case Value::StructArray: {
          Bytes body;
          for (const Value& e : v.elems) {
            const Bytes x = e.kind == Value::Raw ? e.raw : writeProps(p, e.props);
            body.insert(body.end(), x.begin(), x.end());
          }
          const Prop& it = *v.innerTag;
          put<int32_t>(b, static_cast<int32_t>(v.elems.size()));
          wrName(p, b, it.name, it.num);
          wrName(p, b, it.type);
          put<int32_t>(b, static_cast<int32_t>(body.size()));
          put<int32_t>(b, it.aidx);
          wrName(p, b, it.structName);
          b.insert(b.end(), it.sguid.begin(), it.sguid.end());
          b.push_back(it.hasGuid);
          if (it.hasGuid) b.insert(b.end(), it.pguid.begin(), it.pguid.end());
          b.insert(b.end(), body.begin(), body.end());
          return b;
        }
      }
      return b;
    }
  }

  Bytes writeProps(Package& p, const std::vector<Prop>& props) {
    Bytes b;
    for (const Prop& pr : props) { const Bytes x = writeProp(p, pr); b.insert(b.end(), x.begin(), x.end()); }
    wrName(p, b, "None");
    return b;
  }

  bool parseExport(Package& p, std::vector<Prop>& props, Bytes& tail, size_t& start, size_t& size, std::string& why) {
    return parseExport(p, 0, props, tail, start, size, why);
  }
  bool parseExport(Package& p, size_t index, std::vector<Prop>& props, Bytes& tail, size_t& start, size_t& size, std::string& why) {
    if (index >= p.exports.size()) { why = "no export " + std::to_string(index); return false; }
    const int64_t off = p.exportOffset(index) - p.total(), sz = p.exportSize(index);
    if (off < 0 || sz < 0 || static_cast<size_t>(off + sz) > p.uexp.size()) { why = "export place"; return false; }
    start = static_cast<size_t>(off);
    size = static_cast<size_t>(sz);
    size_t at = start;
    props.clear();
    if (!readProps(p, p.uexp, at, start + size, props, why)) return false;
    tail.assign(p.uexp.begin() + static_cast<ptrdiff_t>(at), p.uexp.begin() + static_cast<ptrdiff_t>(start + size));
    return true;
  }

  // ---- the edits ----
  namespace {
    Prop* field(std::vector<Prop>& props, const char* name) {
      for (Prop& x : props) if (x.name == name) return &x;
      return nullptr;
    }
    const std::string* paramName(std::vector<Prop>& elem) {
      Prop* info = field(elem, "ParameterInfo");
      if (!info || info->value.kind != Value::Props) return nullptr;
      Prop* n = field(info->value.props, "Name");
      return n && n->value.kind == Value::Name ? &n->value.name.first : nullptr;
    }
    // Export `index` written again from its properties (the others as they are; exports lie in order in .uexp)
    bool rebuildExport(Package& p, size_t index, const std::vector<Prop>& props, const Bytes& tail, size_t start, size_t size, Bytes& outA,
                       Bytes& outE, std::string& why) {
      std::vector<int64_t> sizes;
      int64_t pos = p.total();
      for (size_t i = 0; i < p.exports.size(); i++) {
        if (p.exportOffset(i) != pos) { why = "exports not in order"; return false; }
        pos += p.exportSize(i);
        sizes.push_back(p.exportSize(i));
      }
      Bytes body = writeProps(p, props);
      body.insert(body.end(), tail.begin(), tail.end());
      Bytes uexp(p.uexp.begin(), p.uexp.begin() + static_cast<ptrdiff_t>(start));
      uexp.insert(uexp.end(), body.begin(), body.end());
      uexp.insert(uexp.end(), p.uexp.begin() + static_cast<ptrdiff_t>(start + size), p.uexp.end());
      sizes[index] = static_cast<int64_t>(body.size());
      return p.build(uexp, sizes, outA, outE, why);
    }
    bool rebuildExport(Package& p, const std::vector<Prop>& props, const Bytes& tail, size_t start, size_t size, Bytes& outA, Bytes& outE,
                       std::string& why) {
      if (p.exports.size() != 1) { why = "more than one export"; return false; }
      return rebuildExport(p, 0, props, tail, start, size, outA, outE, why);
    }
  }

  bool setVector(Package& mic, const std::string& param, const float rgba[4], const uint8_t guid[16], Bytes& outA, Bytes& outE,
                 bool& added, std::string& why) {
    std::vector<Prop> props;
    Bytes tail;
    size_t start = 0, size = 0;
    if (!parseExport(mic, props, tail, start, size, why)) return false;
    Prop* arr = field(props, "VectorParameterValues");
    if (!arr) {  // None yet: made as the texture (or scalar) list is, with one LinearColor element
      const Prop* like = field(props, "TextureParameterValues");
      if (!like) like = field(props, "ScalarParameterValues");
      if (!like || like->value.kind != Value::StructArray || like->value.elems.empty() || like->value.elems[0].kind != Value::Props) {
        why = "no parameter list to model a color list on";
        return false;
      }
      Prop list = *like;
      list.name = "VectorParameterValues";
      list.num = 0;
      auto tag = std::make_shared<Prop>(*like->value.innerTag);
      tag->name = "VectorParameterValues";
      tag->structName = "VectorParameterValue";
      list.value.innerTag = tag;
      Value e = like->value.elems[0];
      Prop* pv = field(e.props, "ParameterValue");
      Prop* pg = field(e.props, "ExpressionGUID");
      if (!field(e.props, "ParameterInfo") || !paramName(e.props) || !pv || !pg || pg->value.raw.size() != 16) { why = "element shape"; return false; }
      pv->type = "StructProperty";
      pv->structName = "LinearColor";
      pv->sguid.assign(16, 0);
      pv->inner.clear();
      pv->enumName.clear();
      pv->hasGuid = 0;
      pv->pguid.clear();
      pv->value = Value{};
      pv->value.kind = Value::Raw;
      pv->value.raw.assign(16, 0);
      list.value.elems.clear();
      list.value.elems.push_back(std::move(e));
      list.value.count = 1;
      // after the scalar list (the order the engine writes them in), else first
      size_t at = 0;
      for (size_t i = 0; i < props.size(); i++) if (props[i].name == "ScalarParameterValues") at = i + 1;
      props.insert(props.begin() + static_cast<ptrdiff_t>(at), std::move(list));
      arr = field(props, "VectorParameterValues");
      // its one element is the model's: it becomes this parameter
      Value& only = arr->value.elems[0];
      field(field(only.props, "ParameterInfo")->value.props, "Name")->value.name = {param, 0};
      std::memcpy(field(only.props, "ParameterValue")->value.raw.data(), rgba, 16);
      field(only.props, "ExpressionGUID")->value.raw.assign(guid, guid + 16);
      added = true;
      return rebuildExport(mic, props, tail, start, size, outA, outE, why);
    }
    if (arr->value.kind != Value::StructArray || arr->value.elems.empty()) { why = "a color list of another shape"; return false; }
    Bytes color(16);
    std::memcpy(color.data(), rgba, 16);
    added = true;
    for (Value& e : arr->value.elems) {
      const std::string* n = e.kind == Value::Props ? paramName(e.props) : nullptr;
      if (n && *n == param) {
        Prop* pv = field(e.props, "ParameterValue");
        if (!pv || pv->value.raw.size() != 16) { why = "parameter value"; return false; }
        pv->value.raw = color;
        added = false;
        break;
      }
    }
    if (added) {
      Value e = arr->value.elems[0];  // (the first element's shape, with this name, value and GUID)
      if (e.kind != Value::Props || !paramName(e.props)) { why = "element shape"; return false; }
      Prop* info = field(e.props, "ParameterInfo");
      field(info->value.props, "Name")->value.name = {param, 0};
      Prop* pv = field(e.props, "ParameterValue");
      Prop* pg = field(e.props, "ExpressionGUID");
      if (!pv || !pg || pv->value.raw.size() != 16 || pg->value.raw.size() != 16) { why = "element fields"; return false; }
      pv->value.raw = color;
      pg->value.raw.assign(guid, guid + 16);
      arr->value.elems.push_back(std::move(e));
    }
    return rebuildExport(mic, props, tail, start, size, outA, outE, why);
  }

  bool repoint(Package& mic, const std::string& oldPath, const std::string& newPath, Bytes& outA, Bytes& outE, std::string& why) {
    int hit = -1, hits = 0;
    for (size_t k = 0; k < mic.imports.size(); k++) if (mic.name(mic.imports[k][5]) == oldPath) { hit = static_cast<int>(k); hits++; }
    if (hits != 1) { why = std::to_string(hits) + " imports of " + oldPath; return false; }
    const int32_t ni = mic.nameIndex(newPath, true);
    if (!mic.build(outA, outE, why)) return false;
    const int64_t delta = static_cast<int64_t>(outA.size()) - static_cast<int64_t>(mic.uasset.size());
    const size_t at = static_cast<size_t>(mic.importOffset() + delta) + 28 * static_cast<size_t>(hit) + 20;
    putAt<int32_t>(outA, at, ni);
    return true;
  }

  bool renameTexture(Package& tex, const std::string& newPath, Bytes& outA, Bytes& outE, std::string& why) {
    if (tex.exports.empty()) { why = "no export"; return false; }
    int32_t objName;
    std::memcpy(&objName, tex.exports[0].data() + 16, 4);
    if (objName < 0 || objName >= static_cast<int32_t>(tex.names.size())) { why = "export name"; return false; }
    const std::string& obj = tex.name(objName);
    int own = -1;
    for (size_t i = 0; i < tex.names.size(); i++) {
      const std::string& s = tex.names[i].s;
      if (s.rfind("/Game/", 0) == 0 && s.size() > obj.size() && s.compare(s.size() - obj.size() - 1, std::string::npos, "/" + obj) == 0) own = static_cast<int>(i);
    }
    if (own < 0) { why = "its own path isn't in its names"; return false; }
    if (newPath.size() <= obj.size() || newPath.compare(newPath.size() - obj.size() - 1, std::string::npos, "/" + obj) != 0) { why = "the object name must stay"; return false; }
    tex.names[static_cast<size_t>(own)] = {newPath, hashNonCase(newPath), hashCase(newPath)};
    return tex.build(outA, outE, why);
  }

  std::vector<float> ratioed(const std::vector<float>& orig, const float rgb[3]) {  // (as the editor does: each RGB in the ratio, its total kept)
    std::vector<float> out = orig;
    const float sum = rgb[0] + rgb[1] + rgb[2];
    for (size_t i = 0; i + 2 < out.size(); i += 3) {
      const float total = orig[i] + orig[i + 1] + orig[i + 2];
      for (int c = 0; c < 3; c++) out[i + c] = sum > 0.0f ? total * rgb[c] / sum : 0.0f;
    }
    return out;
  }

  bool setEffectColor(Package& ptc, const std::string& label, const float rgb[3], Bytes& outA, Bytes& outE, std::string& why) {
    // the palette's export: ColorTable [{Name, ColorOverLife (an export of this package, 1-based)}]
    int32_t module = 0;
    for (size_t i = 0; i < ptc.exports.size() && !module; i++) {
      std::vector<Prop> props;
      Bytes tail;
      size_t start = 0, size = 0;
      if (!parseExport(ptc, i, props, tail, start, size, why)) return false;
      Prop* table = field(props, "ColorTable");
      if (!table || table->value.kind != Value::StructArray) continue;
      for (Value& e : table->value.elems) {
        if (e.kind != Value::Props) continue;
        Prop* n = field(e.props, "Name");
        Prop* m = field(e.props, "ColorOverLife");
        if (n && n->value.kind == Value::Name && n->value.name.first == label && m && m->value.raw.size() == 4) std::memcpy(&module, m->value.raw.data(), 4);
      }
    }
    if (module <= 0 || module > static_cast<int32_t>(ptc.exports.size())) { why = "no effect color " + label; return false; }
    const size_t mi = static_cast<size_t>(module - 1);
    std::vector<Prop> props;
    Bytes tail;
    size_t start = 0, size = 0;
    if (!parseExport(ptc, mi, props, tail, start, size, why)) return false;
    Prop* col = field(props, "ColorOverLife");
    Prop* tab = col && col->value.kind == Value::Props ? field(col->value.props, "Table") : nullptr;
    Prop* vals = tab && tab->value.kind == Value::Props ? field(tab->value.props, "Values") : nullptr;
    if (!vals || vals->value.kind != Value::Array || vals->inner != "FloatProperty" || vals->value.count <= 0 || vals->value.count % 3 ||
        vals->value.raw.size() != static_cast<size_t>(vals->value.count) * 4) {
      why = label + ": not an RGB table";
      return false;
    }
    // laid out as the editor reads it: the table, then MinValueVec, MaxValueVec (zero when the file leaves them out)
    const size_t n = static_cast<size_t>(vals->value.count);
    std::vector<float> orig(n + 6, 0.0f);
    std::memcpy(orig.data(), vals->value.raw.data(), n * 4);
    Prop* vmin = field(col->value.props, "MinValueVec");
    Prop* vmax = field(col->value.props, "MaxValueVec");
    if (vmin && vmin->value.raw.size() == 12) std::memcpy(orig.data() + n, vmin->value.raw.data(), 12);
    if (vmax && vmax->value.raw.size() == 12) std::memcpy(orig.data() + n + 3, vmax->value.raw.data(), 12);
    const std::vector<float> out = ratioed(orig, rgb);
    std::memcpy(vals->value.raw.data(), out.data(), n * 4);
    if (vmin && vmin->value.raw.size() == 12) std::memcpy(vmin->value.raw.data(), out.data() + n, 12);
    if (vmax && vmax->value.raw.size() == 12) std::memcpy(vmax->value.raw.data(), out.data() + n + 3, 12);
    return rebuildExport(ptc, mi, props, tail, start, size, outA, outE, why);
  }

  bool verifyPackage(const Bytes& a, const Bytes& e, std::string& why) {
    Package p;
    if (!p.parse(a, e, why)) return false;
    for (const NameEntry& n : p.names)
      if (n.h1 != hashNonCase(n.s) || n.h2 != hashCase(n.s)) { why = "name hash of " + n.s; return false; }
    for (size_t i = 0; i < p.exports.size(); i++) {  // (every export's properties)
      std::vector<Prop> props;
      Bytes tail;
      size_t start = 0, size = 0;
      if (!parseExport(p, i, props, tail, start, size, why)) return false;
      const size_t n = p.names.size();
      Bytes again = writeProps(p, props);
      again.insert(again.end(), tail.begin(), tail.end());
      if (p.names.size() != n || again.size() != size || std::memcmp(again.data(), e.data() + start, size) != 0) {
        why = "export " + std::to_string(i) + "'s properties don't write back the same";
        return false;
      }
    }
    Bytes a2, e2;
    if (!p.build(a2, e2, why)) return false;
    if (a2 != a || e2 != e) { why = "doesn't build back the same"; return false; }
    return true;
  }
}
