#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace part_preview {
  struct PickVert { int16_t p[3]; uint16_t uv[2]; uint8_t n, pad; };
  static_assert(sizeof(PickVert) == 12);
  struct PickInf { uint16_t b, w; };
  static_assert(sizeof(PickInf) == 4);
  struct PickSlot { std::string name, kind; float lo[2] = {}, span[2] = {}, plo[3] = {}, pspan[3] = {}; std::vector<PickVert> verts; std::vector<PickInf> inf; std::vector<uint32_t> first; std::vector<uint32_t> tris; };
  struct PickBone { std::string name; float pos[3] = {}, inv[4] = {}; bool used = false; };
  struct PickMesh { std::vector<PickBone> bones; std::vector<PickSlot> slots; };
  bool readPick(const std::vector<uint8_t>& b, std::map<std::string, PickMesh>& out, std::vector<std::string>* order = nullptr);
}
