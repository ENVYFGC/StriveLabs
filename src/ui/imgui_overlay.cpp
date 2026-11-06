#include "imgui_overlay.h"
#include "menu.h"
#include "replay_manager.h"
#include "combo_log.h"
#include "keybinds.h"
#include "combo_trials.h"
#include "recolor_test.h"
#include "recolor_share.h"
#include "update_check.h"
#include <random>
#include <tuple>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include <algorithm>
#include <cfloat>
#include <cctype>
#include <cmath>
#include <functional>
#include <filesystem>
#include <set>
#include <map>
#include <wincodec.h>
#include <mutex>
#include <cstring>
#include <fstream>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

bool modScreenActive();  // dllmain.cpp
bool onlineVersionScreen();  // dllmain.cpp

#include "imgui_menu.inc"

namespace {
  using Present_t = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
  using ResizeBuffers_t = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

  Present_t g_present = nullptr;
  ResizeBuffers_t g_resize = nullptr;

  ID3D11Device* g_device = nullptr;
  ID3D11DeviceContext* g_context = nullptr;
  ID3D11RenderTargetView* g_rtv = nullptr;
  HWND g_hwnd = nullptr;
  WNDPROC g_wndproc = nullptr;
  bool g_init_failed = false;
  std::atomic<int> g_other_presents{0};  // Presents that wanted drawing before any UnrealWindow one. Game Pass may use another class.
  std::atomic<bool> g_running{false};
  std::mutex g_frame_mutex;  // one ImGui frame at a time

