#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace script_pak {
  using Bytes = std::vector<uint8_t>;
  struct Block { int kind = 0; std::string name; Bytes code; };
  struct LineEdit { std::string name; int offset = 0, field = 0, op = 0; Bytes sig, sig_alt, orig, now; };
  struct Result {
    Bytes uexp, uasset;
    int blocks = 0, lines = 0;
    std::vector<std::string> left_out, notes;
  };
  bool rebuild(const Bytes& uexp, const Bytes& uasset, const short* sizes, int max_op, const std::vector<Block>& swaps, const std::vector<LineEdit>& edits,
               Result& out, std::string& err);
}
