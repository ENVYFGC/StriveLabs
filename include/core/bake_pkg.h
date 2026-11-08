#pragma once

// Cooked UE 4.25 packages as this game ships them (unversioned .uasset + .uexp), far enough to rewrite slot
// materials for baking: add names, change an export's size, set a vector parameter, point a texture import at
// another package, and copy a texture package to another path. A package written back unchanged must come out
// byte for byte the same (verifyPackage).
#include "bake_core.h"

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bake {
  struct NameEntry { std::string s; uint16_t h1 = 0, h2 = 0; };
  uint16_t hashNonCase(const std::string& s);  // (the two hashes every name entry carries)
  uint16_t hashCase(const std::string& s);

  struct Package {
    Bytes uasset, uexp;
    std::vector<NameEntry> names;
    std::vector<std::array<int32_t, 7>> imports;  // class package (name, num), class (name, num), outer, object (name, num)
    std::vector<std::array<uint8_t, 104>> exports;

    bool parse(const Bytes& a, const Bytes& e, std::string& why);
    const std::string& name(int32_t i) const { return names[static_cast<size_t>(i)].s; }
    int32_t nameIndex(const std::string& s, bool add);
    int64_t exportSize(size_t i) const;
    int64_t exportOffset(size_t i) const;
    int32_t total() const { return total_; }
    int32_t importOffset() const { return import_off_; }
    // The package again, with its names as they are now, `newUexp` and the exports' new sizes
    bool build(const Bytes& newUexp, const std::vector<int64_t>& sizes, Bytes& outA, Bytes& outE, std::string& why) const;
    bool build(Bytes& outA, Bytes& outE, std::string& why) const;

   private:
    int32_t total_ = 0, name_off_ = 0, names_end_ = 0, export_off_ = 0, import_off_ = 0;
    size_t p_total_ = 0, p_names_ = 0, p_bulk_ = 0, p_gens_ = 0;
    int32_t gens_ = 0;
    std::vector<std::pair<size_t, int32_t>> offsets_;  // (summary offsets past the name map: where, value)
  };

  // Tagged properties, kept so they write back exactly
  struct Prop;
  struct Value {
    enum Kind { Raw, Props, Name, EnumName, Array, StructArray } kind = Raw;
    Bytes raw;
    std::vector<Prop> props;
    std::pair<std::string, int32_t> name;
    int32_t count = 0;
    std::shared_ptr<Prop> innerTag;     // (StructArray)
    std::vector<Value> elems;           // (StructArray: each Raw or Props)
  };
  struct Prop {
    std::string name, type, structName, enumName, inner, inner2;
    int32_t num = 0, aidx = 0;
    Bytes sguid, pguid;
    uint8_t boolv = 0, hasGuid = 0;
    Value value;
  };
  bool readProps(Package& p, const Bytes& b, size_t& at, size_t end, std::vector<Prop>& out, std::string& why);
  Bytes writeProps(Package& p, const std::vector<Prop>& props);
  // The export's properties, and the bytes after them in its serial data
  bool parseExport(Package& p, std::vector<Prop>& props, Bytes& tail, size_t& start, size_t& size, std::string& why);
  bool parseExport(Package& p, size_t index, std::vector<Prop>& props, Bytes& tail, size_t& start, size_t& size, std::string& why);

  // ---- the edits baking makes (each returns the new .uasset/.uexp) ----
  // A vector parameter on a material instance: overwritten if it has it, else added (zero GUID unless given)
  bool setVector(Package& mic, const std::string& param, const float rgba[4], const uint8_t guid[16], Bytes& outA, Bytes& outE,
                 bool& added, std::string& why);
  // The material's import of package `oldPath` pointing at `newPath` (same object name inside)
  bool repoint(Package& mic, const std::string& oldPath, const std::string& newPath, Bytes& outA, Bytes& outE, std::string& why);
  // A texture package copied to `newPath` (same object name): its own path is its one name entry
  bool renameTexture(Package& tex, const std::string& newPath, Bytes& outA, Bytes& outE, std::string& why);
  // An effect color of a slot's palette (PTCColorAndMaterial): every RGB of its color-over-life table, MinValueVec
  // and MaxValueVec put in rgb's ratio, each keeping its total, exactly as the editor does to the loaded asset
  bool setEffectColor(Package& ptc, const std::string& label, const float rgb[3], Bytes& outA, Bytes& outE, std::string& why);
  std::vector<float> ratioed(const std::vector<float>& orig, const float rgb[3]);
  // Parses the result again and writes it back unchanged: it must come out byte for byte
  bool verifyPackage(const Bytes& a, const Bytes& e, std::string& why);
}