  // Swap one entry of a COM vtable. Steam and Discord overlays detour the code
  // behind these entries; replacing the entry keeps their hooks in the chain.
  void* patch_vtable(void** vtable, int index, void* hook) {
    DWORD old;
    VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old);
    void* orig = vtable[index];
    vtable[index] = hook;
    VirtualProtect(&vtable[index], sizeof(void*), old, &old);
    return orig;
  }

  void create_rtv(IDXGISwapChain* swap) {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) && back) {
      g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
      back->Release();
    }
  }

  // Mouse and keyboard go to ImGui only while the F5 menu is open.
  LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_running && ((modScreenActive() && (ModMenu::instance().isShowing() || menu_ui::g_timeline_visible || recolor_test::isOpen())) ||
                      (!modScreenActive() && menu_ui::g_online_picker))) {
      ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
      const ImGuiIO& io = ImGui::GetIO();
      const bool mouse = msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST;
      if (mouse && io.WantCaptureMouse) return 0;
      const bool key = msg >= WM_KEYFIRST && msg <= WM_KEYLAST;
      if (key && io.WantTextInput) return 0;
      if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && recolor_test::freeCameraKeys()) {
        switch (wp) {
          case 'W': case 'A': case 'S': case 'D': case 'Q': case 'E': case 'R':
          case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: return 0;
          default: break;
        }
      }
    }
    return CallWindowProcW(g_wndproc, hwnd, msg, wp, lp);
  }

  // StriveLabs_logo.png -> D3D11 texture through Windows Imaging Component.
  ID3D11ShaderResourceView* load_png(const std::wstring& path, float& aspect) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // fine if COM is already up on this thread
    IWICImagingFactory* wic = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    UINT w = 0, h = 0;
    std::vector<BYTE> px;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(conv->GetSize(&w, &h)) && w && h) {
      px.resize(static_cast<size_t>(w) * h * 4);
      if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data()))) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{px.data(), w * 4, 0};
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(g_device->CreateTexture2D(&td, &sd, &tex)) && tex) {
          g_device->CreateShaderResourceView(tex, nullptr, &srv);
          tex->Release();
          aspect = static_cast<float>(h) / static_cast<float>(w);
        }
      }
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (wic) wic->Release();
    return srv;
  }

  // Makes an ImGui texture from RGBA pixels and releases the old one, if any.
  ImTextureID upload_rgba(const uint8_t* px, int w, int h, ImTextureID old) {
    if (old) reinterpret_cast<ID3D11ShaderResourceView*>(old)->Release();
    if (!g_device || !px || w <= 0 || h <= 0) return 0;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w); td.Height = static_cast<UINT>(h); td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{px, static_cast<UINT>(w) * 4, 0};
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&td, &sd, &tex)) && tex) {
      g_device->CreateShaderResourceView(tex, nullptr, &srv);
      tex->Release();
    }
    return reinterpret_cast<ImTextureID>(srv);
  }

  ID3D11SamplerState* g_point = nullptr;  // Nearest-texel sampling, for the zoomed-in texture view
  void point_sampling(const ImDrawList*, const ImDrawCmd*) { if (g_point && g_context) g_context->PSSetSamplers(0, 1, &g_point); }

  bool init(IDXGISwapChain* swap) {
    if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device))) || !g_device) return false;
    g_device->GetImmediateContext(&g_context);
    DXGI_SWAP_CHAIN_DESC desc{};
    swap->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Window positions (popped pages, tension window) are kept in the ue4ss folder.
    static const std::string ini = (ModMenu::dataDir() / "StriveLabs_windows.ini").string();
    io.IniFilename = ini.c_str();
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    menu_ui::style();
    menu_ui::load_fonts(static_cast<float>(desc.BufferDesc.Height));
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    menu_ui::upload_rgba = &upload_rgba;
    menu_ui::device_gen++;
    {
      D3D11_SAMPLER_DESC sd{};
      sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
      sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
      sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
      sd.MaxLOD = D3D11_FLOAT32_MAX;
      if (g_point) { g_point->Release(); g_point = nullptr; }
      g_device->CreateSamplerState(&sd, &g_point);
      menu_ui::point_sampling = g_point ? &point_sampling : nullptr;
    }
    if (auto* srv = load_png((ModMenu::dataDir() / "StriveLabs_logo.png").wstring(), menu_ui::logo_aspect))
      menu_ui::logo = reinterpret_cast<ImTextureID>(srv);
    else
      COMBO_LOG("imgui: StriveLabs_logo.png not loaded");
    create_rtv(swap);
    g_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wndproc)));
    return true;
  }

  void draw_ui() { menu_ui::draw(); }

  // Every swap chain in the process goes through this hook, not only the game's:
  // loading screens and overlays present too, sometimes from another thread. Draw
  // only into the game's own window, from one thread at a time, and only while a
  // fight is on screen. Menus and loading screens are left completely alone.
  bool is_game_window(IDXGISwapChain* swap) {
    DXGI_SWAP_CHAIN_DESC desc{};
    return SUCCEEDED(swap->GetDesc(&desc)) && desc.OutputWindow == g_hwnd;
  }

  // The color editor's scans. Reads the back buffer (the game's frame, before our overlay), every step-th pixel,
  // as 8-bit RGB. Returns 1 when read, 0 when it can't read right now (tried again later), and -1 for a format
  // it can't read (why says which).
  float half_to_float(uint16_t v) {
    const uint32_t s = (v >> 15) & 1, e = (v >> 10) & 0x1F, m = v & 0x3FF;
    if (e == 0) return (s ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(m), -24);
    if (e == 31) return m ? 0.0f : (s ? -1.0f : 1.0f) * 65504.0f;
    return (s ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
  }
  int grab_frame(IDXGISwapChain* swap, std::vector<uint8_t>& out, int& w, int& h, int step, std::string& why) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) return 0;
    D3D11_TEXTURE2D_DESC d{};
    back->GetDesc(&d);
    const DXGI_FORMAT f = d.Format;
    const bool bgra = f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_UNORM || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    const bool rgba = f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const bool rgb10 = f == DXGI_FORMAT_R10G10B10A2_UNORM;
    const bool half = f == DXGI_FORMAT_R16G16B16A16_FLOAT;  // HDR in scRGB, where 1.0 is SDR white
    if (!bgra && !rgba && !rgb10 && !half) {
      back->Release();
      why = "the screen's format (" + std::to_string(static_cast<int>(f)) + ") can't be read.";
      return -1;
    }
    ID3D11Texture2D* src = back;  // With MSAA, it's resolved into a plain texture first
    ID3D11Texture2D* resolved = nullptr;
    if (d.SampleDesc.Count != 1) {
      UINT support = 0;
      if (FAILED(g_device->CheckFormatSupport(f, &support)) || !(support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE)) {
        back->Release();
        why = "the screen's antialiasing can't be read (format " + std::to_string(static_cast<int>(f)) + ").";
        return -1;
      }
      D3D11_TEXTURE2D_DESC rd = d;
      rd.SampleDesc.Count = 1; rd.SampleDesc.Quality = 0; rd.Usage = D3D11_USAGE_DEFAULT; rd.BindFlags = 0; rd.CPUAccessFlags = 0; rd.MiscFlags = 0;
      if (FAILED(g_device->CreateTexture2D(&rd, nullptr, &resolved)) || !resolved) { back->Release(); return 0; }
      g_context->ResolveSubresource(resolved, 0, back, 0, f);
      src = resolved;
    }
    D3D11_TEXTURE2D_DESC sd = d;
    sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &st)) || !st) {
      if (resolved) resolved->Release();
      back->Release();
      return 0;
    }
    g_context->CopyResource(st, src);
    D3D11_MAPPED_SUBRESOURCE m{};
    const bool ok = SUCCEEDED(g_context->Map(st, 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
      w = static_cast<int>(d.Width) / step; h = static_cast<int>(d.Height) / step;
      out.assign(static_cast<size_t>(w) * h * 3, 0);
      auto to8 = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
      for (int y = 0; y < h; y++) {
        const uint8_t* rowb = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * step * m.RowPitch;
        for (int x = 0; x < w; x++) {
          uint8_t r, g, b;
          if (half) {
            const uint16_t* p = reinterpret_cast<const uint16_t*>(rowb) + static_cast<size_t>(x) * step * 4;
            r = to8(half_to_float(p[0])); g = to8(half_to_float(p[1])); b = to8(half_to_float(p[2]));
          } else {
            const uint32_t px = reinterpret_cast<const uint32_t*>(rowb)[x * step];
            if (rgb10) { r = static_cast<uint8_t>((px & 0x3FF) >> 2); g = static_cast<uint8_t>((px >> 10 & 0x3FF) >> 2); b = static_cast<uint8_t>((px >> 20 & 0x3FF) >> 2); }
            else if (bgra) { b = px & 0xFF; g = px >> 8 & 0xFF; r = px >> 16 & 0xFF; }
            else { r = px & 0xFF; g = px >> 8 & 0xFF; b = px >> 16 & 0xFF; }
          }
          uint8_t* o = &out[(static_cast<size_t>(y) * w + x) * 3];
          o[0] = r; o[1] = g; o[2] = b;
        }
      }
      g_context->Unmap(st, 0);
    }
    st->Release();
    if (resolved) resolved->Release();
    back->Release();
    return ok ? 1 : 0;
  }
  // A scan has frame 0 with the fighter all dark, frame 1 with all its regions lit, frame k+2 with the regions whose
  // number has bit k lit, and the last frame dark again. Each pixel is judged against its own dark and lit values,
  // so the stage's light, HDR or a color filter can't move it across a fixed line. What doesn't change between dark
  // and lit (the stage, the HUD, an overlay) stays out, and so does what changed between the first and last dark
  // frames (something moved). A scan that can't be read cleanly is refused, with the reason, rather than guessed.
  void scan_capture(IDXGISwapChain* swap) {
    static std::vector<std::vector<uint8_t>> frames;
    static int w = 0, h = 0, seen_id = -1, seen_frame = -1, presents = 0;
    int id = 0, frame = 0, count = 0, groups = 0;
    if (!recolor_test::scanWanted(id, frame, count, groups)) return;
    if (id != seen_id || frame != seen_frame) { seen_id = id; seen_frame = frame; presents = 0; }
    if (++presents < 4) return;
    if (frame == 0) frames.assign(static_cast<size_t>(count), {});
    if (static_cast<int>(frames.size()) != count || count < 4 || count > 40) return;  // Joined half way, so the game gives up on it
    int fw = 0, fh = 0;
    std::string why;
    const int got = grab_frame(swap, frames[static_cast<size_t>(frame)], fw, fh, 2, why);
    const bool whole = recolor_test::scanWhole();
    if (got < 0) { std::vector<std::vector<uint8_t>>().swap(frames); recolor_test::scanFailed(id, why); return; }
    if (got == 0) return;  // Try again on the next present
    if (frame == 0) { w = fw; h = fh; }
    std::vector<recolor_test::ScanSample> out;
    if (fw != w || fh != h || w <= 0 || h <= 0) { std::vector<std::vector<uint8_t>>().swap(frames); recolor_test::scanFailed(id, "the window changed size during it."); return; }
    if (frame < count - 1) { recolor_test::scanGrabbed(id, frame); return; }
    struct Free { std::vector<std::vector<uint8_t>>& f; ~Free() { std::vector<std::vector<uint8_t>>().swap(f); } } free_frames{frames};  // After the last frame, all of them are let go once read
    const size_t n = static_cast<size_t>(w) * h;
    const int bits = count - 3, last = count - 1;
    auto lum = [&](int k, size_t i) { const uint8_t* p = &frames[static_cast<size_t>(k)][i * 3]; return p[0] + p[1] + p[2]; };
    // How much a lit region lifts here (the top tenth of it), to tell a real lift from a faint one.
    std::vector<int> lift;
    for (size_t i = 0; i < n; i++) if (const int d = lum(1, i) - lum(0, i); d >= 60) lift.push_back(d);
    if (lift.size() < 8) {  // Nothing lit up, so the fighter isn't in view
      COMBO_LOG("color editor: scan " + std::to_string(id) + ": nothing lit");
      recolor_test::scanResult(id, w * 2, h * 2, out);
      return;
    }
    std::nth_element(lift.begin(), lift.begin() + static_cast<std::ptrdiff_t>(lift.size() * 9 / 10), lift.end());
    const int least = (std::max)(60, lift[lift.size() * 9 / 10] / 3);
    std::vector<uint16_t> code(n, 0);
    size_t fighter = 0, moved = 0, clear_px = 0;
    for (size_t i = 0; i < n; i++) {
      const int l0 = lum(0, i), span = lum(1, i) - l0;
      if (span < least) continue;  // Doesn't light up. Not the fighter's, or too faint to read.
      fighter++;
      // One region (Sol's DI wing flare, whose flame flickers by itself): whatever lights up is it, moving or not
      if (groups == 1 && whole) { code[i] = 1; clear_px++; continue; }
      if (std::abs(lum(last, i) - l0) * 4 > span) { moved++; continue; }  // Changed between the dark frames, so it moved
      int c = 0;
      bool clear = true;
      for (int k = 0; k < bits && clear; k++) {
        const int t = (lum(k + 2, i) - l0) * 100 / span;  // 0 is its dark value, 100 its lit one
        if (t >= 65) c |= 1 << k;
        else if (t > 35) clear = false;  // Half lit (an edge's blend, or a filter), so it's out
      }
      if (clear && c > 0 && c <= groups) { code[i] = static_cast<uint16_t>(c); clear_px++; }
    }
    // Most of the fighter is unreadable, so refuse rather than guess. Only when enough of it is in view: a side that
    // shows next to nothing of it (hidden behind something) is read as it is, so a few stray pixels can't stop a mapping
    if (fighter >= (std::max)(static_cast<size_t>(64), n / 400) && clear_px * 2 < fighter) {
      COMBO_LOG("color editor: scan " + std::to_string(id) + ": " + std::to_string(clear_px) + " of " + std::to_string(fighter) + " pixels read cleanly, " +
                std::to_string(moved) + " moved");
      recolor_test::scanFailed(id, moved * 4 > fighter ? std::string("the picture kept changing during it (an overlay, or the fighter moving).")
                                                       : std::string("the picture is being altered (an overlay or filter like ReShade, or HDR). Turn it off."));
      return;
    }
    // A pixel counts when two of its four neighbours agree: strays drop out, thin lines stay.
    std::vector<uint16_t> kept(n, 0);
    std::vector<int> pixels(static_cast<size_t>(groups) + 1, 0);
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        const size_t i = static_cast<size_t>(y) * w + x;
        const int c = code[i];
        if (!c) continue;
        const int same = (x > 0 && code[i - 1] == c) + (x + 1 < w && code[i + 1] == c) + (y > 0 && code[i - w] == c) + (y + 1 < h && code[i + w] == c);
        if (same >= 2) { kept[i] = static_cast<uint16_t>(c); pixels[static_cast<size_t>(c)]++; }
      }
    std::vector<int> taken(static_cast<size_t>(groups) + 1, 0);
    for (size_t i = 0; i < n; i++) {
      const int c = kept[i];
      if (!c || pixels[static_cast<size_t>(c)] < 8) continue;  // Only a few stray pixels, so it's noise
      const int every = (std::max)(1, pixels[static_cast<size_t>(c)] / 64);
      if (taken[static_cast<size_t>(c)]++ % every) continue;
      out.push_back({static_cast<uint16_t>(c), static_cast<uint16_t>(i % w * 2), static_cast<uint16_t>(i / w * 2)});
    }
    float px = 0, py = 0;
    if (recolor_test::scanPoint(px, py)) {  // A pick takes every pixel right around the clicked point (thin lines included)
      const int cx = static_cast<int>(px * w), cy = static_cast<int>(py * h), r = 8;
      for (int y = (std::max)(0, cy - r); y <= (std::min)(h - 1, cy + r); y++)
        for (int x = (std::max)(0, cx - r); x <= (std::min)(w - 1, cx + r); x++) {
          const size_t i = static_cast<size_t>(y) * w + x;
          const int c = kept[i] ? kept[i] : code[i];
          if (c) out.push_back({static_cast<uint16_t>(c), static_cast<uint16_t>(x * 2), static_cast<uint16_t>(y * 2)});
        }
    }
    COMBO_LOG("color editor: scan " + std::to_string(id) + ": " + std::to_string(out.size()) + " samples");
    recolor_test::scanResult(id, w * 2, h * 2, out);
  }

  bool is_unreal_window(IDXGISwapChain* swap) {  // the game's own window (not a splash, movie or overlay one)
    DXGI_SWAP_CHAIN_DESC desc{};
    wchar_t cls[32] = {};
    return SUCCEEDED(swap->GetDesc(&desc)) && desc.OutputWindow && GetClassNameW(desc.OutputWindow, cls, 32) && wcscmp(cls, L"UnrealWindow") == 0;
  }
  HRESULT __stdcall hook_present(IDXGISwapChain* swap, UINT sync, UINT flags) {
    if (!(modScreenActive() || onlineVersionScreen() || menu_ui::g_online_picker)) return g_present(swap, sync, flags);  // Nothing to draw, so present the game's frame as it is
    if (!g_running && !g_init_failed && (is_unreal_window(swap) || ++g_other_presents > 300)) {  // Set up when first needed, on the game's own window
      std::lock_guard<std::mutex> once(g_frame_mutex);  // two threads must not both set up
      if (!g_running && !g_init_failed) {
        if (init(swap)) {
          g_running = true;
          COMBO_LOG("imgui: running");
        } else {
          g_init_failed = true;
          COMBO_LOG("imgui: init failed, using the HUD menu");
        }
      }
    }
    // The lock is let go before the real Present, which may resize on this thread.
    std::unique_lock<std::mutex> frame;
    if (g_running && g_rtv && is_game_window(swap)
        && (frame = std::unique_lock<std::mutex>(g_frame_mutex, std::try_to_lock)).owns_lock()) {
      // A window size or the overlay font changed: rebuild the fonts, so text stays sharp
      bool rebuild = ModMenu::overlayFont() != menu_ui::loaded_font;
      for (int e = 0; e < menu_ui::E_COUNT; e++)
        rebuild |= std::fabs(ModMenu::uiScale(e) - menu_ui::loaded_scales[e]) > 0.001f;
      if (rebuild) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        ImGui::GetIO().Fonts->Clear();
        DXGI_SWAP_CHAIN_DESC desc{};
        swap->GetDesc(&desc);
        menu_ui::load_fonts(static_cast<float>(desc.BufferDesc.Height));
        ImGui_ImplDX11_CreateDeviceObjects();
      }
      if (is_game_window(swap)) {  // Before our overlay draws
        DXGI_SWAP_CHAIN_DESC desc{};
        swap->GetDesc(&desc);
        recolor_test::overlayFrame(ImGui::GetIO().WantTextInput, static_cast<int>(desc.BufferDesc.Width), static_cast<int>(desc.BufferDesc.Height));
        scan_capture(swap);
      }
      ImGui_ImplDX11_NewFrame();
      ImGui_ImplWin32_NewFrame();
      ImGui::NewFrame();
      ImGui::GetIO().MouseDrawCursor = (modScreenActive() && (ModMenu::instance().isShowing() || menu_ui::g_timeline_visible || recolor_test::isOpen())) ||
                                       (!modScreenActive() && menu_ui::g_online_picker);
      draw_ui();
      ImGui::Render();
      g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
      ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
      frame.unlock();
    }
    return g_present(swap, sync, flags);
  }

  HRESULT __stdcall hook_resize(IDXGISwapChain* swap, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    if (!g_running || !is_game_window(swap)) return g_resize(swap, count, w, h, fmt, flags);
    std::lock_guard<std::mutex> frame(g_frame_mutex);
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    const HRESULT hr = g_resize(swap, count, w, h, fmt, flags);
    if (SUCCEEDED(hr)) create_rtv(swap);
    return hr;
  }
}

