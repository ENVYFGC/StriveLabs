#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace tex_upload {
  struct Sample { int x = 0, y = 0, w = 0, h = 0; std::vector<uint8_t> rgba; };
  void setLog(void (*fn)(const std::string&));
  void setQuiet(void (*fn)(bool));
  void setActive(bool on);
  void watch(void* tex, int w, int h, Sample s, int span);
  bool ready(void* tex);
  int order();
  bool put(void* tex, int w, int h, std::shared_ptr<const std::vector<uint8_t>> px);
  bool putPair(void* a, void* b, int w, int h, std::shared_ptr<const std::vector<uint8_t>> pa, std::shared_ptr<const std::vector<uint8_t>> pb);
  bool takeLost(void* tex);
  void forget(void* tex);
  void frame(ID3D11Device* dev, ID3D11DeviceContext* ctx);
}
