// DEBUG crash catcher. The F9-in-replay crash left no UE4SS dump and no Windows
// error record, so this logs the first hard faults to combo_log.txt (code, module
// + offset, likely callers from the stack) and writes one minidump next to it.
#include "combo_log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <DbgHelp.h>
#include <Psapi.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>

#pragma comment(lib, "dbghelp.lib")

namespace {
  std::atomic<int> g_logged{0};
  std::filesystem::path g_dump_path;

  std::string where(uintptr_t addr) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(addr), &mod) || !mod)
      return {};
    char name[MAX_PATH] = "";
    GetModuleBaseNameA(GetCurrentProcess(), mod, name, MAX_PATH);
    char buf[MAX_PATH + 32];
    snprintf(buf, sizeof(buf), "%s+0x%llx", name, static_cast<unsigned long long>(addr - reinterpret_cast<uintptr_t>(mod)));
    return buf;
  }

  // Only GGST itself, UE4SS or this mod are worth a dump. Overlays (OBS graphics-hook, drivers) raise and
  // handle their own faults all the time. Their address ranges are taken once at install, so nothing is looked
  // up inside the handler.
  struct Range { uintptr_t lo = 0, hi = 0; };
  Range g_ours[4];
  Range g_crt[6];  // the C runtime and ntdll, where a fault is ours if our code called in
  std::atomic<int> g_other{0};
  bool inOurs(uintptr_t a) {
    for (const Range& r : g_ours) if (r.lo && a >= r.lo && a < r.hi) return true;
    return false;
  }
  bool inCrt(uintptr_t a) {
    for (const Range& r : g_crt) if (r.lo && a >= r.lo && a < r.hi) return true;
    return false;
  }
  // A fault counts as ours if it's in one of them, in no module at all (a call through a null or freed
  // pointer), or if one of them is near the top of the stack (our bad pointer into memcpy and such).
  bool ours(uintptr_t rip, uintptr_t rsp) {
    if (inOurs(rip)) return true;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(rip), &mbi, sizeof(mbi)) || mbi.Type != MEM_IMAGE) return true;
    if (!inCrt(rip)) return false;  // another module's own fault, whoever is further down its stack
    const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const auto lo = reinterpret_cast<uintptr_t>(tib->StackLimit), hi = reinterpret_cast<uintptr_t>(tib->StackBase);
    for (int i = 0; i < 32; i++) {
      const uintptr_t at = rsp + i * sizeof(uintptr_t);
      if (at < lo || at + sizeof(uintptr_t) > hi) break;
      if (inOurs(*reinterpret_cast<const uintptr_t*>(at))) return true;
    }
    return false;
  }

  bool fatal(DWORD code) {
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
           code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_IN_PAGE_ERROR;
  }

  LONG CALLBACK handler(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!fatal(code)) return EXCEPTION_CONTINUE_SEARCH;
    const auto rip = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    const bool mine = ours(rip, ep->ContextRecord->Rsp);
    if ((mine ? g_logged : g_other).fetch_add(1) >= 3) return EXCEPTION_CONTINUE_SEARCH;  // each kind gets its own few lines
    char buf[256];
    snprintf(buf, sizeof(buf), "CRASH? code=0x%08lX at %s  (thread %lu)", code, where(rip).c_str(), GetCurrentThreadId());
    std::string line = buf;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
      snprintf(buf, sizeof(buf), "  %s address 0x%llx", ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
               static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1]));
      line += buf;
    }
    if (!mine) {  // another module's fault gets the one line only
      COMBO_LOG(line + "  (in another module, no dump)");
      return EXCEPTION_CONTINUE_SEARCH;
    }
    COMBO_LOG(line);
    // Likely callers: code addresses found on the stack
    std::string stack = "  stack:";
    const auto* sp = reinterpret_cast<const uintptr_t*>(ep->ContextRecord->Rsp);
    int found = 0;
    for (int i = 0; i < 512 && found < 14; i++) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(&sp[i], &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
      const uintptr_t v = sp[i];
      MEMORY_BASIC_INFORMATION code_mbi{};
      if (v < 0x10000 || !VirtualQuery(reinterpret_cast<void*>(v), &code_mbi, sizeof(code_mbi))) continue;
      if (!(code_mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) continue;
      const std::string w = where(v);
      if (w.empty()) continue;
      stack += " " + w;
      found++;
    }
    COMBO_LOG(stack);
    // One minidump per session, next to the log
    static std::atomic<bool> dumped{false};
    if (!dumped.exchange(true) && !g_dump_path.empty()) {
      HANDLE f = CreateFileW(g_dump_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                          static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo),
                          &mei, nullptr, nullptr);
        CloseHandle(f);
        COMBO_LOG("  dump written: StriveLabs_crash.dmp");
      }
    }
    return EXCEPTION_CONTINUE_SEARCH;  // let the game and UE4SS handle it as before
  }

}

namespace crash_log {
  void install(const std::filesystem::path& dir) {
    g_dump_path = dir / "StriveLabs_crash.dmp";
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&handler), &self);
    const HMODULE mods[4] = {GetModuleHandleA("GGST-Win64-Shipping.exe"), GetModuleHandleA("GGST-WinGDK-Shipping.exe"),
                             GetModuleHandleA("UE4SS.dll"), self};
    auto range = [](HMODULE m) {
      MODULEINFO mi{};
      return m && GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof(mi))
                 ? Range{reinterpret_cast<uintptr_t>(mi.lpBaseOfDll), reinterpret_cast<uintptr_t>(mi.lpBaseOfDll) + mi.SizeOfImage}
                 : Range{};
    };
    for (int i = 0; i < 4; i++) g_ours[i] = range(mods[i]);
    const char* crt[6] = {"vcruntime140.dll", "vcruntime140_1.dll", "ucrtbase.dll", "msvcp140.dll", "ntdll.dll", "KERNELBASE.dll"};
    for (int i = 0; i < 6; i++) g_crt[i] = range(GetModuleHandleA(crt[i]));
    AddVectoredExceptionHandler(1, handler);
    COMBO_LOG("crash catcher: on");
  }
}
