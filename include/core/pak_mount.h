#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Mounting a .pak while the game runs (the engine's own pak mount, through its core delegates), for baked
// recolors. How to find them on UE 4.25 is from WistfulHopes' UEModManager for this game (after trumank's
// pak_reloader): the pak file manager's destructor loads the delegates with mov rcx,[rip+x]; the 2nd and 3rd
// distinct ones are Mount and Unmount.
namespace pak_mount {
  // Finds the delegates and checks them against the mounted pak list; logs what it found. Calls nothing.
  void probe(const std::filesystem::path& dir);
  bool found();
  // Mounts a pak at the given order (higher wins); its .sig copy must sit next to it. Game thread.
  bool mount(const std::wstring& path, int32_t order);
  bool mounted(const std::wstring& path);
  // Reads a whole file the way the game does (from whichever pak holds it, decrypted and unpacked), e.g.
  // L"../../../RED/Content/Chara/SOL/Costume01/Material/Color02/SOL_base.uexp". Game thread.
  bool readFile(const std::wstring& path, std::vector<uint8_t>& out);
  // The files under a folder, as the game sees them (every pak), e.g.
  // L"../../../RED/Content/Chara/SOL/Costume01/Material/Color02". Game thread.
  bool listDir(const std::wstring& dir, std::vector<std::wstring>& files);
  bool listDirs(const std::wstring& dir, std::vector<std::wstring>& folders);  // (the folders under it)
  // No unmount, on purpose: in 4.25 it deletes the pak file object that textures loaded from it may still be
  // streaming through. A re-bake mounts a new file at a higher order (it shadows the old one); an unbake mounts the
  // game's originals the same way; old files are deleted at the next start, before anything mounts them.
}
