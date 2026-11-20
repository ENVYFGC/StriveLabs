#include "part_preview.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

namespace part_preview {
  bool readPick(const std::vector<uint8_t>& b, std::map<std::string, PickMesh>& out, std::vector<std::string>* order) {
    size_t o = 0;
    auto take = [&](void* d, size_t n) { if (n > b.size() - o) return false; std::memcpy(d, b.data() + o, n); o += n; return true; };
    auto str = [&](std::string& s) {
      uint16_t n = 0;
      if (!take(&n, 2) || n > b.size() - o) return false;
      s.assign(reinterpret_cast<const char*>(b.data() + o), n);
      o += n;
      return true;
    };
    uint32_t magic = 0, ver = 0, meshes = 0;
    if (!take(&magic, 4) || magic != 0x4B504C53 || !take(&ver, 4) || ver != 2 || !take(&meshes, 4) || meshes > 4096) return false;
    for (uint32_t m = 0; m < meshes; m++) {
      std::string name;
      uint16_t nb = 0;
      if (!str(name) || !take(&nb, 2)) return false;
      PickMesh pm;
      pm.bones.resize(nb);
      for (PickBone& bn : pm.bones) {
        float rot[4];
        if (!str(bn.name) || !take(bn.pos, 12) || !take(rot, 16)) return false;
        bn.inv[0] = -rot[0]; bn.inv[1] = -rot[1]; bn.inv[2] = -rot[2]; bn.inv[3] = rot[3];
      }
      uint32_t ns = 0;
      if (!take(&ns, 4) || ns > 256) return false;
      pm.slots.resize(ns);
      for (PickSlot& s : pm.slots) {
        uint32_t nv = 0, nt = 0, ni = 0;
        uint8_t wide = 0;
        if (!str(s.name) || !str(s.kind) || !take(s.lo, 8) || !take(s.span, 8) || !take(s.plo, 12) || !take(s.pspan, 12) || !take(&nv, 4) || !take(&nt, 4) || !take(&wide, 1) || !take(&ni, 4)) return false;
        if (nv > (b.size() - o) / sizeof(PickVert)) return false;
        s.verts.resize(nv);
        if (nv && !take(s.verts.data(), static_cast<size_t>(nv) * sizeof(PickVert))) return false;
        if (ni > (b.size() - o) / sizeof(PickInf)) return false;
        s.inf.resize(ni);
        if (ni && !take(s.inf.data(), static_cast<size_t>(ni) * sizeof(PickInf))) return false;
        s.first.resize(static_cast<size_t>(nv) + 1);
        uint64_t at = 0;
        for (uint32_t i = 0; i < nv; i++) { s.first[i] = static_cast<uint32_t>(at); at += s.verts[i].n; if (at > ni) return false; }
        s.first[nv] = static_cast<uint32_t>(at);
        if (at != ni) return false;
        const size_t isz = wide ? 4 : 2;
        if (nt > (b.size() - o) / (3 * isz)) return false;
        s.tris.resize(static_cast<size_t>(nt) * 3);
        for (uint32_t& i : s.tris) {
          if (wide) { if (!take(&i, 4)) return false; }
          else { uint16_t k = 0; if (!take(&k, 2)) return false; i = k; }
          if (i >= nv) return false;
        }
        for (const PickInf& f : s.inf) if (f.b < nb) pm.bones[f.b].used = true;
      }
      if (order) order->push_back(name);
      out[name] = std::move(pm);
    }
    return true;
  }
}
