#include "update_check.h"
#include <Windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <objbase.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace update_check {
  namespace {
    std::mutex g_m;
    std::string g_newer;
    std::atomic<bool> g_started{false};
    std::vector<int> parts(const std::string& v) {  // "v3.10.1" -> 3 10 1, stopping at the first other character ("v3.5-rc2" is 3 5)
      std::vector<int> out;
      int n = -1;
      for (size_t i = !v.empty() && (v[0] == 'v' || v[0] == 'V') ? 1 : 0; i < v.size(); i++) {
        const char c = v[i];
        if (c >= '0' && c <= '9') n = (n < 0 ? 0 : n * 10) + (c - '0');
        else if (c == '.' && n >= 0) { out.push_back(n); n = -1; }
        else break;
      }
      if (n >= 0) out.push_back(n);
      return out;
    }
    std::string latestTag() {  // GitHub's latest release, "" on any failure (offline, blocked, rate limited)
      std::string body;
      HINTERNET s = WinHttpOpen(L"StriveLabs", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
      if (!s) return {};
      WinHttpSetTimeouts(s, 5000, 5000, 5000, 5000);
      if (HINTERNET c = WinHttpConnect(s, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0)) {
        if (HINTERNET r = WinHttpOpenRequest(c, L"GET", L"/repos/ENVYFGC/StriveLabs/releases/latest", nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {
          DWORD status = 0, len = sizeof(status);
          if (WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(r, nullptr) &&
              WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX) &&
              status == 200) {
            DWORD got = 0;
            char buf[4096];
            while (body.size() < (1u << 20) && WinHttpReadData(r, buf, sizeof(buf), &got) && got) body.append(buf, got);
          }
          WinHttpCloseHandle(r);
        }
        WinHttpCloseHandle(c);
      }
      WinHttpCloseHandle(s);
      const size_t k = body.find("\"tag_name\"");
      if (k == std::string::npos) return {};
      const size_t a = body.find('"', body.find(':', k) + 1), b = a == std::string::npos ? a : body.find('"', a + 1);
      if (b == std::string::npos || b - a > 32) return {};
      return body.substr(a + 1, b - a - 1);
    }
  }
  void start() {
    if (g_started.exchange(true)) return;
    try {  // no thread available means no check, and never an exception out of the Present hook
      std::thread([] {
        try {
          const std::string tag = latestTag();
          if (!tag.empty() && parts(tag) > parts(kVersion)) { std::lock_guard<std::mutex> lk(g_m); g_newer = tag; }
        } catch (...) {}
      }).detach();
    } catch (...) {}
  }
  void openReleases() {  // runs on its own thread, never the render thread, since the shell wants an STA and may take a while
    try {
      std::thread([] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ShellExecuteA(nullptr, "open", releasesUrl(), nullptr, nullptr, SW_SHOWNORMAL);
        if (SUCCEEDED(hr)) CoUninitialize();
      }).detach();
    } catch (...) {}
  }
  std::string newer() { std::lock_guard<std::mutex> lk(g_m); return g_newer; }
  const char* releasesUrl() { return "https://github.com/ENVYFGC/StriveLabs/releases/latest"; }
  bool gamePatchedSince() {
    // ponytail: compare the game exe's link time with the newest tested one, allowing a week's slack since the
    // Steam and Xbox builds of one patch are linked days apart. Add each new patch's stamp here once tested.
    constexpr uint32_t kNewestTested = 0x6A843AAA;  // the Steam build linked 2026-08-18 (installed 2026-09-24)
    static const bool patched = [] {
      const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
      if (!base) return false;
      const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
      if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
      const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
      if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
      return nt->FileHeader.TimeDateStamp > kNewestTested + 7u * 24 * 3600;
    }();
    return patched;
  }
}
