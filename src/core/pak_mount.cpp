#include "pak_mount.h"
#include "combo_log.h"
#include "bake_core.h"

#include <Windows.h>
#include <Psapi.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace pak_mount {
  namespace {
    // UE 4.25 layouts (from UEModManager): a bound raw delegate is {vtable, object, method}; the object here is the
    // pak file manager, {vtable, lower level, TArray<{order, FPakFile*}>}; an FPakFile has its file name at +8.
    struct FStr { const wchar_t* data; int32_t num, max; };
    struct PakEntry { uint32_t order; const uint8_t* file; };
    struct PakManager { void* vtable; void* lower; PakEntry* data; int32_t num, max; };
    struct Delegate { void* vtable; PakManager* pak; void* fn; };

    std::filesystem::path g_dir;
    Delegate* g_mount = nullptr;
    Delegate* g_unmount = nullptr;
    bool g_found = false;

    uintptr_t g_lo = 0, g_hi = 0;  // (the game's image)
    std::vector<std::pair<uintptr_t, uintptr_t>> g_code;

    bool readable(const void* p, size_t n) {
      if (!p) return false;
      const uint8_t* at = static_cast<const uint8_t*>(p);
      const uint8_t* const end = at + n;
      do {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(at, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
        if ((mbi.Protect & 0xFF) == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return false;
        at = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
      } while (at < end);
      return true;
    }
    bool inCode(const void* p) {
      const auto a = reinterpret_cast<uintptr_t>(p);
      for (const auto& [lo, hi] : g_code) if (a >= lo && a < hi) return true;
      return false;
    }
    bool inImage(const void* p) { const auto a = reinterpret_cast<uintptr_t>(p); return a >= g_lo && a < g_hi; }

    void image() {
      HMODULE m = GetModuleHandleA("GGST-Win64-Shipping.exe");
      if (!m) m = GetModuleHandleA(nullptr);
      MODULEINFO info{};
      GetModuleInformation(GetCurrentProcess(), m, &info, sizeof(info));
      g_lo = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
      g_hi = g_lo + info.SizeOfImage;
      const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_lo + reinterpret_cast<const IMAGE_DOS_HEADER*>(g_lo)->e_lfanew);
      const auto* sec = IMAGE_FIRST_SECTION(nt);
      for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)
          g_code.emplace_back(g_lo + sec->VirtualAddress, g_lo + sec->VirtualAddress + sec->Misc.VirtualSize);
    }

    // 40 56 48 83 EC 30 48 89 5C 24 ?? 48 8D 05 ?? ?? ?? ?? 48 89 7C 24 (every match in the code sections)
    std::vector<const uint8_t*> matches() {
      static const int sig[] = {0x40, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x89, 0x5C, 0x24, -1, 0x48, 0x8D, 0x05,
                                -1, -1, -1, -1, 0x48, 0x89, 0x7C, 0x24};
      constexpr size_t n = sizeof(sig) / sizeof(sig[0]);
      std::vector<const uint8_t*> out;
      for (const auto& [lo, hi] : g_code) {
        if (hi - lo < n || !readable(reinterpret_cast<void*>(lo), hi - lo)) continue;
        for (auto p = reinterpret_cast<const uint8_t*>(lo); p + n <= reinterpret_cast<const uint8_t*>(hi); p++) {
          if (p[0] != 0x40 || p[1] != 0x56) continue;
          size_t k = 2;
          while (k < n && (sig[k] < 0 || p[k] == sig[k])) k++;
          if (k == n) out.push_back(p);
        }
      }
      return out;
    }

    std::string narrow(const wchar_t* w, int n) {
      std::string s;
      for (int i = 0; i < n && w[i]; i++) s += (w[i] < 128) ? static_cast<char>(w[i]) : '?';
      return s;
    }
    std::string hex(const void* p) { char b[32]; snprintf(b, sizeof b, "%p", p); return b; }
    std::string rel(const void* p) {
      char b[32];
      snprintf(b, sizeof b, "exe+0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p) - g_lo));
      return inImage(p) ? std::string(b) : hex(p);
    }

    // The mounted paks, or empty if the manager doesn't look like one
    std::vector<std::pair<uint32_t, std::string>> paks(const PakManager* m) {
      std::vector<std::pair<uint32_t, std::string>> out;
      if (!readable(m, sizeof(PakManager)) || m->num <= 0 || m->num > 4096 || m->max < m->num) return out;
      if (!readable(m->data, sizeof(PakEntry) * m->num)) return out;
      for (int i = 0; i < m->num; i++) {
        const PakEntry& e = m->data[i];
        if (!readable(e.file, 8 + sizeof(FStr))) return {};
        FStr name;
        std::memcpy(&name, e.file + 8, sizeof name);
        if (name.num <= 0 || name.num > 1024 || !readable(name.data, sizeof(wchar_t) * name.num)) return {};
        out.emplace_back(e.order, narrow(name.data, name.num));
      }
      return out;
    }
  }

  namespace {
  }

  namespace {
    // The pak manager's file functions, by slot. From a dump of its virtual functions: slots
    // 11/12/13 are get/set lower level and the name; then, in UE 4.25's order, 14 exists (true when a pak has it),
    // 15 size, 16 delete, 17 read-only (true when a pak has it), ... 24 open for reading (25 forwards to it).
    constexpr int kExists = 14, kSize = 15, kOpenRead = 24;
    constexpr int kAnchors[3] = {11, 12, 13};
    bool anchorsHold(const PakManager* m) {  // (the three one-liners where they were: mov rax,[rcx+8]; mov [rcx+8],rdx; lea rax,[rip+x])
      const auto* vt = static_cast<void* const*>(m->vtable);
      if (!readable(vt, 8 * 33)) return false;
      const auto* a = static_cast<const uint8_t*>(vt[kAnchors[0]]);
      const auto* b = static_cast<const uint8_t*>(vt[kAnchors[1]]);
      const auto* c = static_cast<const uint8_t*>(vt[kAnchors[2]]);
      if (!readable(a, 5) || !readable(b, 5) || !readable(c, 8)) return false;
      static const uint8_t ga[] = {0x48, 0x8B, 0x41, 0x08, 0xC3}, gb[] = {0x48, 0x89, 0x51, 0x08, 0xC3};
      return std::memcmp(a, ga, 5) == 0 && std::memcmp(b, gb, 5) == 0 && c[0] == 0x48 && c[1] == 0x8D && c[2] == 0x05 && c[7] == 0xC3;
    }
    template <typename Fn> Fn slot(const PakManager* m, int i) { return reinterpret_cast<Fn>(static_cast<void* const*>(m->vtable)[i]); }

    // The file handle's virtual functions (from a dump, in UE 4.25's order): 0 destroy, 1 tell,
    // 2 seek, 3 seek from end (uses 2), 4 read, 5-7 write/flush/truncate (refuse), 8 size
    constexpr int kHDestroy = 0, kHRead = 4, kHSize = 8;
    bool g_reader = false;  // (the file functions checked out on a known file)
  }

  namespace {
    // A directory visitor for slot 32 (IterateDirectory(dir, visitor&): the overload that takes a function builds
    // {vtable, callable} on the stack and calls slot 32 with it). Whether Visit is its 1st or 2nd virtual function,
    // both slots here are the same recording function; the destructor is never called (it's passed by reference).
    constexpr int kIterate = 32;
    struct Visitor { void** vt; std::vector<std::wstring>* out; bool dirs = false; };  // (its own list: no shared state; dirs: folders, not files)
    void visitNothing(void*, unsigned) {}  // (slot 0, the destructor's place: never called, harmless if it were)
    bool visitOne(Visitor* self, const wchar_t* name, bool is_dir) {
      if (self && self->out && name && is_dir == self->dirs) self->out->emplace_back(name);
      return true;
    }
    void* g_visitor_vt[2] = {reinterpret_cast<void*>(&visitNothing), reinterpret_cast<void*>(&visitOne)};
  }

  bool listDirs(const std::wstring& dir, std::vector<std::wstring>& out) {
    out.clear();
    if (!g_reader) return false;
    PakManager* m = g_mount->pak;
    using Iterate = bool (*)(PakManager*, const wchar_t*, Visitor&);
    Visitor v{g_visitor_vt, &out, true};
    return slot<Iterate>(m, kIterate)(m, dir.c_str(), v);
  }
  bool listDir(const std::wstring& dir, std::vector<std::wstring>& files) {
    files.clear();
    if (!g_reader) return false;
    PakManager* m = g_mount->pak;
    using Iterate = bool (*)(PakManager*, const wchar_t*, Visitor&);
    Visitor v{g_visitor_vt, &files, false};
    return slot<Iterate>(m, kIterate)(m, dir.c_str(), v);
  }

  bool readFile(const std::wstring& path, std::vector<uint8_t>& out) {
    out.clear();
    if (!g_reader) return false;
    PakManager* m = g_mount->pak;
    using Open = void* (*)(PakManager*, const wchar_t*, bool);
    void* h = slot<Open>(m, kOpenRead)(m, path.c_str(), false);
    if (!readable(h, 8)) return false;
    const auto* vt = *static_cast<void* const* const*>(h);
    using HSize = int64_t (*)(void*);
    using HRead = bool (*)(void*, uint8_t*, int64_t);
    using HDestroy = void (*)(void*, unsigned);
    const int64_t n = reinterpret_cast<HSize>(vt[kHSize])(h);
    bool ok = n >= 0 && n < (int64_t{1} << 31);
    if (ok) {
      out.resize(static_cast<size_t>(n));
      ok = n == 0 || reinterpret_cast<HRead>(vt[kHRead])(h, out.data(), n);
    }
    reinterpret_cast<HDestroy>(vt[kHDestroy])(h, 1);  // (the scalar deleting destructor: closes and frees it)
    if (!ok) out.clear();
    return ok;
  }

  namespace {
    // (a check: exists and size on a known file and a made-up one; then the reader on three known files, their
    // SHA1s logged for comparing with the pak read offline)
    void readProbe(PakManager* m) {
      if (!anchorsHold(m)) { COMBO_LOG("pak read: the file functions aren't where they were: not used"); return; }
      const wchar_t* known = L"../../../RED/Content/Chara/SOL/Costume01/Material/Color02/SOL_base.uasset";
      const wchar_t* none = L"../../../RED/Content/Chara/SOL/Costume01/Material/Color02/StriveLabs_no_such_file.uasset";
      using Exists = bool (*)(PakManager*, const wchar_t*);
      using Size = int64_t (*)(PakManager*, const wchar_t*);
      const bool e1 = slot<Exists>(m, kExists)(m, known), e0 = slot<Exists>(m, kExists)(m, none);
      const int64_t s1 = slot<Size>(m, kSize)(m, known), s0 = slot<Size>(m, kSize)(m, none);
      if (!(e1 && !e0 && s1 == 848 && s0 == -1)) {
        COMBO_LOG("pak read: exists " + std::to_string(e1) + "/" + std::to_string(e0) + ", size " + std::to_string(s1) + "/" +
                  std::to_string(s0) + " (expected 1/0 and 848/-1): not used");
        return;
      }
      g_reader = true;
    }
  }

  bool found() { return g_found; }

  namespace {
    void logList() {
      if (!g_found) return;
      for (const auto& [order, name] : paks(g_mount->pak)) COMBO_LOG("pak mount:   mounted (order " + std::to_string(order) + "): " + name);
    }
  }

  namespace {
    std::string fileName(std::string s) {  // (lower case, after the last slash: how the engine's list is compared)
      for (char& c : s) c = c == '\\' ? '/' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
      const auto at = s.find_last_of('/');
      return at == std::string::npos ? s : s.substr(at + 1);
    }
    std::string fileName(const std::wstring& w) {
      std::string s;
      for (wchar_t c : w) s += c < 128 ? static_cast<char>(c) : '?';
      return fileName(s);
    }
    bool usable(const Delegate* d) { return g_found && readable(d, sizeof(Delegate)) && inCode(d->fn); }
  }

  bool mounted(const std::wstring& path) {
    if (!g_found) return false;
    const std::string want = fileName(path);
    for (const auto& [o, name] : paks(g_mount->pak))
      if (fileName(name) == want) return true;
    return false;
  }

  bool mount(const std::wstring& path, int32_t order) {
    if (!usable(g_mount) || mounted(path)) return false;
    // (the delegate takes a const FString&: our own buffer, null included in the count; the engine copies it)
    std::wstring buf = path;
    const FStr s{buf.c_str(), static_cast<int32_t>(buf.size() + 1), static_cast<int32_t>(buf.size() + 1)};
    using Fn = bool (*)(PakManager*, const FStr&, int32_t, void*);
    if (!reinterpret_cast<Fn>(g_mount->fn)(g_mount->pak, s, order, nullptr)) return false;
    if (mounted(path)) return true;  // (success only if it's in the list now)
    COMBO_LOG("pak mount: the mount said yes but the pak isn't in the list");
    return false;
  }

  void probe(const std::filesystem::path& dir) {
    g_dir = dir;
    try {
      const ULONGLONG t0 = GetTickCount64();
      image();
      const auto hits = matches();
      COMBO_LOG("pak mount: " + std::to_string(hits.size()) + " match(es) for the pak manager destructor (scan " +
                std::to_string(GetTickCount64() - t0) + " ms)");
      if (hits.size() != 1) { COMBO_LOG("pak mount: not exactly one match: not used"); return; }  // (a patched exe: re-verify by hand)
      for (const uint8_t* d : hits) {
        COMBO_LOG("pak mount:  match at " + rel(d));
        // distinct mov rcx,[rip+x] targets in the next 3000 bytes, counted from 1 as UEModManager does
        int num = 1;
        const void* last = nullptr;
        Delegate* mount = nullptr;
        Delegate* unmount = nullptr;
        for (const uint8_t* i = d; i < d + 3000 && readable(i, 7); i++) {
          if (i[0] != 0x48 || i[1] != 0x8B || i[2] != 0x0D) continue;
          int32_t rip;
          std::memcpy(&rip, i + 3, 4);
          const void* slot = i + 7 + rip;
          if (slot == last) continue;
          last = slot;
          Delegate* inst = readable(slot, 8) ? *static_cast<Delegate* const*>(slot) : nullptr;
          COMBO_LOG("pak mount:   delegate " + std::to_string(num) + " slot " + rel(slot) + " -> " + hex(inst) +
                    (readable(inst, sizeof(Delegate)) ? " (object " + hex(inst->pak) + ", method " + rel(inst->fn) + ")" : ""));
          if (num == 2) mount = inst;
          if (num == 3) { unmount = inst; break; }
          num++;
        }
        if (!readable(mount, sizeof(Delegate)) || !readable(unmount, sizeof(Delegate))) { COMBO_LOG("pak mount:   delegates not readable"); continue; }
        if (mount->pak != unmount->pak) { COMBO_LOG("pak mount:   the two delegates are bound to different objects"); continue; }
        if (!inCode(mount->fn) || !inCode(unmount->fn) || mount->fn == unmount->fn) { COMBO_LOG("pak mount:   methods aren't distinct game code"); continue; }
        const auto list = paks(mount->pak);
        bool base = false;
        for (const auto& [order, name] : list) {
          COMBO_LOG("pak mount:   mounted (order " + std::to_string(order) + "): " + name);
          if (name.size() >= 29 && name.compare(name.size() - 29, 29, "pakchunk0-WindowsNoEditor.pak") == 0) base = true;
        }
        if (!base) { COMBO_LOG("pak mount:   the game's own pak isn't in the list: not the pak manager"); continue; }
        g_mount = mount;
        g_unmount = unmount;
        g_found = true;
        readProbe(mount->pak);
        COMBO_LOG("pak mount: found (" + std::to_string(list.size()) + " paks mounted)");
        return;
      }
      COMBO_LOG("pak mount: not found");
    } catch (...) {
      COMBO_LOG("pak mount: probe failed");
    }
  }
}
