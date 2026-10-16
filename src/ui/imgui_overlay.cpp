#include "imgui_overlay.h"
#include "menu.h"
#include "combo_log.h"

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
  std::atomic<bool> g_running{false};

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
    if (g_running && modScreenActive() && (ModMenu::instance().isShowing() || menu_ui::g_timeline_visible)) {
      ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
      const ImGuiIO& io = ImGui::GetIO();
      const bool mouse = msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST;
      if (mouse && io.WantCaptureMouse) return 0;
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
    if (auto* srv = load_png((ModMenu::dataDir() / "StriveLabs_logo.png").wstring(), menu_ui::logo_aspect))
      menu_ui::logo = reinterpret_cast<ImTextureID>(srv);
    else
      COMBO_LOG("imgui: StriveLabs_logo.png not loaded");
    create_rtv(swap);
    g_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wndproc)));
    return true;
  }

  void draw_ui() { menu_ui::draw(); }

  HRESULT __stdcall hook_present(IDXGISwapChain* swap, UINT sync, UINT flags) {
    if (!g_running && !g_init_failed) {
      if (init(swap)) {
        g_running = true;
        COMBO_LOG("imgui: running");
      } else {
        g_init_failed = true;
        COMBO_LOG("imgui: init failed, using the HUD menu");
      }
    }
    if (g_running && g_rtv) {
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
      ImGui_ImplDX11_NewFrame();
      ImGui_ImplWin32_NewFrame();
      ImGui::NewFrame();
      ImGui::GetIO().MouseDrawCursor = modScreenActive() && (ModMenu::instance().isShowing() || menu_ui::g_timeline_visible);
      draw_ui();
      ImGui::Render();
      g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
      ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    return g_present(swap, sync, flags);
  }

  HRESULT __stdcall hook_resize(IDXGISwapChain* swap, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    const HRESULT hr = g_resize(swap, count, w, h, fmt, flags);
    if (g_running && SUCCEEDED(hr)) create_rtv(swap);
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
      g_present = reinterpret_cast<Present_t>(patch_vtable(vtable, 8, reinterpret_cast<void*>(&hook_present)));
      g_resize = reinterpret_cast<ResizeBuffers_t>(patch_vtable(vtable, 13, reinterpret_cast<void*>(&hook_resize)));
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