namespace imgui_overlay {
  bool running() { return g_running; }

  void setFrameBar(const FrameBarView& view) {
    auto to_utf8 = [](const std::wstring& w) {
      const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
      std::string u(static_cast<size_t>(n), '\0');
      WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), u.data(), n, nullptr, nullptr);
      return u;
    };
    std::lock_guard<std::mutex> lock(menu_ui::b_mutex);
    menu_ui::b_view = view;
    menu_ui::b_info[0] = to_utf8(view.info[0]);
    menu_ui::b_info[1] = to_utf8(view.info[1]);
    menu_ui::b_time = GetTickCount64();
  }

  void resetWindows() { menu_ui::g_layout_gen++; }

  void setGameMenuOpen(bool open) { menu_ui::g_game_menu_open = open; }
  void setOnlinePicker(bool open) { menu_ui::g_online_picker = open; }
  bool onlinePickerOpen() { return menu_ui::g_online_picker; }
  int takeOnlinePickStep() { return menu_ui::g_online_pick_step.exchange(0); }

  void setBattleVersion(const std::string& name) {
    std::lock_guard<std::mutex> lock(menu_ui::bv_mutex);
    menu_ui::bv_text = name;
    menu_ui::bv_time = GetTickCount64();
  }

  void setDamageRows(int side, const std::vector<DamageRow>& rows) {
    if (side < 0 || side > 1) return;
    std::lock_guard<std::mutex> lock(menu_ui::d_mutex);
    menu_ui::d_rows[side] = rows;
    menu_ui::d_time = GetTickCount64();
  }

  void setTensionLines(const std::vector<std::wstring>& lines) {
    std::vector<std::string> out;
    out.reserve(lines.size());
    for (const auto& w : lines) {
      const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
      std::string u(static_cast<size_t>(n), '\0');
      WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), u.data(), n, nullptr, nullptr);
      out.push_back(std::move(u));
    }
    std::lock_guard<std::mutex> lock(menu_ui::t_mutex);
    menu_ui::t_lines.swap(out);
    menu_ui::t_time = GetTickCount64();
  }

  bool install() {
    // A throwaway device and swap chain on a hidden window, only to read the
    // swap chain vtable that the game's own swap chain shares.
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr),
                   nullptr, nullptr, nullptr, nullptr, L"StriveLabsDummyDX", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* swap = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    const HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                     D3D11_SDK_VERSION, &sd, &swap, &dev, nullptr, &ctx);
    bool ok = false;
    if (SUCCEEDED(hr) && swap) {
      auto** vtable = *reinterpret_cast<void***>(swap);
      // Store the originals first. The game keeps presenting on its render thread meanwhile, and may call a hook the
      // moment it's in.
      g_present = reinterpret_cast<Present_t>(vtable[8]);
      g_resize = reinterpret_cast<ResizeBuffers_t>(vtable[13]);
      std::atomic_thread_fence(std::memory_order_release);
      patch_vtable(vtable, 8, reinterpret_cast<void*>(&hook_present));
      patch_vtable(vtable, 13, reinterpret_cast<void*>(&hook_resize));
      ok = true;
    }
    if (swap) swap->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    COMBO_LOG(ok ? "imgui: present hook installed" : "imgui: no DirectX 11 swap chain, using the HUD menu");
    return ok;
  }
}
