#pragma once

// Baking recolors into .pak files: the parts that don't touch the game (DXT1/DXT5/BC7 coding, the texture's mips, the pak
// file itself), so they can be tested on their own against the game's files.
#include <cstdint>
#include <string>
#include <vector>

namespace bake {
  using Bytes = std::vector<uint8_t>;

  // The block formats the slot textures use (mostly DXT5 and DXT1, a few BC7)
  enum class Format { DXT1, DXT5, BC7 };
  int blockBytes(Format f);  // (8, 16, 16)
  // One 4x4 block of RGBA8 (row by row, 64 bytes) <-> its bytes. DXT5's colour half is always 4-colour; DXT1 uses
  // 3-colour + transparent when a pixel's alpha is under 128; BC7 is written in mode 6 (one subset, RGBA), and only
  // mode 6 decodes here (the game's other BC7 blocks are never decoded: unchanged blocks are copied as they are).
  void encodeBlock(Format f, const uint8_t rgba[64], uint8_t* out);
  bool decodeBlock(Format f, const uint8_t* in, uint8_t rgba[64]);
  inline void encodeBlock(const uint8_t rgba[64], uint8_t out[16]) { encodeBlock(Format::DXT5, rgba, out); }
  inline void decodeBlock(const uint8_t in[16], uint8_t rgba[64]) { decodeBlock(Format::DXT5, in, rgba); }

  // A cooked block-compressed Texture2D as the game ships it: its mips (the big ones in .ubulk, the small ones inline in .uexp).
  struct Mip { size_t header = 0; uint32_t flags = 0; int32_t size = 0, w = 0, h = 0; int64_t offset = 0; bool inline_ = false; };
  struct Texture {
    Bytes uasset, uexp, ubulk;
    Format format = Format::DXT5;
    std::vector<Mip> mips;  // (biggest first)
    bool parse(std::string& why);  // its pixel format and mip headers from .uexp; checks sizes add up
    uint8_t* mipData(size_t i);    // the mip's blocks
  };

  // Writes `image` (RGBA8, the top mip's size) into the texture. Every 4x4 block whose pixels equal `was` (the same
  // texture as the game draws it, which the editor composes from) keeps the game's original bytes, at every mip;
  // only the rest is encoded, from a 2x2 box filter of `image` for the smaller mips. Sizes and offsets never change.
  // Returns how many blocks were encoded (0: nothing changed).
  size_t writeImage(Texture& t, const std::vector<uint8_t>& image, const std::vector<uint8_t>& was, std::string& why);

  std::string sha1hex(const Bytes& d);

  // A pak (version 9, unencrypted index, mount point ../../../), its files zlib-compressed in 64 KB blocks as the
  // game's own are (an empty file stored as it is), and a check that reads it back and unpacks every file.
  struct PakFile { std::string name; Bytes data; };  // name like RED/Content/Chara/SOL/.../SOL_base.uexp
  bool writePak(const std::wstring& path, const std::vector<PakFile>& files, std::string& why);
  bool checkPak(const std::wstring& path, const std::vector<PakFile>& files, std::string& why);
  // The file names in a pak this writer made (version 9, unencrypted index; compressed or, older bakes, not)
  bool pakNames(const std::wstring& path, std::vector<std::string>& names);
}
