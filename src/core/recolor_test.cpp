// Live color editor for the fighters in training (Ctrl+Alt+E, or the window's close box):
// 1. the character's official colors are loaded and read from the running game (each
// color's BaseMap / SssMap drawn into a render target and saved as PNG), one per frame;
// the body (MI_MAIN_BASE) and the weapon (MI_WEP_BASE) have their own;
// 2. a worker builds the region map: pixels with the same lit and shadow color in every
// color are one region (only the big ones are listed);
// 3. edits start from the worn color's own textures: every pixel of a region moves by the
// same offset, so edges and line details stay; the new textures go into the game's
// own material (its pooled dynamic instance), and the originals come back on reset;
// 4. the parts of the model are mapped once per character and kind (kept on disk): the free
// camera looks at the fighter from three sides while the regions light up in a binary
// code, so one short scan per side says where every region is on screen; the skeleton's
// bones under each region's pixels say what it is ("Jacket", "Hair", "Grip"). Selecting a
// region then glides the camera straight to it.
#include <polyhook2/Detour/x64Detour.hpp>
#include "recolor_test.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <compressapi.h>
#pragma comment(lib, "cabinet.lib")  // the Compression API, for recolor sharing
#include "paint_layer.h"
#include "arcsys.h"
#include "combo_log.h"
#include "combo_trials.h"
#include "sigscan.h"

#include <Unreal/FString.hpp>
#include <Unreal/Hooks.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Windows.h>
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

using namespace RC;
using namespace RC::Unreal;

static_assert(sizeof(RC::Unreal::FString) == 16 && sizeof(RC::Unreal::FName) == 8, "parameter layouts below");

namespace recolor_test {
namespace {
  using Clock = std::chrono::steady_clock;
  std::filesystem::path g_dir;
  // Recovered unsaved changes waiting to be read (see recovery, below).
  std::map<std::string, std::filesystem::path> g_edit_restore;  // target key -> its recovered edits file
  std::map<std::string, std::filesystem::path> g_fx_restore;    // slot -> its recovered effect colors
  std::map<std::string, std::filesystem::path> g_cap_restore;   // slot ("SOL/Color08") -> its recovered captured effect colors

  std::string narrow(const StringType& s) {
    std::string o;
    for (auto c : s) o += c < 0x80 ? static_cast<char>(c) : '?';
    return o;
  }

  // ---- engine calls ---------------------------------------------------------------
  UFunction* fn(const TCHAR* path) { return UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path); }
  UObject* cdo(const TCHAR* path) { return UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path); }

  int numMaterials(UObject* comp) {
    static UFunction* f = fn(STR("/Script/Engine.PrimitiveComponent:GetNumMaterials"));
    if (!f) return 0;
    alignas(8) uint8_t p[16] = {};
    comp->ProcessEvent(f, p);
    return *reinterpret_cast<int*>(p);
  }
  UObject* material(UObject* comp, int i) {
    static UFunction* f = fn(STR("/Script/Engine.PrimitiveComponent:GetMaterial"));
    if (!f) return nullptr;
    struct { int32_t index; int32_t pad; UObject* ret; } p{i, 0, nullptr};
    comp->ProcessEvent(f, &p);
    return p.ret;
  }
  UObject* getTexture(UObject* mid, const TCHAR* param) {
    static UFunction* f = fn(STR("/Script/Engine.MaterialInstanceDynamic:K2_GetTextureParameterValue"));
    if (!f || !mid || !mid->GetClassPrivate() || narrow(mid->GetClassPrivate()->GetName()).find("MaterialInstanceDynamic") == std::string::npos)
      return nullptr;  // a dynamic material's function, not to be called on anything else
    struct { FName name; UObject* ret; } p{FName(param, FNAME_Add), nullptr};
    mid->ProcessEvent(f, &p);
    return p.ret;
  }
  void setTexture(UObject* mid, const TCHAR* param, UObject* tex) {
    static UFunction* f = fn(STR("/Script/Engine.MaterialInstanceDynamic:SetTextureParameterValue"));
    if (!f || !tex) return;
    struct { FName name; UObject* value; } p{FName(param, FNAME_Add), tex};
    mid->ProcessEvent(f, &p);
  }
  UObject* parentOf(UObject* m) {
    auto* p = m->GetValuePtrByPropertyNameInChain<UObject*>(STR("Parent"));
    return p ? *p : nullptr;
  }
  // The color material this one was made from, if any: ".../ColorNN/MI_<kind>_BASE", kind "MAIN"
  // (the body), "WEP" (the weapon) or another part's (Dizzy's MAIN1, Venom's BALL, ...).
  // Color materials not named MI_<KIND>_BASE whose texture no such part uses, taken as parts of their own
  // (discover() decides): full name -> kind; and per character/kind the material's own name, to read the colors.
  // Kept on disk (cache/<CHR>/extra_parts.txt) so the menus know them before any fight, in every color slot.
  std::map<std::string, std::string> g_extra_mi;      // "CHR/MI_NAME" (upper case) -> kind
  std::map<std::string, std::wstring> g_extra_read;   // "CHR/KIND" -> the material's own name
  bool slotOfPath(const std::string& name, std::string& chr, std::string& color);
  std::string extraKey(const std::string& full) {  // "CHR/MI_NAME" for a color material's full name, or "" if it isn't one
    std::string chr, color;
    const size_t dot = full.rfind('.');
    if (dot == std::string::npos || !slotOfPath(full, chr, color)) return {};
    std::string k = chr + "/" + full.substr(dot + 1);
    for (auto& ch : k) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return k;
  }
  // Gets the kind from any color material's name.
  // "MI_DI_FLARE_UNIQUEC" -> "DI_FLARE_UNIQUEC", "MI_WEP_BASE2" -> "WEP2".
  bool kindOfName(const std::string& full, std::string& kind) {
    const size_t dot = full.rfind(".MI_");
    if (dot == std::string::npos) return false;
    kind = full.substr(dot + 4);
    for (auto& ch : kind) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    for (size_t at; (at = kind.find("_BASE")) != std::string::npos;) kind.erase(at, 5);
    return !kind.empty() && kind.size() <= 24 && std::all_of(kind.begin(), kind.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
  }
  UObject* extraSource(UObject* m, std::string& name, std::string& kind) {  // A candidate is any color material up the chain.
    for (int depth = 0; m && depth < 6; depth++, m = parentOf(m)) {
      name = narrow(m->GetFullName());
      if (name.find("/Material/Color") != std::string::npos && kindOfName(name, kind)) return m;
    }
    name.clear();
    return nullptr;
  }
  UObject* colorSource(UObject* m, std::string& name, std::string& kind) {
    for (int depth = 0; m && depth < 6; depth++, m = parentOf(m)) {
      name = narrow(m->GetFullName());
      if (name.find("/Material/Color") == std::string::npos) continue;
      if (!g_extra_mi.empty())
        if (auto x = g_extra_mi.find(extraKey(name)); x != g_extra_mi.end()) { kind = x->second; return m; }  // One of those, and approved.
      const size_t dot = name.rfind(".MI_");
      if (dot == std::string::npos || name.size() < dot + 10 || _stricmp(name.c_str() + name.size() - 5, "_BASE") != 0) continue;
      kind = name.substr(dot + 4, name.size() - 5 - (dot + 4));
      for (auto& ch : kind) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));  // So the files always use one spelling.
      if (!kind.empty() && kind.size() <= 24 &&
          std::all_of(kind.begin(), kind.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }))
        return m;  // The kind is what the cache files are named after.
    }
    name.clear();
    return nullptr;
  }
  std::vector<UObject*> fighters() {
    std::vector<UObject*> pawns;
    UObjectGlobals::FindAllOf(STR("REDPawnPlayer"), pawns);
    pawns.erase(std::remove_if(pawns.begin(), pawns.end(), [](UObject* p) {
      return p->GetFullName().find(STR("PersistentLevel")) == StringType::npos;
    }), pawns.end());
    std::sort(pawns.begin(), pawns.end());
    return pawns;
  }
  std::vector<UObject*> fighterParts() {
    std::vector<UObject*> pawns = fighters(), comps, out;
    UObjectGlobals::FindAllOf(STR("SkeletalMeshComponent"), comps);
    for (UObject* c : comps)
      if (std::find(pawns.begin(), pawns.end(), c->GetOuterPrivate()) != pawns.end()) out.push_back(c);
    return out;
  }
  UObject* renderLib() { static UObject* l = cdo(STR("/Script/Engine.Default__KismetRenderingLibrary")); return l; }
  int sizeOf(UObject* tex, const TCHAR* f) {
    UFunction* fnc = tex->GetFunctionByNameInChain(f);
    if (!fnc) return 0;
    alignas(8) uint8_t p[16] = {};
    tex->ProcessEvent(fnc, p);
    return *reinterpret_cast<int*>(p);
  }
  // A texture drawn 1:1 into a render target and saved by the engine as a PNG.
  bool drawToFile(UObject* world, UObject* tex, int w, int h, const std::filesystem::path& out) {
    static UFunction* create = fn(STR("/Script/Engine.KismetRenderingLibrary:CreateRenderTarget2D"));
    static UFunction* begin = fn(STR("/Script/Engine.KismetRenderingLibrary:BeginDrawCanvasToRenderTarget"));
    static UFunction* end = fn(STR("/Script/Engine.KismetRenderingLibrary:EndDrawCanvasToRenderTarget"));
    static UFunction* save = fn(STR("/Script/Engine.KismetRenderingLibrary:ExportRenderTarget"));
    static UFunction* release = fn(STR("/Script/Engine.KismetRenderingLibrary:ReleaseRenderTarget2D"));
    static UFunction* draw = fn(STR("/Script/Engine.Canvas:K2_DrawTexture"));
    if (!renderLib() || !create || !begin || !end || !save || !draw) return false;
    alignas(16) uint8_t c[0x40] = {};
    *reinterpret_cast<UObject**>(c) = world;
    *reinterpret_cast<int*>(c + 0x08) = w;
    *reinterpret_cast<int*>(c + 0x0C) = h;
    c[0x10] = 2;  // RTF_RGBA8, clear color 0, no mips
    renderLib()->ProcessEvent(create, c);
    UObject* rt = *reinterpret_cast<UObject**>(c + 0x28);
    if (!rt) return false;
    alignas(16) uint8_t b[0x80] = {};
    *reinterpret_cast<UObject**>(b) = world;
    *reinterpret_cast<UObject**>(b + 0x08) = rt;
    renderLib()->ProcessEvent(begin, b);
    UObject* canvas = *reinterpret_cast<UObject**>(b + 0x10);
    if (canvas) {
      alignas(16) uint8_t dp[0x60] = {};
      *reinterpret_cast<UObject**>(dp) = tex;
      *reinterpret_cast<float*>(dp + 0x10) = static_cast<float>(w);
      *reinterpret_cast<float*>(dp + 0x14) = static_cast<float>(h);
      *reinterpret_cast<float*>(dp + 0x20) = 1.0f;
      *reinterpret_cast<float*>(dp + 0x24) = 1.0f;
      const float white[4] = {1, 1, 1, 1};
      std::memcpy(dp + 0x28, white, sizeof(white));
      dp[0x38] = 0;  // BLEND_Opaque
      *reinterpret_cast<float*>(dp + 0x40) = 0.5f;
      *reinterpret_cast<float*>(dp + 0x44) = 0.5f;
      canvas->ProcessEvent(draw, dp);
    }
    alignas(16) uint8_t e[0x60] = {};
    *reinterpret_cast<UObject**>(e) = world;
    std::memcpy(e + 0x08, b + 0x20, 0x40);
    renderLib()->ProcessEvent(end, e);
    if (canvas) {
      struct { UObject* world; UObject* rt; FString dir; FString name; } x{world, rt, FString(out.parent_path().wstring().c_str()),
                                                                            FString(out.filename().wstring().c_str())};
      renderLib()->ProcessEvent(save, &x);
    }
    if (release) { struct { UObject* rt; } r{rt}; renderLib()->ProcessEvent(release, &r); }
    std::error_code ec;
    return canvas && std::filesystem::exists(out, ec);
  }
  UObject* loadAsset(const std::wstring& path) {
    static UFunction* f = fn(STR("/Script/Engine.KismetSystemLibrary:LoadAsset_Blocking"));
    static UObject* sys = cdo(STR("/Script/Engine.Default__KismetSystemLibrary"));
    if (!f || !sys) return nullptr;
    struct {
      int32_t weak_index = -1, weak_serial = 0;  // TSoftObjectPtr is a weak pointer, a tag, then the soft path
      int32_t tag = 0, pad = 0;
      FName asset;
      FString sub;
      UObject* ret = nullptr;
    } p;
    static_assert(offsetof(decltype(p), ret) == 0x28, "LoadAsset_Blocking layout");
    p.asset = FName(path.c_str(), FNAME_Add);
    sys->ProcessEvent(f, &p);
    return p.ret;
  }
  // The color material's own texture for a parameter. Each list entry is 0x28 bytes, with the name first and the
  // texture at +0x10.
  UObject* micTexture(UObject* mic, const char* param) {
    struct Arr { uint8_t* data; int32_t num, max; };
    auto* arr = reinterpret_cast<Arr*>(reinterpret_cast<uint8_t*>(mic) + 0x100);
    for (int k = 0; arr->data && k < arr->num && k < 32; k++) {
      uint8_t* el = arr->data + k * 0x28;
      if (narrow(reinterpret_cast<FName*>(el)->ToString()) == param) return *reinterpret_cast<UObject**>(el + 0x10);
    }
    return nullptr;
  }
  UObject* importPng(UObject* world, std::vector<uint8_t>& png) {
    static UFunction* f = fn(STR("/Script/Engine.KismetRenderingLibrary:ImportBufferAsTexture2D"));
    if (!f || !renderLib()) return nullptr;
    struct { UObject* world; uint8_t* data; int32_t num, max; UObject* ret; } p{world, png.data(), static_cast<int32_t>(png.size()),
                                                                                static_cast<int32_t>(png.size()), nullptr};
    renderLib()->ProcessEvent(f, &p);
    return p.ret;
  }
  // Bones. The parameter blocks follow the engine's offsets, since FName may be 8-aligned in our headers.
  struct Vec { float x = 0, y = 0, z = 0; };
  int numBones(UObject* comp) {
    static UFunction* f = fn(STR("/Script/Engine.SkinnedMeshComponent:GetNumBones"));
    if (!f) return 0;
    alignas(8) uint8_t p[16] = {};
    comp->ProcessEvent(f, p);
    return *reinterpret_cast<int*>(p);
  }
  FName boneName(UObject* comp, int i) {
    static UFunction* f = fn(STR("/Script/Engine.SkinnedMeshComponent:GetBoneName"));
    alignas(8) uint8_t p[16] = {};  // BoneIndex at 0, ReturnValue at 4
    FName name;
    if (!f) return name;
    *reinterpret_cast<int32_t*>(p) = i;
    comp->ProcessEvent(f, p);
    std::memcpy(&name, p + 4, sizeof(FName));
    return name;
  }
  Vec socketAt(UObject* comp, const FName& name) {
    static UFunction* f = fn(STR("/Script/Engine.SceneComponent:GetSocketLocation"));
    alignas(8) uint8_t p[32] = {};  // InSocketName at 0, ReturnValue at 8
    Vec v;
    if (!f) return v;
    std::memcpy(p, &name, sizeof(FName));
    comp->ProcessEvent(f, p);
    std::memcpy(&v, p + 8, sizeof(v));
    return v;
  }

  // ---- images -----------------------------------------------------------------------
  struct Image { int w = 0, h = 0; std::vector<uint8_t> px; };  // RGBA8
  bool decodePng(const std::filesystem::path& file, Image& out) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT w = 0, h = 0;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
              SUCCEEDED(wic->CreateDecoderFromFilename(file.wstring().c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(conv->GetSize(&w, &h)) && w > 0 && h > 0 && w <= 8192 && h <= 8192;
    if (ok) {
      out.w = static_cast<int>(w); out.h = static_cast<int>(h);
      out.px.resize(static_cast<size_t>(w) * h * 4);
      ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.px.size()), out.px.data()));
    }
    for (IUnknown* u : {static_cast<IUnknown*>(conv), static_cast<IUnknown*>(frame), static_cast<IUnknown*>(dec), static_cast<IUnknown*>(wic)})
      if (u) u->Release();
    return ok;
  }
  // PNG with stored (uncompressed) deflate blocks. It takes no time to compress and the engine reads it fine.
  uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0xFFFFFFFFu) {
    static const std::array<uint32_t, 256> table = [] {  // Made once and thread safe, since workers encode too.
      std::array<uint32_t, 256> t{};
      for (uint32_t i = 0; i < 256; i++) {
        uint32_t v = i;
        for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
        t[i] = v;
      }
      return t;
    }();
    for (size_t i = 0; i < n; i++) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
  }
  // The game's color textures are linear (SRGB off); a texture imported by the engine is sRGB,
  // so the GPU would decode ours and everything would come out darker. Encoding the colors
  // with the sRGB curve first makes that decode land back on the values we mean.
  uint8_t toSrgb(uint8_t v) {
    static const std::array<uint8_t, 256> lut = [] {  // Made once and thread safe.
      std::array<uint8_t, 256> t{};
      for (int i = 0; i < 256; i++) {
        const double x = i / 255.0;
        const double s = x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1.0 / 2.4) - 0.055;
        t[static_cast<size_t>(i)] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(s * 255.0)), 0, 255));
      }
      return t;
    }();
    return lut[v];
  }
  std::vector<uint8_t> encodePng(const Image& im, bool for_import = true) {
    std::vector<uint8_t> raw;  // Filter byte 0, then the row. For import the colors are sRGB encoded and alpha is left as is.
    const size_t row = static_cast<size_t>(im.w) * 4;
    raw.reserve((row + 1) * im.h);
    for (int y = 0; y < im.h; y++) {
      raw.push_back(0);
      const size_t start = raw.size();
      raw.insert(raw.end(), im.px.begin() + y * row, im.px.begin() + (y + 1) * row);
      for (size_t i = start; for_import && i < raw.size(); i += 4) {
        raw[i] = toSrgb(raw[i]); raw[i + 1] = toSrgb(raw[i + 1]); raw[i + 2] = toSrgb(raw[i + 2]);
      }
    }
    std::vector<uint8_t> z{0x78, 0x01};
    for (size_t at = 0; at < raw.size();) {
      const size_t n = (std::min)(raw.size() - at, static_cast<size_t>(65535));
      const bool last = at + n == raw.size();
      z.push_back(last ? 1 : 0);
      z.push_back(n & 0xFF); z.push_back(n >> 8);
      z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
      z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
      at += n;
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw.size();) {  // adler32, in chunks so the sums can't overflow
      const size_t end = (std::min)(raw.size(), i + 5552);
      for (; i < end; i++) { a += raw[i]; b += a; }
      a %= 65521; b %= 65521;
    }
    const uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back((adler >> s) & 0xFF);
    std::vector<uint8_t> out{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
      const uint32_t n = static_cast<uint32_t>(data.size());
      for (int s = 24; s >= 0; s -= 8) out.push_back((n >> s) & 0xFF);
      const size_t start = out.size();
      out.insert(out.end(), type, type + 4);
      out.insert(out.end(), data.begin(), data.end());
      const uint32_t c = crc32(out.data() + start, out.size() - start) ^ 0xFFFFFFFFu;
      for (int s = 24; s >= 0; s -= 8) out.push_back((c >> s) & 0xFF);
    };
    std::vector<uint8_t> ihdr(13);
    for (int s = 0; s < 4; s++) { ihdr[s] = (im.w >> (24 - 8 * s)) & 0xFF; ihdr[4 + s] = (im.h >> (24 - 8 * s)) & 0xFF; }
    ihdr[8] = 8; ihdr[9] = 6;  // 8 bits, RGBA
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return out;
  }
  uint32_t rgbAt(const Image& im, size_t p) { return (uint32_t(im.px[p * 4]) << 16) | (uint32_t(im.px[p * 4 + 1]) << 8) | im.px[p * 4 + 2]; }
  int dist2(uint32_t a, uint32_t b) {
    const int dr = int(a >> 16 & 0xFF) - int(b >> 16 & 0xFF), dg = int(a >> 8 & 0xFF) - int(b >> 8 & 0xFF), db = int(a & 0xFF) - int(b & 0xFF);
    return dr * dr + dg * dg + db * db;
  }

  // ---- a character's region map (worker) ---------------------------------------------
  struct CharMap {
    int w = 0, h = 0, regions = 0;
    int major = 0;  // Regions big enough on their own (0.05% of the texture). The small ones come after.
    int pieces = 0;  // Where the small ones (same-color pieces) end. The strokes (texels close to each other) come after.
    std::vector<uint16_t> owner;  // region per pixel, 0xFFFF = none
    std::vector<float> share;
    std::vector<GroupView> groups;  // the outfit's parts, from how the official colors paint them
    std::vector<std::string> prefixes;  // the colors it was made from ("Color01", ...)
  };
  float hueOf(uint32_t c, float& s, float& v) {
    const float r = (c >> 16 & 0xFF) / 255.0f, g = (c >> 8 & 0xFF) / 255.0f, b = (c & 0xFF) / 255.0f;
    const float mx = (std::max)({r, g, b}), mn = (std::min)({r, g, b}), d = mx - mn;
    v = mx;
    s = mx > 0 ? d / mx : 0;
    if (d <= 0) return 0;
    float h = mx == r ? (g - b) / d : mx == g ? 2 + (b - r) / d : 4 + (r - g) / d;
    h /= 6;
    return h < 0 ? h + 1 : h;
  }
  // Regions that share their lit color in nearly every official color are one part. A part is a line (always near
  // black), skin (skin toned in most colors) or neutral (low saturation).
  void findParts(CharMap& m, const std::vector<std::vector<uint32_t>>& lit) {  // lit[region][color]
    const int R = m.regions;
    const size_t C = R ? lit[0].size() : 0;
    std::vector<int> up(R);
    for (int r = 0; r < R; r++) up[r] = r;
    auto root = [&](int r) { while (up[r] != r) r = up[r] = up[up[r]]; return r; };
    for (int a = 0; a < R; a++)
      for (int b = a + 1; b < R; b++) {
        size_t same = 0;
        for (size_t c = 0; c < C; c++) same += dist2(lit[a][c], lit[b][c]) <= 100;
        if (C && same * 100 >= C * 85) up[root(b)] = root(a);
      }
    std::map<int, size_t> at;
    for (int r = 0; r < R; r++) {
      const int g = root(r);
      if (!at.count(g)) { at[g] = m.groups.size(); m.groups.push_back({}); }
      GroupView& gv = m.groups[at[g]];
      gv.regions.push_back(r);
      gv.share += m.share[r];
    }
    for (GroupView& gv : m.groups) {
      size_t dark = 0, skin = 0, grey = 0;
      for (size_t c = 0; c < C; c++) {
        float s, v;
        const float h = hueOf(lit[gv.regions[0]][c], s, v);
        dark += v < 0.13f;
        skin += h >= 0.022f && h <= 0.133f && s >= 0.15f && s <= 0.75f && v >= 0.35f;
        grey += s < 0.18f;
      }
      gv.line = C && dark * 100 >= C * 80;
      gv.skin = !gv.line && C && skin * 100 >= C * 70;
      gv.neutral = !gv.line && !gv.skin && C && grey * 100 >= C * 70;
    }
    std::sort(m.groups.begin(), m.groups.end(), [](const GroupView& a, const GroupView& b) { return a.share > b.share; });
  }
  // prefixes are the colors' file names in dir ("Color01", or "Color01_WEP" for a weapon).
  std::mutex g_pin_m;  // Guards the pinned textures in cache/<CHR>/pinned (see syncPinned). Taken after buildMap's own lock, never before it.
  // own is the slot whose current textures go in (its own map). Every other slot uses its pinned textures (as first
  // read), if kept.
  std::shared_ptr<CharMap> buildMap(std::filesystem::path dir, std::vector<std::string> prefixes, std::string own = {}) {
    static std::mutex one;  // One at a time, since a 4K map holds a few hundred MB while it's being made.
    std::lock_guard<std::mutex> only(one);
    std::lock_guard<std::mutex> pins(g_pin_m);  // So the pins don't change underneath, since they're read two or three times.
    auto m = std::make_shared<CharMap>();
    std::vector<uint64_t> h;
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> pairs;  // each color's lit and shadow textures, both read
    Image b0;  // The first color's lit texture, used by the ring growth.
    for (const auto& c : prefixes) {
      Image im[2];
      std::error_code pec;
      const std::filesystem::path at = c != own && std::filesystem::exists(dir / "pinned" / (c + "_BaseMap.png"), pec) &&
                                               std::filesystem::exists(dir / "pinned" / (c + "_SssMap.png"), pec) ? dir / "pinned" : dir;
      const bool got[2] = {decodePng(at / (c + "_BaseMap.png"), im[0]), decodePng(at / (c + "_SssMap.png"), im[1])};
      bool ok = got[0] && got[1];
      for (int t = 0; t < 2; t++) {
        if (!got[t]) continue;  // As before, whatever of the pair could be read goes into the hash.
        if (h.empty()) { m->w = im[t].w; m->h = im[t].h; h.assign(static_cast<size_t>(im[t].w) * im[t].h, 1469598103934665603ull); }
        if (im[t].w != m->w || im[t].h != m->h) { ok = false; continue; }
        for (size_t p = 0; p < h.size(); p++) h[p] = (h[p] ^ rgbAt(im[t], p)) * 1099511628211ull;
      }
      if (ok) {
        pairs.push_back({at / (c + "_BaseMap.png"), at / (c + "_SssMap.png")});
        if (!b0.w) b0 = std::move(im[0]);
      }
    }
    if (h.empty()) return m;
    const size_t N = h.size();
    std::vector<std::pair<uint32_t, uint64_t>> big;
    {
      std::unordered_map<uint64_t, uint32_t> count;
      count.reserve(1 << 16);
      for (uint64_t v : h) count[v]++;
      const uint32_t min = (std::max)(64u, static_cast<uint32_t>(N / 2000));  // 0.05% of the texture
      for (const auto& [k, n] : count) if (n >= min) big.push_back({n, k});
    }  // Freed here, since a noisy 4K texture gives millions of entries.
    std::sort(big.begin(), big.end(), std::greater<>());
    if (big.size() > 2000) big.resize(2000);
    std::vector<size_t> rep;  // One texel per region, which has the region's exact color in every color.
    {
      std::unordered_map<uint64_t, uint16_t> id;
      for (size_t i = 0; i < big.size(); i++) {
        id[big[i].second] = static_cast<uint16_t>(i);
        m->share.push_back(static_cast<float>(big[i].first) / N);
      }
      m->regions = m->major = static_cast<int>(big.size());
      m->owner.assign(N, 0xFFFF);
      rep.assign(static_cast<size_t>(m->regions), SIZE_MAX);
      for (size_t p = 0; p < N; p++) {
        auto it = id.find(h[p]);
        if (it == id.end()) continue;
        m->owner[p] = it->second;
        if (rep[it->second] == SIZE_MAX) rep[it->second] = p;
      }
    }
    // Texels in no region are shading painted into a part (every texel a slightly different color) and thin lines too
    // small to count. Shading joins the region next to it, ring by ring outward, when it's near that region's color in
    // the first color. It stays only if it's near it, lit and shadow, in every color (shading changes with the region,
    // fabric or line art beside it doesn't). What's left in pieces of 64 texels or more becomes small regions of their
    // own, numbered after the others so their numbers and saves stay.
    const int W = m->w, H = m->h, T2 = 70 * 70;
    auto near4 = [&](size_t p, auto&& fn) {
      const int x = static_cast<int>(p % W), y = static_cast<int>(p / W);
      if (x > 0) fn(p - 1);
      if (x + 1 < W) fn(p + 1);
      if (y > 0) fn(p - W);
      if (y + 1 < H) fn(p + W);
    };
    // One texel of each piece that may become a small region (a piece has the same colors all over).
    std::unordered_map<uint64_t, size_t> piece_rep;
    {
      std::unordered_map<uint64_t, uint32_t> left;
      for (size_t p = 0; p < N; p++) if (m->owner[p] == 0xFFFF) left[h[p]]++;
      for (size_t p = 0; p < N; p++)
        if (m->owner[p] == 0xFFFF && left[h[p]] >= 64) piece_rep.emplace(h[p], p);
    }
    std::vector<uint16_t> cand;
    std::vector<uint8_t> keep;
    if (b0.w == W && b0.h == H && m->regions > 0) {
      std::vector<uint32_t> lit0(static_cast<size_t>(m->regions));
      for (int r = 0; r < m->regions; r++) lit0[r] = rgbAt(b0, rep[r]);
      cand = m->owner;
      std::vector<uint8_t> queued(N, 0);
      std::vector<size_t> ring, next;
      for (size_t p = 0; p < N; p++)
        if (cand[p] != 0xFFFF)
          near4(p, [&](size_t q) { if (cand[q] == 0xFFFF && !queued[q]) { queued[q] = 1; ring.push_back(q); } });
      std::vector<std::pair<size_t, uint16_t>> got;
      while (!ring.empty()) {
        got.clear();
        for (size_t p : ring) {
          const uint32_t c = rgbAt(b0, p);
          int best = T2 + 1;
          uint16_t pick = 0xFFFF;
          near4(p, [&](size_t q) {
            if (cand[q] == 0xFFFF) return;
            const int d = dist2(c, lit0[cand[q]]);
            if (d < best) { best = d; pick = cand[q]; }
          });
          if (pick != 0xFFFF) got.push_back({p, pick}); else queued[p] = 0;  // Another side may reach it later.
        }
        for (const auto& [p, r] : got) cand[p] = r;
        next.clear();
        for (const auto& [p, r] : got)
          near4(p, [&](size_t q) { if (cand[q] == 0xFFFF && !queued[q]) { queued[q] = 1; next.push_back(q); } });
        ring.swap(next);
      }
      keep.assign(N, 0);
      for (size_t p = 0; p < N; p++) keep[p] = m->owner[p] == 0xFFFF && cand[p] != 0xFFFF;
    }
    b0 = Image{};
    // For every color, check the grown texels (lit and shadow) and read each region's and piece's lit color. For the
    // texels in no region, find which neighbour (right, below) stays close to them in every color (strokes).
    std::vector<size_t> loose;
    for (size_t p = 0; p < N; p++) if (m->owner[p] == 0xFFFF) loose.push_back(p);
    std::vector<uint8_t> close_to(loose.size(), 0);  // Bit 0 is the right neighbour, bit 1 the one below.
    for (size_t i = 0; i < loose.size(); i++) {
      const size_t p = loose[i];
      if (static_cast<int>(p % W) + 1 < W && m->owner[p + 1] == 0xFFFF) close_to[i] |= 1;
      if (p + W < N && m->owner[p + W] == 0xFFFF) close_to[i] |= 2;
    }
    constexpr int kNear2 = 20 * 20;
    std::vector<std::vector<uint32_t>> lit(static_cast<size_t>(m->regions));
    std::unordered_map<uint64_t, std::vector<uint32_t>> piece_lit;
    for (const auto& [fb, fs] : pairs) {
      Image ib, is;
      if (!decodePng(fb, ib) || ib.w != W || ib.h != H) continue;
      const bool sss = decodePng(fs, is) && is.w == W && is.h == H;
      for (int r = 0; r < m->regions; r++) lit[r].push_back(rgbAt(ib, rep[r]));
      for (const auto& [k, p] : piece_rep) piece_lit[k].push_back(rgbAt(ib, p));
      for (size_t i = 0; i < loose.size(); i++) {
        if (!close_to[i]) continue;
        const size_t p = loose[i];
        if ((close_to[i] & 1) && (dist2(rgbAt(ib, p), rgbAt(ib, p + 1)) > kNear2 || (sss && dist2(rgbAt(is, p), rgbAt(is, p + 1)) > kNear2))) close_to[i] &= ~1;
        if ((close_to[i] & 2) && (dist2(rgbAt(ib, p), rgbAt(ib, p + W)) > kNear2 || (sss && dist2(rgbAt(is, p), rgbAt(is, p + W)) > kNear2))) close_to[i] &= ~2;
      }
      if (keep.empty()) continue;
      for (size_t p = 0; p < N; p++) {
        if (!keep[p]) continue;
        const size_t q = rep[cand[p]];
        if (dist2(rgbAt(ib, p), rgbAt(ib, q)) > T2 || (sss && dist2(rgbAt(is, p), rgbAt(is, q)) > T2)) keep[p] = 0;
      }
    }
    for (size_t p = 0; p < keep.size(); p++) if (keep[p]) m->owner[p] = cand[p];
    std::vector<uint16_t>().swap(cand);
    std::vector<uint8_t>().swap(keep);
    {  // The small regions are the pieces still in no region.
      std::unordered_map<uint64_t, uint32_t> left;
      for (size_t p = 0; p < N; p++) if (m->owner[p] == 0xFFFF && piece_rep.count(h[p])) left[h[p]]++;
      std::vector<std::pair<uint32_t, uint64_t>> pieces;
      for (const auto& [k, n] : left) if (n >= 64) pieces.push_back({n, k});
      std::sort(pieces.begin(), pieces.end(), std::greater<>());
      if (pieces.size() > static_cast<size_t>(2000 - m->regions)) pieces.resize(static_cast<size_t>(2000 - m->regions));
      std::unordered_map<uint64_t, uint16_t> sid;
      for (size_t i = 0; i < pieces.size(); i++) {
        sid[pieces[i].second] = static_cast<uint16_t>(m->regions + i);
        m->share.push_back(static_cast<float>(pieces[i].first) / N);
        lit.push_back(piece_lit[pieces[i].second]);
      }
      if (!sid.empty())
        for (size_t p = 0; p < N; p++) {
          if (m->owner[p] != 0xFFFF) continue;
          auto it = sid.find(h[p]);
          if (it != sid.end()) m->owner[p] = it->second;
        }
      m->regions += static_cast<int>(pieces.size());
    }
    m->pieces = m->regions;
    {  // Strokes are what's left in no region, joined where neighbours stay close in every color, 64 texels or more
      std::vector<int32_t> at(N, -1);  // texel -> its index in loose (still in no region)
      for (size_t i = 0; i < loose.size(); i++) if (m->owner[loose[i]] == 0xFFFF) at[loose[i]] = static_cast<int32_t>(i);
      std::vector<int32_t> up(loose.size());
      for (size_t i = 0; i < up.size(); i++) up[i] = static_cast<int32_t>(i);
      auto top = [&](int32_t x) { while (up[x] != x) { up[x] = up[up[x]]; x = up[x]; } return x; };
      for (size_t i = 0; i < loose.size(); i++) {
        if (at[loose[i]] < 0) continue;
        const size_t p = loose[i];
        for (const auto& [bit, q] : {std::pair<int, size_t>{1, p + 1}, std::pair<int, size_t>{2, p + W}}) {
          if (!(close_to[i] & bit) || q >= N || at[q] < 0) continue;
          const int32_t a = top(static_cast<int32_t>(i)), b = top(at[q]);
          if (a != b) up[(std::max)(a, b)] = (std::min)(a, b);  // The lower index leads, so it's the same on every run.
        }
      }
      std::unordered_map<int32_t, uint32_t> size;
      for (size_t i = 0; i < loose.size(); i++) if (at[loose[i]] >= 0) size[top(static_cast<int32_t>(i))]++;
      std::vector<std::pair<uint32_t, int32_t>> strokes;  // (texels, -lead), so biggest first, then by place
      for (const auto& [lead, n] : size) if (n >= 64) strokes.push_back({n, -lead});
      std::sort(strokes.begin(), strokes.end(), std::greater<>());
      if (strokes.size() > static_cast<size_t>(2000 - m->regions)) strokes.resize(static_cast<size_t>(2000 - m->regions));
      std::unordered_map<int32_t, uint16_t> sid;
      std::vector<size_t> srep;
      for (size_t i = 0; i < strokes.size(); i++) {
        sid[-strokes[i].second] = static_cast<uint16_t>(m->regions + i);
        m->share.push_back(static_cast<float>(strokes[i].first) / N);
        srep.push_back(loose[static_cast<size_t>(-strokes[i].second)]);  // Its lead texel, the first one in it.
      }
      if (!sid.empty()) {
        for (size_t i = 0; i < loose.size(); i++) {
          if (at[loose[i]] < 0) continue;
          auto it = sid.find(top(static_cast<int32_t>(i)));
          if (it != sid.end()) m->owner[loose[i]] = it->second;
        }
        const size_t first = lit.size();
        lit.resize(first + srep.size());
        for (const auto& [fb, fs] : pairs) {  // Their colors take one more look at each color's lit texture.
          Image ib;
          if (!decodePng(fb, ib) || ib.w != W || ib.h != H) continue;
          for (size_t i = 0; i < srep.size(); i++) lit[first + i].push_back(rgbAt(ib, srep[i]));
        }
        for (size_t i = first; i < lit.size(); i++) if (lit[i].size() != (lit.empty() ? 0 : lit.front().size())) lit[i].assign(lit.front().size(), 0);
        m->regions += static_cast<int>(srep.size());
      }
    }
    findParts(*m, lit);
    return m;
  }

  // Reads a map file (MAP3, the colors' list then the map) whole and checks it, or returns nothing. The list comes
  // back as the map's prefixes. With want_* set, only a map of that size and big-region count is accepted (the rest
  // isn't read).
  std::shared_ptr<CharMap> readMapFile(const std::filesystem::path& file, int want_w = 0, int want_h = 0, int want_major = -1) {
    std::ifstream f(file, std::ios::binary);
    auto rd = [&](void* d, size_t n) { return static_cast<bool>(f.read(static_cast<char*>(d), static_cast<std::streamsize>(n))); };
    uint32_t magic = 0, siglen = 0;
    if (f && rd(&magic, 4) && magic == 0x3350414D /* "MAP3" */ && rd(&siglen, 4) && siglen < 1 << 16) {
      std::string s2(siglen, '\0');
      auto m = std::make_shared<CharMap>();
      uint32_t ng = 0;
      if (rd(s2.data(), siglen) && rd(&m->w, 4) && rd(&m->h, 4) && rd(&m->regions, 4) && rd(&m->major, 4) && rd(&m->pieces, 4) && m->w > 0 && m->h > 0 &&
          m->w <= 8192 && m->h <= 8192 && m->regions >= 0 && m->regions <= 2000 && m->major >= 0 && m->major <= m->pieces && m->pieces <= m->regions &&
          (!want_w || (m->w == want_w && m->h == want_h)) && (want_major < 0 || m->major == want_major)) {
        m->share.resize(static_cast<size_t>(m->regions));
        m->owner.resize(static_cast<size_t>(m->w) * m->h);
        bool ok = rd(m->share.data(), m->share.size() * 4) && rd(m->owner.data(), m->owner.size() * 2) && rd(&ng, 4) && ng <= 2000;
        for (uint32_t g = 0; ok && g < ng; g++) {
          GroupView gv;
          uint32_t nr = 0;
          uint8_t flags = 0;
          ok = rd(&nr, 4) && nr <= 2000;
          if (ok) { gv.regions.resize(nr); ok = rd(gv.regions.data(), nr * sizeof(int)) && rd(&gv.share, 4) && rd(&flags, 1); }
          gv.line = flags & 1; gv.skin = flags & 2; gv.neutral = flags & 4;
          if (ok) m->groups.push_back(std::move(gv));
        }
        // A bad file is never trusted and gets rebuilt, so every region number has to be within the map.
        for (uint16_t o : m->owner) ok = ok && (o == 0xFFFF || o < m->regions);
        for (float sh : m->share) ok = ok && std::isfinite(sh) && sh >= 0.0f && sh <= 1.0f;
        for (const GroupView& gv : m->groups) {
          ok = ok && !gv.regions.empty() && std::isfinite(gv.share);
          for (int r : gv.regions) ok = ok && r >= 0 && r < m->regions;
        }
        if (ok) {
          std::stringstream ss(s2);
          for (std::string c; std::getline(ss, c, ';');) m->prefixes.push_back(c);
          return m;
        }
      }
    }
    return nullptr;
  }
  bool writeMapFile(const std::filesystem::path& file, const CharMap& m) {  // Written beside it, then renamed, so it's whole or not there at all.
    std::string sig;
    for (const auto& p : m.prefixes) sig += p + ";";
    static std::atomic<int> n{0};
    std::filesystem::path tmp = file;
    tmp += "." + std::to_string(GetCurrentThreadId()) + "_" + std::to_string(n++);
    bool ok;
    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      auto wr = [&](const void* d, size_t n) { f.write(static_cast<const char*>(d), static_cast<std::streamsize>(n)); };
      const uint32_t magic = 0x3350414D, siglen = static_cast<uint32_t>(sig.size()), ng = static_cast<uint32_t>(m.groups.size());
      wr(&magic, 4); wr(&siglen, 4); wr(sig.data(), sig.size());
      wr(&m.w, 4); wr(&m.h, 4); wr(&m.regions, 4); wr(&m.major, 4); wr(&m.pieces, 4);
      wr(m.share.data(), m.share.size() * 4); wr(m.owner.data(), m.owner.size() * 2); wr(&ng, 4);
      for (const GroupView& gv : m.groups) {
        const uint32_t nr = static_cast<uint32_t>(gv.regions.size());
        const uint8_t flags = (gv.line ? 1 : 0) | (gv.skin ? 2 : 0) | (gv.neutral ? 4 : 0);
        wr(&nr, 4); wr(gv.regions.data(), nr * sizeof(int)); wr(&gv.share, 4); wr(&flags, 1);
      }
      f.flush();
      ok = f.good();
    }
    std::error_code ec;
    if (ok) std::filesystem::rename(tmp, file, ec);
    if (!ok || ec) { std::filesystem::remove(tmp, ec); return false; }
    return true;
  }
  void archiveMap(const std::filesystem::path& dir, const std::string& kind, const CharMap& m);
  // The colors read for a character's part in training (colors_<kind>.txt, written when the reading is done). Only a
  // complete set may stand in for reading them again. whole means every color listed is there with both textures.
  std::vector<std::string> cachedPrefixesIn(const std::filesystem::path& dir, const std::string& kind, bool* whole = nullptr) {
    std::vector<std::string> out;
    std::ifstream f(dir / ("colors_" + kind + ".txt"));
    if (whole) *whole = static_cast<bool>(f);
    std::string line;
    std::error_code ec;
    while (std::getline(f, line)) {
      const bool name_ok = line.size() >= 7 && line.compare(0, 5, "Color") == 0 && std::isdigit(static_cast<unsigned char>(line[5])) &&
                           std::isdigit(static_cast<unsigned char>(line[6])) &&
                           std::all_of(line.begin(), line.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
      if (name_ok && std::filesystem::exists(dir / (line + "_BaseMap.png"), ec)) out.push_back(line);
      if (whole && (!name_ok || !std::filesystem::exists(dir / (line + "_BaseMap.png"), ec) || !std::filesystem::exists(dir / (line + "_SssMap.png"), ec))) *whole = false;
    }
    return out;
  }
  std::vector<std::string> cachedPrefixes(const std::string& chr, const std::string& kind) { return cachedPrefixesIn(g_dir / "cache" / chr, kind); }
  // The same, kept while colors_<KIND>.txt is unchanged. Runs on the game thread, where the editor asks every frame.
  const std::vector<std::string>& cachedPrefixesNow(const std::string& chr, const std::string& kind) {
    static std::map<std::string, std::pair<std::filesystem::file_time_type, std::vector<std::string>>> memo;
    std::error_code ec;
    const auto when = std::filesystem::last_write_time(g_dir / "cache" / chr / ("colors_" + kind + ".txt"), ec);
    auto& m = memo[chr + "/" + kind];
    if (ec || m.first != when || m.second.empty()) m = {ec ? std::filesystem::file_time_type{} : when, cachedPrefixes(chr, kind)};
    return m.second;
  }

  // The map is saved next to the colors it came from (map_<kind>.bin), with the list of those colors, and built
  // again only if the list differs. Saves decoding every color's textures each session.
  // own picks a slot's own map (own/map_<prefix>.bin, made again when the slot's textures are newer than it).
  std::shared_ptr<CharMap> mapFor(std::filesystem::path dir, std::string kind, std::vector<std::string> prefixes, std::string own = {}) {
    const auto file = own.empty() ? dir / ("map_" + kind + ".bin") : dir / "own" / ("map_" + own + ".bin");
    auto old = readMapFile(file);
    std::error_code ec;
    const bool fresh = own.empty() || (old && std::filesystem::last_write_time(file, ec) >= std::filesystem::last_write_time(dir / (own + "_BaseMap.png"), ec) &&
                                       std::filesystem::last_write_time(file, ec) >= std::filesystem::last_write_time(dir / (own + "_SssMap.png"), ec) && !ec);
    if (old && old->prefixes == prefixes && fresh) { archiveMap(dir, kind, *old); return old; }  // Kept too, so a save made on it can move onto another.
    auto m = buildMap(dir, prefixes, own);
    if (m) m->prefixes = prefixes;
    bool whole = false;
    const bool all = cachedPrefixesIn(dir, kind, &whole) == prefixes && whole;  // A map of only some of the colors never replaces the file.
    if (m && m->regions > 0 && all) {
      if (old) archiveMap(dir, kind, *old);  // Keep the map it replaces, so saves made on it can still be moved.
      std::filesystem::create_directories(file.parent_path(), ec);
      writeMapFile(file, *m);
      archiveMap(dir, kind, *m);
    }
    return m;
  }

  // ---- the worn color, ready to edit (worker) ----------------------------------------
  struct Worn {
    Image base, sss;
    std::vector<uint16_t> owner;         // The map's owners, plus edge pixels next to a region in its color
    std::vector<uint32_t> lit, shadow;   // The worn color's own, per region
  };
  std::shared_ptr<Worn> prepareWorn(std::shared_ptr<CharMap> map, std::filesystem::path base, std::filesystem::path sss) {
    auto w = std::make_shared<Worn>();
    if (!decodePng(base, w->base) || !decodePng(sss, w->sss)) return nullptr;
    if (w->base.w != map->w || w->base.h != map->h || w->sss.w != map->w || w->sss.h != map->h) return nullptr;
    const int R = map->regions;
    const size_t N = map->owner.size();
    std::vector<std::unordered_map<uint32_t, uint32_t>> lc(R), sc(R);
    for (size_t p = 0; p < N; p += 3) {
      const uint16_t r = map->owner[p];
      if (r == 0xFFFF) continue;
      lc[r][rgbAt(w->base, p)]++;
      sc[r][rgbAt(w->sss, p)]++;
    }
    auto mode = [](const std::unordered_map<uint32_t, uint32_t>& m) {
      uint32_t best = 0, n = 0;
      for (const auto& [k, v] : m) if (v > n) { n = v; best = k; }
      return best;
    };
    w->lit.resize(R); w->shadow.resize(R);
    for (int r = 0; r < R; r++) { w->lit[r] = mode(lc[r]); w->shadow[r] = mode(sc[r]); }
    // Edge pixels (blends between regions). Up to 3 px out, a pixel joins a neighbour's region when its color is
    // close to that region's.
    w->owner = map->owner;
    const int W = map->w, H = map->h;
    for (int pass = 0; pass < 3; pass++) {
      std::vector<uint16_t> next = w->owner;
      for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
          const size_t p = static_cast<size_t>(y) * W + x;
          if (w->owner[p] != 0xFFFF) continue;
          const uint32_t c = rgbAt(w->base, p);
          int best = 70 * 70 + 1;
          for (const auto& [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
            const int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
            const uint16_t r = w->owner[static_cast<size_t>(ny) * W + nx];
            if (r == 0xFFFF) continue;
            const int d = dist2(c, w->lit[r]);
            if (d < best) { best = d; next[p] = r; }
          }
        }
      w->owner.swap(next);
    }
    return w;
  }

  // ---- editor state (game thread) -----------------------------------------------------
  struct Edit { bool has_lit = false, has_shadow = false, derive = false; uint32_t lit = 0, shadow = 0; };
  struct Target {
    UObject* pawn = nullptr;                          // the fighter wearing it
    std::string chr, color, kind;                     // "SOL", "Color01", "MAIN" (the body) or "WEP" (the weapon). "ANJ_C02" is Anji's 2nd costume
    std::string src;                                  // the color folder its textures come from, if not its own (Anji's koi: Color01's in every color)
    std::vector<UObject*> mids;                       // the game's materials wearing it
    size_t own_mids = 0;                              // how many of them are on its meshes (the rest: the fight pool's live copies)
    std::vector<std::pair<UObject*, UObject*>> orig;  // Each material's own BaseMap and SssMap
    std::vector<UObject*> comps;                      // the meshes wearing it (their bones name the parts)
    bool kept_hidden = false;                         // a model the game only shows during a move (Sol's Dragon Install)
    std::vector<const uint8_t*> items;                // Object slot of each mid, then each comp, when found (for alive checks, as in Kept)
  };
  struct Reading {
    std::string chr, kind;
    int next = 1;
    std::vector<std::string> found;
    int stuck = 0;                           // Frames a color has waited for its files
    int only = 0;                            // One color is read again (its textures changed), the others are kept
    std::map<std::string, std::string> ids;  // prefix -> its textures' ids, as read
  };
  struct Shown { UObject* base = nullptr; UObject* sss = nullptr; };
  // Where a region is on the model, from the mapping (kept on disk per character and kind).
  struct Part {
    std::string bone;          // the bone most of its pixels sit on
    float off[3] = {};         // Offset from it, only when no pixel was near a bone
    float frac = 0, dist = 0;  // the share of the view it took, at that camera distance
  };
  // A region's place on the model, from the bone atlas: a point on a mesh, in the space of the bone it moves with,
  // and the way the surface faces there. own: the region is at the middle of the atlas cell it came from (where the
  // atlas has its point).
  struct AtlasMesh;
  struct Place { std::string mesh, bone; float p[3] = {}, n[3] = {}; bool own = false; const AtlasMesh* am = nullptr; int bi = -1; };  // (am, bi: its mesh and bone in the atlas)
  const std::vector<std::vector<Place>>* placesFor(const Target& t);  // Null when the part has no atlas (defined with anchorOf)
  bool atlasCovers(const Target& t);                                 // The part's meshes are in an atlas, so it needs no mapping
  std::set<std::string> drawnMeshes(const Target& t);                // The part's meshes drawn now (none if none is), lower case

  std::mutex g_m;  // guards g_view and g_reqs
  View g_view;
  struct Req { int kind; int region; uint32_t rgb; std::vector<Pick> picks = {}; std::vector<int> regions = {}; std::string text = {}; float f[4] = {};
               std::vector<float> nums = {}; };
  enum { kToggle, kLit, kShadow, kSelect, kReset, kNext, kFollow, kPairs, kBlackout, kCamera, kFollowSel, kPart, kRemap, kExport, kVec, kVecReset, kRename, kLogDrawn, kFx, kFxReset, kCapture, kCap, kCapReset, kLists, kExportRecolor, kImportRecolor, kCopySlot, kReread, kSave, kDiscardClose, kCancelClose, kUndo, kRedo, kRecovery, kPickAt, kPaintBegin, kPaintTo, kPaintEnd, kPaintFill, kPaintClear, kImportTex, kGroupSame, kPaletteSave, kPaletteLoad, kPaletteEdit, kPaletteDelete, kListEffects, kMat, kMatReset, kHide, kOnScreen, kPaintShape, kResetColor, kFxOne, kFxOneReset };
  std::vector<Req> g_reqs;

  bool g_open = false;
  std::vector<Target> g_targets;
  int g_target = 0;
  std::map<std::string, std::shared_ptr<CharMap>> g_maps;  // By map key (character and kind)
  std::set<std::string> g_map_failed;                      // map keys whose colors couldn't be read
  std::unique_ptr<Reading> g_reading;
  std::future<std::shared_ptr<CharMap>> g_map_job;
  std::string g_map_job_key;
  std::shared_ptr<Worn> g_worn;
  int g_worn_serial = 0;  // New each time g_worn is replaced. Frames made from it record which one.
  std::string g_worn_key;  // the target g_worn is for
  std::set<std::string> g_remapped;  // Map keys rebuilt this session for a new color (only once each)
  // A pick is the point clicked (0..1), then one scan of every region from where the camera is.
  bool g_pick_wanted = false, g_pick_scanning = false, g_pick_quiet = false;
  float g_pick_xy[2] = {};
  int g_picked = -1, g_pick_serial = 0;
  std::vector<int> g_pick_choices;
  // The flat texture view (see textureView). Made here and handed over under g_prev_m.
  std::atomic<bool> g_texview_on{false};
  std::atomic<int> g_texview_map{0};  // 0 lit, 1 shadow, 2 the decal's lines
  std::atomic<bool> g_texview_plain{false};  // A paint tool is out, so the picture is undimmed with no box
  std::mutex g_prev_m;
  std::vector<uint8_t> g_prev_px;
  int g_prev_w = 0, g_prev_h = 0, g_prev_serial = 0;
  float g_prev_box[4] = {};
  struct DsCache { std::vector<uint16_t> owner; std::vector<uint32_t> base, sss; int w = 0, h = 0; };  // the view's downsample
  std::shared_ptr<const DsCache> g_prev_ds;  // the regions behind the view's picture, as drawn (under g_prev_m)
  int g_ds_for = -1;  // the worn color the view's downsample (g_ds) is of
  std::atomic<long long> g_texview_ms{0};  // The UI's last frame that drew the view. None for a moment means it was closed.
  uint64_t g_prev_sig = 0;
  Clock::time_point g_prev_at;
  Clock::time_point g_pick_since;
  std::string g_pick_frames;           // The part whose pick the current scan frames are for ("" for none), so they can be used again
  Clock::time_point g_pick_frames_until;
  std::future<std::shared_ptr<Worn>> g_worn_job;
  std::string g_worn_job_key;
  std::set<std::string> g_worn_failed;  // Targets whose worn color couldn't be made (a texture was missing). Not retried until remapped.
  std::map<std::string, std::vector<Edit>> g_edits;  // by target
  std::map<std::string, std::vector<Part>> g_parts;  // By map key. Empty means not mapped.
  bool g_follow = true;
  std::vector<int> g_selected;
  std::vector<int> g_selected_rows;  // Rows the user chose. g_selected adds their twins, which are highlighted but not flown to.
  const std::vector<int>& selRows() { return g_selected_rows.empty() ? g_selected : g_selected_rows; }
  std::map<std::string, Shown> g_shown;  // our textures on each target (rooted while held)
  Shown g_hl, g_hl_off;                  // the selection's highlight, and the other blink phase
  bool g_blackout = true;
  bool g_dirty = false;
  Clock::time_point g_last_change, g_blink_at;
  bool g_blink_on = false;
  int g_blinks_left = 0;  // Flashes left for this selection, then the normal look stays
  std::string g_status;
  bool g_view_dirty = true;
  void* g_fight = nullptr;  // the engine the targets' materials belong to
  bool g_find_due = false;  // the editor was opened: the fighters are searched for again if the last search is old
  std::string g_setup_home;  // the part the first-time setup started on, shown again when the fighter's other parts are mapped
  std::vector<UObject*> g_pawns;  // the fighters on stage when they were found (sorted)

  std::string keyOf(const Target& t) { return t.chr + "/" + t.color + "/" + t.kind; }
  // The cache's files: "Color01_BaseMap.png" for the body, "Color01_WEP_BaseMap.png" for the weapon.
  std::string prefixOf(const std::string& color, const std::string& kind) { return kind == "MAIN" ? color : color + "_" + kind; }
  const std::string& srcOf(const Target& t) { return t.src.empty() ? t.color : t.src; }
  // Which region map a slot uses. Either "P", the shared one, made from the pinned textures (every slot as first
  // read), or for a slot whose textures changed since (a skin pak), its own, by its prefix (cache/<CHR>/own_<KIND>.txt
  // lists them). A pak on one slot changes only that slot's regions.
  std::vector<std::string> readLines(const std::filesystem::path& f);
  bool colorName(const std::string& c);
  std::mutex g_own_m;
  std::map<std::string, std::set<std::string>> g_own;  // "<chr>/<kind>" -> the prefixes with a map of their own
  std::filesystem::path ownFile(const std::filesystem::path& dir, const std::string& kind) { return dir / ("own_" + kind + ".txt"); }
  std::string mapIdOf(const std::string& chr, const std::string& kind, const std::string& prefix) {
    std::lock_guard<std::mutex> lk(g_own_m);
    auto it = g_own.find(chr + "/" + kind);
    if (it == g_own.end()) {  // Read once, and again after a map job, since the list may have changed.
      std::set<std::string> own;
      for (const std::string& l : readLines(ownFile(g_dir / "cache" / chr, kind))) if (colorName(l)) own.insert(l);
      it = g_own.emplace(chr + "/" + kind, std::move(own)).first;
    }
    return it->second.count(prefix) ? prefix : std::string("P");
  }
  void forgetOwn(const std::string& chr, const std::string& kind) { std::lock_guard<std::mutex> lk(g_own_m); g_own.erase(chr + "/" + kind); }
  std::string mapKey(const Target& t) { return t.chr + "/" + t.kind + "/" + mapIdOf(t.chr, t.kind, prefixOf(srcOf(t), t.kind)); }
  // A texture's id is its LightingGuid, made with the asset (a skin mod's textures have their own). "" if none.
  std::string texId(UObject* tex) {
    auto* id = tex ? tex->GetValuePtrByPropertyNameInChain<uint32_t>(STR("LightingGuid")) : nullptr;
    if (!id || !(id[0] | id[1] | id[2] | id[3])) return {};
    char g[40];
    snprintf(g, sizeof(g), "%08X%08X%08X%08X", id[0], id[1], id[2], id[3]);
    return g;
  }
  // The ids of each slot's textures when they were read. ids_<KIND>.txt lines are prefix, tab, lit id, space,
  // shadow id. Game thread.
  std::map<std::string, std::map<std::string, std::string>> g_tex_ids;  // "<chr>/<kind>" -> prefix -> ids
  std::filesystem::path idsFile(const std::string& chr, const std::string& kind) { return g_dir / "cache" / chr / ("ids_" + kind + ".txt"); }
  std::map<std::string, std::string>& texIds(const std::string& chr, const std::string& kind) {
    auto it = g_tex_ids.find(chr + "/" + kind);
    if (it != g_tex_ids.end()) return it->second;
    std::map<std::string, std::string> ids;
    for (const std::string& l : readLines(idsFile(chr, kind))) {
      const size_t tab = l.find('\t');
      if (tab != std::string::npos && colorName(l.substr(0, tab))) ids[l.substr(0, tab)] = l.substr(tab + 1);
    }
    return g_tex_ids[chr + "/" + kind] = std::move(ids);
  }
  // Compares what a part wears with what was read for its slot. 0 is the same (or can't tell), 1 is changed (a skin
  // mod added or removed since), 2 is unknown (read before ids were kept).
  int texState(const Target& t) {  // Uses the part's own color material, so the game's textures and never ours
    if (t.mids.empty()) return 0;
    std::string n, k;
    UObject* mic = colorSource(t.mids.front(), n, k);
    if (!mic) return 0;
    const std::string a = texId(micTexture(mic, "BaseMap")), b = texId(micTexture(mic, "SssMap"));
    if (a.empty() || b.empty()) return 0;
    const auto& ids = texIds(t.chr, t.kind);
    auto it = ids.find(prefixOf(srcOf(t), t.kind));
    return it == ids.end() ? 2 : it->second == a + " " + b ? 0 : 1;
  }
  std::set<std::string> g_reread_tried;  // "<chr>/<kind>/<prefix>" read again this session. Only once, so never in a loop
  std::string mapIdOfKey(const std::string& mk) { return mk.substr(mk.rfind('/') + 1); }
  std::string ownOfKey(const std::string& mk) { const std::string id = mapIdOfKey(mk); return id == "P" ? std::string() : id; }
  bool kindOfKey(const std::string& mk, const std::string& chr, const std::string& kind) { return mk.rfind(chr + "/" + kind + "/", 0) == 0; }
  // "/Game/Chara/SOL/Costume01/Material/Color08/MI_MAIN_BASE.MI_MAIN_BASE" -> "SOL", "Color08". The editor treats
  // another costume as a character of its own ("ANJ_C02"), with its own textures, regions and saves.
  bool slotOfPath(const std::string& name, std::string& chr, std::string& color) {
    const size_t c = name.find("/Chara/"), k = name.find("/Material/Color");
    if (c == std::string::npos || k == std::string::npos || k < c + 10 || name.size() < k + 17) return false;
    chr = name.substr(c + 7, 3);
    const size_t cos = name.find("/Costume", c);
    if (cos != std::string::npos && cos + 10 <= k) {
      const std::string n = name.substr(cos + 8, 2);
      if (n != "01" && std::isdigit(static_cast<unsigned char>(n[0])) && std::isdigit(static_cast<unsigned char>(n[1]))) chr += "_C" + n;
    }
    color = name.substr(k + 10, 7);
    return true;
  }
  std::string codeOf(const std::string& chr) { return chr.substr(0, 3); }  // "ANJ_C02" -> "ANJ"
  std::wstring materialDir(const std::string& chr, const std::string& color) {  // "/Game/Chara/ANJ/Costume02/Material/Color01"
    const std::string costume = chr.size() > 5 ? "Costume" + chr.substr(5) : "Costume01", code = codeOf(chr);
    return L"/Game/Chara/" + std::wstring(code.begin(), code.end()) + L"/" + std::wstring(costume.begin(), costume.end()) + L"/Material/" +
           std::wstring(color.begin(), color.end());
  }
  // A mesh's color material, as found. groupParts builds the parts from them.
  // - A fighter wears one color. A part whose material comes from another color's folder still counts as the
  // fighter's color (Anji's koi is Color01's in every color), unless the fighter also has that part in its own
  // color. Then it's left as the game has it (Potemkin's Color12 shows a Color10 material).
  // - Parts drawn from the same texture are one part (Jam's skin and glass use her body's texture). The merged part
  // is the body's, else the weapon's, else the first by name.
  struct Seen { UObject* comp; UObject* m; UObject* mic; UObject* owner; std::string chr, color, kind; };
  std::vector<Target> groupParts(const std::vector<Seen>& seen) {
    std::map<std::pair<UObject*, std::string>, std::map<std::string, int>> colors;  // (owner, chr) -> color -> materials
    std::map<std::pair<UObject*, std::string>, std::map<std::string, int>> body;  // The same, for the body only
    for (const Seen& s : seen) {
      colors[{s.owner, s.chr}][s.color]++;
      if (s.kind == "MAIN") body[{s.owner, s.chr}][s.color]++;
    }
    auto wornColor = [&](UObject* owner, const std::string& chr) {  // The body's most common color, else the most common overall
      auto b = body.find({owner, chr});
      const auto& cs = b != body.end() ? b->second : colors[{owner, chr}];
      return std::max_element(cs.begin(), cs.end(), [](const auto& x, const auto& y) { return x.second < y.second; })->first;
    };
    std::vector<Seen> own;  // Those in the fighter's color
    std::vector<std::string> src;
    for (const Seen& s : seen) {
      const std::string worn = wornColor(s.owner, s.chr);
      if (s.color != worn) {
        const bool has_own = std::any_of(seen.begin(), seen.end(), [&](const Seen& o) { return o.owner == s.owner && o.chr == s.chr && o.kind == s.kind && o.color == worn; });
        static std::set<std::string> said;
        const std::string line = s.chr + " " + worn + " wears " + s.color + "'s " + s.kind + (has_own ? ": left as it is" : ": edited as " + worn + "'s");
        if (said.insert(line).second) COMBO_LOG("color editor: " + line);
        if (has_own) continue;
        own.push_back(s);
        own.back().color = worn;
        src.push_back(s.color);
        continue;
      }
      own.push_back(s);
      src.push_back(std::string());
    }
    // The texture of each part, from its color materials' own BaseMap (never ours)
    std::map<std::string, std::set<UObject*>> tex;  // "chr/color/kind"
    for (const Seen& s : own)
      if (UObject* b = micTexture(s.mic, "BaseMap")) tex[s.chr + "/" + s.color + "/" + s.kind].insert(b);
    auto rank = [](const std::string& k) { return k == "MAIN" ? std::string("0") : k == "WEP" ? std::string("1") : "2" + k; };
    std::map<std::string, std::string> into;  // "chr/color/kind" -> the kind it's edited as
    for (const auto& [a, ta] : tex) {
      std::string best = a.substr(a.rfind('/') + 1);
      for (const auto& [b, tb] : tex) {
        if (a == b || a.substr(0, a.rfind('/')) != b.substr(0, b.rfind('/'))) continue;
        if (std::none_of(ta.begin(), ta.end(), [&](UObject* x) { return tb.count(x) > 0; })) continue;
        const std::string kb = b.substr(b.rfind('/') + 1);
        if (rank(kb) < rank(best)) best = kb;
      }
      if (best != a.substr(a.rfind('/') + 1)) {
        into[a] = best;
        static std::set<std::string> said;
        if (said.insert(a).second) COMBO_LOG("color editor: " + a + " is drawn from " + best + "'s texture: edited with it");
      }
    }
    std::vector<Target> found;
    for (size_t i = 0; i < own.size(); i++) {
      const Seen& s = own[i];
      Target t;
      t.chr = s.chr;
      t.color = s.color;
      auto in = into.find(s.chr + "/" + s.color + "/" + s.kind);
      t.kind = in == into.end() ? s.kind : in->second;
      auto it = std::find_if(found.begin(), found.end(), [&](const Target& f) { return keyOf(f) == keyOf(t); });
      if (it == found.end()) { found.push_back(t); it = found.end() - 1; }
      if (in == into.end() && !src[i].empty()) it->src = src[i];
      if (std::find(it->mids.begin(), it->mids.end(), s.m) == it->mids.end()) it->mids.push_back(s.m);
      if (std::find(it->comps.begin(), it->comps.end(), s.comp) == it->comps.end()) it->comps.push_back(s.comp);
      if (!it->pawn) it->pawn = s.owner;
    }
    // The part's own materials go first, since the first one's SssMap decides which take the edited one.
    for (Target& t : found)
      std::stable_partition(t.mids.begin(), t.mids.end(), [&](UObject* m) {
        std::string n, k;
        return colorSource(m, n, k) && k == t.kind;
      });
    return found;
  }
  Shown& edited(const Target& t) { return g_shown[keyOf(t)]; }
  Target* target() { return g_target >= 0 && g_target < static_cast<int>(g_targets.size()) ? &g_targets[g_target] : nullptr; }
  // Kept from garbage collection while we hold them. This goes through the object item because the installed
  // UE4SS exports that, not UObject::SetRootSet's inline helper.
  void root(UObject* o) { if (o && o->GetObjectItem()) o->GetObjectItem()->SetRootSet(); }
  void unroot(Shown& s) {
    for (UObject* t : {s.base, s.sss})
      if (t && t->GetObjectItem()) t->GetObjectItem()->UnsetRootSet();
    s = Shown{};
  }
  // The pair shown for a region now. That's an edit, or the worn color's own (the shadow follows the lit color by
  // the same per-channel ratio as the worn color's pair).
  std::pair<uint32_t, uint32_t> pairOfW(const Worn& w, bool follow, int r, const std::vector<Edit>& ed) {
    const uint32_t ol = w.lit[r], os = w.shadow[r];
    if (r >= static_cast<int>(ed.size())) return {ol, os};
    const Edit& e = ed[r];
    const uint32_t l = e.has_lit ? e.lit : ol;
    if (e.has_shadow) return {l, e.shadow};
    if (!e.has_lit || (!follow && !e.derive)) return {l, os};
    uint32_t s = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
      const float ratio = (float(os >> sh & 0xFF) + 8.0f) / (float(ol >> sh & 0xFF) + 8.0f);
      const int v = std::clamp(static_cast<int>(std::lround((float(l >> sh & 0xFF) + 8.0f) * ratio - 8.0f)), 0, 255);
      s |= static_cast<uint32_t>(v) << sh;
    }
    return {l, s};
  }
  std::pair<uint32_t, uint32_t> pairOf(int r, const std::vector<Edit>& ed) { return pairOfW(*g_worn, g_follow, r, ed); }
  // A region's color from a picture: the most common one among the texels counted, or their average when no color
  // has a fifth of them (a shaded texture). counts by color; sum r g b of all of them; n how many.
  uint32_t regionColor(const std::unordered_map<uint32_t, uint32_t>& counts, const uint64_t sum[3], uint32_t n) {
    uint32_t best = 0, most = 0;
    for (const auto& [c, k] : counts) if (k > most) { most = k; best = c; }
    if (most * 5 >= n) return best;
    return static_cast<uint32_t>((sum[0] / n) << 16 | (sum[1] / n) << 8 | (sum[2] / n));
  }
  // New textures from the worn color, with edited regions (and the highlighted ones, in magenta) moved.
  // Blackout makes everything black except the highlighted regions, which are white (lit) and light grey (shadow).
  void buildW(const Worn& w, bool follow, const std::vector<Edit>& ed, const std::vector<int>& highlight, Image& ob, Image& os, bool blackout = false);
  void build(const std::vector<Edit>& ed, const std::vector<int>& highlight, Image& ob, Image& os, bool blackout = false) {
    buildW(*g_worn, g_follow, ed, highlight, ob, os, blackout);
  }
  void buildW(const Worn& w, bool follow, const std::vector<Edit>& ed, const std::vector<int>& highlight, Image& ob, Image& os, bool blackout) {
    if (blackout) {
      std::vector<uint8_t> lit_up(w.lit.size(), 0);
      for (int r : highlight) if (r >= 0 && r < static_cast<int>(lit_up.size())) lit_up[r] = 1;
      ob = w.base; os = w.sss;  // alpha kept
      const size_t N = w.owner.size();
      for (size_t p = 0; p < N; p++) {
        const uint16_t r = w.owner[p];
        const bool on = r != 0xFFFF && lit_up[r];
        for (int c = 0; c < 3; c++) { ob.px[p * 4 + c] = on ? 255 : 0; os.px[p * 4 + c] = on ? 200 : 0; }
      }
      return;
    }
    const int R = static_cast<int>(w.lit.size());
    std::vector<std::array<int, 6>> off(R);
    std::vector<uint8_t> on(R, 0);
    for (int r = 0; r < R; r++) {
      std::pair<uint32_t, uint32_t> want = pairOfW(w, follow, r, ed);
      if (std::find(highlight.begin(), highlight.end(), r) != highlight.end()) want = {0xFF00FF, 0x7A007A};
      if (want.first == w.lit[r] && want.second == w.shadow[r]) continue;
      on[r] = 1;
      for (int c = 0; c < 3; c++) {
        const int sh = 16 - 8 * c;
        off[r][c] = int(want.first >> sh & 0xFF) - int(w.lit[r] >> sh & 0xFF);
        off[r][3 + c] = int(want.second >> sh & 0xFF) - int(w.shadow[r] >> sh & 0xFF);
      }
    }
    ob = w.base; os = w.sss;
    const size_t N = w.owner.size();
    for (size_t p = 0; p < N; p++) {
      const uint16_t r = w.owner[p];
      if (r == 0xFFFF || !on[r]) continue;
      for (int c = 0; c < 3; c++) {
        ob.px[p * 4 + c] = static_cast<uint8_t>(std::clamp(int(ob.px[p * 4 + c]) + off[r][c], 0, 255));
        os.px[p * 4 + c] = static_cast<uint8_t>(std::clamp(int(os.px[p * 4 + c]) + off[r][3 + c], 0, 255));
      }
    }
  }
  struct Pngs { std::vector<uint8_t> base, sss; };
  using PaintBytes = std::shared_ptr<const std::vector<uint8_t>>;
  void paintOver(const PaintBytes& paint, Image& ob, Image& os) {  // Worker. Composes the paint's layers over the region colors.
    if (!paint || paint->empty()) return;
    paint::Canvas c;
    if (!c.load(*paint, ob.w, ob.h) || ob.w != os.w || ob.h != os.h) return;  // A different size is left off.
    for (auto [im, map] : {std::pair<Image*, paint::Map>{&ob, paint::Map::Lit}, std::pair<Image*, paint::Map>{&os, paint::Map::Shadow}}) {
      paint::Image base{im->w, im->h, std::move(im->px)}, out;
      if (c.compose(map, base, out)) im->px = std::move(out.px); else im->px = std::move(base.px);
    }
  }
  Pngs encodeEdited(std::shared_ptr<Worn> w, std::vector<Edit> ed, bool follow, PaintBytes paint) {  // Worker. Reads only what it's given.
    Image ob, os;
    buildW(*w, follow, ed, {}, ob, os);
    paintOver(paint, ob, os);
    return {encodePng(ob), encodePng(os)};
  }
  bool importPngs(UObject* world, Pngs& p, Shown& out) {  // (game thread)
    UObject* tb = importPng(world, p.base);
    UObject* ts = importPng(world, p.sss);
    if (!tb || !ts) return false;
    root(tb); root(ts);
    out = {tb, ts};
    return true;
  }
  std::vector<std::future<Pngs>> g_png_orphans;  // Jobs no longer wanted, dropped once done (never waited on)
  std::atomic<int> g_encoding{0};
  std::future<Pngs> g_tex_job;  // the shown part's edited textures being made (update() puts them on when done)
  Image halved(const Image& im) {  // Every other texel (nearest, so region colors stay exact)
    Image o;
    o.w = (std::max)(1, im.w / 2);
    o.h = (std::max)(1, im.h / 2);
    o.px.resize(static_cast<size_t>(o.w) * o.h * 4);
    for (int y = 0; y < o.h; y++)
      for (int x = 0; x < o.w; x++)
        std::memcpy(&o.px[(static_cast<size_t>(y) * o.w + x) * 4], &im.px[(static_cast<size_t>(y * 2) * im.w + x * 2) * 4], 4);
    return o;
  }
  Pngs encodeHighlight(std::shared_ptr<Worn> w, std::vector<Edit> ed, std::vector<int> sel, bool follow, bool blackout) {  // (worker)
    struct Count { ~Count() { g_encoding--; } } done;
    Image ob, os;
    buildW(*w, follow, ed, sel, ob, os, blackout);
    return {encodePng(halved(ob)), encodePng(halved(os))};
  }
  std::future<Pngs> g_hl_job;       // A highlight being made, either the selection's (off false) or the blackout's (off true)
  bool g_hl_job_off = false;
  std::vector<int> g_hl_job_sel;
  std::string g_hl_job_key;
  std::string g_tex_key;                // Jobs running. One at a time, since each holds about half a GB at 4K.
  Pngs encodeOne(std::shared_ptr<Worn> w, std::vector<Edit> ed, bool follow, PaintBytes paint) {
    struct Count { ~Count() { g_encoding--; } } done;
    return encodeEdited(std::move(w), std::move(ed), follow, std::move(paint));
  }
  std::future<Pngs> startEncode(std::shared_ptr<Worn> w, std::vector<Edit> ed, bool follow, PaintBytes paint) {  // Only when none is running.
    g_encoding++;
    try { return std::async(std::launch::async, encodeOne, std::move(w), std::move(ed), follow, std::move(paint)); }
    catch (...) { g_encoding--; return {}; }  // No thread, so no job. Tried again later.
  }
  void dropPngOrphans() {
    g_png_orphans.erase(std::remove_if(g_png_orphans.begin(), g_png_orphans.end(),
                                       [](auto& f) { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }),
                        g_png_orphans.end());
  }
  bool importPair(UObject* world, const Image& ob, const Image& os, Shown& out) {
    auto pb = encodePng(ob), ps = encodePng(os);
    UObject* tb = importPng(world, pb);
    UObject* ts = importPng(world, ps);
    if (!tb || !ts) return false;
    root(tb); root(ts);
    out = {tb, ts};
    return true;
  }
  bool makeTextures(UObject* world, const std::vector<Edit>& ed, const std::vector<int>& highlight, Shown& out, bool blackout = false) {
    Image ob, os;
    build(ed, highlight, ob, os, blackout);
    return importPair(world, ob, os, out);
  }
  // A scan frame, at about 1024 texels (enough to keep thin regions, quick to make). Frame 0 is all black. In
  // frame k, the regions whose group number has bit k are light grey, lit and shadow alike (so the light doesn't
  // matter), and the rest are black. Where two groups meet, the bigger region gives up a texel of black in every
  // frame. A pixel blended across the edge then only fades one region's number in or out, and never spells a third.
  std::vector<uint16_t> g_blk;  // the scan frames' region per block (made once per worn color)
  int g_blk_serial = -1, g_blk_major = -1, g_blk_f = -1;
  int g_scan_major = 0;  // the shown part's big region count (small regions may widen in the frames)
  // Frames are 0 all dark, 1 every region lit (each pixel's own lit value), 2..bits+1 one bit of the region's
  // number each, and bits+2 dark again (anything that moved since frame 0 is left out).
  bool makeScanFrame(UObject* world, const std::vector<int>& group, int frame, Shown& out, int cap, int bits) {
    const Worn& w = *g_worn;
    const int f = (std::max)(1, (std::max)(w.base.w, w.base.h) / cap), W = w.base.w / f, H = w.base.h / f;
    if (g_blk_serial != g_worn_serial || g_blk_major != g_scan_major || g_blk_f != f || g_blk.size() != static_cast<size_t>(W) * H) {
      // Each block of f x f texels takes its middle texel's region, unless a small region (a thin line) is in it. Then
      // it takes that one, a bit wider but still there. Big regions scan as they always did. Once per worn color.
      g_blk.assign(static_cast<size_t>(W) * H, 0xFFFF);
      for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
          uint16_t best = w.owner[static_cast<size_t>(y * f + f / 2) * w.base.w + (x * f + f / 2)];
          for (int by = 0; by < f && f > 1; by++)
            for (int bx = 0; bx < f; bx++) {
              const uint16_t r = w.owner[static_cast<size_t>(y * f + by) * w.base.w + (x * f + bx)];
              if (r != 0xFFFF && r >= g_scan_major && (best == 0xFFFF || best < g_scan_major || r > best)) best = r;
            }
          g_blk[static_cast<size_t>(y) * W + x] = best;
        }
      g_blk_serial = g_worn_serial;
      g_blk_major = g_scan_major;
      g_blk_f = f;
    }
    const std::vector<uint16_t>& blk = g_blk;
    auto groupOf = [&](uint16_t r) { return r != 0xFFFF && r < group.size() ? group[r] : 0; };
    Image ob;
    ob.w = W;
    ob.h = H;
    ob.px.assign(static_cast<size_t>(W) * H * 4, 0);
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        const size_t q = static_cast<size_t>(y) * W + x;
        const uint16_t r = blk[q];
        const int g = groupOf(r);
        bool on = g > 0 && (frame == 1 || (frame >= 2 && frame <= bits + 1 && (g >> (frame - 2) & 1)));
        for (int dy = -1; on && dy <= 1; dy++)
          for (int dx = -1; on && dx <= 1; dx++) {
            if ((!dx && !dy) || x + dx < 0 || y + dy < 0 || x + dx >= W || y + dy >= H) continue;
            const uint16_t r2 = blk[static_cast<size_t>(y + dy) * W + (x + dx)];
            const int g2 = groupOf(r2);
            if (r2 != r && g2 > 0 && g2 != g && r2 > r) on = false;  // a smaller region's edge (regions go biggest first)
          }
        for (int c = 0; c < 3; c++) ob.px[q * 4 + c] = on ? 210 : 0;  // Alpha 0 means no rim light and no shade gradient
      }
    auto png = encodePng(ob);  // Lit and shadow are alike, so one texture serves both
    UObject* tex = importPng(world, png);
    if (!tex) return false;
    root(tex);
    out = {tex, tex};
    return true;
  }
  UObject* meshOf(UObject* c);  // (below, with the preview)
  const std::vector<Shown>& scanTextures();  // (below, with the scan)
  int bitsFor(int groups) { int b = 0; while ((1 << b) <= groups) b++; return b; }
  bool outlineOnly(UObject* m);  // Defined below, with the material instances' textures
  UObject* gameSss(UObject* m);
  void show(Target& t, const Shown& s, bool scan = false) {  // scan means the mapping scan's frames, not a look
    for (size_t i = 0; i < t.mids.size(); i++) {
      if (i < t.items.size() && t.items[i] && (*reinterpret_cast<UObject* const*>(t.items[i]) != t.mids[i] ||
                                               (*reinterpret_cast<const int32_t*>(t.items[i] + 8) & ((1 << 28) | (1 << 29)))))
        continue;  // Gone, so never touched (its object slot is read, as in Kept)
      if (outlineOnly(t.mids[i])) {  // An outline gets the shadow texture only. The scan reads the part's own texels.
        if (!scan) setTexture(t.mids[i], STR("SssMap"), s.sss ? s.sss : gameSss(t.mids[i]));
        continue;
      }
      setTexture(t.mids[i], STR("BaseMap"), s.base ? s.base : t.orig[i].first);
      if (t.orig[i].second == t.orig.front().second)  // A material with an SssMap of its own is left alone
        setTexture(t.mids[i], STR("SssMap"), s.sss ? s.sss : t.orig[i].second);
    }
  }
  bool anyEdit(const std::vector<Edit>& ed) {
    return std::any_of(ed.begin(), ed.end(), [](const Edit& e) { return e.has_lit || e.has_shadow; });
  }
  // ---- Material colors. A part's materials' OutlineColor, one per kind of material (Sol's Dragon Install glow and
  // its edges are drawn in it, not from the texture). ----------------------------
  bool isDynamic(UObject* m) {
    return m && m->GetClassPrivate() && narrow(m->GetClassPrivate()->GetName()).find("MaterialInstanceDynamic") != std::string::npos;
  }
  bool hasVector(UObject* m, const std::string& name) {  // In its own parameters or its parents', ignoring case as FNames do
    struct Arr { uint8_t* data; int32_t num, max; };
    for (int depth = 0; m && depth < 5; depth++, m = parentOf(m))
      if (auto* a = m->GetValuePtrByPropertyNameInChain<Arr>(STR("VectorParameterValues")))
        for (int i = 0; i < a->num && i < 64; i++)  // FVectorParameterValue is 48 bytes, name first
          if (_stricmp(narrow(reinterpret_cast<const FName*>(a->data + i * 48)->ToString()).c_str(), name.c_str()) == 0) return true;
    return false;
  }
  // The material colors offered besides OutlineColor, and what to call them. They come from the cast audit of
  // values the official palettes change per slot.
  const std::pair<const char*, const char*> kVecParams[] = {
      {"OutlineColor", ""}, {"OutlineColorEx", "Edge glow"}, {"Taste_color", "Tint"}, {"Rim_Col", "Rim light"}, {"ShadowRim_Col", "Shadow rim"},
      {"Rim_Color", "Rim light"}, {"ShadowRim_Color", "Shadow rim"}, {"Color", "Color"}, {"crystal_colorA", "Crystal A"}, {"crystal_colorB", "Crystal B"},
      {"AuraColor", "Aura"}, {"GlowColor", "Glow color"}, {"wire_color", "Wire"}, {"Font_color", "Font"}, {"Star_color", "Star"},
      {"base_color", "Base color"}, {"Color modifier", "Decal tint"}};
  std::wstring wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
  void getVector(UObject* mid, const TCHAR* name, float out[4]) {
    static UFunction* f = fn(STR("/Script/Engine.MaterialInstanceDynamic:K2_GetVectorParameterValue"));
    if (!f || !isDynamic(mid)) return;
    struct { FName name; float ret[4]; } p{FName(name, FNAME_Add), {}};
    mid->ProcessEvent(f, &p);
    std::memcpy(out, p.ret, sizeof(p.ret));
  }
  void setVector(UObject* mid, const TCHAR* name, const float v[4]) {
    static UFunction* f = fn(STR("/Script/Engine.MaterialInstanceDynamic:SetVectorParameterValue"));
    if (!f || !isDynamic(mid)) return;
    struct { FName name; float value[4]; } p{FName(name, FNAME_Add), {v[0], v[1], v[2], v[3]}};
    mid->ProcessEvent(f, &p);
  }
  // ---- A part's decal lines (line art drawn over the color texture, like Jam's thigh creases) get one color,
  // listed with the material colors and saved, undone and exported with them. A worker makes the decal texture
  // again from the game's own. White stays white, the strongest line becomes the color, and every texel between
  // keeps its strength. -----------------------------------------------------
  const std::string kDecalLabel = "Lines (decal)";
  struct DecalPix {  // A decal texture's own pixels, read once. Shared, since they're the game's.
    Image img;
    float smax = 0;        // the strongest line's strength (1 - lightness)
    uint32_t darkest = 0;  // its color
    bool tried = false;
  };
  struct DecalTint {  // one part's tint of it (two fighters, or two parts, never share one)
    UObject* tex = nullptr;   // the game's texture
    UObject* ours = nullptr;  // our tinted one, on now
    std::future<Pngs> job;
    bool edited = false, pending = false;
    float want[3] = {};
    Clock::time_point asked;
    std::vector<UObject*> mids;  // this part's materials drawing it
  };
  std::map<UObject*, DecalPix> g_decal_pix;                         // by the game's texture
  std::map<std::pair<UObject*, std::string>, DecalTint> g_decals;  // by (the game's texture, the part's key)
  std::map<UObject*, std::pair<UObject*, std::string>> g_decal_of;  // Decal material -> its tint's key (its texture, taken before ours went on)
  void unrootOne(UObject* o) { if (o && o->GetObjectItem()) o->GetObjectItem()->UnsetRootSet(); }
  bool decalRead(const Target& t, UObject* m) {  // If m draws a Decal texture, read its pixels once (cached on disk)
    auto known = g_decal_of.find(m);
    UObject* tex = known != g_decal_of.end() ? known->second.first : getTexture(m, STR("Decal"));
    if (!tex) return false;
    DecalPix& px = g_decal_pix[tex];
    if (!px.tried) {
      px.tried = true;
      std::error_code ec;
      const auto file = g_dir / "cache" / t.chr / ("decal_" + t.color + "_" + narrow(tex->GetName()) + ".png");
      std::filesystem::create_directories(file.parent_path(), ec);
      if (!std::filesystem::exists(file, ec) || !decodePng(file, px.img)) {
        px.img = Image{};
        const int w = sizeOf(tex, STR("Blueprint_GetSizeX")), h = sizeOf(tex, STR("Blueprint_GetSizeY"));
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || t.mids.empty() || !drawToFile(t.mids.front(), tex, w, h, file) || !decodePng(file, px.img))
          px.img = Image{};
      }
      for (size_t q = 0; q < static_cast<size_t>(px.img.w) * px.img.h; q++) {
        const uint32_t c = rgbAt(px.img, q);
        const float st = 1.0f - static_cast<float>((c >> 16 & 0xFF) + (c >> 8 & 0xFF) + (c & 0xFF)) / 765.0f;
        if (st > px.smax) { px.smax = st; px.darkest = c; }
      }
    }
    if (!px.img.w || px.smax <= 0.05f) return false;  // Unreadable or no lines, so nothing to color
    const auto key = std::make_pair(tex, keyOf(t));
    g_decal_of.emplace(m, key);
    DecalTint& d = g_decals[key];
    d.tex = tex;
    if (std::find(d.mids.begin(), d.mids.end(), m) == d.mids.end()) d.mids.push_back(m);
    return true;
  }
  bool isDecal(UObject* m) { return g_decal_of.count(m) > 0; }
  void decalSet(UObject* m, const float c[4]) {
    auto it = g_decal_of.find(m);
    if (it == g_decal_of.end()) return;
    DecalTint& d = g_decals[it->second];
    d.edited = d.pending = true;
    d.asked = Clock::now();
    std::memcpy(d.want, c, sizeof(d.want));
  }
  void decalRestore(UObject* m) {
    auto it = g_decal_of.find(m);
    if (it == g_decal_of.end()) return;
    DecalTint& d = g_decals[it->second];
    d.edited = d.pending = false;
    for (UObject* x : d.mids) setTexture(x, STR("Decal"), d.tex);
    unrootOne(d.ours);  // The game's own is back on, so let ours go
    d.ours = nullptr;
  }
  // Each update puts a finished tint on its part's materials and starts the latest one asked for once the color
  // has stopped moving for a moment (a drag sends one every frame). One at a time per part.
  void decalTick(UObject* world) {
    for (auto& [key, d] : g_decals) {
      if (d.job.valid()) {
        if (d.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
        Pngs png = d.job.get();
        if (d.edited && !png.base.empty() && world)
          if (UObject* nt = importPng(world, png.base)) {
            root(nt);
            unrootOne(d.ours);
            d.ours = nt;
            for (UObject* m : d.mids) setTexture(m, STR("Decal"), nt);
          }
      }
      if (!d.pending || !d.edited || Clock::now() - d.asked < std::chrono::milliseconds(120)) continue;
      const DecalPix& px = g_decal_pix[d.tex];
      if (!px.img.w) { d.pending = false; continue; }
      d.pending = false;
      const float smax = px.smax, T[3] = {std::clamp(d.want[0], 0.0f, 1.0f), std::clamp(d.want[1], 0.0f, 1.0f), std::clamp(d.want[2], 0.0f, 1.0f)};
      d.job = std::async(std::launch::async, [src = px.img, smax, T]() {
        Image out = src;
        for (size_t q = 0; q < static_cast<size_t>(out.w) * out.h; q++) {
          uint8_t* c = &out.px[q * 4];
          const float st = 1.0f - static_cast<float>(c[0] + c[1] + c[2]) / 765.0f, k = std::clamp(st / smax, 0.0f, 1.0f);
          for (int i = 0; i < 3; i++) c[i] = static_cast<uint8_t>(std::lround(255.0f - k * (255.0f - 255.0f * T[i])));
        }
        return Pngs{encodePng(out), {}};
      });
    }
  }
  void decalOriginals(const std::set<UObject*>& on_stage) {  // the game's decals back on materials still on stage
    for (auto& [key, d] : g_decals)
      for (UObject* m : d.mids)
        if (on_stage.count(m) && d.ours && getTexture(m, STR("Decal")) == d.ours) setTexture(m, STR("Decal"), d.tex);
  }
  void decalForget() {  // The fight is gone, and its materials with it
    for (auto& [key, d] : g_decals) {
      unrootOne(d.ours);
      if (d.job.valid()) g_png_orphans.push_back(std::move(d.job));
    }
    g_decals.clear();
    g_decal_of.clear();
    g_decal_pix.clear();
  }
  // Puts a material color on, or the material's own back. It's either a vector parameter (OutlineColor,
  // Taste_color...) or a decal's lines.
  void vecSet(UObject* m, const std::string& param, const float c[4]) { if (isDecal(m)) decalSet(m, c); else setVector(m, wide(param).c_str(), c); }
  struct VecGroup {
    std::string label;
    std::vector<UObject*> mids;
    float cur[4] = {};
    bool edited = false;
    std::string param = "OutlineColor";  // The first member's
    std::vector<std::string> params;     // The parameter for each material in mids (one material can carry Rim_Col and Rim_Color)
    const std::string& paramOf(size_t i) const { return i < params.size() ? params[i] : param; }
  };
  std::vector<VecGroup> g_vecs;
  // Per fight, each material's own colors (per parameter), taken the first time it's seen since later it may wear
  // ours. Also the target it belongs to. A material on two parts' meshes is edited from the first only.
  std::map<std::pair<UObject*, std::string>, std::array<float, 4>> g_vec_orig;
  std::map<UObject*, std::string> g_vec_owner;
  std::map<std::string, std::pair<std::vector<UObject*>, std::vector<VecGroup>>> g_menu_rows;  // The menu's material color rows, per part
  bool g_vec_unsaved = false;
  Clock::time_point g_vec_changed;
  void vecRestore(UObject* m, const std::string& param) {  // Puts its own back. If none is known, it's left as it is.
    if (isDecal(m)) { decalRestore(m); return; }
    auto o = g_vec_orig.find({m, param});
    if (o != g_vec_orig.end()) setVector(m, wide(param).c_str(), o->second.data());
  }
  void restoreVecs(const std::string& key) {  // every material of this target back to its own colors
    {  // Only while its materials are still there. Once the fight that had them is over, they may be gone.
      auto t = std::find_if(g_targets.begin(), g_targets.end(), [&](const Target& o) { return keyOf(o) == key; });
      if (t == g_targets.end() || t->items.size() < t->mids.size()) return;
      for (size_t i = 0; i < t->mids.size(); i++) {
        const uint8_t* it = t->items[i];
        if (!it || *reinterpret_cast<UObject* const*>(it) != t->mids[i] || (*reinterpret_cast<const int32_t*>(it + 8) & ((1 << 28) | (1 << 29)))) return;
      }
    }
    for (const auto& [mp, orig] : g_vec_orig) {
      auto ow = g_vec_owner.find(mp.first);
      if (ow != g_vec_owner.end() && ow->second == key) vecRestore(mp.first, mp.second);
    }
    for (const auto& [m, owner] : g_vec_owner)  // Decals get their own texture back
      if (owner == key && isDecal(m)) decalRestore(m);
  }
  const float* vecOrig(UObject* m, const std::string& param) { auto o = g_vec_orig.find({m, param}); return o == g_vec_orig.end() ? nullptr : o->second.data(); }
  std::string g_vecs_for;  // The target they belong to ("" means find them again)
  std::map<std::string, std::map<std::string, std::array<float, 4>>> g_vec_saved;  // The edited ones, by target, then by label
  std::string vecLabel(UObject* m);
  // A vector parameter's value as the material asset sets it. A dynamic material's own overrides are skipped, since
  // they're ours.
  bool vecDefault(UObject* m, const std::string& name, float out[4]) {
    struct Arr { uint8_t* data; int32_t num, max; };
    if (isDynamic(m)) m = parentOf(m);
    for (int depth = 0; m && depth < 5; depth++, m = parentOf(m))
      if (auto* a = m->GetValuePtrByPropertyNameInChain<Arr>(STR("VectorParameterValues")))
        for (int i = 0; i < a->num && i < 64; i++)  // FVectorParameterValue is info (16 bytes), FLinearColor at +16, then guid
          if (_stricmp(narrow(reinterpret_cast<const FName*>(a->data + i * 48)->ToString()).c_str(), name.c_str()) == 0) {
            std::memcpy(out, a->data + i * 48 + 16, sizeof(float) * 4);
            return true;
          }
    return false;
  }
  std::string matShort(UObject* m) {  // "MI_EDY_BASE" -> "edy base"
    std::string n = parentOf(m) ? narrow(parentOf(m)->GetName()) : narrow(m->GetName());
    if (n.rfind("MI_", 0) == 0) n = n.substr(3);
    for (auto& c : n) c = c == '_' ? ' ' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return n;
  }
  std::vector<VecGroup> vecRows(const std::vector<UObject*>& mats) {
    std::vector<VecGroup> rows;
    std::vector<std::string> keys, friendly;
    // Two spellings of one setting. The masters read one, so a row sets both, keyed by the one the palette sets.
    const auto alias = [](const std::string& p) -> const char* {
      return p == "Rim_Col" ? "Rim_Color" : p == "ShadowRim_Col" ? "ShadowRim_Color" : nullptr;
    };
    for (UObject* m : mats)
      for (const auto& [param, says] : kVecParams) {
        const std::string pn = param;
        if (pn == "Rim_Color" || pn == "ShadowRim_Color") continue;  // Handled with its other spelling
        const char* also = alias(pn);
        float probe[4];  // What the material asset sets, never our own overrides, so the rows keep their shape after edits
        const bool own = vecDefault(m, pn, probe), other = also && vecDefault(m, also, probe);
        if (!own && !other) continue;
        std::string key, label;
        if (std::string(param) == "OutlineColor") {
          label = vecLabel(m);
          key = "O|" + label;
        } else {
          float v[4] = {};
          vecDefault(m, own ? pn : std::string(also), v);
          char buf[96];
          snprintf(buf, sizeof(buf), "%s|%.3f %.3f %.3f %.3f", says, v[0], v[1], v[2], v[3]);
          key = buf;
          label = says;
        }
        const std::string first = own ? pn : std::string(also), second = own ? (also ? std::string(also) : std::string()) : pn;  // The one it sets comes first
        size_t at = static_cast<size_t>(std::find(keys.begin(), keys.end(), key) - keys.begin());  // An index, because the push below can move the vector
        if (at == keys.size()) {
          keys.push_back(key);
          friendly.push_back(pn == "OutlineColor" ? std::string() : label);
          rows.push_back(VecGroup{label});
          rows.back().param = first;
        }
        VecGroup& g = rows[at];
        g.mids.push_back(m);
        g.params.push_back(first);
        if (!second.empty()) { g.mids.push_back(m); g.params.push_back(second); }  // Set both spellings, whichever the master reads
      }
    for (size_t i = 0; i < rows.size(); i++) {  // A name shared by rows with other values says which material's it is
      if (friendly[i].empty()) continue;
      const bool shared = std::count(friendly.begin(), friendly.end(), friendly[i]) > 1;
      if (!shared) continue;
      std::string least;
      for (UObject* m : rows[i].mids) { const std::string n = matShort(m); if (least.empty() || n < least) least = n; }
      rows[i].label = friendly[i] + " (" + least + ")";
    }
    for (size_t i = 0; i < rows.size(); i++)  // Still the same, so add the parameter too
      for (size_t j = i + 1; j < rows.size(); j++)
        if (rows[j].label == rows[i].label && !friendly[j].empty()) rows[j].label += " " + rows[j].param;
    return rows;
  }
  std::string vecLabel(UObject* m) {
    std::string n = parentOf(m) ? narrow(parentOf(m)->GetName()) : narrow(m->GetName()), u = n;
    for (auto& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (u.find("FLARE") != std::string::npos) return "Flare";
    if (u.find("GLOWOUTLINE") != std::string::npos) return "Glow outline";
    if (u.find("GLOW") != std::string::npos) return "Glow";
    if (u.find("OUTLINE") != std::string::npos) return "Outline";
    return n;
  }
  void buildVecs(const Target& t) {
    g_vecs.clear();
    g_vecs_for = keyOf(t);
    std::vector<UObject*> seen, mats;  // mats holds the ones with color parameters, for the rows
    for (UObject* comp : t.comps)
      for (int i = 0, n = numMaterials(comp); i < n; i++) {
        UObject* m = material(comp, i);
        if (!isDynamic(m) || std::find(seen.begin(), seen.end(), m) != seen.end()) continue;
        seen.push_back(m);
        if (g_extra_read.count(t.chr + "/" + t.kind) && std::find(t.mids.begin(), t.mids.end(), m) == t.mids.end())
          continue;  // A part of its own, like Sol's DI wing, shares another part's meshes. Only its own material's colors count.
        if (!hasVector(m, "OutlineColor") && (isDecal(m) || getTexture(m, STR("Decal")))) {  // a decal's lines
          const auto owner = g_vec_owner.emplace(m, keyOf(t)).first;
          if (owner->second != keyOf(t) || !decalRead(t, m)) { if (owner->second == keyOf(t) && !isDecal(m)) g_vec_owner.erase(owner); continue; }
          const DecalTint& d = g_decals[g_decal_of[m]];
          const DecalPix& px = g_decal_pix[d.tex];
          const float dk[4] = {(px.darkest >> 16 & 0xFF) / 255.0f, (px.darkest >> 8 & 0xFF) / 255.0f, (px.darkest & 0xFF) / 255.0f, 1.0f};
          if (!g_vec_orig.count({m, "Decal"})) std::memcpy(g_vec_orig[{m, "Decal"}].data(), dk, sizeof(dk));  // Its own is the strongest line's color
          auto g = std::find_if(g_vecs.begin(), g_vecs.end(), [&](const VecGroup& x) { return x.label == kDecalLabel; });
          if (g == g_vecs.end()) {
            g_vecs.push_back(VecGroup{kDecalLabel});
            g = g_vecs.end() - 1;
            g->param = "Decal";
            std::memcpy(g->cur, d.edited ? d.want : dk, sizeof(float) * 3);
            g->cur[3] = 1.0f;
          }
          g->mids.push_back(m);
          g->params.push_back("Decal");
          continue;
        }
        if (std::any_of(g_targets.begin(), g_targets.end(), [&](const Target& o) {  // Another part's own color material on these meshes is listed with that part
              return o.pawn == t.pawn && o.kind != t.kind && std::find(o.mids.begin(), o.mids.end(), m) != o.mids.end();
            }))
          continue;
        if (!std::any_of(std::begin(kVecParams), std::end(kVecParams), [&](const auto& pr) { return hasVector(m, pr.first); })) continue;
        const auto owner = g_vec_owner.emplace(m, keyOf(t)).first;
        if (owner->second != keyOf(t)) continue;  // another part's
        mats.push_back(m);
      }
    for (VecGroup& row : vecRows(mats)) {
      for (size_t i = 0; i < row.mids.size(); i++) {
        const std::string& prm = row.params[i];
        if (!g_vec_orig.count({row.mids[i], prm})) getVector(row.mids[i], wide(prm).c_str(), g_vec_orig[{row.mids[i], prm}].data());
      }
      getVector(row.mids.front(), wide(row.params.front()).c_str(), row.cur);  // as it is now (ours, if edited)
      g_vecs.push_back(std::move(row));
    }
    auto& saved = g_vec_saved[keyOf(t)];
    for (VecGroup& g : g_vecs) {
      auto it = saved.find(g.label);
      if (it == saved.end()) continue;
      std::memcpy(g.cur, it->second.data(), sizeof(g.cur));
      g.edited = true;
      for (size_t i = 0; i < g.mids.size(); i++) vecSet(g.mids[i], g.paramOf(i), g.cur);
    }
    COMBO_LOG("color editor: " + std::to_string(g_vecs.size()) + " material colors on " + keyOf(t));
  }

  // ---- Effect colors. The color slot's PTCColorAndMaterial names its particle colors (Sol's fire_color01 ...)
  // as ParticleModuleColorOverLife modules. Each module's ColorOverLife lookup table (read whenever a particle
  // spawns) is rewritten so every RGB keeps its total (the game's brightness) in the picked ratio.
  // Layouts from UE 4.25: FRawDistributionVector = FDistributionLookupTable (TimeScale, TimeBias,
  // TArray<float> Values at +8, ...; 32 bytes), MinValue, MaxValue, MinValueVec +40, MaxValueVec +52.
  // The game's own values go back when the fight stops. --------------------------
  bool capHolds(UObject* module);  // True if a captured effect color has this module
  int takeByName(UObject* ps);     // Takes an effect's by-name copies of the slot's colors
  void takeAllByName(const std::string& pattern);
  struct Kept {
    UObject* obj = nullptr;
    const uint8_t* item = nullptr;
    bool alive() const {
      return item && *reinterpret_cast<UObject* const*>(item) == obj &&
             !(*reinterpret_cast<const int32_t*>(item + 8) & ((1 << 28) | (1 << 29)));
    }
  };
  Kept keep(UObject* o) { return {o, o ? reinterpret_cast<const uint8_t*>(o->GetObjectItem()) : nullptr}; }  // o must be known alive now
  struct FxCopy { UObject* module; const TCHAR* prop; std::vector<float> orig; Kept k; std::string effect;
                  bool named = false; };  // named is a by-name copy or the fighter's palette (the slot's color, never one effect's own)  // An effect's own module holding the same values. k tells if it's still loaded.
  std::string effectOf(UObject* m) {  // the effect (ParticleSystem) a module is part of
    for (int depth = 0; m && depth < 6; depth++, m = m->GetOuterPrivate())
      if (m->GetClassPrivate() && narrow(m->GetClassPrivate()->GetName()) == "ParticleSystem") return narrow(m->GetName());
    return {};
  }
  std::set<UObject*> g_fx_checked;   // effects looked at for by-name copies of the slot's effect colors (each once)
  struct FxColor {
    std::string label, uses;
    UObject* module = nullptr;
    std::vector<FxCopy> copies;  // Modules of loaded effects that copy the slot's values into themselves at load
    std::vector<float> orig;  // the table's values, then MinValueVec, MaxValueVec
    float rgb[3] = {1, 1, 1};
    bool edited = false;
    std::map<std::string, std::array<float, 3>> per;  // An effect's own color in place of this one, by the effect's name
  };
  // The color an effect's copy gets. Its own, else the slot's when changed, or null for the game's.
  const float* fxRgbFor(const FxColor& f, const std::string& effect) {
    auto it = f.per.find(effect);
    return it != f.per.end() ? it->second.data() : f.edited ? f.rgb : nullptr;
  }
  bool fxChanged(const FxColor& f) { return f.edited || !f.per.empty(); }
  // What an effect is in the game, for its own row ("" just its name).
  const char* fxEffectSays(const std::string& chr, const std::string& effect) {
    static const std::map<std::string, const char*> kyk = {
        {"dky_floor_aura", "Ground aura"}, {"dky_aura_floor_loop", "Ground aura, standing"}, {"dky_aura_floor_walk", "Ground aura, walking"},
        {"dky_aura_floor_backwalk", "Ground aura, walking back"}, {"dky_cs_wingaura", "Aura burst (Dragon Install starting)"},
        {"dky_aura_arm_loop", "Arm aura"}, {"dky_aura_stand_loop", "Wing aura, standing"}, {"dky_aura_stand_loop_oku", "Wing aura, standing (behind)"},
        {"dky_aura_crouch_loop", "Wing aura, crouching"}, {"dky_aura_crouch_loop_oku", "Wing aura, crouching (behind)"}, {"dky_aura_yoin", "Aura afterglow"},
        {"dky_aura_naname_yoin", "Aura afterglow (diagonal)"}};
    if (codeOf(chr) != "KYK") return "";
    auto it = kyk.find(effect);
    return it == kyk.end() ? "" : it->second;
  }
  struct FxSlot {
    std::wstring path;         // the slot's PTCColorAndMaterial
    UObject* held = nullptr;   // that asset, kept loaded by us while we may write to it
    std::vector<FxColor> list;
  };
  std::map<std::string, FxSlot> g_fx_slots;  // Every slot seen this fight (both fighters), by "SOL/Color08"
  std::string g_fx_for;                      // the one shown
  std::vector<FxColor> g_fx_none;
  std::vector<FxColor>& fxList() { auto it = g_fx_slots.find(g_fx_for); return it == g_fx_slots.end() ? g_fx_none : it->second.list; }
  bool g_fx_unsaved = false;
  Clock::time_point g_fx_changed;
  std::map<std::string, std::map<std::string, std::array<float, 3>>> g_fx_saved;  // by "SOL/Color08", by label
  struct FArr { float* data; int32_t num, max; };
  const char* fxUses(const std::string& chr, const std::string& label) {  // from the community's index of Sol's particles
    static const std::map<std::string, const char*> sol = {
        {"fire_color01", "Vortex ground, Tyrant Rave background, 5S, Bandit Bringer, 5D"},
        {"fire_color02", "5S and 5D sparks, Bandit Bringer hit, Tyrant Rave uppercut background"},
        {"fire_color03", "Volcanic Viper ash, Heavy Mob Cemetery impact"},
        {"fire_color04", "Volcanic Viper, Gun Flame, Heavy Mob Cemetery background"},
        {"fire_light", "the light Gun Flame and Bandit Bringer cast"},
        {"fire_light_short", "the light Volcanic Viper, Fafnir and Vortex cast"},
        {"fire_cs", "Gun Flame smoke, Heavy Mob Cemetery hand"},
        {"fire_dsl01", "Tyrant Rave hand, Heavy Mob Cemetery wind on hit"},
        {"fire_dsl02", "Bandit Bringer and Tyrant Rave uppercut fist"}};
    static const std::map<std::string, const char*> kyk = {  // From the effects that take them by name
        {"dky_aura", "Dragon Install wing aura (on his back and arm), and the flare running in DI"},
        {"dky_aura_bloom", "Dragon Install aura around his feet, and the glow around the wing aura"}};
    const auto* uses = chr == "SOL" ? &sol : chr == "KYK" ? &kyk : nullptr;
    if (!uses) return "";
    auto it = uses->find(label);
    return it == uses->end() ? "" : it->second;
  }
  // The moves that play each effect color (and each effect), every character. Worked out offline from the game's
  // own files (the palettes, the effects that take each color by name, and the move scripts that play them) and
  // shipped as effect_uses.txt beside the editor's data: "CHR<TAB>color<TAB>moves" and "CHR<TAB>@effect<TAB>moves".
  const std::string& fxMoves(const std::string& chr, const std::string& key) {
    static std::map<std::string, std::string> table;
    static bool read = false;
    if (!read) {
      read = true;
      std::ifstream f(g_dir / "effect_uses.txt");
      std::string line;
      while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // (CRLF line ends)
        const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b != std::string::npos) table[line.substr(0, b)] = line.substr(b + 1);
      }
    }
    static const std::string none;
    auto it = table.find(codeOf(chr) + "\t" + key);
    return it == table.end() ? none : it->second;
  }
  uint8_t* fxDist(UObject* module, const TCHAR* prop = STR("ColorOverLife")) { return module ? module->GetValuePtrByPropertyNameInChain<uint8_t>(prop) : nullptr; }
  // The color modules of the loaded effects, and the property each keeps its RGB in.
  // ASW's own REDParticleModuleColorByName copies a palette color by name (ColorPatternName) into its ColorOverLife
  // when the effect loads. Sol's Dragon Install HMC burst takes fire_dsl01 that way.
  const std::pair<const TCHAR*, const TCHAR*> kFxClasses[4] = {{STR("ParticleModuleColorOverLife"), STR("ColorOverLife")},
                                                               {STR("ParticleModuleColor"), STR("StartColor")},
                                                               {STR("ParticleModuleColorScaleOverLife"), STR("ColorScaleOverLife")},
                                                               {STR("REDParticleModuleColorByName"), STR("ColorOverLife")}};
  // A ColorByName module's palette name, or "" for any other module.
  std::string fxPattern(UObject* m) {
    if (!m || !m->GetClassPrivate() || narrow(m->GetClassPrivate()->GetName()) != "REDParticleModuleColorByName") return {};
    auto* n = m->GetValuePtrByPropertyNameInChain<FName>(STR("ColorPatternName"));
    return n ? narrow(n->ToString()) : std::string();
  }
  std::set<UObject*> fxLiveModules() {
    std::set<UObject*> out;
    for (const auto& [cls, prop] : kFxClasses) {
      std::vector<UObject*> all;
      UObjectGlobals::FindAllOf(cls, all);
      out.insert(all.begin(), all.end());
    }
    return out;
  }
  void distWrite(uint8_t* d, const std::vector<float>& orig, const float* values) {  // values is laid out like orig (table, MinValueVec, MaxValueVec)
    if (!d) return;
    FArr* v = reinterpret_cast<FArr*>(d + 8);
    if (v->num + 6 != static_cast<int>(orig.size())) return;
    std::memcpy(v->data, values, sizeof(float) * v->num);
    std::memcpy(d + 40, values + v->num, sizeof(float) * 3);
    std::memcpy(d + 52, values + v->num + 3, sizeof(float) * 3);
  }
  std::vector<float> distRead(uint8_t* d) {
    FArr* v = reinterpret_cast<FArr*>(d + 8);
    std::vector<float> o(v->data, v->data + v->num);
    o.insert(o.end(), reinterpret_cast<float*>(d + 40), reinterpret_cast<float*>(d + 40) + 3);
    o.insert(o.end(), reinterpret_cast<float*>(d + 52), reinterpret_cast<float*>(d + 52) + 3);
    return o;
  }
  std::vector<float> ratioed(const std::vector<float>& orig, const float rgb[3]) {  // Puts every RGB in the ratio, keeping its total
    std::vector<float> out = orig;
    const float sum = rgb[0] + rgb[1] + rgb[2];
    for (size_t i = 0; i + 2 < out.size(); i += 3) {
      const float total = orig[i] + orig[i + 1] + orig[i + 2];
      for (int c = 0; c < 3; c++) out[i + c] = sum > 0.0f ? total * rgb[c] / sum : 0.0f;
    }
    return out;
  }
  // The asset's color table, or null if it's not loaded (or the module's no longer in it).
  bool fxModuleListed(UObject* asset, UObject* module) {
    struct E { uint8_t name[8]; UObject* mod; };
    struct A { E* data; int32_t num, max; };
    auto* t = asset ? asset->GetValuePtrByPropertyNameInChain<A>(STR("ColorTable")) : nullptr;
    if (!t) return false;
    for (int i = 0; i < t->num && i < 64; i++) if (t->data[i].mod == module) return true;
    return false;
  }
  void fxWrite(FxColor& f, const float* values) {  // values holds the table's, then MinValueVec and MaxValueVec (as in orig)
    uint8_t* d = fxDist(f.module);
    if (!d) return;
    FArr* v = reinterpret_cast<FArr*>(d + 8);
    if (v->num + 6 != static_cast<int>(f.orig.size())) return;
    std::memcpy(v->data, values, sizeof(float) * v->num);
    std::memcpy(d + 40, values + v->num, sizeof(float) * 3);
    std::memcpy(d + 52, values + v->num + 3, sizeof(float) * 3);
  }
  void fxApply(FxColor& f) {  // the slot's module and every copy of it
    const std::vector<float> out = ratioed(f.orig, f.rgb);
    fxWrite(f, f.edited ? out.data() : f.orig.data());
    for (const FxCopy& c : f.copies) {
      if (!c.k.alive()) continue;  // Its effect has unloaded since
      const float* rgb = c.named ? (f.edited ? f.rgb : nullptr) : fxRgbFor(f, c.effect);
      const std::vector<float> co = rgb ? ratioed(c.orig, rgb) : c.orig;
      distWrite(fxDist(c.module, c.prop), c.orig, co.data());
    }
  }
  void fxRestore() {  // Puts the game's values back on every slot (the asset was kept loaded, but it's still checked), then lets go
    std::set<UObject*> live;  // The copies, only where the effect is still loaded
    if (std::any_of(g_fx_slots.begin(), g_fx_slots.end(), [](const auto& kv) {
          return std::any_of(kv.second.list.begin(), kv.second.list.end(), [](const FxColor& f) { return fxChanged(f) && !f.copies.empty(); });
        }))
      live = fxLiveModules();
    for (auto& [key, slot] : g_fx_slots) {
      UObject* asset = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, slot.path.c_str());
      for (FxColor& f : slot.list) {
        if (f.edited && fxModuleListed(asset, f.module)) fxWrite(f, f.orig.data());
        if (fxChanged(f))
          for (const FxCopy& c : f.copies)
            if (c.k.alive() && live.count(c.module)) distWrite(fxDist(c.module, c.prop), c.orig, c.orig.data());
      }
      if (slot.held && asset == slot.held && slot.held->GetObjectItem()) slot.held->GetObjectItem()->UnsetRootSet();
    }
    g_fx_slots.clear();
    g_fx_checked.clear();
    g_fx_for.clear();
  }
  std::filesystem::path fxFile(const std::string& chr, const std::string& color) { return g_dir / "saved" / chr / (color + "_effects.txt"); }
  bool fxSaveSlot(const std::string& slot) {  // Returns false if it couldn't be written
    if (slot.empty()) return true;
    const size_t slash = slot.find('/');
    const std::string chr = slot.substr(0, slash), color = slot.substr(slash + 1);
    std::error_code ec;
    std::filesystem::create_directories(fxFile(chr, color).parent_path(), ec);
    std::ofstream f(fxFile(chr, color), std::ios::trunc);
    for (const auto& [label, c] : g_fx_saved[slot]) f << label << '\t' << c[0] << ' ' << c[1] << ' ' << c[2] << "\n";
    f.flush();
    return f.good();
  }
  void fxSave() { fxSaveSlot(g_fx_for); }
  void fxBuild(const Target& t) {
    g_fx_for = t.chr + "/" + t.color;
    if (g_fx_slots.count(g_fx_for)) return;  // Another part of the same slot, or back from the other fighter
    FxSlot& slot = g_fx_slots[g_fx_for];
    slot.path = materialDir(t.chr, t.color) + L"/PTCColorAndMaterial.PTCColorAndMaterial";
    UObject* asset = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, slot.path.c_str());
    if (asset && asset->GetObjectItem() &&  // Kept loaded while we may change it, unless the game already keeps it (RootSet, flags +8 bit 30)
        !(*reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(asset->GetObjectItem()) + 8) & (1 << 30))) {
      asset->GetObjectItem()->SetRootSet();
      slot.held = asset;
    }
    std::vector<FxColor>& g_fx = slot.list;
    struct E { FName name; UObject* mod; };
    struct A { E* data; int32_t num, max; };
    auto* table = asset ? asset->GetValuePtrByPropertyNameInChain<A>(STR("ColorTable")) : nullptr;
    if (!g_fx_saved.count(g_fx_for)) {  // what was saved for this slot (or recovered)
      auto st = g_fx_restore.find(g_fx_for);
      std::ifstream f(st != g_fx_restore.end() ? st->second : fxFile(t.chr, t.color));
      if (st != g_fx_restore.end()) g_fx_restore.erase(st);
      std::string line;
      while (std::getline(f, line)) {
        const size_t tab = line.find('\t');
        std::array<float, 3> c{};
        std::istringstream v(tab == std::string::npos ? std::string() : line.substr(tab + 1));
        if (v >> c[0] >> c[1] >> c[2]) g_fx_saved[g_fx_for][line.substr(0, tab)] = c;
      }
      g_fx_saved[g_fx_for];
    }
    for (int i = 0; table && i < table->num && i < 64; i++) {
      UObject* mod = table->data[i].mod;
      uint8_t* d = fxDist(mod);
      if (!d) continue;
      FArr* v = reinterpret_cast<FArr*>(d + 8);
      if (v->num <= 0 || v->num > 4096 || v->num % 3) continue;  // Must be an RGB table
      FxColor f;
      f.label = narrow(table->data[i].name.ToString());
      f.uses = fxUses(t.chr, f.label);
      if (const std::string& mv = fxMoves(t.chr, f.label); !mv.empty()) f.uses += (f.uses.empty() ? "Moves: " : "\nMoves: ") + mv;
      f.module = mod;
      f.orig.assign(v->data, v->data + v->num);
      f.orig.insert(f.orig.end(), reinterpret_cast<float*>(d + 40), reinterpret_cast<float*>(d + 40) + 3);
      f.orig.insert(f.orig.end(), reinterpret_cast<float*>(d + 52), reinterpret_cast<float*>(d + 52) + 3);
      const float m = (std::max)({f.orig[0], f.orig[1], f.orig[2], 1e-6f});  // Shows the first color's hue
      for (int c = 0; c < 3; c++) f.rgb[c] = f.orig[static_cast<size_t>(c)] / m;
      auto s = g_fx_saved[g_fx_for].find(f.label);
      if (s != g_fx_saved[g_fx_for].end()) { std::memcpy(f.rgb, s->second.data(), sizeof(f.rgb)); f.edited = true; }
      for (const auto& [k, c] : g_fx_saved[g_fx_for])  // "<label>@<effect>" is an effect's own
        if (k.size() > f.label.size() + 1 && k.compare(0, f.label.size() + 1, f.label + "@") == 0) f.per[k.substr(f.label.size() + 1)] = c;
      g_fx.push_back(f);
      if (g_fx.back().edited) fxApply(g_fx.back());
    }
    {  // Each fighter's own copy of the palette (REDPawnPlayer...PTCColorAndMaterial, made when it loads). Effects taking a
       // color by name read it as they spawn (Ky's DI aura). Its colors are the slot's, as the game copied them.
      std::vector<UObject*> pals;
      UObjectGlobals::FindAllOf(STR("REDPTCColorAndMaterialAsset"), pals);
      auto slotOfPawn = [](UObject* pawn) {  // The slot its body's color material is from, or another part's if it has no body
        std::string any;
        for (UObject* c : fighterParts()) {
          if (c->GetOuterPrivate() != pawn) continue;
          for (int i = 0, n = numMaterials(c); i < n && i < 32; i++) {
            std::string name, kind, chr, color;
            UObject* m = material(c, i);
            if (!m || !colorSource(m, name, kind) || !slotOfPath(name, chr, color)) continue;
            if (kind == "MAIN") return chr + "/" + color;
            if (any.empty()) any = chr + "/" + color;
          }
        }
        return any;
      };
      for (UObject* p : pals) {
        UObject* outer = p ? p->GetOuterPrivate() : nullptr;
        if (p == asset || !outer || !outer->GetClassPrivate() || narrow(outer->GetClassPrivate()->GetName()).find("Pawn") == std::string::npos) continue;
        static std::set<std::string> said;  // (why one wasn't taken, said once per fighter and slot)
        const std::string pawn_slot = slotOfPawn(outer);
        if (pawn_slot != g_fx_for) {  // Another slot's fighter, like a mirror's other color, even with the same effect colors
          if (said.insert(narrow(outer->GetName()) + "|" + g_fx_for).second)
            COMBO_LOG("color editor: " + narrow(outer->GetName()) + "'s own palette is " + (pawn_slot.empty() ? std::string("of no slot found") : pawn_slot) + ", not " + g_fx_for);
          continue;
        }
        std::string differs;  // (the first color not the slot's)
        auto* tb = p->GetValuePtrByPropertyNameInChain<A>(STR("ColorTable"));
        std::vector<std::pair<size_t, UObject*>> match;
        bool same = tb != nullptr;
        for (int i = 0; same && i < tb->num && i < 64; i++) {
          const std::string name = narrow(tb->data[i].name.ToString());
          auto f = std::find_if(g_fx.begin(), g_fx.end(), [&](const FxColor& x) { return x.label == name; });
          if (f == g_fx.end()) continue;
          uint8_t* d = fxDist(tb->data[i].mod);
          uint8_t* ad = fxDist(f->module);
          const std::vector<float> now = d ? distRead(d) : std::vector<float>();
          same = !now.empty() && (now == f->orig || (ad && now == distRead(ad)));  // The slot's colors, either as the game has them or as we changed them
          if (same) match.push_back({static_cast<size_t>(f - g_fx.begin()), tb->data[i].mod});
          else {
            char b[160];
            snprintf(b, sizeof(b), "%s: %zu values, first %.4g %.4g %.4g; the slot's %zu values, first %.4g %.4g %.4g", name.c_str(), now.size(),
                     now.size() > 2 ? now[0] : 0.f, now.size() > 2 ? now[1] : 0.f, now.size() > 2 ? now[2] : 0.f, f->orig.size(),
                     f->orig.size() > 2 ? f->orig[0] : 0.f, f->orig.size() > 2 ? f->orig[1] : 0.f, f->orig.size() > 2 ? f->orig[2] : 0.f);
            differs = b;
          }
        }
        if (!same || match.empty()) {  // Another slot's palette
          if (said.insert(narrow(outer->GetName()) + "|" + g_fx_for + "|v").second)
            COMBO_LOG("color editor: " + narrow(outer->GetName()) + "'s own palette is " + g_fx_for + "'s but not taken: " +
                      (match.empty() && differs.empty() ? std::string("none of its colors is one of the slot's") : differs));
          continue;
        }
        for (const auto& [k, mod] : match) {
          FxColor& f = g_fx[k];
          if (std::any_of(f.copies.begin(), f.copies.end(), [&](const FxCopy& c) { return c.module == mod; })) continue;
          f.copies.push_back({mod, STR("ColorOverLife"), f.orig, keep(mod), std::string()});
          f.copies.back().named = true;
        }
        COMBO_LOG("color editor: " + narrow(outer->GetName()) + "'s own palette takes " + g_fx_for + "'s effect colors (" + std::to_string(match.size()) + ")");
      }
    }
    {  // Loaded effects' own modules holding exactly a color's values are copies, changed along with it
      std::string found;
      for (const auto& [cls, prop] : kFxClasses) {
        std::vector<UObject*> all;
        UObjectGlobals::FindAllOf(cls, all);
        for (UObject* m : all) {
          uint8_t* d = fxDist(m, prop);
          if (!d) continue;
          FArr* v = reinterpret_cast<FArr*>(d + 8);
          if (v->num <= 0 || v->num > 4096) continue;
          if (capHolds(m)) continue;  // A capture has it
          const std::string pattern = fxPattern(m);
          for (FxColor& f : g_fx) {
            if (m == f.module || static_cast<size_t>(v->num) + 6 != f.orig.size() ||
                std::memcmp(v->data, f.orig.data(), sizeof(float) * v->num) != 0)
              continue;
            if (!pattern.empty() && pattern != f.label) continue;  // A by-name copy only counts for the color it names
            // Only an effect's own module. Another slot's palette can hold the same values (a pak's slots often do), and
            // this slot's edits would go into that slot's palette.
            const std::string eff = effectOf(m);
            if (eff.empty()) break;
            f.copies.push_back({m, prop, distRead(d), keep(m), eff});
            f.copies.back().named = !pattern.empty();
            break;
          }
        }
      }
      takeAllByName("");  // By-name copies are found by where they sit
      for (FxColor& f : g_fx) {
        found += " " + f.label + "=" + std::to_string(f.copies.size());
        if (fxChanged(f)) fxApply(f);  // Saved colors go onto the copies too
      }
      COMBO_LOG("color editor: effect color copies in the loaded effects:" + found);
    }
    COMBO_LOG("color editor: " + std::to_string(g_fx.size()) + " effect colors on " + g_fx_for + (asset ? "" : " (no PTCColorAndMaterial loaded)"));
  }

  // ---- Smears are effects whose emitters ask for a material by name (REDParticleModuleMaterialByName, Sol's
  // sol252_bake asks for "sol_wep"). The slot's PTCColorAndMaterial maps the name to a color material, and the
  // effect component's EmitterMaterials[emitter] is set to our live copy of it (in UE 4.25 a mesh emitter's
  // CurrentMaterials take it every tick, in Tick_MaterialOverrides). The game reuses effect components, so ours
  // comes off when a component plays something else. ----------------------
  struct UArr { UObject** data; int32_t num, max; };
  void matDress(UObject* psc, UObject* tpl, std::ptrdiff_t em_off);  // For effect material colors. Defined below, with the captured effects.
  void matUndressAll();
  void matForgetComponent(UObject* psc);  // Destroyed, so forget it without reading it
  void matForgetGone();                   // Forgets what's no longer in the pool
  void matForget();
  std::map<UObject*, std::vector<std::pair<int, std::string>>> g_named;  // template -> (emitter, material name) asked for
  struct Smeared { UObject* tpl = nullptr; std::vector<std::pair<int, Kept>> prev; };  // what each emitter had before ours
  std::map<UObject*, Smeared> g_smeared;      // component -> what we changed
  // A kept object, checked through its slot in the global object array (which outlives the object). It's alive if
  // the slot still holds it and it isn't being destroyed. FUObjectItem has Object at +0 and Flags at +8, and 4.25's
  // EInternalObjectFlags are Unreachable 1 << 28, PendingKill 1 << 29. Never reads the object itself.
  std::vector<Kept> g_smear_pool;             // the game's pooled effect components
  std::vector<Kept> g_pool_last_fight;        // the pool when the last fight ended: the old material copies are let go once none of it is alive
  std::vector<UObject*> g_mat_retired;         // effect material copies let go, still rooted (see matForget)
  std::map<std::string, Kept> g_smear_live;  // Material name -> our live copy (none if not edited). Checked before use: a re-found fighter's materials go
  int g_smear_tick = 0;
  bool g_smear_scanned = false;
  struct Timing { double update = 0, after = 0, smear = 0; int frames = 0; Clock::time_point since = Clock::now(); } g_time;
  struct Timed {  // adds the scope's time to a total
    double& to; Clock::time_point t0 = Clock::now();
    ~Timed() { to += std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }
  };
  // An update's time by step, logged when the whole of it is slow (so a freeze says where it was)
  struct Laps {
    Clock::time_point t0 = Clock::now(), last = t0;
    std::string steps;
    void mark(const char* step) {
      const auto now = Clock::now();
      const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
      if (ms >= 20) steps += std::string(" ") + step + " " + std::to_string(ms) + " ms";
      last = now;
    }
    ~Laps() {
      const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
      if (ms >= 300) { mark("rest"); COMBO_LOG("color editor: slow update, " + std::to_string(ms) + " ms:" + steps); }
    }
  };
  void timingTick() {
    g_time.frames++;
    if (Clock::now() - g_time.since < std::chrono::seconds(10)) return;
    char b[200];
    snprintf(b, sizeof(b), "perf: per frame %.2f ms (update %.2f, after %.2f; smears %.2f) over %d frames", (g_time.update + g_time.after) / g_time.frames,
             g_time.update / g_time.frames, g_time.after / g_time.frames, g_time.smear / g_time.frames, g_time.frames);
    COMBO_LOG(b);
    g_time = Timing{};
  }  // The full scan done this fight. It walks the whole object array, so once, not every few frames.
  // Effect components the game made since the last update. A smear's three pieces are new each time and last two
  // frames each, so an occasional scan misses them. From UE4SS's object-creation hook.
  std::mutex g_smear_new_m;
  std::vector<Kept> g_smear_new;
  std::atomic<UClass*> g_psc_class{nullptr};
  bool g_smear_stale = true;                  // Targets or edits changed, so look the names up again
  const std::vector<std::pair<int, std::string>>& namedMaterials(UObject* tpl) {
    auto it = g_named.find(tpl);
    if (it != g_named.end()) return it->second;
    std::vector<std::pair<int, std::string>> out;
    auto* emitters = tpl->GetValuePtrByPropertyNameInChain<UArr>(STR("Emitters"));
    for (int e = 0; emitters && e < emitters->num && e < 32; e++) {
      UObject* em = emitters->data[e];
      auto* lods = em ? em->GetValuePtrByPropertyNameInChain<UArr>(STR("LODLevels")) : nullptr;
      if (!lods || lods->num < 1 || !lods->data[0]) continue;
      auto* mods = lods->data[0]->GetValuePtrByPropertyNameInChain<UArr>(STR("Modules"));
      for (int m = 0; mods && m < mods->num && m < 64; m++) {
        UObject* mod = mods->data[m];
        if (!mod || !mod->GetClassPrivate() || narrow(mod->GetClassPrivate()->GetName()) != "REDParticleModuleMaterialByName") continue;
        if (auto* n = mod->GetValuePtrByPropertyNameInChain<FName>(STR("MaterialName"))) out.push_back({e, narrow(n->ToString())});
      }
    }
    if (!out.empty()) {  // Each effect asking for a material by name, once
      std::string names;
      for (const auto& [e, n] : out) names += " emitter " + std::to_string(e) + "=" + n;
      COMBO_LOG("smear: " + narrow(tpl->GetName()) + " asks for" + names);
    }
    return g_named.emplace(tpl, std::move(out)).first->second;
  }
  // ---- Hiding effects (to find which one draws something), by effect name or all ("*")
  std::set<std::string> g_hidden;
  std::vector<std::string> g_on_screen;  // "What's on screen" (empty: off)
  bool isActive(UObject* c) {  // ActorComponent::IsActive
    static UFunction* f = fn(STR("/Script/Engine.ActorComponent:IsActive"));
    if (!f) return false;
    alignas(8) uint8_t p[8] = {};
    c->ProcessEvent(f, p);
    return p[0] != 0;
  }
  std::map<UObject*, UObject*> g_hid;  // component we hid -> the effect it played then
  void hideShowAll(bool forget_names);
  void setVisible(UObject* c, bool on);  // Defined below (SceneComponent::SetVisibility)
  void hideTick(UObject* psc, UObject* tpl) {
    if (g_hidden.empty() && g_hid.empty()) return;
    static std::map<UObject*, std::string> name_of;  // Each effect's name, looked up once
    bool want = false;
    if (tpl && !g_hidden.empty()) {
      auto n = name_of.find(tpl);
      if (n == name_of.end()) n = name_of.emplace(tpl, narrow(tpl->GetName())).first;
      want = g_hidden.count("*") || g_hidden.count(n->second);
    }
    auto h = g_hid.find(psc);
    if (want) { setVisible(psc, false); g_hid[psc] = tpl; }  // Every update, since the pool can show it again when it plays
    // ponytail: shown again as visible, not as it was (the pool's components are visible while they play)
    else if (h != g_hid.end()) { setVisible(psc, true); g_hid.erase(h); }
  }
  void setEmitterMaterial(UObject* psc, int index, UObject* mat) {  // UParticleSystemComponent::SetMaterial: EmitterMaterials[index]
    static UFunction* f = fn(STR("/Script/Engine.PrimitiveComponent:SetMaterial"));
    if (!f) return;
    struct { int32_t index; int32_t pad; UObject* mat; } p{index, 0, mat};
    psc->ProcessEvent(f, &p);
  }
  // An effect's own material drawn from a part's texture (MI_MAIN_BASE_Eff, with its own shader and the body's
  // BaseMap). A dynamic copy of it is made once per part and wears the part's edited textures.
  // The copy takes everything the game sets on the part's own material (its lighting each frame, our edited
  // textures). A bare copy has none of the lighting and draws black.
  std::string paramsOf(UObject* m);
  struct EffCopy { UObject* mid = nullptr; UObject* base = nullptr; UObject* src = nullptr; const uint8_t* src_item = nullptr; UObject* mic = nullptr; UObject* own_sss = nullptr; };
  // Puts the effect material's own textures back over the copied ones, except BaseMap and a SssMap that's the
  // part's. Its own colors and numbers aren't put back. Where the part's material sets the same one, that's the
  // game's lighting now and it wins. Where it doesn't, the effect's comes through as the parent's anyway.
  // Layout as in paramsOf: FMaterialParameterInfo (16) then the texture pointer, 40 bytes an entry.
  void effOwnBack(UObject* to, UObject* mic, UObject* part_sss) {
    struct Arr { uint8_t* data; int32_t num, max; };
    static UFunction* st = fn(STR("/Script/Engine.MaterialInstanceDynamic:SetTextureParameterValue"));
    if (!to || !mic) return;
    if (auto* a = mic->GetValuePtrByPropertyNameInChain<Arr>(STR("TextureParameterValues")); a && st)
      for (int i = 0; i < a->num && i < 32; i++) {
        const FName n = *reinterpret_cast<const FName*>(a->data + i * 40);
        UObject* tex = *reinterpret_cast<UObject* const*>(a->data + i * 40 + 16);
        const std::string nn = narrow(n.ToString());
        if (!tex || nn == "BaseMap" || (nn == "SssMap" && tex == part_sss)) continue;
        struct { FName name; UObject* value; } p{n, tex};
        to->ProcessEvent(st, &p);
      }
  }
  void copyOverrides(UObject* to, UObject* from) {  // UMaterialInstanceDynamic::CopyParameterOverrides(from)
    static UFunction* f = fn(STR("/Script/Engine.MaterialInstanceDynamic:CopyParameterOverrides"));
    if (!f || !to || !from || !isDynamic(to)) return;
    struct { UObject* mi; } prm{from};
    to->ProcessEvent(f, &prm);
  }
  std::map<std::pair<UObject*, std::string>, EffCopy> g_eff_copies;  // by (the effect's material, the part's key)
  UObject* makeDynamic(UObject* world, UObject* parent) {  // KismetMaterialLibrary::CreateDynamicMaterialInstance (4.25: world, parent, name)
    static UObject* lib = cdo(STR("/Script/Engine.Default__KismetMaterialLibrary"));
    static UFunction* f = fn(STR("/Script/Engine.KismetMaterialLibrary:CreateDynamicMaterialInstance"));
    if (!lib || !f || !world || !parent) return nullptr;
    struct { UObject* world; UObject* parent; FName name; UObject* ret; } prm{world, parent, FName(), nullptr};
    lib->ProcessEvent(f, &prm);
    return prm.ret;
  }
  UObject* chainTexture(UObject* m, const char* param) {  // the texture a material instance draws with: its own, else a parent's
    for (int depth = 0; m && depth < 6; depth++, m = parentOf(m)) {
      if (!m->GetClassPrivate() || narrow(m->GetClassPrivate()->GetName()).find("MaterialInstance") == std::string::npos) return nullptr;
      if (UObject* tex = micTexture(m, param)) return tex;
    }
    return nullptr;
  }
  // The shadow texture the game gives a material. It comes from its instances and is never ours, since ours is set
  // on the dynamic one.
  UObject* gameSss(UObject* m) { return m ? chainTexture(isDynamic(m) ? parentOf(m) : m, "SssMap") : nullptr; }
  // A material with no color texture that draws a shadow texture. These are the outlines (every character's
  // MI_*_OUTLINE). Ky's Dragon Install arm lines are one, MI_DI_OUTLINE from DKY_Sss. Each palette's own shadow
  // texture colors them that way.
  bool outlineOnly(UObject* m) {
    std::string n, k;
    return isDynamic(m) && parentOf(m) && !chainTexture(parentOf(m), "BaseMap") && gameSss(m) && (!colorSource(m, n, k) || !getTexture(m, STR("BaseMap")));
  }
  // The outlines that draw a part's shadow texture take its recolored one too: on any of the fighter's meshes (Venom's
  // weapon outlines read his body's). taken tells whether an outline is another part's.
  template <class F> void addOutlines(Target& t, const std::vector<UObject*>& comps, F taken) {
    UObject* sss = t.mids.empty() ? nullptr : gameSss(t.mids.front());  // The part's own, from the first material
    if (!sss) return;
    for (UObject* comp : comps)
      for (int i = 0, n = numMaterials(comp); i < n && i < 64; i++) {
        UObject* m = material(comp, i);
        if (!m || std::find(t.mids.begin(), t.mids.end(), m) != t.mids.end() || taken(m) || !outlineOnly(m) || gameSss(m) != sss) continue;
        t.mids.push_back(m);
      }
  }
  UObject* effCopyFor(UObject* mic, const Target& t) {
    if (!mic->GetClassPrivate() || narrow(mic->GetClassPrivate()->GetName()).find("MaterialInstance") == std::string::npos) return nullptr;  // micTexture reads an instance's layout, so only instances
    UObject* base = chainTexture(mic, "BaseMap");  // Its own or its parent's. Elphelt's MI_WEP_BASE_Eff inherits the weapon's.
    if (!base) return nullptr;
    for (size_t i = 0; i < t.mids.size() && i < t.orig.size(); i++) {
      if (t.orig[i].first != base) continue;
      auto sh = g_shown.find(keyOf(t));
      if (sh == g_shown.end() || !sh->second.base) return nullptr;  // Nothing of ours is on yet
      EffCopy& c = g_eff_copies[{mic, keyOf(t)}];
      if (!c.mid) {
        c.mid = makeDynamic(t.mids.front(), mic);
        if (!c.mid) {
          static std::set<UObject*> said;
          if (said.insert(mic).second) COMBO_LOG("smear: couldn't make a copy of " + narrow(mic->GetName()));
          g_eff_copies.erase({mic, keyOf(t)});
          return nullptr;
        }
        root(c.mid);
        COMBO_LOG("smear: " + narrow(mic->GetName()) + " (an effect's own copy of " + keyOf(t) + "'s material) gets our textures; copy in " +
                  (c.mid->GetOuterPrivate() ? narrow(c.mid->GetOuterPrivate()->GetFullName()) : std::string("nothing")));  // Expected to be the transient package
      }
      c.src = t.mids[i];
      c.src_item = c.src->GetObjectItem() ? reinterpret_cast<const uint8_t*>(c.src->GetObjectItem()) : nullptr;
      c.mic = mic;
      c.own_sss = t.orig[i].second;  // The part's SssMap. The effect's own, if different, is put back.
      if (c.base != sh->second.base) {
        copyOverrides(c.mid, c.src);
        effOwnBack(c.mid, c.mic, c.own_sss);
        c.base = sh->second.base;
        static std::set<UObject*> said;  // What the copy draws with, once
        if (said.insert(mic).second) COMBO_LOG("smear: copy of " + narrow(mic->GetName()) + " now has" + paramsOf(c.mid));
      }
      return c.mid;
    }
    return nullptr;
  }
  void effCopiesSync() {  // Each update copies the part's material as it is now (its lighting, ours), then puts the effect's own values back
    for (auto& [k, c] : g_eff_copies) {
      if (!c.mid || !c.src) continue;
      if (!Kept{c.src, c.src_item}.alive()) { c.src = nullptr; continue; }  // The part's material is gone, so leave it as it was
      copyOverrides(c.mid, c.src);
      effOwnBack(c.mid, c.mic, c.own_sss);
    }
  }
  void effCopiesForget() {  // The fight is gone
    for (auto& [k, c] : g_eff_copies) unrootOne(c.mid);
    g_eff_copies.clear();
  }
  bool wearsOurs(const Target& o) {  // Edited, or wearing our textures now (a part with only paint)
    if (anyEdit(g_edits[keyOf(o)])) return true;
    auto sh = g_shown.find(keyOf(o));
    return sh != g_shown.end() && sh->second.base;
  }
  // The live copy for a material name of the shown slot. That's the name's color material in the slot's table,
  // worn by a target as one of our edited copies, or an effect's own copy of a part's material, given the part's
  // edited textures.
  UObject* liveForName(const std::string& name) {
    Target* t = target();
    if (!t) return nullptr;
    const std::wstring path = materialDir(t->chr, t->color) + L"/PTCColorAndMaterial.PTCColorAndMaterial";
    UObject* asset = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path.c_str());
    struct E { FName name; UArr mats; };
    struct A { E* data; int32_t num, max; };
    auto* table = asset ? asset->GetValuePtrByPropertyNameInChain<A>(STR("MaterialTable")) : nullptr;
    static std::set<std::string> said;
    if (said.insert(name).second) {
      std::string rows;
      for (int i = 0; table && i < table->num && i < 64; i++)
        rows += " " + narrow(table->data[i].name.ToString()) + "=" + (table->data[i].mats.num > 0 && table->data[i].mats.data[0] ? narrow(table->data[i].mats.data[0]->GetFullName()) : std::string("-"));
      COMBO_LOG("smear: looking up " + name + " in " + narrow(path) + (asset ? "" : " (not loaded)") + ":" + rows);
    }
    for (int i = 0; table && i < table->num && i < 64; i++) {
      if (narrow(table->data[i].name.ToString()) != name || table->data[i].mats.num < 1) continue;
      UObject* mic = table->data[i].mats.data[0];
      for (const Target& o : g_targets)
        if (o.chr == t->chr && o.color == t->color && wearsOurs(o))  // Unedited parts keep the game's own material
          for (UObject* mid : o.mids) {
            std::string n, k;
            if (colorSource(mid, n, k) == mic && mid != mic) return mid;
          }
      if (!mic) continue;
      for (const Target& o : g_targets)  // Not a part's own. Is it an effect's copy drawn from a part's texture?
        if (o.chr == t->chr && o.color == t->color && wearsOurs(o))
          if (UObject* c = effCopyFor(mic, o)) return c;
    }
    return nullptr;
  }
  // The effect components made since the last update that the game's effect pool owns (those last the fight).
  // Taken every update, used or not, because older ones may be gone (that crashed once).
  std::vector<Kept> takeNewEffects() {
    if (!g_psc_class) g_psc_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.ParticleSystemComponent"));
    std::vector<Kept> fresh;  // Only the pool's, filtered and recorded when they were made
    { std::lock_guard<std::mutex> lk(g_smear_new_m); fresh.swap(g_smear_new); }
    return fresh;
  }
  // Whose effect it is, meaning the fighter its component hangs from, if any. Only effects of the shown part's
  // fighter take ours, or of another fighter of the same character in the same color (the same edits). One that
  // can't be told takes ours unless the same character is on stage in another color (a mirror match).
  bool ourSmear(UObject* psc, UObject* tpl) {
    const Target* t = target();
    if (!t) return false;
    static std::ptrdiff_t att = -1;
    if (att < 0) {
      auto* a = psc->GetValuePtrByPropertyNameInChain<UObject*>(STR("AttachParent"));
      att = a ? reinterpret_cast<uint8_t*>(a) - reinterpret_cast<uint8_t*>(psc) : 0;
    }
    UObject* owner = nullptr;
    UObject* c = psc;
    for (int depth = 0; att > 0 && c && !owner && depth < 16; depth++) {
      UObject* parent = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(c) + att);
      if (!parent) break;
      if (std::find(g_pawns.begin(), g_pawns.end(), parent->GetOuterPrivate()) != g_pawns.end()) owner = parent->GetOuterPrivate();
      c = parent;
    }
    static std::set<UObject*> said;  // Which fighter each effect belongs to
    if (said.insert(tpl).second) COMBO_LOG("smear: " + narrow(tpl->GetName()) + " hangs from " + (owner ? narrow(owner->GetName()) : std::string("no fighter")));
    if (owner) {
      for (const Target& o : g_targets)
        for (UObject* comp : o.comps)
          if (comp->GetOuterPrivate() == owner) return o.chr == t->chr && o.color == t->color;
      return false;
    }
    const std::string me = codeOf(t->chr);
    return std::none_of(g_targets.begin(), g_targets.end(), [&](const Target& o) { return codeOf(o.chr) == me && (o.chr != t->chr || o.color != t->color); });
  }
  void undress(UObject* psc, const Smeared& d) {  // Puts back what it had. If that's gone since, the template's own, by setting no override.
    for (const auto& [e, prev] : d.prev) setEmitterMaterial(psc, e, prev.alive() ? prev.obj : nullptr);
  }
  void dressSmears() {  // Each update (before and after the game's), effects asking for our materials get the live copies
    Timed timed{g_time.smear};
    if (g_smear_tick++ % 60 == 0) g_smear_stale = true;  // Look the names up again now and then, it's cheap
    if (!g_smear_scanned) {  // the pool, once per fight (new ones come from the creation hook)
      g_smear_scanned = true;
      std::vector<UObject*> all;
      UObjectGlobals::FindAllOf(STR("ParticleSystemComponent"), all);
      if (!g_pool_last_fight.empty()) {
        const size_t alive = static_cast<size_t>(std::count_if(g_pool_last_fight.begin(), g_pool_last_fight.end(), [](const Kept& k) { return k.alive(); }));
        COMBO_LOG("smear: " + std::to_string(alive) + " of the last fight's " + std::to_string(g_pool_last_fight.size()) + " pooled effect components are still alive");
        if (alive == 0 && !g_mat_retired.empty()) {  // Its effects are gone, and with them anything drawing with our old copies
          COMBO_LOG("color editor: " + std::to_string(g_mat_retired.size()) + " old effect material copies let go (their effects are gone)");
          for (UObject* m : g_mat_retired) unrootOne(m);
          g_mat_retired.clear();
        }
        g_pool_last_fight.clear();
      }
      g_smear_pool.clear();
      std::set<std::string> outers;
      for (UObject* psc : all) {
        UObject* outer = psc->GetOuterPrivate();
        const std::string oc = outer && outer->GetClassPrivate() ? narrow(outer->GetClassPrivate()->GetName()) : std::string("none");
        outers.insert(oc);
        if (oc.find("PSCManager") != std::string::npos && narrow(outer->GetName()).rfind("Default__", 0) != 0) g_smear_pool.push_back(keep(psc));
      }
      for (auto it = g_smeared.begin(); it != g_smeared.end();)  // Gone from the pool, so forgotten and not touched
        it = std::any_of(g_smear_pool.begin(), g_smear_pool.end(), [&](const Kept& k) { return k.obj == it->first; }) ? std::next(it) : g_smeared.erase(it);
      matForgetGone();
      static size_t logged = static_cast<size_t>(-1);
      if (logged == static_cast<size_t>(-1)) {
        logged = g_smear_pool.size();
        std::string o;
        for (const auto& x : outers) o += " " + x;
        COMBO_LOG("smear: " + std::to_string(all.size()) + " effect components, " + std::to_string(g_smear_pool.size()) + " pooled; owners:" + o);
      }
    }
    {  // If a fighter's materials or meshes are gone since they were found, nothing is dressed until they're found again
      bool gone = false;
      for (const Target& t : g_targets) {
        if (t.items.size() != t.mids.size() + t.comps.size()) continue;  // Not from a find, so left as it was
        for (size_t i = 0; i < t.items.size() && !gone; i++) {
          UObject* o = i < t.mids.size() ? t.mids[i] : t.comps[i - t.mids.size()];
          gone = o && !Kept{o, t.items[i]}.alive();
        }
        if (gone) break;
      }
      if (gone) { g_smear_stale = true; return; }
    }
    for (const Kept& k : takeNewEffects())
      if (std::none_of(g_smear_pool.begin(), g_smear_pool.end(), [&](const Kept& p) { return p.obj == k.obj; })) g_smear_pool.push_back(k);
    if (g_smear_stale) { g_smear_live.clear(); g_smear_stale = false; }
    g_smear_pool.erase(std::remove_if(g_smear_pool.begin(), g_smear_pool.end(), [](const Kept& k) {
                         if (k.alive()) return false;
                         g_smeared.erase(k.obj);  // Destroyed, so forgotten without being read
                         matForgetComponent(k.obj);
                         g_hid.erase(k.obj);
                         return true;
                       }),
                       g_smear_pool.end());
    for (const Kept& k : g_smear_pool) {
      UObject* psc = k.obj;
      static std::ptrdiff_t tpl_off = -1, em_off = -1;  // the properties' offsets, found once (same class every time)
      if (tpl_off < 0) {
        auto* t0 = psc->GetValuePtrByPropertyNameInChain<UObject*>(STR("Template"));
        auto* e0 = psc->GetValuePtrByPropertyNameInChain<UArr>(STR("EmitterMaterials"));
        if (!t0 || !e0) continue;
        tpl_off = reinterpret_cast<uint8_t*>(t0) - reinterpret_cast<uint8_t*>(psc);
        em_off = reinterpret_cast<uint8_t*>(e0) - reinterpret_cast<uint8_t*>(psc);
      }
      UObject* tpl = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(psc) + tpl_off);
      auto was = g_smeared.find(psc);
      if (was != g_smeared.end() && was->second.tpl != tpl) {  // Reused for another effect, so put back what it had
        undress(psc, was->second);
        g_smeared.erase(was);
      }
      hideTick(psc, tpl);
      matDress(psc, tpl, em_off);
      if (!tpl) continue;
      const auto& named = namedMaterials(tpl);
      if (named.empty()) continue;
      if (!ourSmear(psc, tpl)) {  // Another fighter's, so left as the game has it
        auto d = g_smeared.find(psc);
        if (d != g_smeared.end()) { undress(psc, d->second); g_smeared.erase(d); }
        continue;
      }
      UObject* m = nullptr;
      for (const auto& [e, n] : named) {
        if (!g_smear_live.count(n)) {
          g_smear_live[n] = keep(liveForName(n));  // Alive now, it was just looked up
          static std::set<std::string> said;
          const std::string line = n + " -> " + (g_smear_live[n].obj ? narrow(g_smear_live[n].obj->GetName()) : std::string("nothing (no edited material for it)"));
          if (said.insert(line).second) COMBO_LOG("smear: " + line);
        }
        m = g_smear_live[n].alive() ? g_smear_live[n].obj : nullptr;
        if (!m) break;
      }
      auto cur = g_smeared.find(psc);
      if (!m) {  // Nothing edited now, so put back what it had
        if (cur != g_smeared.end()) { undress(psc, cur->second); g_smeared.erase(cur); }
        continue;
      }
      auto* em = reinterpret_cast<UArr*>(reinterpret_cast<uint8_t*>(psc) + em_off);
      if (cur == g_smeared.end()) {
        Smeared d;
        d.tpl = tpl;
        for (const auto& [e, n] : named) d.prev.push_back({e, keep(em && e < em->num ? em->data[e] : nullptr)});
        cur = g_smeared.emplace(psc, d).first;
      }
      for (const auto& [e, n] : named) {
        UObject* want = g_smear_live[n].alive() ? g_smear_live[n].obj : nullptr;
        if (want && (!em || em->num <= e || em->data[e] != want)) {
          setEmitterMaterial(psc, e, want);
          auto* after = psc->GetValuePtrByPropertyNameInChain<UArr>(STR("EmitterMaterials"));
          static std::set<UObject*> said;
          if (said.insert(psc).second)
            COMBO_LOG("smear: set " + narrow(want->GetName()) + " on " + narrow(psc->GetName()) + " (" + narrow(tpl->GetName()) + ") emitter " + std::to_string(e) +
                      ": EmitterMaterials now " + (after && e < after->num && after->data[e] == want ? "holds it" : "does NOT hold it"));
        }
      }
    }
  }
  void undressSmears() {  // Puts back what each had on every component still alive (the fight is ending)
    for (const Kept& k : g_smear_pool) {
      auto it = g_smeared.find(k.obj);
      if (it != g_smeared.end() && k.alive()) undress(k.obj, it->second);
    }
    matUndressAll();
    hideShowAll(false);  // Effects hidden by name are hidden again when they're dressed next
    g_smeared.clear();
    g_smear_pool.clear();
    g_smear_live.clear();
  }

  // ---- Captured effects. The effects that play during a capture (from the effect pool) have every color module
  // listed, editable like the slot's effect colors (each RGB keeps its total). Kept on disk per color slot by the
  // effect's path, and put on while that slot is the one shown. The game's values go back when the fight stops or
  // another slot is shown.
  // ponytail: the effects themselves are one per character, so in a mirror the shown slot's colors are on both fighters ----
  struct CapColor {
    std::string effect, path, label;  // effect name, its object path (to find it again), "#3 color over life"
    int index = 0;                    // the module's place in the effect (emitter * 100 + module)
    Kept module;
    const TCHAR* prop = nullptr;
    std::vector<float> orig;
    float rgb[3] = {1, 1, 1};
    bool edited = false;
  };
  std::vector<CapColor> g_cap;
  // Rows can go from the middle. A request names a row by its place in the list the menu showed, so each carries
  // the list's version and one from an older list is dropped.
  uint32_t g_cap_gen = 0;
  // An effect material's color parameter (noise_color...). Saved in g_cap_saved as "material path<tab>@parameter".
  struct MatColor { std::string effect, label, path, param; Kept mic; float orig[4] = {}; float rgb[3] = {1, 1, 1}; bool edited = false; };
  std::vector<MatColor> g_matc;
  std::map<UObject*, UObject*> g_mat_mid;  // material -> our live copy (only materials with an edit)
  std::set<std::string> g_mat_done;        // saved keys put on a copy
  bool capHolds(UObject* module) { return std::any_of(g_cap.begin(), g_cap.end(), [&](const CapColor& c) { return c.module.obj == module; }); }
  std::set<UObject*> g_cap_tpls;     // effects seen during this capture
  std::set<std::string> g_cap_names; // their names, taken while they're alive (for the log)
  std::set<UObject*> g_cap_checked;  // effects looked at for saved colors (each once)
  std::vector<std::pair<std::string, std::string>> g_cap_named;  // (effect, palette color it takes by name)
  Clock::time_point g_cap_until;
  std::string g_cap_slot;             // the slot ("SOL/Color08") whose saved captures were applied
  bool g_cap_unsaved = false;         // g_cap_slot's captured colors changed. Other slots' unsaved ones are staged (g_cap_restore)
  std::map<std::string, std::array<float, 3>> g_cap_saved;  // "path\tindex" -> rgb, for g_cap_slot
  std::string chrOf(const std::string& slot) { return slot.substr(0, slot.find('/')); }
  std::string colorOf(const std::string& slot) { return slot.find('/') == std::string::npos ? std::string() : slot.substr(slot.find('/') + 1); }
  std::filesystem::path capFile(const std::string& slot) { return g_dir / "saved" / chrOf(slot) / (colorOf(slot) + "_captured.txt"); }
  bool capSave() {  // Returns false if it couldn't be written
    if (g_cap_slot.empty()) return true;
    std::error_code ec;
    if (g_cap_saved.empty()) { std::filesystem::remove(capFile(g_cap_slot), ec); return true; }  // Nothing saved means no file, so the slot has no recolor from it
    std::filesystem::create_directories(capFile(g_cap_slot).parent_path(), ec);
    std::ofstream f(capFile(g_cap_slot), std::ios::trunc);
    for (const auto& [k, c] : g_cap_saved) f << k << '\t' << c[0] << ' ' << c[1] << ' ' << c[2] << "\n";
    f.flush();
    return f.good();
  }
  std::map<std::string, std::filesystem::path>& capRestorePaths();
  bool& capUnsavedFlag();
  void capLoad(const std::string& slot) {
    g_cap_slot = slot;
    g_cap_saved.clear();
    {  // The old format had one file for every slot of the character. It's set aside, not put on any slot.
      std::error_code ec;
      const std::filesystem::path old = g_dir / "saved" / chrOf(slot) / "effects_shared.txt";
      std::filesystem::path aside = old;
      aside += ".old";
      if (std::filesystem::exists(old, ec) && !std::filesystem::exists(aside, ec)) {
        std::filesystem::rename(old, aside, ec);
        COMBO_LOG("color editor: " + chrOf(slot) + "'s effects_shared.txt (every slot's captured colors) set aside: they're per slot now");
      }
    }
    auto& staged = capRestorePaths();
    auto st = staged.find(slot);
    std::ifstream f(st != staged.end() ? st->second : capFile(slot));
    if (st != staged.end()) { staged.erase(st); capUnsavedFlag() = true; }  // Recovered, so it counts as unsaved
    std::string line;
    while (std::getline(f, line)) {  // path \t index \t r g b
      const size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
      if (t2 == std::string::npos) continue;
      std::array<float, 3> c{};
      std::istringstream v(line.substr(t2 + 1));
      if (v >> c[0] >> c[1] >> c[2]) g_cap_saved[line.substr(0, t2)] = c;
    }
  }
  void capApply(CapColor& c) {
    if (!c.module.alive()) return;
    const std::vector<float> out = ratioed(c.orig, c.rgb);
    distWrite(fxDist(c.module.obj, c.prop), c.orig, c.edited ? out.data() : c.orig.data());
  }
  bool isMatInstance(UObject* m);
  const std::vector<Kept>& madeFrom(UObject* mic);
  void capRestore() {  // Puts the game's values back on every live module, then lets go
    for (CapColor& c : g_cap)
      if (c.edited && c.module.alive()) distWrite(fxDist(c.module.obj, c.prop), c.orig, c.orig.data());
    for (const auto& [k, c] : g_cap_saved) {  // An edited material color was on the game's own copies too, so theirs go back. Once, when the slot is let go.
      if (g_cap_slot.empty()) break;
      const size_t at = k.find("\t@");
      if (at == std::string::npos) continue;
      UObject* mic = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, wide(k.substr(0, at)).c_str());
      float orig[4] = {};
      if (!isMatInstance(mic) || !vecDefault(mic, k.substr(at + 2), orig)) continue;
      for (const Kept& m : madeFrom(mic))
        if (m.alive()) setVector(m.obj, wide(k.substr(at + 2)).c_str(), orig);
    }
    g_cap.clear();
    g_cap_gen++;
    matForget();
    g_cap_tpls.clear();
    g_cap_names.clear();
    g_cap_checked.clear();
    g_cap_named.clear();
    g_cap_until = Clock::time_point{};
  }
  // A by-name module's own ColorOverLife stays empty. When the effect loads, the game copies the palette color into
  // the emitter's ColorOverLife beside it. That module is the color's copy whatever it holds now (an older edit can
  // be on it), so it's found by where it sits, not by its values. Its game values are the palette's.
  UObject* systemOf(UObject* m) {
    for (int depth = 0; m && depth < 6; depth++, m = m->GetOuterPrivate())
      if (m->GetClassPrivate() && narrow(m->GetClassPrivate()->GetName()) == "ParticleSystem") return m;
    return nullptr;
  }
  int takeByName(UObject* ps) {
    int n = 0;
    auto* emitters = ps ? ps->GetValuePtrByPropertyNameInChain<UArr>(STR("Emitters")) : nullptr;
    for (int e = 0; emitters && e < emitters->num && e < 32; e++) {
      UObject* em = emitters->data[e];
      auto* lods = em ? em->GetValuePtrByPropertyNameInChain<UArr>(STR("LODLevels")) : nullptr;
      if (!lods || lods->num < 1 || !lods->data[0]) continue;
      auto* mods = lods->data[0]->GetValuePtrByPropertyNameInChain<UArr>(STR("Modules"));
      if (!mods) continue;
      std::string pattern;
      for (int mi = 0; mi < mods->num && mi < 64 && pattern.empty(); mi++) pattern = fxPattern(mods->data[mi]);
      if (pattern.empty()) continue;
      auto f = std::find_if(fxList().begin(), fxList().end(), [&](const FxColor& x) { return x.label == pattern; });
      if (f == fxList().end()) continue;
      int at = -1, count = 0;  // The emitter's one ColorOverLife. With two, which one it copies into isn't known.
      for (int mi = 0; mi < mods->num && mi < 64; mi++) {
        UObject* mod = mods->data[mi];
        if (!mod || !mod->GetClassPrivate() || narrow(mod->GetClassPrivate()->GetName()) != "ParticleModuleColorOverLife") continue;
        count++;
        at = mi;
      }
      if (count != 1) continue;
      UObject* mod = mods->data[at];
      uint8_t* d = fxDist(mod);
      if (!d) continue;
      FArr* v = reinterpret_cast<FArr*>(d + 8);
      if (static_cast<size_t>(v->num) + 6 != f->orig.size()) continue;
      if (std::any_of(g_fx_slots.begin(), g_fx_slots.end(), [&](const auto& kv) {  // Already any slot's, since effects are shared, a mirror's other slot too
            return std::any_of(kv.second.list.begin(), kv.second.list.end(), [&](const FxColor& x) {
              return std::any_of(x.copies.begin(), x.copies.end(), [&](const FxCopy& c) { return c.module == mod; });
            });
          }))
        continue;
      {  // Your own color on this emitter (a saved captured row) stays yours. The palette doesn't take it over.
        const std::string ckey = narrow(ps->GetPathName()) + "\t" + std::to_string(e * 100 + at);
        if (g_cap_saved.count(ckey)) continue;
      }
      {  // An unedited captured row on it goes, since the palette color is what sets it
        const size_t before = g_cap.size();
        g_cap.erase(std::remove_if(g_cap.begin(), g_cap.end(), [&](const CapColor& c) { return c.module.obj == mod; }), g_cap.end());
        if (g_cap.size() != before) g_cap_gen++;
        if (g_cap_saved.erase(narrow(ps->GetPathName()) + "\t" + std::to_string(e * 100 + at)) || g_cap.size() != before) { capUnsavedFlag() = true; g_view_dirty = true; }
      }
      f->copies.push_back({mod, STR("ColorOverLife"), f->orig, keep(mod), narrow(ps->GetName())});
      f->copies.back().named = true;
      const FxCopy& c = f->copies.back();
      const float* rgb = f->edited ? f->rgb : nullptr;
      const std::vector<float> co = rgb ? ratioed(c.orig, rgb) : c.orig;
      distWrite(d, c.orig, co.data());
      COMBO_LOG("color editor: " + narrow(ps->GetName()) + " emitter " + std::to_string(e + 1) + " takes " + pattern + " by name" + (rgb ? ": your color on it" : ""));
      n++;
    }
    return n;
  }
  void takeAllByName(const std::string& pattern) {  // every loaded effect taking this color (or any, "")
    std::vector<UObject*> named;
    UObjectGlobals::FindAllOf(STR("REDParticleModuleColorByName"), named);
    std::set<UObject*> systems;
    for (UObject* m : named)
      if (keep(m).alive() && (pattern.empty() || fxPattern(m) == pattern))
        if (UObject* ps = systemOf(m); ps && keep(ps).alive()) systems.insert(ps);
    for (UObject* ps : systems) takeByName(ps);
  }
  // ---- Effect material colors. An emitter's material is its mesh material module's (all one), else the required
  // module's. Only material instances count, since an instance's own parameters are what's listed and changed.
  bool isMatInstance(UObject* m) {
    return m && m->GetClassPrivate() && narrow(m->GetClassPrivate()->GetName()).find("MaterialInstance") != std::string::npos && !isDynamic(m);
  }
  std::map<UObject*, std::vector<std::pair<int, UObject*>>> g_tpl_mats;  // template -> (emitter, material)
  // ponytail: cached by template address for the session (as g_named). A template unloaded and another loaded at
  // the same address would reuse it.
  const std::vector<std::pair<int, UObject*>>& emitterMats(UObject* tpl) {
    auto it = g_tpl_mats.find(tpl);
    if (it != g_tpl_mats.end()) return it->second;
    std::vector<std::pair<int, UObject*>> out;
    auto* emitters = tpl->GetValuePtrByPropertyNameInChain<UArr>(STR("Emitters"));
    for (int e = 0; emitters && e < emitters->num && e < 32; e++) {
      UObject* em = emitters->data[e];
      auto* lods = em ? em->GetValuePtrByPropertyNameInChain<UArr>(STR("LODLevels")) : nullptr;
      if (!lods || lods->num < 1 || !lods->data[0]) continue;
      UObject* mat = nullptr;
      bool mesh = false, mixed = false;
      auto* mods = lods->data[0]->GetValuePtrByPropertyNameInChain<UArr>(STR("Modules"));
      for (int m = 0; mods && m < mods->num && m < 64; m++) {
        UObject* mod = mods->data[m];
        if (!mod || !mod->GetClassPrivate()) continue;
        const std::string cls = narrow(mod->GetClassPrivate()->GetName());
        if (cls == "REDParticleModuleMaterialByName") { mixed = true; break; }  // A smear's, set there
        if (cls != "ParticleModuleMeshMaterial") continue;
        mesh = true;
        if (auto* mm = mod->GetValuePtrByPropertyNameInChain<UArr>(STR("MeshMaterials")))
          for (int i = 0; i < mm->num && i < 16; i++) {
            if (mat && mm->data[i] != mat) mixed = true;
            mat = mm->data[i];
          }
      }
      if (mixed) continue;
      if (!mesh)
        if (auto** rq = lods->data[0]->GetValuePtrByPropertyNameInChain<UObject*>(STR("RequiredModule")); rq && *rq)
          if (auto** m = (*rq)->GetValuePtrByPropertyNameInChain<UObject*>(STR("Material"))) mat = *m;
      if (isMatInstance(mat)) out.push_back({e, mat});
    }
    return g_tpl_mats[tpl] = out;
  }
  // An instance's vector parameters (its own and its parents': every one).
  std::vector<std::string> matColorParams(UObject* m) {
    struct Arr { uint8_t* data; int num, max; };
    std::vector<std::string> out;
    for (int depth = 0; m && depth < 5 && !isDynamic(m); depth++, m = parentOf(m))
      if (auto* a = m->GetValuePtrByPropertyNameInChain<Arr>(STR("VectorParameterValues")))
        for (int i = 0; i < a->num && i < 64; i++) {
          const std::string n = narrow(reinterpret_cast<const FName*>(a->data + i * 48)->ToString());
          if (!n.empty() && n != "None" && std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
        }
    return out;
  }
  // Our copy of a material, with every edit of it on (made when the first edit comes; null if it can't be).
  UObject* matCopy(UObject* mic) {
    auto it = g_mat_mid.find(mic);
    if (it != g_mat_mid.end()) return it->second;
    const Target* t = target();
    if (!t || t->mids.empty()) return nullptr;  // Needs a world to make it in. Tried again with the next edit.
    if (!t->items.empty() && !Kept{t->mids.front(), t->items.front()}.alive()) return nullptr;  // The fighter is being reloaded, so later
    UObject* mid = makeDynamic(t->mids.front(), mic);
    if (!mid) {
      static std::set<UObject*> said;
      if (said.insert(mic).second) COMBO_LOG("color editor: couldn't make a copy of " + narrow(mic->GetName()));
      return nullptr;
    }
    root(mid);
    UObject* outer = mid->GetOuterPrivate();
    COMBO_LOG("color editor: live copy of effect material " + narrow(mic->GetName()) + " (in " + (outer ? narrow(outer->GetFullName()) : std::string("nothing")) + ")");
    return g_mat_mid[mic] = mid;
  }
  // The live materials the game made from an instance (a fighter's mesh wears a dynamic copy of its slot's materials).
  // ponytail: looks through every dynamic material in the game, at most once a second per instance
  const std::vector<Kept>& madeFrom(UObject* mic) {
    static Clock::time_point when;
    static std::map<UObject*, std::vector<Kept>> by_parent;  // One look through them all, at most every 5 s, since it walks every object
    if (Clock::now() - when > std::chrono::seconds(5)) {
      when = Clock::now();
      by_parent.clear();
      std::vector<UObject*> all;
      UObjectGlobals::FindAllOf(STR("MaterialInstanceDynamic"), all);
      for (UObject* m : all) {
        if (!m || !keep(m).alive()) continue;
        if (std::any_of(g_mat_mid.begin(), g_mat_mid.end(), [&](const auto& kv) { return kv.second == m; })) continue;  // (ours)
        if (std::find(g_mat_retired.begin(), g_mat_retired.end(), m) != g_mat_retired.end()) continue;  // (ours, let go)
        if (UObject* p = parentOf(m)) by_parent[p].push_back(keep(m));
      }
    }
    static const std::vector<Kept> none;
    auto it = by_parent.find(mic);
    return it == by_parent.end() ? none : it->second;
  }
  void matSet(UObject* mic, const std::string& param, const float orig[4], const float* rgb) {  // Null rgb means the game's value
    UObject* mid = matCopy(mic);
    float v[4] = {orig[0], orig[1], orig[2], orig[3]};
    if (rgb) {
      const std::vector<float> o{orig[0], orig[1], orig[2]};
      const std::vector<float> r = ratioed(o, rgb);  // Keeps its total, since an effect's colors are bright
      std::copy(r.begin(), r.end(), v);
    }
    if (mid) setVector(mid, wide(param).c_str(), v);
    for (const Kept& k : madeFrom(mic))
      if (k.alive()) setVector(k.obj, wide(param).c_str(), v);
  }
  std::map<UObject*, Smeared> g_matdressed;  // component -> the emitters we gave a copy
  void matDress(UObject* psc, UObject* tpl, std::ptrdiff_t em_off) {
    auto cur = g_matdressed.find(psc);
    if (cur != g_matdressed.end() && cur->second.tpl != tpl) { undress(psc, cur->second); g_matdressed.erase(cur); cur = g_matdressed.end(); }
    std::vector<std::pair<int, UObject*>> want;
    static std::map<UObject*, std::string> path_of;  // A template's path, found once, since this runs for every pooled component each update
    auto pathOf = [&](UObject* o) -> const std::string& {
      auto p = path_of.find(o);
      return p != path_of.end() ? p->second : path_of[o] = narrow(o->GetPathName());
    };
    static std::string chr_of, own;  // "/Chara/SOL/", made again only when the character changes
    if (chr_of != chrOf(g_cap_slot)) { chr_of = chrOf(g_cap_slot); own = "/Chara/" + codeOf(chr_of) + "/"; }
    if (!g_mat_mid.empty() && tpl && !g_cap_slot.empty() && pathOf(tpl).find(own) != std::string::npos)
      for (const auto& [e, mic] : emitterMats(tpl)) {
        auto m = g_mat_mid.find(mic);
        if (m != g_mat_mid.end() && m->second) want.push_back({e, m->second});
      }
    if (want.empty()) {
      if (cur != g_matdressed.end()) { undress(psc, cur->second); g_matdressed.erase(cur); }
      return;
    }
    auto em = [&]() { return reinterpret_cast<UArr*>(reinterpret_cast<uint8_t*>(psc) + em_off); };  // Read again, since setting can move it
    if (cur == g_matdressed.end()) {
      Smeared d;
      d.tpl = tpl;
      for (const auto& [e, m] : want) d.prev.push_back({e, keep(e < em()->num ? em()->data[e] : nullptr)});
      cur = g_matdressed.emplace(psc, d).first;
    }
    for (const auto& [e, m] : want)
      if (em()->num <= e || em()->data[e] != m) setEmitterMaterial(psc, e, m);
  }
  void matForgetComponent(UObject* psc) { g_matdressed.erase(psc); }
  void hideShowAll(bool forget_names) {
    for (const Kept& k : g_smear_pool)
      if (g_hid.count(k.obj) && k.alive()) setVisible(k.obj, true);
    g_hid.clear();
    if (forget_names) g_hidden.clear();
  }
  void matForgetGone() {
    for (auto it = g_matdressed.begin(); it != g_matdressed.end();)
      it = std::any_of(g_smear_pool.begin(), g_smear_pool.end(), [&](const Kept& k) { return k.obj == it->first; }) ? std::next(it) : g_matdressed.erase(it);
    for (auto it = g_hid.begin(); it != g_hid.end();)
      it = std::any_of(g_smear_pool.begin(), g_smear_pool.end(), [&](const Kept& k) { return k.obj == it->first; }) ? std::next(it) : g_hid.erase(it);
  }
  void matUndressAll() {
    for (const Kept& k : g_smear_pool) {
      auto it = g_matdressed.find(k.obj);
      if (it != g_matdressed.end() && k.alive()) undress(k.obj, it->second);
    }
    g_matdressed.clear();
  }
  void matPending() {  // Puts saved edits whose material is loaded now on a copy (every 20 updates)
    for (const auto& [k, c] : g_cap_saved) {
      const size_t at = k.find("\t@");
      if (at == std::string::npos) continue;  // Every pass, so a mesh's material the game makes later gets it too. Setting the same value again is cheap.
      UObject* mic = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, wide(k.substr(0, at)).c_str());
      if (!isMatInstance(mic)) continue;
      const std::string param = k.substr(at + 2);
      float orig[4] = {};
      if (!vecDefault(mic, param, orig)) continue;
      matSet(mic, param, orig, c.data());
    }
  }
  // Copies let go. They stay rooted while the effect pool they may be on is alive: an effect can still draw with one
  // after its slot is put back (an emitter keeps the material it was given, unseen by the garbage collector), and a
  // copy freed under it crashed the game (Reset whole color with captured material colors on). The pool outlives a
  // fight, and the copies are in the transient package, so they hold no world.
  // ponytail: kept until the pool is gone, which may be the whole session (a few small materials per reset or slot change)
  void matForget() {  // The edits were read again, or another character. Copies come off as the effects are dressed next.
    g_matc.clear();
    for (const auto& [mic, mid] : g_mat_mid)
      if (mid) g_mat_retired.push_back(mid);  // (still rooted: let go once the effect pool is gone)
    g_mat_mid.clear();
    g_mat_done.clear();
  }

  // An effect's color modules into the list (once per module), with what's saved for them applied.
  void capAdd(UObject* tpl) {
    if (!tpl || !g_cap_tpls.insert(tpl).second) return;
    const std::string effect = narrow(tpl->GetName()), path = narrow(tpl->GetPathName());
    g_cap_names.insert(effect);
    for (const auto& [e, mic] : emitterMats(tpl))  // its materials' colors (one row per material and parameter in this effect)
      for (const std::string& param : matColorParams(mic)) {
        if (std::any_of(g_matc.begin(), g_matc.end(), [&](const MatColor& x) { return x.effect == effect && x.mic.obj == mic && x.param == param; })) continue;
        MatColor mc;
        mc.effect = effect;
        mc.label = narrow(mic->GetName()) + "  " + param;
        mc.path = narrow(mic->GetPathName());
        mc.param = param;
        mc.mic = keep(mic);
        if (!vecDefault(mic, param, mc.orig)) continue;
        const float mx = (std::max)({mc.orig[0], mc.orig[1], mc.orig[2], 1e-6f});
        for (int k = 0; k < 3; k++) mc.rgb[k] = (std::max)(mc.orig[k], 0.0f) / mx;
        auto sv = g_cap_saved.find(mc.path + "\t@" + param);
        if (sv != g_cap_saved.end()) { std::memcpy(mc.rgb, sv->second.data(), sizeof(mc.rgb)); mc.edited = true; }
        g_matc.push_back(mc);
      }
    auto* emitters = tpl->GetValuePtrByPropertyNameInChain<UArr>(STR("Emitters"));
    for (int e = 0; emitters && e < emitters->num && e < 32; e++) {
      UObject* em = emitters->data[e];
      auto* lods = em ? em->GetValuePtrByPropertyNameInChain<UArr>(STR("LODLevels")) : nullptr;
      if (!lods || lods->num < 1 || !lods->data[0]) continue;
      auto* mods = lods->data[0]->GetValuePtrByPropertyNameInChain<UArr>(STR("Modules"));
      for (int m = 0; mods && m < mods->num && m < 64; m++) {
        UObject* mod = mods->data[m];
        if (!mod || !mod->GetClassPrivate()) continue;
        const std::string cls = narrow(mod->GetClassPrivate()->GetName());
        if (cls == "REDParticleModuleColorByName") {  // Its color comes from the slot's palette by name. Listed, but set in Effect colors.
          const std::string pattern = fxPattern(mod);
          const std::pair<std::string, std::string> nb{effect, pattern};
          if (!pattern.empty() && std::find(g_cap_named.begin(), g_cap_named.end(), nb) == g_cap_named.end()) g_cap_named.push_back(nb);
          continue;
        }
        const TCHAR* prop = nullptr;
        const char* kind = nullptr;
        if (cls == "ParticleModuleColorOverLife") { prop = STR("ColorOverLife"); kind = "color over life"; }
        else if (cls == "ParticleModuleColor") { prop = STR("StartColor"); kind = "start color"; }
        else if (cls == "ParticleModuleColorScaleOverLife") { prop = STR("ColorScaleOverLife"); kind = "color scale"; }
        if (!prop || std::any_of(g_cap.begin(), g_cap.end(), [&](const CapColor& c) { return c.module.obj == mod; })) continue;
        uint8_t* d = fxDist(mod, prop);
        if (!d) continue;
        FArr* v = reinterpret_cast<FArr*>(d + 8);
        if (v->num <= 0 || v->num > 4096 || v->num % 3) continue;
        // A copy of the slot's effect colors is edited there, per slot, not here.
        bool slot_copy = false;
        for (const auto& [k, slot] : g_fx_slots)
          for (const FxColor& f : slot.list)
            for (const FxCopy& cp : f.copies) slot_copy |= cp.module == mod;
        if (slot_copy) continue;
        CapColor c;
        c.effect = effect;
        c.path = path;
        c.index = e * 100 + m;
        c.label = "emitter " + std::to_string(e + 1) + ", " + kind;
        c.module = keep(mod);
        c.prop = prop;
        c.orig = distRead(d);
        const float mx = (std::max)({c.orig[0], c.orig[1], c.orig[2], 1e-6f});
        for (int k = 0; k < 3; k++) c.rgb[k] = c.orig[static_cast<size_t>(k)] / mx;
        auto s = g_cap_saved.find(path + "\t" + std::to_string(c.index));
        if (s != g_cap_saved.end()) { std::memcpy(c.rgb, s->second.data(), sizeof(c.rgb)); c.edited = true; }
        g_cap.push_back(c);
        if (g_cap.back().edited) capApply(g_cap.back());
      }
    }
  }
  void capTick() {  // During a capture, the pool's effects as they play
    // Saved colors on effects that load later (Sol's Dragon Install moves load with the move): put on as they appear.
    static int every = 0;
    if (g_cap_until == Clock::time_point{} && ++every % 20 == 0) {
      for (const Kept& k : g_smear_pool) {  // Effects that load later get the slot's colors on their by-name copies (Sol's DI HMC burst)
        if (!k.alive()) continue;
        auto** tp = k.obj->GetValuePtrByPropertyNameInChain<UObject*>(STR("Template"));
        if (!tp || !*tp || !g_fx_checked.insert(*tp).second) continue;
        takeByName(*tp);
      }
    }
    if (!g_cap_saved.empty() && every % 300 == 0) matPending();  // About every 5 s, so materials made later get the saved colors
    if (g_cap_until == Clock::time_point{} && !g_cap_saved.empty() && every % 20 == 0) {
      for (const Kept& k : g_smear_pool) {
        if (!k.alive()) continue;
        auto** tp = k.obj->GetValuePtrByPropertyNameInChain<UObject*>(STR("Template"));
        if (!tp || !*tp || g_cap_tpls.count(*tp) || !g_cap_checked.insert(*tp).second) continue;
        const std::string path = narrow((*tp)->GetPathName());
        const std::string pre = path + "\t";  // Saved keys are "path<tab>index"
        const auto at = g_cap_saved.lower_bound(pre);
        if (at != g_cap_saved.end() && at->first.rfind(pre, 0) == 0) {
          const size_t before = g_cap.size();
          capAdd(*tp);
          if (g_cap.size() != before) g_view_dirty = true;  // Its rows go in the list
        }
      }
    }
    if (g_cap_until == Clock::time_point{}) return;
    for (const Kept& k : g_smear_pool) {
      if (!k.alive()) continue;
      auto** tp = k.obj->GetValuePtrByPropertyNameInChain<UObject*>(STR("Template"));
      if (tp && *tp) capAdd(*tp);
    }
    if (Clock::now() >= g_cap_until) {
      g_cap_until = Clock::time_point{};
      COMBO_LOG("color editor: captured " + std::to_string(g_cap_tpls.size()) + " effects, " + std::to_string(g_cap.size()) + " colors");
      {  // Which effects, and how many color modules each gave, to tell what a move's effects are
        std::map<std::string, int> per;
        for (const std::string& n : g_cap_names) per[n] += 0;
        for (const CapColor& c : g_cap) per[c.effect]++;
        std::string names;
        for (const auto& [n, k] : per) names += " " + n + "(" + std::to_string(k) + ")";
        COMBO_LOG("color editor: captured effects:" + names);
      }
      for (const CapColor& c : g_cap) {  // Each color's own values, to tell which effect draws a color, e.g. Sol's blue HMC burst
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        for (size_t k = 0; k + 2 < c.orig.size(); k += 3)
          for (int ch = 0; ch < 3; ch++) { lo[ch] = (std::min)(lo[ch], c.orig[k + ch]); hi[ch] = (std::max)(hi[ch], c.orig[k + ch]); }
        const bool blue = hi[2] > 0 && hi[2] > 2 * (std::max)(hi[0], hi[1]);
        char buf[200];
        snprintf(buf, sizeof(buf), "%s, %s: R %.3g..%.3g G %.3g..%.3g B %.3g..%.3g%s%s", c.effect.c_str(), c.label.c_str(), lo[0], hi[0], lo[1], hi[1], lo[2], hi[2],
                 blue ? "  BLUE" : "", c.edited ? "  (edited)" : "");
        COMBO_LOG(std::string("color editor:   ") + buf);
      }
      g_view_dirty = true;
    }
  }

  // Edits are kept on disk per character, color and part, and come back the next time. The region
  // count guards them: another region map would put them on the wrong parts.
  std::set<std::string> g_edits_loaded;  // targets whose saved edits were read
  std::filesystem::path editsFile(const Target& t) { return g_dir / "saved" / t.chr / (t.color + "_" + t.kind + ".txt"); }
  std::string stampOf(const CharMap& m) {  // the big regions' count and sizes, hashed (the same as before the small ones)
    uint64_t h = 1469598103934665603ull;
    for (int r = 0; r < m.major && r < static_cast<int>(m.share.size()); r++) h = (h ^ static_cast<uint64_t>(std::llround(m.share[r] * 1e7))) * 1099511628211ull;
    return "edits regions " + std::to_string(m.major) + " map " + std::to_string(h);
  }
  std::string minorsLine(const CharMap& m) {  // "minors <count> <hash>" for the small regions, guarding what's saved on them
    uint64_t h = 1469598103934665603ull;
    for (size_t r = static_cast<size_t>(m.major); r < static_cast<size_t>(m.pieces) && r < m.share.size(); r++)
      h = (h ^ static_cast<uint64_t>(std::llround(m.share[r] * 1e7))) * 1099511628211ull;
    return "minors " + std::to_string(m.pieces - m.major) + " " + std::to_string(h);
  }
  std::string blobsLine(const CharMap& m) {  // "blobs <count> <hash>" for the strokes, after the small ones
    uint64_t h = 1469598103934665603ull;
    for (size_t r = static_cast<size_t>(m.pieces); r < m.share.size(); r++) h = (h ^ static_cast<uint64_t>(std::llround(m.share[r] * 1e7))) * 1099511628211ull;
    return "blobs " + std::to_string(m.regions - m.pieces) + " " + std::to_string(h);
  }
  std::string mapStamp(const Target& t) {
    auto mp = g_maps.find(mapKey(t));
    return mp == g_maps.end() ? std::string("none") : stampOf(*mp->second);
  }
  std::string colorsLine(const CharMap& m) {  // "colors Color01,Color02,...", the colors the map was made from
    std::string l = "colors ";
    for (size_t i = 0; i < m.prefixes.size(); i++) l += (i ? "," : "") + m.prefixes[i];
    return l;
  }
  // A patch that adds a color makes a new region map (a region is where every color agrees). A save made for the
  // old one is moved onto it. The old map is made again from the colors the save names (still in the cache), and
  // each new region takes what its old region had (a new color only splits regions). Saves from before the colors
  // line get it the first time they match. Runs on a worker, on files only, before the fight reads them.
  std::filesystem::path withSuffix(std::filesystem::path p, const char* suffix) { p += suffix; return p; }  // No narrow conversion, so any install folder works
  std::vector<std::filesystem::directory_entry> listDir(const std::filesystem::path& d) {
    std::vector<std::filesystem::directory_entry> out;
    try {
      std::error_code ec;
      for (auto it = std::filesystem::directory_iterator(d, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        const std::wstring n = it->path().filename().wstring();
        if (std::all_of(n.begin(), n.end(), [](wchar_t c) { return c > 31 && c < 127; })) out.push_back(*it);
      }
    } catch (...) {  // Never takes the game down
    }
    return out;
  }
  // Save files are written from the game thread and moved by the map worker. Both hold it briefly.
  std::mutex g_files_m;
  bool colorName(const std::string& c) {  // "Color08" or "Color08_WEP", a cache file's own name and nothing else
    return c.size() >= 7 && c.compare(0, 5, "Color") == 0 && std::isdigit(static_cast<unsigned char>(c[5])) && std::isdigit(static_cast<unsigned char>(c[6])) &&
           std::all_of(c.begin(), c.end(), [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; });
  }
  std::vector<std::string> readLines(const std::filesystem::path& f) {
    std::vector<std::string> out;
    std::ifstream in(f);
    std::string l;
    while (std::getline(in, l)) { if (!l.empty() && l.back() == '\r') l.pop_back(); out.push_back(l); }
    return out;
  }
  // Writes the lines in place of what was read, if the file still holds just that (another write meanwhile wins).
  // Whole or not at all, since it's written beside it, checked, then renamed over it.
  bool replaceLines(const std::filesystem::path& f, const std::vector<std::string>& was, const std::vector<std::string>& now) {
    std::lock_guard<std::mutex> lk(g_files_m);
    if (readLines(f) != was) return false;
    const std::filesystem::path tmp = withSuffix(f, ".migrating");
    bool ok;
    {
      std::ofstream o(tmp, std::ios::trunc);
      for (const std::string& l : now) o << l << "\n";
      o.flush();
      ok = o.good();
    }
    std::error_code ec;
    if (ok) std::filesystem::rename(tmp, f, ec);
    if (!ok || ec) { std::filesystem::remove(tmp, ec); return false; }
    return true;
  }
  // Per new region, the old region under most of its texels (-1 if mostly in none), if it has pct% of them.
  // Both maps must be the same size (one pass over the texels).
  std::vector<int> overlapOf(const CharMap& om, const CharMap& now, int pct) {
    std::vector<int> tr(static_cast<size_t>(now.regions), -1);
    if (om.owner.size() != now.owner.size()) return tr;
    std::vector<std::unordered_map<int, int>> votes(static_cast<size_t>(now.regions));
    std::vector<int> size(static_cast<size_t>(now.regions), 0);
    for (size_t q = 0; q < now.owner.size(); q++) {
      const uint16_t r = now.owner[q];
      if (r == 0xFFFF) continue;
      votes[r][om.owner[q] == 0xFFFF ? -1 : om.owner[q]]++;
      size[r]++;
    }
    for (size_t r = 0; r < tr.size(); r++) {
      int best = -1, n = 0;
      for (const auto& [o, c] : votes[r]) if (c > n || (c == n && o < best)) { n = c; best = o; }  // Ties go to the lower number, the same every run
      if (best >= 0 && static_cast<int64_t>(n) * 100 >= static_cast<int64_t>(size[r]) * pct) tr[r] = best;
    }
    return tr;
  }
  // Moves a save's lines (edits, or names with tabs) made on map om onto now. New region r takes old region tr[r]'s
  // (-1 for nothing). Old small regions or strokes of another map (their list differs) are dropped, as when read.
  // Lines of no region (material colors) stay, and the colors and region lists become now's.
  // had gets the old regions with a line, homed those of them some new region took.
  std::vector<std::string> linesOnto(const std::vector<std::string>& lines, bool names, const CharMap& om, const CharMap& now, const std::vector<int>& tr,
                                     size_t& had, int& carried, size_t* homed = nullptr) {
    std::map<int, std::string> by_old;  // old region -> the rest of its line
    std::vector<std::string> out{stampOf(now)}, rest;
    std::string old_minors, old_blobs;
    for (const std::string& l : lines) if (l.rfind("minors ", 0) == 0) old_minors = l;
    for (const std::string& l : lines) if (l.rfind("blobs ", 0) == 0) old_blobs = l;
    const int old_limit = old_minors != minorsLine(om) ? om.major : old_blobs != blobsLine(om) ? om.pieces : om.regions;  // (only regions that were these)
    for (size_t i = 1; i < lines.size(); i++) {
      const std::string& l = lines[i];
      if (l.rfind("colors ", 0) == 0 || l.rfind("minors ", 0) == 0 || l.rfind("blobs ", 0) == 0) continue;
      const size_t sep = l.find(names ? '\t' : ' ');
      const bool numbered = sep != std::string::npos && sep > 0 &&
                            std::all_of(l.begin(), l.begin() + static_cast<std::ptrdiff_t>(sep), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
      const int r = numbered ? std::atoi(l.substr(0, sep).c_str()) : -1;
      if (numbered) { if (r < old_limit) by_old[r] = l.substr(sep); } else rest.push_back(l);
    }
    carried = 0;
    std::set<int> took;
    for (size_t r = 0; r < tr.size(); r++) {
      auto it = tr[r] >= 0 ? by_old.find(tr[r]) : by_old.end();
      if (it != by_old.end()) { out.push_back(std::to_string(r) + it->second); carried++; took.insert(tr[r]); }
    }
    if (homed) *homed = took.size();
    out.insert(out.end(), rest.begin(), rest.end());
    out.push_back(colorsLine(now));
    out.push_back(minorsLine(now));
    out.push_back(blobsLine(now));
    had = by_old.size();
    return out;
  }
  // Old maps are kept in cache/<CHR>/maps_old as map_<KIND>_<hash>.bin (the stamp's hash) before a map is replaced
  // or read again. A save or palette made on one is moved onto the new map when read.
  std::string mapHash(const CharMap& m) { const std::string s = stampOf(m); return s.substr(s.rfind(' ') + 1); }
  std::filesystem::path oldMapFile(const std::filesystem::path& dir, const std::string& kind, const std::string& hash) {
    return dir / "maps_old" / ("map_" + kind + "_" + hash + ".bin");
  }
  void archiveMap(const std::filesystem::path& dir, const std::string& kind, const CharMap& m) {
    static std::mutex one;  // The menus' map jobs and the fight's may overlap
    std::lock_guard<std::mutex> only(one);
    std::error_code ec;
    const std::filesystem::path to = oldMapFile(dir, kind, mapHash(m));
    if (std::filesystem::exists(to, ec)) { std::filesystem::last_write_time(to, std::filesystem::file_time_type::clock::now(), ec); return; }  // Already kept, so just mark it the newest again
    std::filesystem::create_directories(to.parent_path(), ec);
    if (!writeMapFile(to, m)) return;
    COMBO_LOG("color editor: kept " + dir.filename().string() + "/" + kind + "'s region map " + mapHash(m) + " in maps_old");
    const std::string pre = "map_" + kind + "_";
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> kept;
    for (const auto& e : listDir(to.parent_path())) {
      const std::string n = e.path().filename().string();
      if (n.size() > pre.size() + 4 && n.rfind(pre, 0) == 0 && n.compare(n.size() - 4, 4, ".bin") == 0 &&
          std::all_of(n.begin() + static_cast<std::ptrdiff_t>(pre.size()), n.end() - 4, [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
        kept.push_back({std::filesystem::last_write_time(e.path(), ec), e.path()});
    }
    std::sort(kept.begin(), kept.end());
    for (size_t i = 0; i + 20 < kept.size(); i++) std::filesystem::remove(kept[i].second, ec);  // Keep 20 per part and delete the oldest
  }
  void archiveMapFile(const std::filesystem::path& dir, const std::string& kind) {
    if (auto m = readMapFile(dir / ("map_" + kind + ".bin"))) archiveMap(dir, kind, *m);
  }
  // A kept old map worked out against the current one, once per pair per session. It holds the old map without its
  // texels and, for each new region, the old region it takes (om is null if none was kept or it doesn't fit).
  // Game thread. Dropped whenever a map changes.
  struct OldFit { std::shared_ptr<CharMap> om; std::vector<int> tr; };
  std::map<std::string, OldFit> g_old_fits;  // Keyed by "<chr>/<kind> <old hash> <new stamp>"
  // Moves a save (edits or names) made on another map onto now, if that map was kept. The file is rewritten with
  // now's stamp and the previous version is kept as .premigrate_<old hash>. A new region takes an old region's data
  // only if it has half that region's texels or more. If fewer than half the old edited regions find a place, the
  // maps aren't related and nothing is moved. Returns false if nothing moved (it's set aside when read, as before).
  bool moveOntoMap(const std::filesystem::path& path, const std::string& chr, const std::string& kind, const CharMap& now, bool names,
                   std::vector<std::string>* keep = nullptr) {  // If keep is set, the moved lines are handed back and no file is written
    std::vector<std::string> lines;
    { std::lock_guard<std::mutex> lk(g_files_m); lines = readLines(path); }
    const std::string stamp = stampOf(now), head = "edits regions ";
    if (lines.empty() || lines[0] == stamp || lines[0].rfind(head, 0) != 0) return false;
    const size_t at = lines[0].find(" map ");
    if (at == std::string::npos || at <= head.size()) return false;
    const std::string count = lines[0].substr(head.size(), at - head.size()), hash = lines[0].substr(at + 5);
    auto digits = [](const std::string& v) { return !v.empty() && v.size() <= 20 && std::all_of(v.begin(), v.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }); };
    if (!digits(count) || count.size() > 4 || !digits(hash)) return false;
    const std::string key = chr + "/" + kind + " " + hash + " " + stamp;
    auto fit = g_old_fits.find(key);
    if (fit == g_old_fits.end()) {
      OldFit f;
      std::error_code ec;
      const std::filesystem::path kept = oldMapFile(g_dir / "cache" / chr, kind, hash);
      auto om = std::filesystem::exists(kept, ec) ? readMapFile(kept, now.w, now.h, std::atoi(count.c_str())) : nullptr;  // For another size, only the header is read
      if (om && stampOf(*om) == lines[0]) {
        f.tr = overlapOf(*om, now, 50);
        om->owner = std::vector<uint16_t>();  // The texels aren't needed again
        om->groups = std::vector<GroupView>();
        f.om = om;
      }
      fit = g_old_fits.emplace(key, std::move(f)).first;
    }
    if (!fit->second.om) return false;
    size_t had = 0, homed = 0;
    int carried = 0;
    const std::vector<std::string> out = linesOnto(lines, names, *fit->second.om, now, fit->second.tr, had, carried, &homed);
    if (homed * 2 < had) {
      COMBO_LOG("color editor: " + path.filename().string() + " doesn't fit map " + mapHash(now) + " (" + std::to_string(homed) + " of " + std::to_string(had) +
                " edited regions from map " + hash + " found a place): not moved");
      return false;
    }
    if (keep) { *keep = out; return true; }
    std::error_code ec;
    {
      std::lock_guard<std::mutex> lk(g_files_m);
      const std::filesystem::path bak = withSuffix(path, (".premigrate_" + hash).c_str());
      if (!std::filesystem::exists(bak, ec)) std::filesystem::copy_file(path, bak, ec);  // Never overwritten, so the first copy stays
    }
    if (ec || !replaceLines(path, lines, out)) return false;  // Without a copy of the old one, leave it alone
    COMBO_LOG("color editor: moved " + path.filename().string() + " from map " + hash + " onto " + mapHash(now) + " (" + std::to_string(carried) + " edited regions)");
    return true;
  }
  // A patch that adds a color makes a new region map (a region is where every color agrees). A save made for the
  // old map is moved onto it. The old map is made again from the colors the save names (still in the cache), and
  // each new region takes what its old region had. A new color only splits regions, so only a map made from more
  // colors, the save's among them, takes it. Saves from before the colors line get it the first time they match.
  // Runs on the worker and only touches files, before the fight reads them.
  std::string migrateSaves(const std::string& chr, const std::string& kind, const CharMap& now) {
    static std::mutex one;  // A menu's job and the fight's may overlap, so one at a time
    std::lock_guard<std::mutex> only(one);
    const std::string stamp = stampOf(now), list = colorsLine(now);
    std::map<std::string, std::shared_ptr<CharMap>> olds;  // Keyed by colors line
    std::map<std::string, std::vector<int>> to_old;         // Keyed by colors line. For each new region, its old one (-1 for none)
    std::string done;
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& dir : {g_dir / "saved" / chr, g_dir / "saved" / ".recovery" / chr})
      for (const auto& it : listDir(dir)) {
        const std::string n = it.path().filename().string();
        const bool names = n == "names_" + kind + ".txt";
        const bool edits = n.size() == 7 + 1 + kind.size() + 4 && n.compare(0, 5, "Color") == 0 && std::isdigit(static_cast<unsigned char>(n[5])) &&
                           std::isdigit(static_cast<unsigned char>(n[6])) && n.compare(7, std::string::npos, "_" + kind + ".txt") == 0;
        if (names || edits) files.push_back(it.path());
      }
    for (const auto& path : files) {
      const std::string n = path.filename().string();
      const bool names = n.rfind("names_", 0) == 0;
      std::vector<std::string> lines;
      { std::lock_guard<std::mutex> lk(g_files_m); lines = readLines(path); }
      if (lines.empty()) continue;
      std::string colors;
      for (const std::string& l : lines) if (l.rfind("colors ", 0) == 0) colors = l;
      std::string minors;
      for (const std::string& l : lines) if (l.rfind("minors ", 0) == 0) minors = l;
      const bool same_big = lines[0] == stamp;
      std::string blobs;
      for (const std::string& l : lines) if (l.rfind("blobs ", 0) == 0) blobs = l;
      if (same_big && (colors.empty() || colors == list ||
                       ((minors.empty() || minors == minorsLine(now)) && (blobs.empty() || blobs == blobsLine(now))))) {  // (this map's: the lists from now on)
        if (colors.empty()) { auto with = lines; with.push_back(list); replaceLines(path, lines, with); }
        continue;
      }
      if (colors.empty() || colors == list) continue;  // No list or the same colors, so it's set aside when read, as before
      if (!olds.count(colors)) {
        olds[colors] = nullptr;
        std::vector<std::string> pre;
        std::stringstream ss(colors.substr(7));
        bool ok = true;
        for (std::string c; std::getline(ss, c, ',');) {
          ok = ok && colorName(c) && std::find(now.prefixes.begin(), now.prefixes.end(), c) != now.prefixes.end() &&  // Only onto a map made from more colors
               std::filesystem::exists(g_dir / "cache" / chr / (c + "_BaseMap.png"), ec) && std::filesystem::exists(g_dir / "cache" / chr / (c + "_SssMap.png"), ec);
          pre.push_back(c);
        }
        ok = ok && !pre.empty() && pre.size() < now.prefixes.size();
        std::shared_ptr<CharMap> om = ok ? buildMap(g_dir / "cache" / chr, pre) : nullptr;
        if (om && om->regions > 0 && om->w == now.w && om->h == now.h) {
          olds[colors] = om;
          to_old[colors] = overlapOf(*om, now, 90);  // Not inside one old region, so nothing is carried over
        }
      }
      const auto& om = olds[colors];
      if (!om || lines[0] != stampOf(*om)) continue;  // Not the map it was made for, so it's set aside when read, as before
      // If the big regions are unchanged, theirs stay as they are and only the small regions' are moved.
      std::vector<int> tr = to_old[colors];
      if (same_big) for (int r = 0; r < now.major && r < static_cast<int>(tr.size()); r++) tr[static_cast<size_t>(r)] = r;  // Same big regions, so match them by number
      size_t had = 0;
      int carried = 0;
      const std::vector<std::string> out = linesOnto(lines, names, *om, now, tr, had, carried);
      {
        std::lock_guard<std::mutex> lk(g_files_m);
        std::filesystem::copy_file(path, withSuffix(path, ".before_new_colors"), std::filesystem::copy_options::overwrite_existing, ec);
      }
      if (ec) continue;  // Without a copy of the old one, leave it alone
      if (replaceLines(path, lines, out)) done += " " + n + " (" + std::to_string(had) + " regions -> " + std::to_string(carried) + ")";
    }
    if (!done.empty()) COMBO_LOG("color editor: " + chr + "/" + kind + ": saves moved onto the new region map:" + done);
    return done;
  }
  // The pinned textures (cache/<CHR>/pinned) are every slot as first read, which the shared map is built from.
  // After a read, each slot's textures are compared with its pinned ones. A slot that changed (a skin pak) is listed
  // in own_<KIND>.txt and gets its own map. If most slots changed at once it's a game patch, not a pak, so the new
  // ones are pinned instead (one shared map again, with saves moved onto it by overlap). Worker.
  std::vector<uint8_t> readBytes(const std::filesystem::path& f);
  bool sameTexture(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    if (std::filesystem::equivalent(a, b, ec)) return true;
    if (std::filesystem::file_size(a, ec) == std::filesystem::file_size(b, ec) && !ec && readBytes(a) == readBytes(b)) return true;
    Image ia, ib;
    if (!decodePng(a, ia) || !decodePng(b, ib) || ia.w != ib.w || ia.h != ib.h) return false;
    const size_t n = static_cast<size_t>(ia.w) * ia.h;
    size_t diff = 0;
    for (size_t p = 0; p < n && diff * 1000 <= n; p++) diff += rgbAt(ia, p) != rgbAt(ib, p);
    return diff * 1000 <= n;  // Only a few texels differ, so it's the same texture drawn again
  }
  void pinAs(const std::filesystem::path& cur, const std::filesystem::path& pin) {  // A hard link if possible so no copy is made, else a copy
    std::error_code ec;
    std::filesystem::remove(pin, ec);
    std::filesystem::create_hard_link(cur, pin, ec);
    if (ec) { ec.clear(); std::filesystem::copy_file(cur, pin, ec); }
  }
  void syncPinned(const std::filesystem::path& dir, const std::string& kind, const std::vector<std::string>& prefixes) {
    std::lock_guard<std::mutex> lk(g_pin_m);
    std::error_code ec;
    const auto own = ownFile(dir, kind), colors = dir / ("colors_" + kind + ".txt");
    if (std::filesystem::exists(own, ec) && std::filesystem::last_write_time(own, ec) >= std::filesystem::last_write_time(colors, ec) && !ec) return;  // Nothing read since
    std::filesystem::create_directories(dir / "pinned", ec);
    std::vector<std::string> differ;
    for (const std::string& c : prefixes) {
      bool same = true;
      for (const char* m : {"_BaseMap.png", "_SssMap.png"}) {
        const auto cur = dir / (c + m), pin = dir / "pinned" / (c + m);
        if (!std::filesystem::exists(cur, ec)) continue;
        if (!std::filesystem::exists(pin, ec)) { pinAs(cur, pin); continue; }  // First seen, so pin it as it is
        same = same && sameTexture(cur, pin);
      }
      if (!same) differ.push_back(c);
    }
    if (!differ.empty() && differ.size() * 2 > prefixes.size()) {  // A game patch: pin the new textures and make the shared map again
      for (const std::string& c : prefixes)
        for (const char* m : {"_BaseMap.png", "_SssMap.png"})
          if (std::filesystem::exists(dir / (c + m), ec)) pinAs(dir / (c + m), dir / "pinned" / (c + m));
      archiveMapFile(dir, kind);
      std::filesystem::remove(dir / ("map_" + kind + ".bin"), ec);
      COMBO_LOG("color editor: " + dir.filename().string() + "/" + kind + ": " + std::to_string(differ.size()) + " of " + std::to_string(prefixes.size()) +
                " colors changed, the new ones pinned");
      differ.clear();
    }
    {
      std::ofstream f(withSuffix(own, ".new"), std::ios::trunc);
      for (const std::string& c : differ) f << c << "\n";
    }
    std::filesystem::rename(withSuffix(own, ".new"), own, ec);
    if (!differ.empty()) {
      std::string l;
      for (const std::string& c : differ) l += " " + c;
      COMBO_LOG("color editor: " + dir.filename().string() + "/" + kind + ": changed since first read (their own region maps):" + l);
    }
  }
  // own is the slot's own map. No pins are compared and no saves moved, since only the shared map's job does that.
  std::shared_ptr<CharMap> mapAndMove(std::filesystem::path dir, std::string kind, std::vector<std::string> prefixes, std::string own = {}) {  // Worker
    if (own.empty()) {
      try { syncPinned(dir, kind, prefixes); } catch (...) {}
    }
    auto m = mapFor(dir, kind, prefixes, own);
    try {
      if (m && m->regions > 0 && own.empty()) migrateSaves(dir.filename().string(), kind, *m);
    } catch (...) {  // Never takes the game down, and the saves are left as they were
    }
    return m;
  }
  bool saveEditsTo(const Target& t, const std::vector<Edit>& ed, const std::filesystem::path& to) {  // Returns false if it couldn't be written
    std::lock_guard<std::mutex> lk(g_files_m);
    std::error_code ec;
    std::filesystem::create_directories(to.parent_path(), ec);
    std::ofstream f(to, std::ios::trunc);
    f << mapStamp(t) << "\n";
    for (size_t r = 0; r < ed.size(); r++) {
      const Edit& e = ed[r];
      if (e.has_lit || e.has_shadow) f << r << ' ' << e.has_lit << ' ' << e.lit << ' ' << e.has_shadow << ' ' << e.shadow << ' ' << e.derive << "\n";
    }
    for (const auto& [label, c] : g_vec_saved[keyOf(t)])  // Material colors, as "vec <label>\t<r g b a>"
      f << "vec " << label << '\t' << c[0] << ' ' << c[1] << ' ' << c[2] << ' ' << c[3] << "\n";
    if (auto mp = g_maps.find(mapKey(t)); mp != g_maps.end())
      f << colorsLine(*mp->second) << "\n" << minorsLine(*mp->second) << "\n" << blobsLine(*mp->second) << "\n";  // The colors it's for
    f.flush();
    return f.good();
  }
  bool saveEdits(const Target& t, const std::vector<Edit>& ed) { return saveEditsTo(t, ed, editsFile(t)); }
  // ---- Painting (the Texture window's brush, eraser and fill). Layers over a part's region colors, one per part
  // and color slot (paint_layer.h). Saved beside the edits as <Color>_<KIND>_paint.slpn and put on whenever the
  // part's textures are made (a snapshot, composed on the worker). It's texel exact, so a new region map doesn't
  // touch it. ----------------------------------------------------------------------------------------------
  struct PaintPart {
    paint::Canvas canvas;
    bool ready = false, bad_file = false;  // bad_file means the saved one couldn't be read (left on disk as .old)
    std::vector<uint8_t> mask;              // Mask for "keep inside the region"
    std::vector<std::pair<float, float>> pts;  // the open stroke's points (the shadow takes them too for "both")
    paint::Map map = paint::Map::Lit;
    bool both = false, stroking = false;
    paint::Blend blend = paint::Blend::Normal;
    paint::Rgba rgb{255, 255, 255, 255};
    float size = 16, hardness = 0.8f, opacity = 1, flow = 1;
    bool eraser = false;
    PaintBytes snap;                        // What save() gives, for encodes. Made again after a change
    bool snap_stale = true;
    paint::Image base[2], comp[2];          // For the texture view: region colors alone and with the paint (full size)
    uint64_t base_sig = 0;
    uint64_t gen = 0;                       // Bumped on every change so the view's picture is made again
  };
  std::map<std::string, PaintPart> g_paint;                          // Keyed by part key (read when first painted or shown)
  std::map<std::string, std::filesystem::path> g_paint_restore;      // recovered copies waiting to be read
  std::filesystem::path paintFile(const Target& t) { return g_dir / "saved" / t.chr / (t.color + "_" + t.kind + "_paint.slpn"); }
  std::vector<uint8_t> readBytes(const std::filesystem::path& f) {  // (whole, in one read)
    std::ifstream in(f, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const std::streamoff n = in.tellg();
    if (n <= 0) return {};
    std::vector<uint8_t> out(static_cast<size_t>(n));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(out.data()), n)) return {};
    return out;
  }
  // The part's paint for an encode. That's the open canvas's snapshot, or else what's on disk (a recovery first).
  PaintBytes paintBytes(const Target& t) {
    auto it = g_paint.find(keyOf(t));
    if (it != g_paint.end() && it->second.ready) {
      PaintPart& p = it->second;
      if (p.canvas.empty()) return nullptr;
      if (p.snap_stale || !p.snap) { p.snap = std::make_shared<const std::vector<uint8_t>>(p.canvas.save()); p.snap_stale = false; }
      return p.snap;
    }
    auto st = g_paint_restore.find(keyOf(t));
    std::error_code ec;
    const std::filesystem::path f = st != g_paint_restore.end() ? st->second : paintFile(t);
    if (!std::filesystem::exists(f, ec)) return nullptr;
    auto bytes = std::make_shared<std::vector<uint8_t>>(readBytes(f));
    return bytes->empty() ? nullptr : PaintBytes(bytes);
  }
  bool hasPaint(const Target& t) {
    auto it = g_paint.find(keyOf(t));
    if (it != g_paint.end() && it->second.ready) return !it->second.canvas.empty();
    std::error_code ec;
    return g_paint_restore.count(keyOf(t)) || std::filesystem::exists(paintFile(t), ec);
  }
  void markUnsaved(const Target& t);
  PaintPart* paintFor(const Target& t, int w, int h) {  // the shown part's canvas, sized to its texture and read from disk once
    PaintPart& p = g_paint[keyOf(t)];
    if (p.ready) return p.canvas.width() == w && p.canvas.height() == h ? &p : nullptr;
    if (w <= 0 || h <= 0 || !p.canvas.reset(w, h)) return nullptr;
    p.ready = true;
    auto st = g_paint_restore.find(keyOf(t));
    std::error_code ec;
    bool loaded = false;
    if (st != g_paint_restore.end()) {  // a recovered copy first (unsaved changes)
      if (std::filesystem::exists(st->second, ec) && p.canvas.load(readBytes(st->second), w, h)) { loaded = true; markUnsaved(t); }
      else COMBO_LOG("color editor: " + keyOf(t) + "'s recovered paint couldn't be read: the saved one instead");
      g_paint_restore.erase(st);
    }
    const std::filesystem::path f = paintFile(t);
    if (!loaded && std::filesystem::exists(f, ec) && !p.canvas.load(readBytes(f), w, h)) {  // Another size or damaged, so set it aside and allow saving
      std::filesystem::rename(f, withSuffix(f, ".old"), ec);
      g_status = "Saved paint couldn't load; kept a backup.";
      COMBO_LOG("color editor: " + keyOf(t) + "'s paint couldn't be read: set aside");
    }
    p.snap_stale = true;
    return &p;
  }
  bool savePaintTo(const Target& t, const std::filesystem::path& to) {  // If empty, the file is removed
    auto it = g_paint.find(keyOf(t));
    if (it == g_paint.end() || !it->second.ready) return false;
    std::error_code ec;
    std::lock_guard<std::mutex> lk(g_files_m);
    if (it->second.canvas.empty()) { std::filesystem::remove(to, ec); return true; }
    std::filesystem::create_directories(to.parent_path(), ec);
    const auto bytes = it->second.canvas.save();
    const auto tmp = withSuffix(to, ".new");
    bool ok;
    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      f.flush();
      ok = f.good();
    }
    if (ok) std::filesystem::rename(tmp, to, ec);
    if (!ok || ec) { std::filesystem::remove(tmp, ec); return false; }
    return true;
  }
  void readVecLine(const std::string& l, std::map<std::string, std::array<float, 4>>& to) {  // "vec <label>\t<r g b a>" (other lines are skipped)
    const size_t tab = l.find('\t');
    if (l.rfind("vec ", 0) != 0 || tab == std::string::npos) return;
    std::array<float, 4> c{};
    std::istringstream v(l.substr(tab + 1));
    if (v >> c[0] >> c[1] >> c[2] >> c[3]) to[l.substr(4, tab - 4)] = c;
  }
  std::string g_aside_note;  // Said once the part is ready: its region colors were set aside
  bool loadEdits(const Target& t, std::vector<Edit>& ed, int regions, bool may_set_aside = true, const std::filesystem::path& from = {}, bool in_memory = false,
                 std::map<std::string, std::array<float, 4>>* vecs = nullptr) {  // vecs is where the material colors go (null for the editor's)
    const bool may_move = may_set_aside || !from.empty() || in_memory;  // The menus only read, so nothing is moved or written
    if (!from.empty()) may_set_aside = false;  // A recovery copy is never set aside here
    std::vector<std::string> moved;  // With in_memory, it's moved here and the file is left as it is
    if (auto mp = g_maps.find(mapKey(t)); mp != g_maps.end() && may_move)  // Made on a kept old map, so move it onto this one first
      moveOntoMap(from.empty() ? editsFile(t) : from, t.chr, t.kind, *mp->second, false, in_memory ? &moved : nullptr);
    std::ifstream file(from.empty() ? editsFile(t) : from);
    std::istringstream mem([&] { std::string all; for (const std::string& l : moved) all += l + "\n"; return all; }());
    std::istream& f = moved.empty() ? static_cast<std::istream&>(file) : mem;
    std::string line;
    if (!f || !std::getline(f, line)) return false;
    if (line != mapStamp(t)) {  // Made for another region map, so keep it aside instead of loading it onto the wrong parts
      // Its material colors go by name, not region, so they're read and kept (a recovery copy's aren't)
      std::vector<std::string> kept;
      bool regions_in = false;
      if (from.empty())
        for (std::string l; std::getline(f, l);) {
          if (!l.empty() && l.back() == '\r') l.pop_back();
          if (l.rfind("vec ", 0) == 0) { readVecLine(l, vecs ? *vecs : g_vec_saved[keyOf(t)]); kept.push_back(l); }
          regions_in = regions_in || (!l.empty() && std::isdigit(static_cast<unsigned char>(l[0])));
        }
      if (!may_set_aside) return false;  // The menus only read
      file.close();
      if (!regions_in && !kept.empty()) {  // Material colors only (no regions to place), so it's simply moved onto this map
        saveEditsTo(t, {}, editsFile(t));
        COMBO_LOG("color editor: " + keyOf(t) + "'s material colors moved onto map " + mapStamp(t));
        return false;
      }
      std::error_code ec;
      std::filesystem::path aside = withSuffix(editsFile(t), ".old");  // An older one set aside is never replaced
      for (int n = 2; std::filesystem::exists(aside, ec) && n < 100; n++) aside = withSuffix(editsFile(t), (".old" + std::to_string(n)).c_str());
      std::filesystem::rename(editsFile(t), aside, ec);
      if (!ec && !kept.empty()) saveEditsTo(t, {}, editsFile(t));  // Its material colors on this map, the region colors in the .old
      COMBO_LOG("color editor: " + keyOf(t) + "'s save was made on another region map: its region colors set aside (.old)" +
                (kept.empty() ? std::string() : ", its " + std::to_string(kept.size()) + " material colors kept"));
      g_aside_note = partName(t.kind) + "'s region colors were made on another region map, so they couldn't be placed (kept as a .old file)" +
                     (kept.empty() ? std::string(".") : ". Its material colors are on.");
      return false;
    }
    ed.assign(static_cast<size_t>(regions), Edit{});
    int r = 0;
    Edit e;
    while (f >> r >> e.has_lit >> e.lit >> e.has_shadow >> e.shadow >> e.derive)
      if (r >= 0 && r < regions) ed[static_cast<size_t>(r)] = e;
    f.clear();  // then the material colors
    std::string minors, blobs;
    while (std::getline(f, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.rfind("minors ", 0) == 0) minors = line;
      if (line.rfind("blobs ", 0) == 0) blobs = line;
      readVecLine(line, vecs ? *vecs : g_vec_saved[keyOf(t)]);
    }
    if (auto mp = g_maps.find(mapKey(t)); mp != g_maps.end()) {  // (other small regions or strokes: theirs dropped)
      const size_t from = minors != minorsLine(*mp->second) ? static_cast<size_t>(mp->second->major)
                          : blobs != blobsLine(*mp->second) ? static_cast<size_t>(mp->second->pieces) : ed.size();
      for (size_t r = from; r < ed.size(); r++) ed[r] = Edit{};
    }
    return true;
  }
  // ---- Default names from the community's texture templates, converted to layouts/<CHR>_<KIND>.png + .txt (a
  // part index per texel, the part names, and "@ x y name" text labels written onto parts). A region gets the
  // part covering at least half of it, or else the name of a text label on it. -------------------
  std::map<std::string, std::vector<std::string>> g_tpl_names;  // Keyed by map key ("" for a region with no name)
  const std::vector<std::string>& templateNames(const Target& t) {
    auto it = g_tpl_names.find(mapKey(t));
    if (it != g_tpl_names.end()) return it->second;
    std::vector<std::string>& out = g_tpl_names[mapKey(t)];
    auto mp = g_maps.find(mapKey(t));
    if (mp == g_maps.end()) { g_tpl_names.erase(mapKey(t)); static const std::vector<std::string> none; return none; }  // Asked again once mapped
    const CharMap& m = *mp->second;
    out.assign(static_cast<size_t>(m.regions), std::string());
    const auto dir = g_dir / "layouts";
    std::ifstream f(dir / (t.chr + "_" + t.kind + ".txt"));
    if (!f) return out;
    std::vector<std::string> names;
    std::vector<std::tuple<int, int, std::string>> points;
    std::string line;
    while (std::getline(f, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.rfind("@ ", 0) == 0) {
        std::istringstream ls(line.substr(2));
        int x = 0, y = 0;
        std::string name;
        if (ls >> x >> y && std::getline(ls >> std::ws, name)) points.emplace_back(x, y, name);
      } else {
        names.push_back(line);
      }
    }
    Image lab;
    const bool have_mask = decodePng(dir / (t.chr + "_" + t.kind + ".png"), lab) && lab.w > 0 && lab.h > 0;
    if (have_mask) {  // each region's most common part, if it covers half of it
      std::vector<std::map<int, int>> votes(static_cast<size_t>(m.regions));
      std::vector<int> total(static_cast<size_t>(m.regions), 0);
      for (int y = 0; y < m.h; y += 2)
        for (int x = 0; x < m.w; x += 2) {
          const uint16_t r = m.owner[static_cast<size_t>(y) * m.w + x];
          if (r == 0xFFFF) continue;
          const int lx = x * lab.w / m.w, ly = y * lab.h / m.h;
          const int part = lab.px[(static_cast<size_t>(ly) * lab.w + lx) * 4];
          total[r]++;
          if (part > 0) votes[r][part]++;
        }
      for (int r = 0; r < m.regions; r++) {
        int best = 0, n = 0;
        for (const auto& [part, c] : votes[static_cast<size_t>(r)]) if (c > n) { n = c; best = part; }
        if (best > 0 && n * 2 >= total[static_cast<size_t>(r)] && best <= static_cast<int>(names.size())) out[static_cast<size_t>(r)] = names[static_cast<size_t>(best - 1)];
      }
    }
    const int tw = have_mask ? lab.w : m.w, th = have_mask ? lab.h : m.h;  // Labels are in the template's pixels
    for (const auto& [px, py, name] : points) {  // a text label names the region under it (the most common one nearby)
      std::map<int, int> around;
      const int cx = px * m.w / (std::max)(tw, 1), cy = py * m.h / (std::max)(th, 1);
      for (int dy = -6; dy <= 6; dy += 2)
        for (int dx = -6; dx <= 6; dx += 2) {
          const int x = cx + dx, y = cy + dy;
          if (x < 0 || y < 0 || x >= m.w || y >= m.h) continue;
          const uint16_t r = m.owner[static_cast<size_t>(y) * m.w + x];
          if (r != 0xFFFF) around[r]++;
        }
      int best = -1, n = 0;
      for (const auto& [r, c] : around) if (c > n) { n = c; best = r; }
      if (best >= 0 && out[static_cast<size_t>(best)].empty()) out[static_cast<size_t>(best)] = name;
    }
    int named = 0;
    for (const auto& n : out) named += !n.empty();
    COMBO_LOG("color editor: template names for " + mapKey(t) + ": " + std::to_string(named) + " of " + std::to_string(m.regions) + " regions");
    return out;
  }

  // The user's names for regions, per character and part (every color shares the regions), to remember what each
  // one is. Guarded by the region map's stamp, like the edits.
  std::map<std::string, std::vector<std::string>> g_names;  // Keyed by map key
  std::filesystem::path namesFile(const Target& t) {
    const std::string own = ownOfKey(mapKey(t));
    return g_dir / "saved" / t.chr / ("names_" + t.kind + (own.empty() ? std::string() : "_" + own) + ".txt");
  }
  std::vector<std::string>& namesOf(const Target& t, int regions) {
    auto it = g_names.find(mapKey(t));
    if (it == g_names.end()) {
      it = g_names.emplace(mapKey(t), std::vector<std::string>(static_cast<size_t>(regions))).first;
      if (!ownOfKey(mapKey(t)).empty()) {  // A slot's own map starts from the shared names, moved onto it below
        std::lock_guard<std::mutex> lk(g_files_m);
        std::error_code ec;
        const auto shared = g_dir / "saved" / t.chr / ("names_" + t.kind + ".txt");
        if (!std::filesystem::exists(namesFile(t), ec) && std::filesystem::exists(shared, ec)) std::filesystem::copy_file(shared, namesFile(t), ec);
      }
      if (auto mp = g_maps.find(mapKey(t)); mp != g_maps.end()) moveOntoMap(namesFile(t), t.chr, t.kind, *mp->second, true);  // Same as the edits
      std::ifstream f(namesFile(t));
      std::string line, minors, blobs;
      if (f && std::getline(f, line) && line == mapStamp(t))
        while (std::getline(f, line)) {
          if (!line.empty() && line.back() == '\r') line.pop_back();
          if (line.rfind("minors ", 0) == 0) { minors = line; continue; }
          if (line.rfind("blobs ", 0) == 0) { blobs = line; continue; }
          const size_t tab = line.find('\t');
          if (tab == std::string::npos || line.rfind("colors ", 0) == 0) continue;
          const int r = std::atoi(line.substr(0, tab).c_str());
          if (r >= 0 && r < regions) it->second[static_cast<size_t>(r)] = line.substr(tab + 1);
        }
      if (auto mp = g_maps.find(mapKey(t)); mp != g_maps.end()) {
        const size_t from = minors != minorsLine(*mp->second) ? static_cast<size_t>(mp->second->major)
                            : blobs != blobsLine(*mp->second) ? static_cast<size_t>(mp->second->pieces) : it->second.size();
        for (size_t r = from; r < it->second.size(); r++) it->second[r].clear();
      }
    }
    if (static_cast<int>(it->second.size()) < regions) it->second.resize(static_cast<size_t>(regions));
    return it->second;
  }
  void saveNames(const Target& t, const std::vector<std::string>& names) {
    std::lock_guard<std::mutex> lk(g_files_m);
    std::error_code ec;
    std::filesystem::create_directories(namesFile(t).parent_path(), ec);
    std::ofstream f(namesFile(t), std::ios::trunc);
    f << mapStamp(t) << "\n";
    for (size_t r = 0; r < names.size(); r++) if (!names[r].empty()) f << r << '\t' << names[r] << "\n";
    if (auto mp = g_maps.find(mapKey(t)); mp != g_maps.end())
      f << colorsLine(*mp->second) << "\n" << minorsLine(*mp->second) << "\n" << blobsLine(*mp->second) << "\n";
  }

  // ---- Recolors. What's saved for a slot is files in saved/<CHR>: <ColorNN>_<part>.txt (region and material
  // colors), <ColorNN>_effects.txt (effect colors) and names_<part>.txt (region names, every slot). A recolor file
  // bundles them, copying to a slot copies them, and saved ones go on by themselves. -----
  std::vector<std::string> g_slots_cache, g_recolor_files_cache;
  std::vector<std::string> g_auto_paused;  // what was left when the editor opened: carried on when it closes
  std::filesystem::path savedDir(const std::string& chr) { return g_dir / "saved" / chr; }
  std::filesystem::path recolorDir() { return g_dir / "recolors"; }
  std::vector<std::string> slotsOf(const std::string& chr) {  // The colors read for the character (from the cache)
    std::set<std::string> out;
    std::error_code ec;
    for (const auto& e : listDir(g_dir / "cache" / chr)) {
      const std::string n = e.path().filename().string();
      if (n.size() > 7 && n.compare(0, 5, "Color") == 0 && n.find("_BaseMap.png") != std::string::npos) out.insert(n.substr(0, 7));
    }
    return {out.begin(), out.end()};
  }
  // A slot's own saved files (not names, not set-aside .old ones), as "<suffix>" -> path.
  std::map<std::string, std::filesystem::path> slotFiles(const std::string& chr, const std::string& color) {
    std::map<std::string, std::filesystem::path> out;
    std::error_code ec;
    for (const auto& e : listDir(savedDir(chr))) {
      const std::string n = e.path().filename().string();
      if (n.size() > color.size() + 5 && n.compare(0, color.size() + 1, color + "_") == 0 && (e.path().extension() == ".txt" || e.path().extension() == ".slpn"))
        out[n.substr(color.size() + 1)] = e.path();
    }
    return out;
  }
  void saveSlotNow(const std::string& chr, const std::string& color) {  // Writes what's in memory for the slot to disk now
    for (const Target& o : g_targets)
      if (o.chr == chr && o.color == color && g_edits_loaded.count(keyOf(o))) saveEdits(o, g_edits[keyOf(o)]);
    if (g_fx_for == chr + "/" + color) fxSave();
  }
  // Reads the slot's saved recolor again and puts it on (after an import onto it).
  std::filesystem::path capRecoveryFile(const std::string& slot);
  void dropUndo(const std::string& slot);  // Defined below, with the undo steps
  std::map<std::string, uint64_t> g_slot_gen;  // "chr/color" -> how many times its recolor changed (reloadSlot)
  std::set<std::string> g_refresh_wait;        // Slots whose other parts are to be made again (when nothing else runs)
  void autoQueue(const std::string& key);  // Defined with the queue
  void reloadSlot(const std::string& chr, const std::string& color) {
    const std::string pre = chr + "/" + color + "/";  // Every part of the slot, on stage or not, since what's in memory for one not shown is old too
    for (const Target& o : g_targets)
      if (o.chr == chr && o.color == color) {
        restoreVecs(keyOf(o));
        if (g_vecs_for == keyOf(o)) g_vecs_for.clear();
        if (g_tex_job.valid() && g_tex_key == keyOf(o)) g_png_orphans.push_back(std::move(g_tex_job));  // Textures from the old edits aren't put on
      }
    auto drop = [&](auto& by_key) { for (auto it = by_key.begin(); it != by_key.end();) it = it->first.rfind(pre, 0) == 0 ? by_key.erase(it) : std::next(it); };
    drop(g_edits);
    drop(g_vec_saved);
    drop(g_paint);  // The paint is read from disk again too, since a copy to this slot or an import put new paint there
    drop(g_paint_restore);
    drop(g_edit_restore);
    for (auto it = g_edits_loaded.begin(); it != g_edits_loaded.end();) it = it->rfind(pre, 0) == 0 ? g_edits_loaded.erase(it) : std::next(it);
    dropUndo(chr + "/" + color);  // Drop the steps of the recolor that was there
    auto slot = g_fx_slots.find(chr + "/" + color);
    if (slot != g_fx_slots.end()) {
      for (FxColor& f : slot->second.list) if (fxChanged(f)) { f.edited = false; f.per.clear(); fxApply(f); }
      if (slot->second.held && slot->second.held->GetObjectItem()) slot->second.held->GetObjectItem()->UnsetRootSet();
      g_fx_slots.erase(slot);
    }
    g_fx_saved.erase(chr + "/" + color);
    if (g_fx_for == chr + "/" + color) g_fx_for.clear();
    if (g_cap_slot == chr + "/" + color) { capRestore(); g_cap_slot.clear(); g_cap_unsaved = false; }  // Its captured colors are read again too
    {  // And none staged or recovered for it, since they'd go over what's on disk now
      std::error_code ec;
      g_cap_restore.erase(chr + "/" + color);
      std::filesystem::remove(capRecoveryFile(chr + "/" + color), ec);
    }
    bool refresh = false;
    {  // Its other parts get the game's own textures, not the old recolor. Their saves are made again below
      const Target* shown = target();
      std::error_code ec;
      if (!g_open && shown && shown->chr == chr && shown->color == color) autoQueue(keyOf(*shown));  // (first, so it's made again before the others)
      for (Target& o : g_targets) {
        if (o.chr != chr || o.color != color || (shown && keyOf(o) == keyOf(*shown))) continue;
        bool alive = o.items.size() >= o.mids.size();  // A fight just ended and its meshes may be gone, so don't touch them
        for (size_t i = 0; alive && i < o.mids.size(); i++) alive = Kept{o.mids[i], o.items[i]}.alive();
        if (!alive) continue;
        show(o, Shown{});
        auto sh = g_shown.find(keyOf(o));
        if (sh != g_shown.end()) { unroot(sh->second); g_shown.erase(sh); }
        g_smear_stale = true;  // Effect copies of the old textures are looked up again
        if (std::filesystem::exists(editsFile(o), ec) || std::filesystem::exists(paintFile(o), ec)) {
          if (!g_open) { autoQueue(keyOf(o)); continue; }  // Closed (a palette's look finished after it closed): the saved recolors' queue puts it on
          if (std::find(g_auto_paused.begin(), g_auto_paused.end(), keyOf(o)) == g_auto_paused.end()) g_auto_paused.push_back(keyOf(o));
          refresh = true;  // Queued already or not, make it again now
        }
      }
    }
    g_slot_gen[chr + "/" + color]++;  // Textures made for its old recolor won't be put on
    if (refresh) g_refresh_wait.insert(chr + "/" + color);  // Made now in the background, one part after another
    g_dirty = true;
    g_last_change = Clock::now() - std::chrono::seconds(1);
  }
  std::string nowStamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char b[32];
    std::strftime(b, sizeof(b), "%Y%m%d-%H%M%S", &tm);
    return b;
  }
  // A recolor file: "STPL 1", "character\tSOL", "from\tColor08", then each saved file as
  // "file\t<name>" ... "end" (names like MAIN.txt, effects.txt, names_MAIN.txt).
  void saveAllNow();
  bool g_save_failed = false;  // The last Save couldn't write everything (what failed stays unsaved)
  std::string base64(const std::vector<uint8_t>& in) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
      const uint32_t n = (uint32_t(in[i]) << 16) | (i + 1 < in.size() ? uint32_t(in[i + 1]) << 8 : 0) | (i + 2 < in.size() ? in[i + 2] : 0);
      out += k[n >> 18 & 63];
      out += k[n >> 12 & 63];
      out += i + 1 < in.size() ? k[n >> 6 & 63] : '=';
      out += i + 2 < in.size() ? k[n & 63] : '=';
    }
    return out;
  }
  std::vector<uint8_t> unbase64(const std::string& in) {  // Anything not base64 ends it, so a damaged file reads short and then fails to load
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
      int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
      if (v < 0) break;
      acc = (acc << 6) | static_cast<uint32_t>(v);
      bits += 6;
      if (bits >= 8) { bits -= 8; out.push_back(static_cast<uint8_t>(acc >> bits & 0xFF)); }
    }
    return out;
  }
  // Writes the shown slot's saved recolor into one file (meta is extra header lines, "name\t..." etc.).
  // Returns "" when done, else the reason it couldn't.
  std::string writeRecolor(const Target& t, const std::filesystem::path& path, const std::string& meta, bool backup = false) {
    saveAllNow();  // What's written is what's saved, so the changes are saved first
    if (g_save_failed && !backup) return "Couldn't save the changes first (a file in use?): not written.";  // (a backup keeps what's on disk anyway)
    const auto files = slotFiles(t.chr, t.color);
    if (files.empty()) return "Nothing saved for " + t.chr + " " + t.color + " yet.";
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(withSuffix(path, ".new"), std::ios::trunc);
    out << "STPL 2\ncharacter\t" << t.chr << "\nfrom\t" << t.color << "\n" << meta;
    auto put = [&](const std::string& as, const std::filesystem::path& p) {  // content lines start with "|"
      out << "file\t" << as << "\n";
      if (p.extension() == ".slpn") {  // Paint is binary, so it's written as base64 lines
        const std::string b = base64(readBytes(p));
        for (size_t i = 0; i < b.size(); i += 96) out << "|" << b.substr(i, 96) << "\n";
      } else {
        std::ifstream in(p);
        std::string line;
        while (std::getline(in, line)) out << "|" << line << "\n";
      }
      out << "end\n";
    };
    std::set<std::string> parts;
    for (const auto& [suffix, p] : files) {
      put(suffix, p);
      if (suffix != "effects.txt" && suffix != "captured.txt" && p.extension() == ".txt") parts.insert(suffix);
    }
    for (const std::string& part : parts) {
      Target x = t;
      x.kind = part.substr(0, part.size() - 4);
      const std::string kind = x.kind, own = ownOfKey(mapKey(x));
      const auto mine = savedDir(t.chr) / ("names_" + kind + (own.empty() ? std::string() : "_" + own) + ".txt");
      if (std::filesystem::exists(mine, ec)) put("names_" + part, mine);
      else if (std::filesystem::exists(savedDir(t.chr) / ("names_" + part), ec)) put("names_" + part, savedDir(t.chr) / ("names_" + part));
    }
    out.flush();
    const bool ok = out.good();
    out.close();
    if (!ok) { std::filesystem::remove(withSuffix(path, ".new"), ec); return "Couldn't write " + narrow(path.filename().wstring()) + "."; }
    std::filesystem::rename(withSuffix(path, ".new"), path, ec);  // (whole, or not at all)
    return ec ? "Couldn't write " + narrow(path.filename().wstring()) + "." : std::string();
  }
  void lookSaveStart(const Target& t, const std::filesystem::path& palette);
  std::string exportRecolorFile(const Target& t) {
    const std::string name = t.chr + "_" + t.color + "_" + nowStamp() + ".stpl";
    const std::string err = writeRecolor(t, recolorDir() / name, "");
    if (!err.empty()) return err;
    lookSaveStart(t, recolorDir() / name);  // Its look is kept in it too, like a palette's
    COMBO_LOG("color editor: exported " + name);
    return "Saved and exported " + name + ".";
  }
  // ---- palettes: named recolors per character (recolors/palettes/<CHR>/*.stpl, with name/author/note lines) ----
  std::filesystem::path paletteDir(const std::string& chr) { return recolorDir() / "palettes" / chr; }
  std::filesystem::path trashFile(const std::string& chr, const std::string& name) {  // palettes/<CHR>/.trash/<when>_<name>, a new one each time
    const auto dir = paletteDir(chr) / ".trash";
    std::error_code ec;
    std::filesystem::path p = dir / (nowStamp() + "_" + name);
    for (int n = 2; std::filesystem::exists(p, ec) && n < 1000; n++) p = dir / (nowStamp() + "_" + std::to_string(n) + "_" + name);
    return p;
  }
  std::string oneLine(std::string v) {  // Header values get no tabs or line breaks and are kept short
    for (char& c : v) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    if (v.size() > 300) v.resize(300);
    return v;
  }
  std::string paletteMeta(const std::string& name, const std::string& author, const std::string& note) {
    return "name\t" + oneLine(name) + "\nauthor\t" + oneLine(author) + "\nnote\t" + oneLine(note) + "\n";
  }
  bool paletteName(const std::string& file) {  // (a plain file name in the folder)
    return !file.empty() && file.find("..") == std::string::npos && file.find_first_of("/\\:") == std::string::npos &&
           file.size() > 5 && file.compare(file.size() - 5, 5, ".stpl") == 0;
  }
  std::vector<View::Palette> g_palettes_cache;
  std::string g_palette_author;
  std::filesystem::path authorFile() { return g_dir / "palette_author.txt"; }
  void listPalettes(const std::string& chr) {
    g_palettes_cache.clear();
    for (const auto& e : listDir(paletteDir(chr))) {
      if (e.path().extension() != ".stpl") continue;
      View::Palette pv;
      pv.file = narrow(e.path().filename().wstring());
      std::ifstream in(e.path());
      std::string line;
      for (int n = 0; n < 12 && std::getline(in, line); n++) {  // The header is everything before the first "file" line
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("file\t", 0) == 0) break;
        if (line.rfind("name\t", 0) == 0) pv.name = line.substr(5);
        else if (line.rfind("author\t", 0) == 0) pv.author = line.substr(7);
        else if (line.rfind("note\t", 0) == 0) pv.note = line.substr(5);
        else if (line.rfind("from\t", 0) == 0) pv.from = line.substr(5);
      }
      if (pv.name.empty()) pv.name = pv.file.substr(0, pv.file.size() - 5);
      std::error_code ec;
      const auto tm = std::filesystem::last_write_time(e.path(), ec);
      pv.when = ec ? std::string() : std::to_string(tm.time_since_epoch().count());  // For sorting, newest first
      g_palettes_cache.push_back(pv);
    }
    std::sort(g_palettes_cache.begin(), g_palettes_cache.end(), [](const View::Palette& a, const View::Palette& b) {
      return a.when.size() != b.when.size() ? a.when.size() > b.when.size() : a.when > b.when;
    });
  }
  std::string fileForName(const std::string& chr, const std::string& name) {  // A new palette's file name, made safe from its name
    std::string base;
    for (char c : name) base += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : c == ' ' ? '_' : '\0';
    base.erase(std::remove(base.begin(), base.end(), '\0'), base.end());
    if (base.empty()) base = "palette";
    if (base.size() > 60) base.resize(60);
    std::error_code ec;
    std::string file = base + ".stpl";
    for (int n = 2; std::filesystem::exists(paletteDir(chr) / file, ec) && n < 1000; n++) file = base + "_" + std::to_string(n) + ".stpl";
    return file;
  }
  void splitMeta(const std::string& text, std::string& file, std::string& name, std::string& author, std::string& note) {  // Fields are separated by \x1f
    std::vector<std::string> f(4);
    size_t i = 0;
    for (char c : text) { if (c == '\x1f') { if (i < 3) i++; } else f[i] += c; }
    file = f[0]; name = f[1]; author = f[2]; note = f[3];
  }
  std::string importRecolorPath(const Target& t, const std::filesystem::path& path, bool looks = false);
  std::string importRecolorFile(const Target& t, const std::string& file) {
    if (file.find("..") != std::string::npos || file.find_first_of("/\\") != std::string::npos) return "Not a recolor file.";
    return importRecolorPath(t, recolorDir() / file, true);
  }
  std::set<std::string> g_hold;  // Slots ("chr/color") saved in a fight. Files are new, but the fight keeps the old look until it ends
  struct LookIn { std::string kind, ids; std::shared_ptr<const std::vector<uint8_t>> data; std::string stamp; };  // a palette's kept look of one part (stamp: its edits' map)
  std::vector<std::pair<std::string, LookIn>> g_look_wait;  // ("chr/color", look) pairs waiting for their part to be read again
  void dropLookWait(const std::string& slot) {  // The slot's recolor was replaced, so an older palette's looks don't go on over it
    g_look_wait.erase(std::remove_if(g_look_wait.begin(), g_look_wait.end(), [&](const auto& w) { return w.first == slot; }), g_look_wait.end());
  }
  void lookApplyStart(const Target& t, const std::vector<LookIn>& looks);
  std::vector<LookIn> looksOf(const std::map<std::string, std::vector<std::string>>& blocks);
  // With looks set, the palette's kept looks go on where this slot's textures aren't the ones it was made on.
  std::string importRecolorPath(const Target& t, const std::filesystem::path& path, bool looks) {
    std::ifstream in(path);
    std::string line;
    if (!in || !std::getline(in, line) || (line != "STPL 1" && line != "STPL 2")) return "Not a recolor file.";
    const bool marked = line == "STPL 2";  // In "STPL 2" content lines start with "|". In 1 they're bare
    std::string chr;
    std::map<std::string, std::vector<std::string>> blocks;
    std::string cur;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (cur.empty()) {
        if (line.rfind("character\t", 0) == 0) chr = line.substr(10);
        else if (line.rfind("file\t", 0) == 0) cur = line.substr(5);
      } else if (line == "end") cur.clear();
      else if (!marked) blocks[cur].push_back(line);
      else if (!line.empty() && line[0] == '|') blocks[cur].push_back(line.substr(1));
    }
    if (chr != t.chr) return "That recolor is for " + (chr.empty() ? std::string("?") : chr) + ", not " + t.chr + ".";
    dropLookWait(t.chr + "/" + t.color);  // The slot's recolor was replaced, so an older palette's waiting looks don't go on
    const std::string file = narrow(path.filename().wstring());
    if (blocks.empty()) return file + " has nothing in it to import.";
    std::error_code ec;
    std::filesystem::create_directories(savedDir(t.chr), ec);
    auto valid = [](const std::string& name) {  // Plain .txt names and paint, in the saved folder only
      const bool painted = name.size() > 11 && name.compare(name.size() - 11, 11, "_paint.slpn") == 0;
      return name.size() >= 5 && name.find_first_of("/\\:") == std::string::npos && name.find("..") == std::string::npos &&
             (name.compare(name.size() - 4, 4, ".txt") == 0 || painted);
    };
    if (std::none_of(blocks.begin(), blocks.end(), [&](const auto& kv) { return valid(kv.first) && kv.first.rfind("names_", 0) != 0; }))
      return file + " has nothing in it to import.";
    std::unique_lock<std::mutex> lk(g_files_m);  // Held until the files are written
    for (const auto& [suffix, p] : slotFiles(t.chr, t.color))  // The recolor replaces the slot's, so parts it doesn't have aren't left on
      if (!blocks.count(suffix)) std::filesystem::rename(p, withSuffix(p, ".before_import"), ec);
    for (const auto& [name, lines] : blocks) {
      const bool painted = name.size() > 11 && name.compare(name.size() - 11, 11, "_paint.slpn") == 0;
      if (!valid(name)) continue;
      const bool names = name.rfind("names_", 0) == 0;
      const std::filesystem::path to = savedDir(t.chr) / (names ? name : t.color + "_" + name);
      if (names && std::filesystem::exists(to, ec)) continue;  // (your own region names stay)
      if (painted) {  // Base64 back to binary, with the slot's old paint kept beside it
        std::string all;
        for (const std::string& l : lines) all += l;
        const std::vector<uint8_t> bytes = unbase64(all);
        if (std::filesystem::exists(to, ec)) std::filesystem::copy_file(to, withSuffix(to, ".before_import"), std::filesystem::copy_options::overwrite_existing, ec);
        bool ok;
        {
          std::ofstream f(withSuffix(to, ".new"), std::ios::binary | std::ios::trunc);
          f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
          f.flush();
          ok = f.good();
        }
        if (ok) std::filesystem::rename(withSuffix(to, ".new"), to, ec);
        else std::filesystem::remove(withSuffix(to, ".new"), ec);
        g_paint.erase(keyOf(t).substr(0, keyOf(t).rfind('/')) + "/" + name.substr(0, name.size() - 11));  // (read again)
        continue;
      }
      {
        std::ofstream f(withSuffix(to, ".new"), std::ios::trunc);
        for (const std::string& l : lines) f << l << "\n";
        f.flush();
        const bool ok = f.good();
        f.close();
        if (ok) std::filesystem::rename(withSuffix(to, ".new"), to, ec);
        else std::filesystem::remove(withSuffix(to, ".new"), ec);
      }
      if (names) g_names.clear();
    }
    lk.unlock();
    const std::vector<LookIn> looks_in = looks ? looksOf(blocks) : std::vector<LookIn>();
    std::string unplaced;  // Parts made on another region map that can't be moved onto this one, with no look to remake them from
    for (const auto& [name, lines] : blocks) {
      if (name.size() <= 4 || name.compare(name.size() - 4, 4, ".txt") != 0 || name.rfind("names_", 0) == 0 || name == "effects.txt" || name == "captured.txt" ||
          lines.empty() || std::none_of(lines.begin(), lines.end(), [](const std::string& l) { return !l.empty() && std::isdigit(static_cast<unsigned char>(l[0])); }))
        continue;
      auto xo = std::find_if(g_targets.begin(), g_targets.end(), [&](const Target& o) { return o.chr == t.chr && o.color == t.color && o.kind == name.substr(0, name.size() - 4); });
      if (xo == g_targets.end()) continue;  // (not on stage: said when it's read)
      const Target& x = *xo;
      auto mp = g_maps.find(mapKey(x));
      if (mp == g_maps.end() || lines[0] == stampOf(*mp->second)) continue;  // (a map not read yet says so when it is)
      if (std::any_of(looks_in.begin(), looks_in.end(), [&](const LookIn& l) { return l.kind == x.kind && !l.ids.empty(); })) continue;
      std::vector<std::string> moved;
      if (moveOntoMap(editsFile(x), x.chr, x.kind, *mp->second, false, &moved)) continue;
      unplaced += (unplaced.empty() ? "" : ", ") + partName(x.kind);
    }
    if (!g_hold.count(t.chr + "/" + t.color)) reloadSlot(t.chr, t.color);  // If held, it's read again once the fight's over
    if (looks && !g_hold.count(t.chr + "/" + t.color)) lookApplyStart(t, looks_in);
    COMBO_LOG("color editor: imported " + file + " onto " + t.chr + " " + t.color + (unplaced.empty() ? std::string() : " (made on another region map: " + unplaced + ")"));
    return "Imported " + file + " onto " + t.color + "." +
           (unplaced.empty() ? std::string() : " " + unplaced + ": made on another region map, so only paint and material colors came in.");
  }
  std::string copyToSlotNow(const Target& t, const std::string& slot) {
    if (slot == t.color || slot.size() != 7 || slot.compare(0, 5, "Color") != 0 || !std::isdigit(static_cast<unsigned char>(slot[5])) ||
        !std::isdigit(static_cast<unsigned char>(slot[6])))
      return "Pick another slot.";
    saveAllNow();  // What's copied is what's saved, so the changes are saved first
    if (g_save_failed) return "Couldn't save the changes first (a file in use?): not copied.";
    const auto files = slotFiles(t.chr, t.color);
    if (files.empty()) return "Nothing saved for " + t.color + " yet.";
    dropLookWait(t.chr + "/" + slot);
    {  // The slot's recolor it replaces is kept, as a load does
      Target to = t;
      to.color = slot;
      (void)writeRecolor(to, trashFile(t.chr, slot + "_before_copy.stpl"), paletteMeta(slot + " before a copy from " + t.color, "", ""), true);
    }
    std::error_code ec;
    std::unique_lock<std::mutex> lk(g_files_m);
    for (const auto& [suffix, p] : files) {  // Copy beside first, so a failed copy leaves the slot's old recolor as it was
      std::filesystem::copy_file(p, savedDir(t.chr) / (slot + "_" + suffix + ".new"), std::filesystem::copy_options::overwrite_existing, ec);
      if (ec) {
        for (const auto& [sx, px] : files) std::filesystem::remove(savedDir(t.chr) / (slot + "_" + sx + ".new"), ec);
        return "Couldn't copy to " + slot + ".";
      }
    }
    for (const auto& [suffix, p] : slotFiles(t.chr, slot)) std::filesystem::remove(p, ec);  // Replaces the slot's old recolor
    for (const auto& [suffix, p] : files)
      std::filesystem::rename(savedDir(t.chr) / (slot + "_" + suffix + ".new"), savedDir(t.chr) / (slot + "_" + suffix), ec);
    lk.unlock();
    reloadSlot(t.chr, slot);  // On stage or not, what's in memory for it is the old recolor
    COMBO_LOG("color editor: " + t.chr + " " + t.color + "'s recolor copied to " + slot);
    return t.color + "'s recolor is on " + slot + " now (it goes on whenever " + t.chr + " is in " + slot + ").";
  }

  // ---- Recovery. Unsaved changes are copied to saved/.recovery/<CHR> (the same files as the real saves) a
  // moment after each change, so a fight ending or the game closing doesn't lose them. They're offered back on
  // opening. Restored files are staged and read, as unsaved changes, when their part is next read. ----------
  std::set<std::string> g_recovery_offered;                     // characters asked about this session
  std::string g_offer_recovery;                                 // the one being asked about
  bool g_recovery_due = false;
  Clock::time_point g_recovery_change;
  struct Target;
  Target fromKey(const std::string& k);
  Target targetOf(const std::string& k);
  std::filesystem::path recoveryDir(const std::string& chr) { return g_dir / "saved" / ".recovery" / chr; }
  std::filesystem::path capRecoveryFile(const std::string& slot) { return recoveryDir(chrOf(slot)) / (colorOf(slot) + "_captured.txt"); }
  std::map<std::string, std::filesystem::path>& capRestorePaths() { return g_cap_restore; }
  bool hasRecovery(const std::string& chr) {
    std::error_code ec;
    return std::filesystem::exists(recoveryDir(chr), ec) && !std::filesystem::is_empty(recoveryDir(chr), ec);
  }

  // ---- Saving is by hand. Changes are marked, Save writes them, and closing with any unsaved asks first ------
  std::set<std::string> g_unsaved;  // target keys ("SOL/Color08/MAIN") with changes not on disk
  std::set<std::string> g_unsaved_slots;  // color slots ("SOL/Color08") whose effect colors changed
  bool& capUnsavedFlag() { return g_cap_unsaved; }
  bool g_confirm_close = false;
  void markUnsaved(const Target& t) { g_unsaved.insert(keyOf(t)); g_recovery_due = true; g_recovery_change = Clock::now(); }
  void writeRecovery() {  // Copies the unsaved changes aside (not the parts still waiting to be read from a recovery)
    std::set<std::string> slots(g_unsaved_slots.begin(), g_unsaved_slots.end());
    for (const std::string& k : g_unsaved) {
      const Target o = targetOf(k);
      if (o.chr.empty()) continue;
      slots.insert(o.chr + "/" + o.color);
      if (!g_paint_restore.count(k) && g_paint.count(k) && g_paint[k].ready)
        savePaintTo(o, recoveryDir(o.chr) / (o.color + "_" + o.kind + "_paint.slpn"));
      if (g_edit_restore.count(k) || !g_maps.count(mapKey(o)) || !g_edits_loaded.count(k)) continue;  // Skip edits that were never read
      saveEditsTo(o, g_edits[k], recoveryDir(o.chr) / (o.color + "_" + o.kind + ".txt"));
    }
    for (const std::string& s : slots) {
      if (g_fx_restore.count(s) || !g_fx_saved.count(s)) continue;
      const size_t slash = s.find('/');
      std::error_code ec;
      std::filesystem::create_directories(recoveryDir(s.substr(0, slash)), ec);
      std::ofstream f(recoveryDir(s.substr(0, slash)) / (s.substr(slash + 1) + "_effects.txt"), std::ios::trunc);
      for (const auto& [label, c] : g_fx_saved[s]) f << label << '\t' << c[0] << ' ' << c[1] << ' ' << c[2] << "\n";
    }
    if (g_cap_unsaved && !g_cap_slot.empty() && !g_cap_restore.count(g_cap_slot)) {
      std::error_code ec;
      std::filesystem::create_directories(recoveryDir(chrOf(g_cap_slot)), ec);
      std::ofstream f(capRecoveryFile(g_cap_slot), std::ios::trunc);
      for (const auto& [k, c] : g_cap_saved) f << k << '\t' << c[0] << ' ' << c[1] << ' ' << c[2] << "\n";
    }
  }
  // Clears the recovery of the characters just saved or discarded (others' stay, to be offered).
  void clearRecovery(const std::set<std::string>& chrs) {
    std::lock_guard<std::mutex> lk(g_files_m);  // (the map worker may be in there)
    std::error_code ec;
    for (const std::string& c : chrs) std::filesystem::remove_all(recoveryDir(c), ec);
    for (auto it = g_edit_restore.begin(); it != g_edit_restore.end();) it = chrs.count(fromKey(it->first).chr) ? g_edit_restore.erase(it) : std::next(it);
    for (auto it = g_fx_restore.begin(); it != g_fx_restore.end();) it = chrs.count(it->first.substr(0, it->first.find('/'))) ? g_fx_restore.erase(it) : std::next(it);
    for (auto it = g_cap_restore.begin(); it != g_cap_restore.end();) it = chrs.count(chrOf(it->first)) ? g_cap_restore.erase(it) : std::next(it);
    for (auto it = g_paint_restore.begin(); it != g_paint_restore.end();) it = chrs.count(fromKey(it->first).chr) ? g_paint_restore.erase(it) : std::next(it);
  }
  std::set<std::string> unsavedCharacters() {  // everyone with something unsaved or staged
    std::set<std::string> out;
    for (const auto& k : g_unsaved) out.insert(fromKey(k).chr);
    for (const auto& sl : g_unsaved_slots) out.insert(sl.substr(0, sl.find('/')));
    for (const auto& [k, p] : g_edit_restore) out.insert(fromKey(k).chr);
    for (const auto& [k, p] : g_paint_restore) out.insert(fromKey(k).chr);
    for (const auto& [sl, p] : g_fx_restore) out.insert(sl.substr(0, sl.find('/')));
    for (const auto& [sl, p] : g_cap_restore) out.insert(chrOf(sl));
    if (g_cap_unsaved && !g_cap_slot.empty()) out.insert(chrOf(g_cap_slot));
    out.erase(std::string());
    return out;
  }
  void capStage() {  // Puts g_cap_slot's unsaved captured colors aside (as a recovery is), then lets go
    if (g_cap_unsaved && !g_cap_slot.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(recoveryDir(chrOf(g_cap_slot)), ec);
      {
        std::ofstream f(capRecoveryFile(g_cap_slot), std::ios::trunc);
        for (const auto& [k, c] : g_cap_saved) f << k << '\t' << c[0] << ' ' << c[1] << ' ' << c[2] << "\n";
      }
      g_cap_restore[g_cap_slot] = capRecoveryFile(g_cap_slot);
    }
    g_cap_unsaved = false;
    capRestore();
    g_cap_slot.clear();
  }
  void stageRecovery(const std::string& chr) {  // the kept files, read as unsaved changes when their parts are next read
    std::error_code ec;
    for (const auto& e : listDir(recoveryDir(chr))) {
      const std::string n = e.path().filename().string();
      if (n == "effects_shared.txt") {  // An older version's file for every slot. Not put on any, but set aside beside the saves
        std::filesystem::create_directories(savedDir(chr), ec);
        std::filesystem::rename(e.path(), savedDir(chr) / "effects_shared_recovered.txt.old", ec);
        continue;
      }
      if (n.size() == 20 && n.compare(0, 5, "Color") == 0 && n.compare(7, 13, "_captured.txt") == 0) {
        const std::string slot = chr + "/" + n.substr(0, 7);
        g_cap_restore[slot] = e.path();
        if (g_cap_slot == slot) { capRestore(); g_cap_slot.clear(); }  // (read again, from the recovery)
        continue;
      }
      if (n.size() > 19 && n.compare(0, 5, "Color") == 0 && n.compare(n.size() - 11, 11, "_paint.slpn") == 0) {  // a part's paint
        const std::string k = chr + "/" + n.substr(0, 7) + "/" + n.substr(8, n.size() - 8 - 11);
        g_paint_restore[k] = e.path();
        g_paint.erase(k);  // (read again from the recovery)
        g_unsaved.insert(k);
        continue;
      }
      if (n.size() < 13 || n.compare(0, 5, "Color") != 0 || e.path().extension() != ".txt") continue;
      const std::string color = n.substr(0, 7), rest = n.substr(8, n.size() - 12);
      if (rest == "effects") {
        const std::string slot = chr + "/" + color;
        g_fx_restore[slot] = e.path();
        auto it = g_fx_slots.find(slot);
        if (it != g_fx_slots.end()) {
          for (FxColor& f : it->second.list) if (fxChanged(f)) { f.edited = false; f.per.clear(); fxApply(f); }
          if (it->second.held && it->second.held->GetObjectItem()) it->second.held->GetObjectItem()->UnsetRootSet();
          g_fx_slots.erase(it);
        }
        g_fx_saved.erase(slot);
        if (g_fx_for == slot) g_fx_for.clear();
        g_unsaved_slots.insert(slot);  // (so Save writes the slot's effect colors)
        continue;
      }
      const std::string k = chr + "/" + color + "/" + rest;
      g_edit_restore[k] = e.path();
      g_edits_loaded.erase(k);
      g_edits.erase(k);
      restoreVecs(k);
      g_vec_saved.erase(k);
      if (g_vecs_for == k) g_vecs_for.clear();
      if (g_tex_job.valid() && g_tex_key == k) g_png_orphans.push_back(std::move(g_tex_job));
      g_unsaved.insert(k);
    }
    g_dirty = true;
  }
  Target targetOf(const std::string& k) {  // The live target if it's here, else one from fromKey
    for (const Target& o : g_targets) if (keyOf(o) == k) return o;
    return fromKey(k);
  }
  Target fromKey(const std::string& k) {  // "SOL/Color08/MAIN" -> a target to save or load by (no materials)
    Target t;
    const size_t a = k.find('/'), b = a == std::string::npos ? a : k.find('/', a + 1);
    if (b != std::string::npos) { t.chr = k.substr(0, a); t.color = k.substr(a + 1, b - a - 1); t.kind = k.substr(b + 1); }
    return t;
  }
  void saveAllNow() {
    std::set<std::string> slots(g_unsaved_slots.begin(), g_unsaved_slots.end());
    std::set<std::string> chrs = unsavedCharacters(), kept;  // kept holds characters with a staged copy that couldn't be put in place
    std::error_code ec;
    for (const std::string& k : g_unsaved) {
      const Target o = targetOf(k);
      if (o.chr.empty()) continue;
      slots.insert(o.chr + "/" + o.color);
      auto st = g_edit_restore.find(k);
      if (st != g_edit_restore.end()) {  // Not read yet, so its recovered file is the save
        std::filesystem::create_directories(editsFile(o).parent_path(), ec);
        std::lock_guard<std::mutex> lk(g_files_m);
        std::filesystem::copy_file(st->second, editsFile(o), std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) kept.insert(o.chr);
        continue;
      }
      if (g_maps.count(mapKey(o)) && g_edits_loaded.count(k) && !saveEdits(o, g_edits[k])) kept.insert(o.chr);  // Skip edits that were never read
    }
    for (const std::string& k : g_unsaved) {  // The paint, from the canvas or a recovered copy not read yet
      const Target o = fromKey(k);
      if (o.chr.empty()) continue;
      auto st = g_paint_restore.find(k);
      if (st != g_paint_restore.end()) {
        std::lock_guard<std::mutex> lk(g_files_m);
        std::filesystem::create_directories(paintFile(o).parent_path(), ec);
        std::filesystem::copy_file(st->second, paintFile(o), std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) kept.insert(o.chr);
        continue;
      }
      auto pp = g_paint.find(k);
      if (pp != g_paint.end() && pp->second.ready && !savePaintTo(o, paintFile(o))) kept.insert(o.chr);
    }
    for (const std::string& s : slots) {
      auto st = g_fx_restore.find(s);
      const size_t slash = s.find('/');
      if (st != g_fx_restore.end()) {
        std::filesystem::create_directories(fxFile(s.substr(0, slash), s.substr(slash + 1)).parent_path(), ec);
        std::filesystem::copy_file(st->second, fxFile(s.substr(0, slash), s.substr(slash + 1)), std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) kept.insert(s.substr(0, slash));
      } else if (g_fx_saved.count(s) && !fxSaveSlot(s)) {
        kept.insert(s.substr(0, slash));
      }
    }
    for (const auto& [slot, path] : g_cap_restore) {
      std::error_code ec2;
      if (std::filesystem::file_size(path, ec2) == 0 && !ec2) { std::filesystem::remove(capFile(slot), ec2); continue; }  // Nothing in it, so no file
      std::filesystem::create_directories(capFile(slot).parent_path(), ec2);
      std::filesystem::copy_file(path, capFile(slot), std::filesystem::copy_options::overwrite_existing, ec);
      if (ec) kept.insert(chrOf(slot));
    }
    const bool cap_ok = !g_cap_unsaved || capSave();
    if (!cap_ok) kept.insert(chrOf(g_cap_slot));
    for (const std::string& c : kept) chrs.erase(c);  // Its recovery stays and is offered again
    clearRecovery(chrs);
    for (auto it = g_unsaved_slots.begin(); it != g_unsaved_slots.end();) it = kept.count(chrOf(*it)) ? std::next(it) : g_unsaved_slots.erase(it);
    g_recovery_due = false;
    for (auto it = g_unsaved.begin(); it != g_unsaved.end();) it = kept.count(fromKey(*it).chr) ? std::next(it) : g_unsaved.erase(it);  // Not saved, so still unsaved
    g_cap_unsaved = !cap_ok;
    g_save_failed = !kept.empty();
  }
  void discardAllNow() {  // back to what's on disk
    const std::set<std::string> chrs = unsavedCharacters();  // Taken before anything is let go. Their recovery is cleared below
    std::set<std::pair<std::string, std::string>> slots;
    for (const std::string& k : g_unsaved) {
      const Target o = fromKey(k);
      if (o.chr.empty()) continue;
      slots.insert({o.chr, o.color});
      g_edits.erase(k);  // Parts not in this fight are read from disk again when they're back
      g_edits_loaded.erase(k);
      g_vec_saved.erase(k);
      g_paint.erase(k);  // The paint is read again from disk
      g_paint_restore.erase(k);
    }
    for (const auto& [chr, color] : slots) { g_fx_saved.erase(chr + "/" + color); reloadSlot(chr, color); }
    if (g_cap_unsaved) { capRestore(); g_cap_slot.clear(); }  // Read again from disk on the next update
    for (const std::string& sl : g_unsaved_slots) {  // Slots with only effect colors changed are read again
      const size_t slash = sl.find('/');
      reloadSlot(sl.substr(0, slash), sl.substr(slash + 1));
    }
    g_unsaved.clear();
    g_unsaved_slots.clear();
    g_cap_unsaved = false;
    clearRecovery(chrs);
    g_recovery_due = false;
  }
  // Undo keeps the shown part's region colors, material colors and its slot's effect colors from before each change.
  struct Snap { std::string key; std::vector<Edit> ed; std::map<std::string, std::array<float, 4>> vec; std::map<std::string, std::array<float, 3>> fx;
                int paint = 0; bool with_ed = false; };  // paint marks a painting step, undone by the part's canvas as this many of its own steps (with_ed: and ed swapped back with it, an import's)
  std::map<std::string, std::vector<Snap>> g_undo, g_redo;  // Keyed by color slot ("SOL/Color08"), covering every part of it and its effect colors
  std::string slotOf(const Target& t) { return t.chr + "/" + t.color; }
  int g_snap_kind = -1, g_snap_index = -1;
  void dropUndo(const std::string& slot) { g_undo.erase(slot); g_redo.erase(slot); g_snap_kind = -1; }
  Clock::time_point g_snap_at;
  Snap snapOf(const Target& t) { return {keyOf(t), g_edits[keyOf(t)], g_vec_saved[keyOf(t)], g_fx_saved[t.chr + "/" + t.color]}; }
  void pushUndo(const Target& t, int kind, int index) {  // (a drag on one color is one step)
    const bool same_drag = kind == g_snap_kind && index == g_snap_index && Clock::now() - g_snap_at < std::chrono::milliseconds(1500) &&
                           (kind == kVec || kind == kFx || kind == kFxOne);  // Those send every frame of a drag. A region's color comes once, on release
    g_snap_kind = kind;
    g_snap_index = index;
    g_snap_at = Clock::now();
    if (same_drag) return;
    auto& u = g_undo[slotOf(t)];
    u.push_back(snapOf(t));
    if (u.size() > 100) u.erase(u.begin());
    g_redo[slotOf(t)].clear();
  }
  void applySnap(const Target& shown, const Snap& s) {  // The step's own part, which may not be the one shown
    const Target t = fromKey(s.key);
    const std::string k = s.key, slot = slotOf(t);
    g_edits[k] = s.ed;
    g_vec_saved[k] = s.vec;
    if (g_vecs_for == k)
      for (VecGroup& g : g_vecs) {
        auto it = s.vec.find(g.label);
        g.edited = it != s.vec.end();
        for (size_t i = 0; i < g.mids.size(); i++) {
          if (g.edited) vecSet(g.mids[i], g.paramOf(i), it->second.data());
          else vecRestore(g.mids[i], g.paramOf(i));
        }
        if (g.edited) std::memcpy(g.cur, it->second.data(), sizeof(g.cur));
        else if (!g.mids.empty() && vecOrig(g.mids.front(), g.param)) std::memcpy(g.cur, vecOrig(g.mids.front(), g.param), sizeof(g.cur));
      }
    g_fx_saved[slot] = s.fx;
    if (g_fx_for == slot)
      for (FxColor& f : fxList()) {
        auto it = s.fx.find(f.label);
        f.edited = it != s.fx.end();
        if (f.edited) std::memcpy(f.rgb, it->second.data(), sizeof(f.rgb));
        f.per.clear();
        for (const auto& [k, c] : s.fx)
          if (k.size() > f.label.size() + 1 && k.compare(0, f.label.size() + 1, f.label + "@") == 0) f.per[k.substr(f.label.size() + 1)] = c;
        fxApply(f);
      }
    if (k == keyOf(shown)) { g_dirty = true; g_last_change = Clock::now() - std::chrono::seconds(1); }
    else if (g_worn_key == k) g_worn_key.clear();  // Another part is made again when it's shown
    markUnsaved(fromKey(k));
    g_snap_kind = -1;
  }

  // Saved recolors go on by themselves, editor open or not. Once per fight, each part with something saved goes
  // through the whole pipeline (colors read, map, worn color, edits), one part after another.
  bool g_auto_done = false;
  std::vector<std::string> g_auto_queue;  // targets by key (indices change when the targets are found again)
  std::string g_auto_was;                 // the part shown before the queue (shown again after it)
  int g_auto_updates = 0;                 // Updates since the front part started. It's given up on after a while (its colors failed)
  int targetIndex(const std::string& key) {
    for (int i = 0; i < static_cast<int>(g_targets.size()); i++) if (keyOf(g_targets[static_cast<size_t>(i)]) == key) return i;
    return -1;
  }
  void autoNext() {  // the next queued part that's still here, or the shown part back
    while (!g_auto_queue.empty() && targetIndex(g_auto_queue.front()) < 0) g_auto_queue.erase(g_auto_queue.begin());
    if (g_auto_queue.empty()) {
      const int was = targetIndex(g_auto_was);
      if (was >= 0) g_target = was;
      return;
    }
    g_target = targetIndex(g_auto_queue.front());
    g_dirty = true;
    g_last_change = Clock::now() - std::chrono::seconds(1);
    g_auto_updates = 0;
  }
  void autoQueue(const std::string& key) {  // With the editor closed, a part whose saved recolor goes on next (after any queued)
    if (std::find(g_auto_queue.begin(), g_auto_queue.end(), key) != g_auto_queue.end()) return;
    const bool idle = g_auto_queue.empty();
    if (idle) g_auto_was = target() ? keyOf(*target()) : std::string();
    g_auto_queue.push_back(key);
    if (idle) autoNext();
  }
  void autoStop() {  // The editor opened, so stop switching parts under it
    if (g_auto_queue.empty()) return;
    g_auto_paused = g_auto_queue;
    g_auto_queue.clear();
    const int was = targetIndex(g_auto_was);
    if (was >= 0) g_target = was;
  }

  // ---- Menus (character select ...) show saved recolors on the preview models too. They're made from the cached
  // colors on disk (read in training before), so the region map and the worn color need no fight. --------
  bool isVisible(UObject* comp);
  struct MenuGroup {
    Target t;
    std::future<std::shared_ptr<Worn>> job;
    std::shared_ptr<Worn> worn;
    Shown shown;
    std::vector<Edit> ed;
    std::map<std::string, std::array<float, 4>> vecs;  // its saved material colors, as read for the menus
    std::future<Pngs> tex;
    bool tried = false, failed = false;
    bool vecs_only = false;  // Only material colors saved, so no textures are made
  };
  std::map<std::string, MenuGroup> g_menu;  // by target key
  std::map<std::string, std::pair<std::vector<UObject*>, std::vector<Kept>>> g_menu_copies;  // A match's part: its materials then, and their live copies found
  // A part's saved material colors for the menus and sharing. That's the editor's while it has the part, else
  // what was read from disk.
  const std::map<std::string, std::array<float, 4>>* menuVecs(const std::string& key) {
    if (g_edits_loaded.count(key)) { auto v = g_vec_saved.find(key); return v == g_vec_saved.end() ? nullptr : &v->second; }
    auto g = g_menu.find(key);
    return g == g_menu.end() ? nullptr : &g->second.vecs;
  }
  // ---- recolor sharing: pictures are sent as raw RGBA, LZMS packed (Windows' Compression API). Raw, not PNG,
  // so the opponent's bytes never reach a picture decoder here. We make the PNG ourselves (encodePng).
  // Under Wine and Proton (Linux, Steam Deck) the Compression API is a stub that crashes the game when called, so
  // there it's never called: pictures are kept unpacked (a packed size equal to the picture's means unpacked, and
  // Windows reads those too), and LZMS made on Windows can't be unpacked there (a failure, not a crash).
  bool underWine() {
    static const bool wine = GetModuleHandleW(L"ntdll.dll") && GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version");
    return wine;
  }
  std::vector<uint8_t> lzmsPack(const std::vector<uint8_t>& in) {
    if (underWine()) return in;
    COMPRESSOR_HANDLE h = nullptr;
    if (in.empty() || !CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &h)) return {};
    struct Close { COMPRESSOR_HANDLE h; ~Close() { CloseCompressor(h); } } close{h};  // (closed whatever happens below)
    SIZE_T need = 0;
    Compress(h, in.data(), in.size(), nullptr, 0, &need);  // Ask for the size it needs
    std::vector<uint8_t> out(need);
    if (!need || !Compress(h, in.data(), in.size(), out.data(), out.size(), &need)) out.clear();
    else out.resize(need);
    if (!out.empty() && out.size() >= in.size()) return in;  // (no smaller: kept unpacked, so a packed size is never the picture's)
    return out;
  }
  std::vector<uint8_t> lzmsUnpack(const uint8_t* in, size_t n, size_t raw) {  // Worker. Gives exactly raw bytes, or none
    if (n && n == raw) return std::vector<uint8_t>(in, in + n);  // Kept unpacked (made under Wine)
    if (underWine()) return {};
    DECOMPRESSOR_HANDLE h = nullptr;
    if (!n || !raw || !CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &h)) return {};
    struct Close { DECOMPRESSOR_HANDLE h; ~Close() { CloseDecompressor(h); } } close{h};
    std::vector<uint8_t> out(raw);
    SIZE_T got = 0;
    if (!Decompress(h, in, n, out.data(), out.size(), &got) || got != raw) out.clear();
    return out;
  }
  // Turns our own PNG (encodePng's layout, one IDAT of stored deflate blocks with filter 0) back into its pixels.
  bool storedPngPixels(const std::vector<uint8_t>& png, Image& im) {
    auto be = [&](size_t at) { return (uint32_t(png[at]) << 24) | (uint32_t(png[at + 1]) << 16) | (uint32_t(png[at + 2]) << 8) | png[at + 3]; };
    if (png.size() < 33 + 12 || std::memcmp(png.data() + 12, "IHDR", 4) != 0 || std::memcmp(png.data() + 37, "IDAT", 4) != 0) return false;
    im.w = static_cast<int>(be(16));
    im.h = static_cast<int>(be(20));
    const size_t len = be(33);
    if (im.w <= 0 || im.h <= 0 || im.w > 8192 || im.h > 8192 || 41 + len > png.size() || len < 2) return false;
    const size_t row = static_cast<size_t>(im.w) * 4;
    im.px.clear();
    im.px.reserve(row * im.h);
    size_t at = 41 + 2, end = 41 + len, col = 0;  // Skip the zlib header. col is the place in the row (0 = the filter byte)
    bool last = false;
    while (!last && at + 5 <= end) {
      last = png[at] & 1;
      if ((png[at] >> 1) & 3) return false;  // Not stored, so not ours
      const size_t n = png[at + 1] | (png[at + 2] << 8);
      at += 5;
      if (at + n > end) return false;
      for (size_t i = 0; i < n; i++) {
        if (col == 0) { if (png[at + i] != 0) return false; }
        else im.px.push_back(png[at + i]);
        col = col == row ? 0 : col + 1;
      }
      at += n;
    }
    return last && im.px.size() == row * im.h;
  }
  struct Packed { bool ok = false; int w[2] = {}, h[2] = {}; std::vector<uint8_t> z[2]; };  // base, shadow
  Packed packPngs(std::shared_ptr<const Pngs> p) try {  // Worker. A failure is a not-ok result, never a throw
    Packed k;
    if (underWine()) return k;  // Unpacked, the pictures are too big to send (and the other side drops them), so no sharing from there
    Image im[2];
    if (!storedPngPixels(p->base, im[0]) || !storedPngPixels(p->sss, im[1])) return k;
    for (int i = 0; i < 2; i++) { k.w[i] = im[i].w; k.h[i] = im[i].h; k.z[i] = lzmsPack(im[i].px); if (k.z[i].empty()) return k; }
    k.ok = true;
    return k;
  } catch (...) {
    return Packed{};
  }

  // Recolored textures made for a slot, kept (rooted) while its saves stay the same. Character select makes them
  // and the fight that follows takes them as they are (building a texture stalls the game thread, not for mid-round).
  struct TexKept {  // packed is for sharing, vecs is the part's saved material colors as read with the textures
    std::filesystem::file_time_type edits, paint;
    bool follow = false;
    Shown shown;
    std::shared_future<Packed> packed;
    std::map<std::string, std::array<float, 4>> vecs;
  };
  std::map<std::string, TexKept> g_menu_tex;
  bool menuTexKept(const Shown& s) {
    return s.base && std::any_of(g_menu_tex.begin(), g_menu_tex.end(), [&](const auto& kv) { return kv.second.shown.base == s.base; });
  }
  std::vector<std::future<std::shared_ptr<Worn>>> g_menu_orphans;  // Jobs of groups that are gone, dropped once done (a future waits when destroyed)
  std::vector<std::future<std::shared_ptr<CharMap>>> g_map_orphans;  // Region maps no menu waits for now, dropped once done
  void dropPngOrphans();
  void menuDropOrphans() {
    dropPngOrphans();
    g_map_orphans.erase(std::remove_if(g_map_orphans.begin(), g_map_orphans.end(),
                                       [](auto& f) { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }),
                        g_map_orphans.end());
    g_menu_orphans.erase(std::remove_if(g_menu_orphans.begin(), g_menu_orphans.end(),
                                        [](auto& f) { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }),
                         g_menu_orphans.end());
  }
  void menuForget(MenuGroup& g) {
    if (!menuTexKept(g.shown)) unroot(g.shown);  // (kept ones stay for the next model of the slot)
    if (g.job.valid() && g.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) g_menu_orphans.push_back(std::move(g.job));
    if (g.tex.valid()) g_png_orphans.push_back(std::move(g.tex));
  }
  std::map<std::string, std::future<std::shared_ptr<CharMap>>> g_menu_maps;
  int g_menu_tick = 0;
  void menuRelease_() {
    for (auto& [k, g] : g_menu) menuForget(g);  // The materials aren't touched, they go with the menu
    g_menu.clear();
    g_menu_copies.clear();
    for (auto& [k, f] : g_menu_maps) if (f.valid()) g_map_orphans.push_back(std::move(f));  // (made again when a menu needs one)
    g_menu_maps.clear();
    menuDropOrphans();
  }
  // An object that's whole: not being loaded, not being destroyed, not a template, and in a level. The
  // FUObjectItem flags in 4.25 are AsyncLoading 1 << 27, Unreachable 1 << 28 and PendingKill 1 << 29.
  bool whole(UObject* o) {
    if (!o || !o->GetObjectItem()) return false;
    const int32_t fl = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(o->GetObjectItem()) + 8);
    if (fl & ((1 << 27) | (1 << 28) | (1 << 29))) return false;
    const auto name = o->GetFullName();
    return name.find(STR("Default__")) == StringType::npos && name.find(STR("PersistentLevel")) != StringType::npos;
  }
  // The opponent's recolor (game thread). Kept after the fight so it can be saved, but its textures are let go then.
  struct RxPart { std::string kind; std::map<std::string, std::array<float, 4>> vecs; std::future<Pngs> job; Shown shown; bool failed = false; };
  struct Rx { std::string chr, color, stpl; std::vector<RxPart> parts; bool live = false; } g_rx;  // live means it's this fight's
  std::string g_my_chr, g_my_color;  // our fighter this fight (from shareNoteSides)
  std::mutex g_share_m;              // what the panel reads and asks (any thread)
  std::string g_share_info, g_share_status, g_share_save_slot;
  // The fight's live copies of a part's materials that are on no mesh: of its color materials, and of its other
  // materials taking the recolor (a glow, an outline) if they're this slot's own. A smear's "sol_wep" and
  // Ramlethal's sword glow are drawn from them. taken(m) says m is a part's already.
  template <class Taken> std::vector<UObject*> poolCopies(const Target& t, const std::vector<UObject*>& pool, const std::set<UObject*>& on_meshes, Taken taken) {
    std::vector<UObject*> out;
    if (!t.src.empty() || t.mids.empty()) return out;  // Another color's material. Its copies may be another fighter's
    std::set<UObject*> mics;
    for (UObject* mid : t.mids) {
      std::string n, k;
      if (UObject* mic = colorSource(mid, n, k); mic && mic != mid) mics.insert(mic);
      else if (UObject* p = parentOf(mid); p && isMatInstance(p)) {
        std::string c2, s2;
        if (slotOfPath(narrow(p->GetFullName()), c2, s2) && c2 == t.chr && s2 == t.color) mics.insert(p);
      }
    }
    for (UObject* m : pool) {
      if (!mics.count(parentOf(m)) || m->GetOuterPrivate() != t.mids.front()->GetOuterPrivate()) continue;  // (this fight's pool only)
      if (on_meshes.count(m) || taken(m)) continue;
      out.push_back(m);
    }
    return out;
  }
  // The materials a part's material color rows are made from, as the editor lists them: every material with a
  // color parameter on its meshes, less decals and what other(m) says is another part's.
  template <class Other> std::vector<UObject*> vecMats(const Target& t, Other other) {
    std::vector<UObject*> seen, mats;
    for (UObject* comp : t.comps)
      for (int i = 0, n = numMaterials(comp); i < n; i++) {
        UObject* m = material(comp, i);
        if (!isDynamic(m) || std::find(seen.begin(), seen.end(), m) != seen.end()) continue;
        seen.push_back(m);
        if (g_extra_read.count(t.chr + "/" + t.kind) && std::find(t.mids.begin(), t.mids.end(), m) == t.mids.end()) continue;  // (a part sharing another's meshes: its own only)
        if (!hasVector(m, "OutlineColor") && (isDecal(m) || getTexture(m, STR("Decal")))) continue;
        if (other(m)) continue;
        if (!std::any_of(std::begin(kVecParams), std::end(kVecParams), [&](const auto& pr) { return hasVector(m, pr.first); })) continue;
        mats.push_back(m);
      }
    return mats;
  }
  // A part outside the editor (menus, matches, the opponent's): its rows, kept per key while its materials stay.
  // parts is every part found there, so another part's own color material isn't listed with this one.
  const std::vector<VecGroup>& menuRows(const std::string& key, const Target& t, const std::map<std::string, Target>& parts) {
    std::vector<UObject*> sig = t.mids;  // Its materials, then the fighter's other parts' (which it mustn't list)
    for (const auto& kv : parts)
      if (kv.second.pawn == t.pawn && kv.second.kind != t.kind) sig.insert(sig.end(), kv.second.mids.begin(), kv.second.mids.end());
    auto& ro = g_menu_rows[key];
    if (ro.first != sig)
      ro = {sig, vecRows(vecMats(t, [&](UObject* m) {
              return std::any_of(parts.begin(), parts.end(), [&](const auto& kv) {
                return kv.second.pawn == t.pawn && kv.second.kind != t.kind && std::find(kv.second.mids.begin(), kv.second.mids.end(), m) != kv.second.mids.end();
              });
            }))};
    return ro.second;
  }
  void rxPutOn(const Target& t, const std::string& key, const std::map<std::string, Target>& parts) {  // Puts its texture and material colors on the opponent's fighter
    auto part = std::find_if(g_rx.parts.begin(), g_rx.parts.end(), [&](const RxPart& p) { return p.kind == t.kind; });
    if (part == g_rx.parts.end() || part->failed || t.mids.empty()) return;  // A part they haven't recolored keeps the game's
    if (!part->shown.base) {
      if (!part->job.valid() || part->job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
      Pngs png = part->job.get();
      if (png.base.empty() || !importPngs(t.mids.front(), png, part->shown)) { part->failed = true; COMBO_LOG("share: the opponent's " + key + " couldn't be made"); return; }
      COMBO_LOG("share: the opponent's recolor on their " + key);
    }
    for (UObject* mid : t.mids) {
      if (outlineOnly(mid)) {  // An outline takes the shadow texture only
        if (getTexture(mid, STR("SssMap")) != part->shown.sss) setTexture(mid, STR("SssMap"), part->shown.sss);
        continue;
      }
      if (getTexture(mid, STR("BaseMap")) != part->shown.base) setTexture(mid, STR("BaseMap"), part->shown.base);
      std::string n, k;
      UObject* mic = colorSource(mid, n, k);
      UObject* mic0 = colorSource(t.mids.front(), n, k);
      if ((mic ? micTexture(mic, "SssMap") : nullptr) == (mic0 ? micTexture(mic0, "SssMap") : nullptr) && getTexture(mid, STR("SssMap")) != part->shown.sss)
        setTexture(mid, STR("SssMap"), part->shown.sss);
    }
    if (!part->vecs.empty()) {
      for (const VecGroup& row : menuRows("rx/" + key, t, parts)) {
        auto c = part->vecs.find(row.label);
        if (c == part->vecs.end()) continue;
        for (size_t i = 0; i < row.mids.size(); i++) setVector(row.mids[i], wide(row.paramOf(i)).c_str(), c->second.data());
      }
    }
  }
  void shareSaveNow();
  void menuUpdateNow(bool in_fight) {
    shareSaveNow();  // A save asked for from the panel
    menuDropOrphans();
    if (asw_engine::get() && !in_fight) { menuRelease_(); return; }  // Training's fight belongs to the editor
    if (g_menu_tick++ % (in_fight ? 60 : 30)) return;  // Every half second or so, or every second in a fight since it looks through every mesh
    std::set<UObject*> fight;  // Any fight's own materials are never touched from here
    for (const Target& t : g_targets) fight.insert(t.mids.begin(), t.mids.end());
    std::map<std::string, Target> found;
    std::vector<UObject*> comps;
    UObjectGlobals::FindAllOf(STR("SkeletalMeshComponent"), comps);
    std::vector<Seen> seen;
    for (UObject* comp : comps) {
      if (!whole(comp) || !isVisible(comp)) continue;  // Hidden ones, like the Dragon Install outfit, aren't worth the work
      for (int i = 0, n = numMaterials(comp); i < n && i < 32; i++) {
        UObject* m = material(comp, i);
        std::string name, kind, chr, color;
        UObject* mic = isDynamic(m) && !fight.count(m) ? colorSource(m, name, kind) : nullptr;
        if (!mic || !slotOfPath(name, chr, color)) continue;
        seen.push_back({comp, m, mic, comp->GetOuterPrivate(), chr, color, kind});
      }
    }
    for (Target& t : groupParts(seen)) {  // One model per character there, so two of the same color share a group
      auto [f, added] = found.emplace(keyOf(t), t);
      if (!added) {
        for (UObject* m : t.mids) if (std::find(f->second.mids.begin(), f->second.mids.end(), m) == f->second.mids.end()) f->second.mids.push_back(m);
        for (UObject* c : t.comps) if (std::find(f->second.comps.begin(), f->second.comps.end(), c) == f->second.comps.end()) f->second.comps.push_back(c);
      }
    }
    {  // As the training editor finds them: the other materials on a part's meshes drawn from its texture, and in a
       // fight the pool's live copies (looked for again only when the part's materials change)
      std::set<UObject*> on_meshes;
      for (const Seen& x : seen) on_meshes.insert(x.m);
      auto taken = [&](UObject* m) {
        return fight.count(m) > 0 || std::any_of(found.begin(), found.end(), [&](const auto& kv) { return std::find(kv.second.mids.begin(), kv.second.mids.end(), m) != kv.second.mids.end(); });
      };
      for (auto& [key, t] : found) {
        std::set<UObject*> bases;
        for (UObject* mid : t.mids) {
          bases.insert(getTexture(mid, STR("BaseMap")));
          std::string n, k;
          if (UObject* mic = colorSource(mid, n, k)) bases.insert(micTexture(mic, "BaseMap"));
        }
        if (auto g = g_menu.find(key); g != g_menu.end()) bases.insert(g->second.shown.base);  // (ours may be on them now)
        if (auto k = g_menu_tex.find(key); k != g_menu_tex.end()) bases.insert(k->second.shown.base);
        bases.erase(nullptr);
        std::vector<UObject*> add;
        for (UObject* comp : t.comps)
          for (int i = 0, n = numMaterials(comp); i < n && i < 32; i++) {
            UObject* m = material(comp, i);
            if (m && isDynamic(m) && !taken(m) && std::find(add.begin(), add.end(), m) == add.end() && bases.count(getTexture(m, STR("BaseMap")))) add.push_back(m);
          }
        t.mids.insert(t.mids.end(), add.begin(), add.end());
      }
      for (auto it = g_menu_copies.begin(); it != g_menu_copies.end();) it = found.count(it->first) ? std::next(it) : g_menu_copies.erase(it);
      if (in_fight) {
        std::vector<UObject*> pool;
        for (auto& [key, t] : found) {
          auto& c = g_menu_copies[key];
          if (c.first != t.mids) {
            if (pool.empty()) UObjectGlobals::FindAllOf(STR("REDMaterialInstanceDynamic"), pool);
            c.first = t.mids;
            c.second.clear();
            for (UObject* m : poolCopies(t, pool, on_meshes, taken)) c.second.push_back(keep(m));
            if (!c.second.empty()) COMBO_LOG("color editor: the match's " + key + ": " + std::to_string(c.second.size()) + " live copies");
          }
          for (const Kept& k : c.second)
            if (k.alive() && !taken(k.obj)) t.mids.push_back(k.obj);
        }
      }
    }
    for (auto& [key, t] : found)  // Their outlines, once every part's own materials are known
      addOutlines(t, [&] {
        std::vector<UObject*> all;  // (the fighter's meshes: every part's)
        for (const auto& kv : found)
          if (kv.second.pawn == t.pawn) all.insert(all.end(), kv.second.comps.begin(), kv.second.comps.end());
        return all;
      }(), [&](UObject* m) {
        return fight.count(m) > 0 || std::any_of(found.begin(), found.end(), [&](const auto& kv) { return std::find(kv.second.mids.begin(), kv.second.mids.end(), m) != kv.second.mids.end(); });
      });
    for (auto it = g_menu.begin(); it != g_menu.end();)  // Models that are gone are let go
      if (!found.count(it->first)) { menuForget(it->second); it = g_menu.erase(it); } else ++it;
    menuDropOrphans();
    std::error_code ec;
    for (auto& [key, t] : found) {
      if (in_fight && g_rx.live && t.chr == g_rx.chr && t.color == g_rx.color && !(t.chr == g_my_chr && t.color == g_my_color)) {
        auto ours = g_menu.find(key);
        if (ours != g_menu.end() && std::none_of(g_rx.parts.begin(), g_rx.parts.end(), [&](const RxPart& p) { return p.kind == t.kind; })) {
          for (UObject* mid : t.mids) {  // Ours for their slot went on before theirs came, so a part they didn't recolor gets the game's own
            std::string n, k;
            if (outlineOnly(mid)) {  // (the shadow texture only, from its instances)
              setTexture(mid, STR("SssMap"), gameSss(mid));
            } else if (UObject* mic = colorSource(mid, n, k)) {
              if (UObject* b = micTexture(mic, "BaseMap")) setTexture(mid, STR("BaseMap"), b);
              if (UObject* sm = micTexture(mic, "SssMap")) setTexture(mid, STR("SssMap"), sm);
            }
          }
          menuForget(ours->second);
          g_menu.erase(ours);
        }
        rxPutOn(t, key, found);  // (the opponent's own recolor on their fighter, not ours for that slot)
        continue;
      }
      if (!std::filesystem::exists(editsFile(t), ec) && !std::filesystem::exists(paintFile(t), ec)) continue;  // (nothing saved for it)
      if (in_fight && g_hold.count(t.chr + "/" + t.color) && !g_menu.count(key)) continue;  // Saved mid-fight and not worn before, so it goes on from the next fight
      MenuGroup& g = g_menu[key];
      g.t.chr = t.chr; g.t.color = t.color; g.t.kind = t.kind; g.t.src = t.src;
      g.t.mids = t.mids;
      g.t.comps = t.comps;  // (its material colors are on every material of its meshes)
      g.t.pawn = t.pawn;
      if (!g.failed && !g.shown.base && texState(t) == 1) {  // Made on other textures, so not put on until it's read again in training
        g.failed = true;
        COMBO_LOG("color editor: " + key + "'s textures changed since they were read (a skin mod added or removed): its recolor waits for training");
        continue;
      }
      if (in_fight && !g_fx_slots.count(t.chr + "/" + t.color)) fxBuild(g.t);  // The slot's saved effect colors, put back with battleGone
      if (g.failed) continue;
      std::error_code tec;
      const auto e_time = std::filesystem::last_write_time(editsFile(t), tec);
      const auto p_time = std::filesystem::last_write_time(paintFile(t), tec);
      if (!g.shown.base) {
        auto kept = g_menu_tex.find(key);
        const bool held = in_fight && g_hold.count(t.chr + "/" + t.color);  // Saved mid-fight, so it keeps the look it had
        if (kept != g_menu_tex.end() && (held || (kept->second.edits == e_time && kept->second.paint == p_time && kept->second.follow == g_follow))) {
          g.shown = kept->second.shown;
          g.vecs = kept->second.vecs;  // Its material colors come with it, read when the textures were made
        }
        if (held && !g.shown.base) continue;  // Nothing kept to show, so the slot's new recolor waits for the next fight
      }
      std::shared_future<Packed> packed_now;
      if (!g.shown.base && !g.vecs_only) {  // Made from the map, the slot's colors and the edits, then the textures on a worker
        const std::string mk = mapKey(t);
        if (!g_maps.count(mk)) {  // the region map, from the cached colors
          auto job = g_menu_maps.find(mk);
          if (job == g_menu_maps.end()) {
            const auto prefixes = cachedPrefixes(t.chr, t.kind);
            if (prefixes.empty()) { g.failed = true; continue; }  // Never read in training, so there's nothing to make it from
            g_menu_maps[mk] = std::async(std::launch::async, mapAndMove, g_dir / "cache" / t.chr, t.kind, prefixes, ownOfKey(mk));
          } else if (job->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto m = job->second.get();
            g_menu_maps.erase(job);
            if (m && m->regions > 0) { g_maps[mk] = m; g_old_fits.clear(); } else g.failed = true;
            if (ownOfKey(mk).empty()) forgetOwn(t.chr, t.kind);  // The slots with their own map are read again
          }
          continue;
        }
        if (!g.tried) {  // What's saved, read once (before the slot's colors, which a part with only material colors doesn't need)
          g.tried = true;
          g.vecs.clear();  // Only what was read for the menus. The editor's unsaved ones stay
          loadEdits(g.t, g.ed, g_maps[mk]->regions, false, {}, false, &g.vecs);
          g.vecs_only = !anyEdit(g.ed) && !hasPaint(g.t);
          if ((g.vecs_only && g.vecs.empty()) || g.t.mids.empty()) { g.failed = true; continue; }
          continue;
        }
        if (!g.worn) {  // the slot's own colors
          if (!g.job.valid()) {
            const auto dir = g_dir / "cache" / t.chr;
            const std::string prefix = prefixOf(srcOf(t), t.kind);
            g.job = std::async(std::launch::async, prepareWorn, g_maps[mk], dir / (prefix + "_BaseMap.png"), dir / (prefix + "_SssMap.png"));
          } else if (g.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            g.worn = g.job.get();
            if (!g.worn) g.failed = true;
          }
          continue;
        }
        if (!g.tex.valid() && !g.shown.base && !g.failed) {  // One encode at a time. This group's turn comes when none is running
          if (g_encoding == 0) g.tex = startEncode(g.worn, g.ed, g_follow, paintBytes(g.t));
          continue;
        }
        if (g.tex.valid()) {
          if (g.tex.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
          Pngs png = g.tex.get();
          if (g.t.mids.empty() || !importPngs(g.t.mids.front(), png, g.shown)) g.failed = true;
          else packed_now = std::async(std::launch::async, packPngs, std::make_shared<const Pngs>(std::move(png))).share();  // For sharing, on a worker
          g.worn.reset();  // Made. The slot's colors were only needed for that
          COMBO_LOG("color editor: saved recolor on the menu's " + key + (g.failed ? " failed" : ""));
        }
        if (g.shown.base) {  // Kept for the next model of this slot, and an older one let go
          auto& k = g_menu_tex[key];
          if (k.shown.base && k.shown.base != g.shown.base) unroot(k.shown);
          k = {e_time, p_time, g_follow, g.shown, packed_now, g.vecs};
          if (g_menu_tex.size() > 8)  // A few slots' worth (about 2 big textures each). Ones no model shows now are let go
            for (auto it = g_menu_tex.begin(); it != g_menu_tex.end();)
              if (!g_menu.count(it->first)) { unroot(it->second.shown); it = g_menu_tex.erase(it); } else ++it;
        }
      }
      if (!g.shown.base && !g.vecs_only) continue;
      auto ownSss = [](UObject* mid) {  // The color material's, never ours
        std::string n, k;
        UObject* mic = colorSource(mid, n, k);
        return mic ? micTexture(mic, "SssMap") : nullptr;
      };
      UObject* sss0 = ownSss(g.t.mids.front());
      for (UObject* mid : g.shown.base ? g.t.mids : std::vector<UObject*>()) {  // Put on (again, if the game put its own back)
        if (outlineOnly(mid)) {  // An outline takes the shadow texture only
          if (getTexture(mid, STR("SssMap")) != g.shown.sss) setTexture(mid, STR("SssMap"), g.shown.sss);
          continue;
        }
        if (getTexture(mid, STR("BaseMap")) != g.shown.base) setTexture(mid, STR("BaseMap"), g.shown.base);
        if (ownSss(mid) == sss0 && getTexture(mid, STR("SssMap")) != g.shown.sss) setTexture(mid, STR("SssMap"), g.shown.sss);
      }
      const auto* saved_vecs = menuVecs(key);
      if (saved_vecs && !saved_vecs->empty()) {  // its material colors, the rows as the editor makes them (made again when its materials change)
        for (const VecGroup& row : menuRows(key, g.t, found)) {
          auto c = saved_vecs->find(row.label);
          if (c == saved_vecs->end()) continue;
          for (size_t i = 0; i < row.mids.size(); i++) setVector(row.mids[i], wide(row.paramOf(i)).c_str(), c->second.data());
        }
      }
    }
  }

  // ---- Export textures: every part of the shown slot, into a new dated folder (older exports are never touched) ----
  // export/<CHR>/<ColorNN>/<stamp>/ holds <Part>_Lit.png and <Part>_Shadow.png (alpha full, since some maps keep data
  // in alpha, Jam's skin is 0, and viewers show that see-through or black) and <Part>_material_colors.txt. alpha/ has
  // the exact game copies (PNG and TGA, alpha kept), and .exported/ has what Import compares an edited file with. One
  // part per worker job.
  // An uncompressed 32-bit TGA (BGRA, top-left first), which is what the modding tools take, with alpha kept exactly.
  std::vector<uint8_t> encodeTga(const Image& im) {
    std::vector<uint8_t> out(18, 0);
    out[2] = 2;  // uncompressed true color
    out[12] = static_cast<uint8_t>(im.w & 0xFF); out[13] = static_cast<uint8_t>(im.w >> 8);
    out[14] = static_cast<uint8_t>(im.h & 0xFF); out[15] = static_cast<uint8_t>(im.h >> 8);
    out[16] = 32;
    out[17] = 0x28;  // 8 alpha bits, rows from the top
    out.reserve(18 + im.px.size());
    for (size_t p = 0; p + 3 < im.px.size(); p += 4) {
      out.push_back(im.px[p + 2]); out.push_back(im.px[p + 1]); out.push_back(im.px[p]); out.push_back(im.px[p + 3]);
    }
    return out;
  }
  // The picture as exported (what Import compares an edited file with), out of the way of the user's files.
  std::filesystem::path exportedCopy(const std::filesystem::path& dir, const std::string& file) { return dir / ".exported" / file; }
  std::mutex g_tex_pick_m;               // Import textures' window: what was ticked, and where it opens (the slot's newest export)
  std::deque<std::filesystem::path> g_tex_pick;  // the files ticked, imported one by one (the editor's window adds, under the lock)
  int g_tex_added = 0;                   // files added since the game thread last looked (under the lock)
  // The rest is the game thread's:
  std::string g_tex_home;                // the part shown when the batch started, shown again after
  std::filesystem::path g_tex_front;     // the file at the front, and since when (30 s to go in)
  Clock::time_point g_tex_front_at;
  bool g_tex_switched = false;           // its part was asked for
  int g_tex_done = 0, g_tex_all = 0;     // of this batch
  std::wstring g_tex_pick_dir;
  std::string fileSafe(std::string s) {  // A part's name made safe for a file name: "DI wing" stays, "a/b" -> "a_b"
    for (char& c : s) if (static_cast<unsigned char>(c) < 32 || std::strchr("<>:\"/\\|?*", c)) c = '_';
    return s;
  }
  std::wstring newestExport(const std::string& chr, const std::string& color) {  // Only complete ones (export.done). The stamps sort by time
    const std::filesystem::path root = g_dir / "export" / chr / color;
    std::error_code ec, ec2;
    std::filesystem::path best;
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
      if (it->path().filename().wstring().front() != L'.' && std::filesystem::exists(it->path() / "export.done", ec2) &&
          (best.empty() || it->path().filename() > best.filename()))
        best = it->path();
    return (!best.empty() ? best : std::filesystem::exists(root, ec) ? root : g_dir / "export").wstring();
  }
  std::string exportStem(const Target& o) {  // One per kind in the slot. A name two kinds share gets the kind, "Flare (DI_FLARE_UNIQUEC)"
    const std::string n = fileSafe(partName(o.kind));
    const bool shared = std::any_of(g_targets.begin(), g_targets.end(), [&](const Target& x) {
      return x.chr == o.chr && x.color == o.color && x.kind != o.kind && _stricmp(fileSafe(partName(x.kind)).c_str(), n.c_str()) == 0;
    });
    return shared ? fileSafe(partName(o.kind) + " (" + o.kind + ")") : n;
  }
  bool writeBytes(const std::filesystem::path& file, const std::vector<uint8_t>& bytes) {
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    return !bytes.empty() && f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())).good();
  }
  void writeMatColors(const std::vector<VecGroup>& rows, const std::filesystem::path& file) {  // the colors the materials draw with besides the textures (glow, edges)
    std::ofstream f(file, std::ios::trunc);
    f << "# material instance, label, vector parameter, R G B A (linear; above 1 glows brighter)\n";
    for (const VecGroup& g : rows) {
      if (g.label == kDecalLabel) {  // Not a parameter but a texture made from the decal
        f << "# " << kDecalLabel << ": the decal texture tinted (white stays, the strongest line becomes " << g.cur[0] << ' ' << g.cur[1] << ' ' << g.cur[2]
          << ")" << (g.edited ? "" : " (unchanged)") << "\n";
        continue;
      }
      std::set<std::string> parents;
      for (UObject* m : g.mids) parents.insert(parentOf(m) ? narrow(parentOf(m)->GetName()) : narrow(m->GetName()));
      for (const std::string& pn : parents)
        f << pn << "\t" << g.label << "\t" << g.param << "\t" << g.cur[0] << ' ' << g.cur[1] << ' ' << g.cur[2] << ' ' << g.cur[3] << (g.edited ? "" : "\t(unchanged)") << "\n";
    }
  }
  // A part that isn't shown: its material color rows as buildVecs lists them, read only (nothing set or claimed).
  // Decal lines are left out since they're read when the part is shown.
  std::vector<VecGroup> vecsOf(const Target& t) {
    std::vector<VecGroup> rows = vecRows(vecMats(t, [&](UObject* m) {
      if (auto ow = g_vec_owner.find(m); ow != g_vec_owner.end() && ow->second != keyOf(t)) return true;  // (another part's)
      return std::any_of(g_targets.begin(), g_targets.end(), [&](const Target& o) {
        return o.pawn == t.pawn && o.kind != t.kind && std::find(o.mids.begin(), o.mids.end(), m) != o.mids.end();
      });
    }));
    const auto saved = g_vec_saved.find(keyOf(t));
    for (VecGroup& g : rows) {
      getVector(g.mids.front(), wide(g.params.front()).c_str(), g.cur);
      if (saved == g_vec_saved.end()) continue;
      if (auto c = saved->second.find(g.label); c != saved->second.end()) { std::memcpy(g.cur, c->second.data(), sizeof(g.cur)); g.edited = true; }
    }
    return rows;
  }
  struct ExportPart {
    std::string file;                // its name in the folder ("DI wing")
    std::string kind, ids;           // For palette looks: the part, and its textures' ids now
    std::shared_ptr<const std::vector<uint8_t>> look;  // When a look is put on: the palette's packed textures
    std::shared_ptr<std::vector<uint8_t>> out;         // For palette looks: what the job made, the packed look or the paint
    std::shared_ptr<Pngs> pngs;                        // (3) the part's textures made
    std::shared_ptr<std::vector<Edit>> eds;            // (2) The region colors the look is put on as
    std::shared_ptr<CharMap> map;
    std::shared_ptr<Worn> worn;      // If none, made from the cache's files on the worker
    std::filesystem::path base, sss;
    std::vector<Edit> ed;
    PaintBytes paint;
  };
  struct Export {
    int mode = 0;                    // 0 an export, 1 a palette's looks packed into it, 2 a palette's looks put on as paint, 3 a slot's other parts made again
    std::vector<std::pair<std::string, std::shared_ptr<Pngs>>> made;  // (3) target key -> its textures
    std::vector<std::tuple<std::string, std::shared_ptr<std::vector<Edit>>, std::shared_ptr<CharMap>>> eds;  // (2) kind -> region colors, on that map
    std::vector<std::string> keys;   // (3) the parts still to gather, by key (one at a time, as their turn comes)
    uint64_t gen = 0;                // (3) The slot's recolor this was made for (if it changed since, not put on)
    std::filesystem::path palette;   // (1) the palette file
    std::string chr, color;          // (2) the slot
    std::vector<std::pair<std::string, std::shared_ptr<std::vector<uint8_t>>>> results;  // (1, 2) per part kind
    std::filesystem::path dir;
    std::vector<ExportPart> todo;    // (each let go once its job has it)
    size_t next = 0;
    int done = 0;
    std::string running, later;      // The part being made, and the parts not ready (reported at the end)
    std::vector<std::string> failed;
    bool follow = true;
    std::future<bool> job;
  };
  std::unique_ptr<Export> g_export;
  std::vector<std::future<bool>> g_bool_orphans;  // Export jobs that were dropped, let go once done (a future waits when destroyed)
  bool exportPart(ExportPart p, bool follow, std::filesystem::path dir) {  // Worker. Reads only what it's given
    struct Count { ~Count() { g_encoding--; } } done;
    if (!p.worn) p.worn = prepareWorn(p.map, p.base, p.sss);
    if (!p.worn) return false;
    Image ob, os;
    buildW(*p.worn, follow, p.ed, {}, ob, os);
    p.worn.reset();
    paintOver(p.paint, ob, os);
    for (const auto& [tail, im] : {std::pair<const char*, Image*>{"_Lit", &ob}, {"_Shadow", &os}}) {
      const std::string f = p.file + tail;
      const std::vector<uint8_t> exact = encodePng(*im, false);  // (the texture's own values, as the game has them)
      if (!writeBytes(dir / "alpha" / (f + ".png"), exact) || !writeBytes(dir / "alpha" / (f + ".tga"), encodeTga(*im))) return false;
      bool full = true;
      for (size_t i = 3; i < im->px.size(); i += 4) if (im->px[i] != 255) { full = false; im->px[i] = 255; }
      if (!writeBytes(dir / (f + ".png"), full ? exact : encodePng(*im, false)) || !writeBytes(dir / ".exported" / (f + ".png"), exact)) return false;
    }
    return true;
  }
  void put32(std::vector<uint8_t>& o, uint32_t v);
  // A part's finished textures, packed: [w][h][lit bytes][lit][shadow bytes][shadow] (u32s), LZMS'd RGBA.
  bool lookPackPart(ExportPart p, bool follow) {  // (worker)
    struct Count { ~Count() { g_encoding--; } } done;
    if (underWine()) return false;  // Unpacked, a look is tens of MB in the palette file, so a palette made there has none
    if (!p.worn) p.worn = prepareWorn(p.map, p.base, p.sss);
    if (!p.worn) return false;
    Image ob, os;
    buildW(*p.worn, follow, p.ed, {}, ob, os);
    p.worn.reset();
    paintOver(p.paint, ob, os);
    const std::vector<uint8_t> zl = lzmsPack(ob.px), zs = lzmsPack(os.px);
    if (zl.empty() || zs.empty() || ob.w != os.w || ob.h != os.h) return false;
    p.out->clear();
    put32(*p.out, static_cast<uint32_t>(ob.w));
    put32(*p.out, static_cast<uint32_t>(ob.h));
    put32(*p.out, static_cast<uint32_t>(zl.size()));
    p.out->insert(p.out->end(), zl.begin(), zl.end());
    put32(*p.out, static_cast<uint32_t>(zs.size()));
    p.out->insert(p.out->end(), zs.begin(), zs.end());
    const std::string b = base64(*p.out);  // The palette's lines, made here and not on the game thread
    std::string lines;
    lines.reserve(b.size() + b.size() / 4096 * 2 + 2);
    for (size_t i = 0; i < b.size(); i += 4096) lines += "|" + b.substr(i, 4096) + "\n";
    p.out->assign(lines.begin(), lines.end());
    return true;
  }
  // The palette's look on this slot's part. Each region gets its color in the look, then paint goes wherever that
  // doesn't show it.
  bool lookApplyPart(ExportPart p, bool follow) {  // Worker. p.out is the part's paint file
    struct Count { ~Count() { g_encoding--; } } done;
    const std::vector<uint8_t>& d = *p.look;
    auto u32 = [&](size_t at) { return at + 4 <= d.size() ? uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 | uint32_t(d[at + 3]) << 24 : 0u; };
    const uint32_t w = u32(0), h = u32(4), nl = u32(8);
    if (!w || !h || w > 8192 || h > 8192 || 12 + static_cast<size_t>(nl) + 4 > d.size()) return false;
    const uint32_t ns = u32(12 + nl);
    if (16 + static_cast<size_t>(nl) + ns > d.size()) return false;
    if (!p.worn) p.worn = prepareWorn(p.map, p.base, p.sss);
    if (!p.worn) return false;
    const Worn& wn = *p.worn;
    if (wn.base.w != static_cast<int>(w) || wn.base.h != static_cast<int>(h) || wn.sss.w != wn.base.w || wn.sss.h != wn.base.h) return false;  // Another size isn't this look's, so nothing is unpacked
    const size_t raw = static_cast<size_t>(w) * h * 4;
    std::vector<Edit> eds_made;  // Handed out only with the paint: one look, both halves or neither
    paint::Image want[2] = {{static_cast<int>(w), static_cast<int>(h), lzmsUnpack(d.data() + 12, nl, raw)},
                            {static_cast<int>(w), static_cast<int>(h), lzmsUnpack(d.data() + 16 + nl, ns, raw)}};
    if (want[0].px.empty() || want[1].px.empty()) return false;
    {  // Each region's color in the look, from its own-color texels (the edges are left to the paint)
      const int R = static_cast<int>(wn.lit.size());
      std::vector<Edit> ed = p.ed;
      ed.resize(static_cast<size_t>(R));
      // Lit and shadow as a pair, from texels that are the region's own color in both. Every other texel is plenty.
      std::vector<std::unordered_map<uint64_t, uint32_t>> pairs(static_cast<size_t>(R));
      std::vector<uint32_t> counted(static_cast<size_t>(R), 0);
      const size_t N = wn.owner.size();
      auto at = [](const std::vector<uint8_t>& px, size_t i) { return (uint32_t(px[i * 4]) << 16) | (uint32_t(px[i * 4 + 1]) << 8) | px[i * 4 + 2]; };
      for (size_t i = 0; i < N; i += 2) {
        const uint16_t r = wn.owner[i];
        if (r == 0xFFFF || r >= R || rgbAt(wn.base, i) != wn.lit[r] || rgbAt(wn.sss, i) != wn.shadow[r]) continue;
        pairs[r][(uint64_t(at(want[0].px, i)) << 32) | at(want[1].px, i)]++;
        counted[r]++;
      }
      bool changed = false;
      for (int r = 0; r < R; r++) {
        uint64_t best = 0;
        uint32_t n = 0;
        for (const auto& [pr, k] : pairs[r]) if (k > n) { n = k; best = pr; }
        if (!n) continue;
        uint32_t l = uint32_t(best >> 32), sh = uint32_t(best & 0xFFFFFFFFu);
        if (n * 5 < counted[r]) {  // No pair has a fifth of it (a shaded look): the average of each, and the paint shows the rest
          uint64_t a[6] = {};
          for (const auto& [pr, k] : pairs[r]) {
            const uint32_t pl = uint32_t(pr >> 32), ps = uint32_t(pr & 0xFFFFFFFFu);
            a[0] += uint64_t(pl >> 16 & 255) * k; a[1] += uint64_t(pl >> 8 & 255) * k; a[2] += uint64_t(pl & 255) * k;
            a[3] += uint64_t(ps >> 16 & 255) * k; a[4] += uint64_t(ps >> 8 & 255) * k; a[5] += uint64_t(ps & 255) * k;
          }
          const uint64_t c = counted[r];
          l = static_cast<uint32_t>((a[0] / c) << 16 | (a[1] / c) << 8 | (a[2] / c));
          sh = static_cast<uint32_t>((a[3] / c) << 16 | (a[4] / c) << 8 | (a[5] / c));
        }
        const std::pair<uint32_t, uint32_t> now = pairOfW(wn, follow, r, ed);
        if (l == now.first && sh == now.second) continue;  // Already its color, so leave it
        Edit& e = ed[static_cast<size_t>(r)];
        e.has_lit = true; e.lit = l;
        e.has_shadow = true; e.shadow = sh;
        e.derive = false;
        changed = true;
      }
      p.ed = ed;
      if (!changed) ed.clear();  // No region changed, so the part's save stays as it is
      eds_made = std::move(ed);
    }
    Image ob, os;
    buildW(wn, follow, p.ed, {}, ob, os);
    p.worn.reset();
    paint::Canvas c;
    if (!(p.paint && !p.paint->empty() && c.load(*p.paint, ob.w, ob.h)) && !c.reset(ob.w, ob.h)) return false;
    int k = 0;
    for (auto [im, map] : {std::pair<Image*, paint::Map>{&ob, paint::Map::Lit}, std::pair<Image*, paint::Map>{&os, paint::Map::Shadow}}) {
      paint::Image base{im->w, im->h, std::move(im->px)}, shown;
      if (!c.compose(map, base, shown)) shown = paint::Image{base.w, base.h, base.px};
      if (!c.set_texels(map, shown, want[k++])) return false;
    }
    *p.out = c.save();
    if (p.out->empty()) return false;
    *p.eds = std::move(eds_made);
    return true;
  }
  bool refreshPart(ExportPart p, bool follow) {  // Worker. Makes a part's textures from what's saved for it
    struct Count { ~Count() { g_encoding--; } } done;
    if (!p.worn) p.worn = prepareWorn(p.map, p.base, p.sss);
    if (!p.worn) return false;
    *p.pngs = encodeEdited(std::move(p.worn), std::move(p.ed), follow, std::move(p.paint));
    return !p.pngs->base.empty() && !p.pngs->sss.empty();
  }
  // The ids of the textures a part's look is made from, as the cache read them ("" if not known). Only while what
  // the fighter wears is still those (texState 0). Otherwise the cache is old (a skin mod changed) and it's "" too.
  std::string cacheIds(const Target& o) {
    if (texState(o) != 0) return {};
    const auto& ids = texIds(o.chr, o.kind);
    auto it = ids.find(prefixOf(srcOf(o), o.kind));
    return it == ids.end() || it->second.find(' ') == std::string::npos || it->second.front() == ' ' || it->second.back() == ' ' ? std::string() : it->second;
  }
  // A part the editor hasn't opened, made again in the background: its saved material colors put on as the editor
  // would (each material's own taken first, so a reset puts it back). Decal lines wait until it's opened.
  void putSavedVecs(const Target& o) {
    std::map<std::string, std::array<float, 4>> saved;
    if (g_edits_loaded.count(keyOf(o))) { if (auto v = g_vec_saved.find(keyOf(o)); v != g_vec_saved.end()) saved = v->second; }  // Opened before: its colors as they are now, unsaved ones too
    else for (const std::string& l : readLines(editsFile(o))) readVecLine(l, saved);
    if (saved.empty()) return;
    for (const VecGroup& row : vecsOf(o)) {
      auto c = saved.find(row.label);
      if (c == saved.end()) continue;
      for (size_t i = 0; i < row.mids.size(); i++) {
        UObject* m = row.mids[i];
        const std::string& prm = row.paramOf(i);
        if (!g_vec_orig.count({m, prm})) getVector(m, wide(prm).c_str(), g_vec_orig[{m, prm}].data());
        g_vec_owner.emplace(m, keyOf(o));
        setVector(m, wide(prm).c_str(), c->second.data());
      }
    }
  }
  bool gatherPart(const Target& o, ExportPart& p);
  void exportStart(const Target& t) {  // Game thread. Reads what each part needs now, and exportTick makes the pictures
    if (g_export) { g_status = "An export is still running."; return; }
    auto ex = std::make_unique<Export>();
    ex->follow = g_follow;
    std::vector<const Target*> parts;
    std::string skipped;
    int n_skipped = 0;
    for (const Target& o : g_targets) {
      if (o.chr != t.chr || o.color != t.color) continue;
      if (std::any_of(parts.begin(), parts.end(), [&](const Target* x) { return x->kind == o.kind; })) continue;  // A mirror match gets one each
      ExportPart p;
      if (!gatherPart(o, p)) {
        skipped += (skipped.empty() ? "" : ", ") + partName(o.kind);
        n_skipped++;
        continue;
      }
      parts.push_back(&o);
      ex->todo.push_back(std::move(p));
    }
    if (n_skipped) ex->later = skipped + " not ready, open " + (n_skipped > 1 ? "them" : "it") + " once and export again.";
    if (ex->todo.empty()) { g_status = "Nothing exported: " + ex->later; return; }
    char stamp[32];
    { const std::time_t now = std::time(nullptr); std::tm tm{}; localtime_s(&tm, &now); std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tm); }
    std::error_code ec;
    ex->dir = g_dir / "export" / t.chr / t.color / stamp;
    for (int n = 2; std::filesystem::exists(ex->dir, ec); n++) ex->dir = g_dir / "export" / t.chr / t.color / (std::string(stamp) + "_" + std::to_string(n));  // Two in one second
    std::filesystem::create_directories(ex->dir / "alpha", ec);
    if (!ec) std::filesystem::create_directories(ex->dir / ".exported", ec);
    if (ec) { g_status = "Couldn't make the export folder."; return; }
    for (size_t i = 0; i < parts.size(); i++) {
      const std::vector<VecGroup> rows = g_vecs_for == keyOf(*parts[i]) ? g_vecs : vecsOf(*parts[i]);
      if (!rows.empty()) writeMatColors(rows, ex->dir / (ex->todo[i].file + "_material_colors.txt"));
    }
    if (g_fx_for == t.chr + "/" + t.color && !fxList().empty()) {  // The slot's effect colors as they are (a skin's own too), for Import
      std::ofstream f(ex->dir / "effects.txt", std::ios::trunc);
      f << "# effect color, R G B (the hue its effects take)\n";
      for (const FxColor& c : fxList()) f << c.label << '\t' << c.rgb[0] << ' ' << c.rgb[1] << ' ' << c.rgb[2] << "\n";
    }
    g_export = std::move(ex);
    g_status = "Exporting...";
  }
  // What a part needs for the export's workers, read now (a part not opened this session gets its map from disk).
  // Returns false if it's not ready.
  bool gatherPart(const Target& o, ExportPart& p) {
    p.file = exportStem(o);
    const std::filesystem::path cache = g_dir / "cache" / o.chr;
    auto mp = g_maps.find(mapKey(o));
    if (mp == g_maps.end() && !(g_map_job.valid() && kindOfKey(g_map_job_key, o.chr, o.kind)) &&
        std::none_of(g_menu_maps.begin(), g_menu_maps.end(), [&](const auto& kv) { return kindOfKey(kv.first, o.chr, o.kind); })) {  // Not opened this session: the map saved with its colors, as the menus use, not while a map is made
      const auto prefixes = cachedPrefixes(o.chr, o.kind);
      const std::string own = ownOfKey(mapKey(o));
      const auto mf = own.empty() ? cache / ("map_" + o.kind + ".bin") : cache / "own" / ("map_" + own + ".bin");
      std::error_code mec;
      const bool fresh = own.empty() || (std::filesystem::last_write_time(mf, mec) >= std::filesystem::last_write_time(cache / (own + "_BaseMap.png"), mec) &&
                                         std::filesystem::last_write_time(mf, mec) >= std::filesystem::last_write_time(cache / (own + "_SssMap.png"), mec) && !mec);
      auto m = prefixes.empty() || !fresh ? nullptr : readMapFile(mf);
      if (m && m->regions > 0 && m->prefixes == prefixes) {
        // ponytail: runs on the game thread. A save that needs moving rebuilds its old map here (a hitch, only after a
        // map change).
        if (own.empty()) try { migrateSaves(o.chr, o.kind, *m); } catch (...) {}  // Saves made on an older map are moved onto it, as the editor's own map job does
        mp = g_maps.emplace(mapKey(o), m).first;
        g_old_fits.clear();
      }
    }
    p.base = cache / (prefixOf(srcOf(o), o.kind) + "_BaseMap.png");
    p.sss = cache / (prefixOf(srcOf(o), o.kind) + "_SssMap.png");
    std::error_code ec;
    if (mp == g_maps.end() || !std::filesystem::exists(p.base, ec) || !std::filesystem::exists(p.sss, ec)) return false;
    p.map = mp->second;
    if (g_worn && g_worn_key == keyOf(o)) p.worn = g_worn;
    if (g_edits_loaded.count(keyOf(o))) p.ed = g_edits[keyOf(o)];
    else {  // Never opened, so read as the editor would: a recovered copy first, moved onto the map in memory only
      auto st = g_edit_restore.find(keyOf(o));
      std::map<std::string, std::array<float, 4>> vecs;  // The editor's own material colors are left alone
      if (st == g_edit_restore.end() || !loadEdits(o, p.ed, p.map->regions, false, st->second, true, &vecs)) loadEdits(o, p.ed, p.map->regions, false, {}, true, &vecs);
    }
    p.paint = paintBytes(o);
    p.kind = o.kind;
    return true;
  }
  std::string g_look_note;  // Reported after the palette's own message
  // A palette just saved gets each part's finished look packed into it, on the export's workers. Parts that aren't
  // ready are left out.
  void lookSaveStart(const Target& t, const std::filesystem::path& palette) {
    if (g_export) return;
    auto ex = std::make_unique<Export>();
    ex->mode = 1;
    ex->palette = palette;
    ex->follow = g_follow;
    std::set<std::string> kinds;  // A mirror match gets one each
    for (const Target& o : g_targets) {
      if (o.chr != t.chr || o.color != t.color || !kinds.insert(o.kind).second) continue;
      ExportPart p;
      p.ids = g_reading ? std::string() : cacheIds(o);
      if (p.ids.empty() || !gatherPart(o, p)) { ex->later += (ex->later.empty() ? "" : ", ") + partName(o.kind); continue; }  // Its textures aren't known yet, so leave it out
      p.out = std::make_shared<std::vector<uint8_t>>();
      ex->results.push_back({o.kind + "\t" + p.ids, p.out});
      ex->todo.push_back(std::move(p));
    }
    if (ex->todo.empty()) { if (!ex->later.empty()) g_look_note = " Its look couldn't be kept yet: open each part once, then save again."; return; }
    g_export = std::move(ex);
  }
  // Puts a palette's looks on (region colors, then paint for the rest) for each part whose textures aren't the
  // ones it was made on.
  void lookApplyStart(const Target& t, const std::vector<LookIn>& looks) {
    if (looks.empty()) return;
    if (g_export) { for (const LookIn& l : looks) g_look_wait.push_back({t.chr + "/" + t.color, l}); return; }
    auto ex = std::make_unique<Export>();
    ex->mode = 2;
    ex->chr = t.chr;
    ex->color = t.color;
    ex->follow = g_follow;
    for (const LookIn& l : looks) {
      auto o = std::find_if(g_targets.begin(), g_targets.end(), [&](const Target& x) { return x.chr == t.chr && x.color == t.color && x.kind == l.kind; });
      if (o == g_targets.end()) continue;
      if (l.ids.empty()) continue;  // Made where its textures weren't known, so leave it as it is
      const std::string now = g_reading ? std::string() : cacheIds(*o);
      if (!now.empty() && now == l.ids && g_maps.count(mapKey(*o)) && (l.stamp.empty() || l.stamp == mapStamp(*o))) continue;  // On the textures and map it was made on, its edits stay as they are
      ExportPart p;
      const bool ready = !now.empty() && gatherPart(*o, p);
      if (ready && now == l.ids && (l.stamp.empty() || l.stamp == mapStamp(*o))) continue;  // (its map was read just now)
      if (!ready) {  // The part has to be read again first, then it goes on
        g_look_wait.push_back({t.chr + "/" + t.color, l});
        g_look_note += (g_look_note.empty() ? "" : ", ") + partName(l.kind);
        continue;
      }
      p.look = l.data;
      p.out = std::make_shared<std::vector<uint8_t>>();
      p.eds = std::make_shared<std::vector<Edit>>();
      ex->results.push_back({o->kind, p.out});
      ex->eds.push_back({o->kind, p.eds, p.map});
      ex->todo.push_back(std::move(p));
    }
    if (!g_look_note.empty()) g_look_note = " Its look goes on " + g_look_note + " once each is shown (open them once).";  // Reported after the load's message
    if (ex->todo.empty()) return;
    COMBO_LOG("color editor: " + t.chr + " " + t.color + ": " + std::to_string(ex->todo.size()) + " parts on other textures than the palette's, its look put on");
    g_status = "Putting on the palette's look...";
    g_export = std::move(ex);
  }
  std::vector<LookIn> looksOf(const std::map<std::string, std::vector<std::string>>& blocks) {  // ("<KIND>_look.slkl" blocks)
    std::vector<LookIn> out;
    for (const auto& [name, lines] : blocks) {
      if (name.size() <= 10 || name.compare(name.size() - 10, 10, "_look.slkl") != 0 || lines.size() < 3 || lines[0] != "look 1" || lines[1].rfind("ids ", 0) != 0) continue;
      const std::string kind = name.substr(0, name.size() - 10);
      if (kind.empty() || !std::all_of(kind.begin(), kind.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; })) continue;
      std::string all;
      size_t n = 0;
      for (size_t i = 2; i < lines.size(); i++) n += lines[i].size();
      all.reserve(n);
      for (size_t i = 2; i < lines.size(); i++) all += lines[i];
      auto data = std::make_shared<std::vector<uint8_t>>(unbase64(all));
      if (data->size() < 16) continue;
      // Only a part the palette recolored (region colors or paint). The rest keep the slot's own textures, a skin mod's too
      const auto ed = blocks.find(kind + ".txt"), pt = blocks.find(kind + "_paint.slpn");
      const bool regions = ed != blocks.end() && std::any_of(ed->second.begin(), ed->second.end(), [](const std::string& l) {
                             return !l.empty() && std::isdigit(static_cast<unsigned char>(l[0]));
                           });
      if (!regions && (pt == blocks.end() || pt->second.empty())) continue;
      out.push_back({kind, lines[1].substr(4), data, ed != blocks.end() && !ed->second.empty() ? ed->second[0] : std::string()});
    }
    return out;
  }
  // The finished looks go into the palette (1), or the parts' paint is written and the slot read again (2).
  void lookFinish(Export& ex) {
    std::error_code ec;
    if (ex.mode == 1) {
      std::string add;
      for (const auto& [head, out] : ex.results) {
        if (!out || out->empty()) continue;
        const size_t tab = head.find('\t');
        add += "file\t" + head.substr(0, tab) + "_look.slkl\n|look 1\n|ids " + head.substr(tab + 1) + "\n";
        add.append(out->begin(), out->end());  // (its lines, made on the worker)
        add += "end\n";
      }
      if (add.empty()) return;
      const auto bytes = readBytes(ex.palette);
      if (bytes.empty()) return;  // (deleted meanwhile)
      {
        std::ofstream f(withSuffix(ex.palette, ".new"), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        f << add;
        f.flush();
        if (!f.good()) { f.close(); std::filesystem::remove(withSuffix(ex.palette, ".new"), ec); return; }
      }
      std::filesystem::rename(withSuffix(ex.palette, ".new"), ex.palette, ec);
      if (ec) { std::filesystem::remove(withSuffix(ex.palette, ".new"), ec); return; }
      COMBO_LOG("color editor: palette " + narrow(ex.palette.filename().wstring()) + ": its look kept (" + std::to_string(ex.done) + " parts)");
      return;
    }
    std::string parts;
    bool wrote = false;  // If anything of the slot's was written, it's read again
    for (const auto& [kind, out] : ex.results) {
      if (!out || out->empty()) continue;
      auto o = std::find_if(g_targets.begin(), g_targets.end(), [&](const Target& x) { return x.chr == ex.chr && x.color == ex.color && x.kind == kind; });
      if (o == g_targets.end()) continue;
      auto e = std::find_if(ex.eds.begin(), ex.eds.end(), [&](const auto& t) { return std::get<0>(t) == kind; });
      if (e == ex.eds.end()) continue;
      const auto& eds = std::get<1>(*e);
      if (eds && !eds->empty()) {  // Only on the map its region colors were made on. Otherwise neither half, and the look stays as it was
        auto mp = g_maps.find(mapKey(*o));
        if (mp == g_maps.end() || mp->second != std::get<2>(*e)) continue;
      }
      const auto to = paintFile(*o);
      {  // Write the paint beside it first
        std::lock_guard<std::mutex> lk(g_files_m);
        std::ofstream f(withSuffix(to, ".new"), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(out->data()), static_cast<std::streamsize>(out->size()));
        f.flush();
        if (!f.good()) { f.close(); std::filesystem::remove(withSuffix(to, ".new"), ec); continue; }
      }
      if (eds && !eds->empty()) {  // Then its region colors, into the part's save with its material colors kept
        auto& vecs = g_vec_saved[keyOf(*o)];
        if (vecs.empty())
          for (const std::string& l : readLines(editsFile(*o))) readVecLine(l, vecs);
        if (!saveEditsTo(*o, *eds, editsFile(*o))) { std::lock_guard<std::mutex> lk(g_files_m); std::filesystem::remove(withSuffix(to, ".new"), ec); continue; }
        wrote = true;
      }
      std::lock_guard<std::mutex> lk(g_files_m);  // Then put the paint in place
      std::filesystem::rename(withSuffix(to, ".new"), to, ec);
      if (ec) std::filesystem::remove(withSuffix(to, ".new"), ec);
      else { parts += (parts.empty() ? "" : ", ") + partName(kind); wrote = true; }
    }
    if (wrote && !g_hold.count(ex.chr + "/" + ex.color)) reloadSlot(ex.chr, ex.color);
    g_status = parts.empty() ? "Loaded. Its look couldn't be put on." : "Loaded, with its look on " + parts + " (made on other textures).";
  }
  void refreshStart(const std::string& slot) {
    if (g_export) { g_refresh_wait.insert(slot); return; }
    if (!g_open) return;  // The editor closed, so the saved recolors go on through the queue instead
    auto ex = std::make_unique<Export>();
    ex->mode = 3;
    ex->follow = g_follow;
    ex->gen = g_slot_gen[slot];
    const Target* shown = target();
    for (const Target& o : g_targets)
      if (o.chr + "/" + o.color == slot && !(shown && keyOf(o) == keyOf(*shown)) &&
          std::find(ex->keys.begin(), ex->keys.end(), keyOf(o)) == ex->keys.end())
        ex->keys.push_back(keyOf(o));
    if (ex->keys.empty()) return;
    g_refresh_wait.erase(slot);
    g_export = std::move(ex);
  }
  void refreshGatherNext(Export& ex) {  // Game thread. The next part with something saved, ready for its job
    std::error_code ec;
    while (!ex.keys.empty()) {
      const std::string key = ex.keys.front();
      ex.keys.erase(ex.keys.begin());
      auto o = std::find_if(g_targets.begin(), g_targets.end(), [&](const Target& x) { return keyOf(x) == key; });
      if (o == g_targets.end() || (target() && keyOf(*target()) == key)) continue;  // Gone, or the part on screen, which the editor makes
      const bool has = g_edits_loaded.count(key) ? anyEdit(g_edits[key]) || hasPaint(*o) : std::filesystem::exists(editsFile(*o), ec) || hasPaint(*o);
      if (!has) continue;  // Nothing saved, so the game's own stays as it is
      ExportPart p;
      if (!gatherPart(*o, p)) continue;  // Not ready. It's made when it's opened, or when the editor closes
      p.pngs = std::make_shared<Pngs>();
      ex.made.push_back({key, p.pngs});
      ex.todo.push_back(std::move(p));
      return;
    }
  }
  void refreshOne(Export& ex, size_t i) {  // Game thread. Puts a part's new textures on if the slot hasn't changed since
    if (i >= ex.made.size()) return;
    const auto& [key, pngs] = ex.made[i];
    if (!pngs || pngs->base.empty() || (target() && keyOf(*target()) == key)) return;  // Shown in the editor meanwhile, so the editor makes this part itself
    std::vector<Target*> on;  // Every model with that key. A mirror's two share one texture, kept once in g_shown
    for (Target& o : g_targets) {
      if (keyOf(o) != key || g_slot_gen[o.chr + "/" + o.color] != ex.gen || o.mids.empty() || o.items.size() < o.mids.size()) continue;
      bool alive = true;
      for (size_t k = 0; alive && k < o.mids.size(); k++) alive = Kept{o.mids[k], o.items[k]}.alive();
      if (alive) on.push_back(&o);
    }
    Shown fresh;
    if (on.empty() || !importPngs(on.front()->mids.front(), *pngs, fresh)) return;
    for (Target* o : on) { show(*o, fresh); putSavedVecs(*o); }
    unroot(edited(*on.front()));  // The one before, once the new one is on everywhere
    edited(*on.front()) = fresh;
    std::error_code fec;
    const Target& o0 = *on.front();
    if (g_fx_slots.count(o0.chr + "/" + o0.color) || !std::filesystem::exists(fxFile(o0.chr, o0.color), fec))  // (else its slot's effect colors go on when the editor closes)
      g_auto_paused.erase(std::remove(g_auto_paused.begin(), g_auto_paused.end(), key), g_auto_paused.end());  // Made, so not again when the editor closes
    g_smear_stale = true;
    g_view_dirty = true;
  }
  void refreshDrop() {  // Lets go of the background rebuild (the editor closed, or the fighters changed)
    g_refresh_wait.clear();
    if (g_export && g_export->mode == 3) {
      if (g_export->job.valid()) g_bool_orphans.push_back(std::move(g_export->job));
      g_export.reset();
    }
  }
  void lookWaitTick() {  // Game thread. The looks waiting for their parts go on once those are read again
    if (g_look_wait.empty() || g_export || g_reading || g_map_job.valid() || g_worn_job.valid()) return;
    static int every = 0;
    if (++every % 30) return;
    std::map<std::string, std::vector<LookIn>> ready;
    for (auto it = g_look_wait.begin(); it != g_look_wait.end();) {
      auto o = std::find_if(g_targets.begin(), g_targets.end(), [&](const Target& x) { return x.chr + "/" + x.color == it->first && x.kind == it->second.kind; });
      if (o == g_targets.end()) { it = g_look_wait.erase(it); continue; }  // Not on stage any more, so dropped
      if (cacheIds(*o).empty() || !g_maps.count(mapKey(*o))) { ++it; continue; }
      ready[it->first].push_back(it->second);
      it = g_look_wait.erase(it);
    }
    for (const auto& [slot, looks] : ready) {
      auto o = std::find_if(g_targets.begin(), g_targets.end(), [&](const Target& x) { return x.chr + "/" + x.color == slot; });
      if (o != g_targets.end()) lookApplyStart(*o, looks);  // One job at a time, so a slot after the first goes back on the list
    }
    if (!ready.empty()) g_look_note.clear();  // (a load's note, said already)
  }
  void exportTick() {  // Game thread, every update. One part at a time when no other encode runs (each ~half a GB at 4K)
    g_bool_orphans.erase(std::remove_if(g_bool_orphans.begin(), g_bool_orphans.end(),
                                        [](auto& f) { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }), g_bool_orphans.end());
    lookWaitTick();
    if (!g_export && !g_refresh_wait.empty() && !g_reading && !g_map_job.valid()) { const std::string sl = *g_refresh_wait.begin(); g_refresh_wait.erase(sl); refreshStart(sl); }
    if (!g_export) return;
    Export& ex = *g_export;
    if (ex.job.valid()) {
      if (ex.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
      bool ok = false;
      try { ok = ex.job.get(); } catch (...) {}  // Out of memory on the worker, so that part isn't made
      if (ok) ex.done++; else ex.failed.push_back(ex.running);
      if (ok && ex.mode == 3) { refreshOne(ex, ex.next - 1); return; }  // On now. The next one goes next frame
    }
    if (ex.mode == 3 && ex.next == ex.todo.size()) refreshGatherNext(ex);
    if (ex.next < ex.todo.size()) {
      if (g_encoding != 0) return;
      ex.running = ex.todo[ex.next].file;
      g_encoding++;
      try {
        if (ex.mode == 3) ex.job = std::async(std::launch::async, refreshPart, std::move(ex.todo[ex.next]), ex.follow);
        else if (ex.mode == 1) ex.job = std::async(std::launch::async, lookPackPart, std::move(ex.todo[ex.next]), ex.follow);
        else if (ex.mode == 2) ex.job = std::async(std::launch::async, lookApplyPart, std::move(ex.todo[ex.next]), ex.follow);
        else ex.job = std::async(std::launch::async, exportPart, std::move(ex.todo[ex.next]), ex.follow, ex.dir);
      }
      catch (...) { g_encoding--; ex.failed.push_back(ex.running); }  // No thread, so that part isn't made
      ex.next++;
      if (ex.mode != 3)  // The other parts were made again quietly, so the status stays as it was
        g_status = (ex.mode == 1 ? "Keeping the palette's look " : ex.mode == 2 ? "Putting on the palette's look " : "Exporting ") + std::to_string(ex.next) + "/" +
                   std::to_string(ex.todo.size()) + "...";
      g_view_dirty = true;
      return;
    }
    if (ex.mode == 3) {  // (each part went on as it was made)
      g_export.reset();
      return;
    }
    if (ex.mode != 0) {
      lookFinish(ex);
      if (ex.mode == 1) {  // (a palette, or an exported recolor file)
        const std::string f = narrow(ex.palette.filename().wstring());
        g_status = !ex.failed.empty() ? "Saved " + f + (underWine() ? " (without its look: that isn't kept on Linux / Proton)." : " (some parts' look couldn't be kept).")
                   : ex.later.empty() ? "Saved " + f + ", with its look."
                                      : "Saved " + f + ", with its look (not " + ex.later + ": open them once, then save again).";
      }
      g_view_dirty = true;
      g_export.reset();
      return;
    }
    const std::filesystem::path rel = ex.dir.lexically_relative(g_dir);
    g_status = "Exported " + std::to_string(ex.done) + (ex.done == 1 ? " part" : " parts") + " to " + narrow((rel.empty() ? ex.dir : rel).wstring());
    if (!ex.failed.empty()) { g_status += "; couldn't make "; for (size_t i = 0; i < ex.failed.size(); i++) g_status += (i ? ", " : "") + ex.failed[i]; }
    if (!ex.later.empty()) g_status += "; " + ex.later;
    COMBO_LOG("color editor: " + g_status);
    if (ex.done > 0) {  // Done. The folder is marked complete and Import opens there
      std::ofstream(ex.dir / "export.done", std::ios::trunc) << ex.done << " parts\n";
      std::lock_guard<std::mutex> lk(g_tex_pick_m);
      g_tex_pick_dir = ex.dir.wstring();
    }
    g_view_dirty = true;
    g_export.reset();
  }

  // ---- the skeleton: where each region is -----------------------------------------------
  struct Bone { UObject* comp = nullptr; FName name; std::string text; bool root = false; };
  std::vector<Bone> g_bones;
  std::string g_bones_for;  // The target they're from ("" for none, read them again)
  bool drawnLately(UObject* comp) {
    static UFunction* f = fn(STR("/Script/Engine.PrimitiveComponent:WasRecentlyRendered"));
    if (!f) return true;
    alignas(8) uint8_t p[16] = {};  // Tolerance at 0, ReturnValue at 4
    *reinterpret_cast<float*>(p) = 0.5f;
    comp->ProcessEvent(f, p);
    return p[4] != 0;
  }
  UObject* standIn(UObject* hidden);
  const std::vector<Bone>& bonesOf(const Target& t) {
    if (g_bones_for == keyOf(t)) return g_bones;
    g_bones.clear();
    std::vector<UObject*> comps;  // A hidden model being previewed, and the meshes on screen wearing it
    for (UObject* c : t.comps) comps.push_back(standIn(c) ? standIn(c) : c);
    std::vector<UObject*> drawn;  // Hidden meshes keep bones too, wherever they were left, so not those
    for (UObject* c : comps) if (drawnLately(c)) drawn.push_back(c);
    for (UObject* c : drawn.empty() ? comps : drawn) {
      const int n = std::clamp(numBones(c), 0, 4000);
      for (int i = 0; i < n; i++) {
        Bone b;
        b.comp = c;
        b.name = boneName(c, i);
        b.text = narrow(b.name.ToString());
        std::string low = b.text;
        for (auto& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        b.root = low.find("root") != std::string::npos;
        g_bones.push_back(b);
      }
    }
    g_bones_for = keyOf(t);
    COMBO_LOG("color editor: " + std::to_string(g_bones.size()) + " bones on " + keyOf(t));
    return g_bones;
  }
  Vec boneAt(const Bone& b) { return socketAt(b.comp, b.name); }

  // ---- the parts, kept on disk ---------------------------------------------------------
  std::filesystem::path partsFile(const std::string& key, const std::string& chr, const std::string& kind) {
    const std::string own = ownOfKey(key);
    return g_dir / "cache" / chr / ("parts_" + kind + (own.empty() ? std::string() : "_" + own) + ".txt");
  }
  void saveParts(const std::string& key, const std::string& chr, const std::string& kind) {
    const auto& parts = g_parts[key];
    std::error_code ec;
    std::filesystem::create_directories(g_dir / "cache" / chr, ec);
    std::ofstream f(partsFile(key, chr, kind), std::ios::trunc);
    f << "v5 regions " << parts.size() << "\n";
    for (size_t r = 0; r < parts.size(); r++) {
      const Part& p = parts[r];
      f << r << '\t' << p.bone << '\t' << p.off[0] << ' ' << p.off[1] << ' ' << p.off[2] << '\t' << p.frac << '\t' << p.dist << "\n";
    }
  }
  bool loadParts(const std::string& key, const std::string& chr, const std::string& kind, int regions, int major) {
    std::ifstream f(partsFile(key, chr, kind));
    std::string line;
    if (!f || !std::getline(f, line) || line != "v5 regions " + std::to_string(regions)) return false;  // Mapped before the small regions, so map it again
    std::vector<Part> parts(static_cast<size_t>(regions));
    while (std::getline(f, line)) {
      std::istringstream ss(line);
      std::string r, bone, off, frac, dist;
      if (!std::getline(ss, r, '\t') || !std::getline(ss, bone, '\t') || !std::getline(ss, off, '\t') || !std::getline(ss, frac, '\t') ||
          !std::getline(ss, dist))
        continue;
      const int i = std::atoi(r.c_str());
      if (i < 0 || i >= regions) continue;
      Part& p = parts[static_cast<size_t>(i)];
      p.bone = bone;
      std::istringstream o(off);
      o >> p.off[0] >> p.off[1] >> p.off[2];
      p.frac = static_cast<float>(std::atof(frac.c_str()));
      p.dist = static_cast<float>(std::atof(dist.c_str()));
    }
    g_parts[key] = std::move(parts);
    return true;
  }
  // The target's parts if mapped (read from disk the first time), else null.
  std::vector<Part>* partsFor(const Target& t) {
    auto mp = g_maps.find(mapKey(t));
    if (mp == g_maps.end()) return nullptr;
    const int R = mp->second->regions;
    auto it = g_parts.find(mapKey(t));
    if (it == g_parts.end()) {
      if (!loadParts(mapKey(t), t.chr, t.kind, R, mp->second->major)) { g_parts[mapKey(t)]; return nullptr; }  // Not on disk, tried once
      it = g_parts.find(mapKey(t));
    }
    return static_cast<int>(it->second.size()) == R ? &it->second : nullptr;
  }

  // Some of the selection can be shown: the mapping saw it (or there's no mapping yet to say).
  bool findable(const Target& t, const std::vector<int>& sel) {  // (from the atlas's places or the mapping's)
    const auto* pl = placesFor(t);
    const std::vector<Part>* parts = partsFor(t);
    return std::any_of(sel.begin(), sel.end(), [&](int r) {
      if (pl && r >= 0 && r < static_cast<int>(pl->size()) && !(*pl)[static_cast<size_t>(r)].empty()) return true;
      if (!parts) return !pl;
      return r >= 0 && r < static_cast<int>(parts->size()) && !(*parts)[static_cast<size_t>(r)].bone.empty();
    });
  }

  struct DrawnFor { const CharMap* map = nullptr; std::vector<uint32_t> count; std::vector<int> parent; std::vector<std::pair<int, int>> touch; int variety = 99;
                   std::vector<uint32_t> shown; std::string only; };  // shown is drawn by meshes other than the hidden model's (only is its part's name)
  std::map<std::string, DrawnFor> g_drawn;  // By map key. For each region, the texels the model draws (none if not known)
  std::map<std::string, Clock::time_point> g_drawn_tried;
  std::map<std::string, std::set<std::string>> g_drawn_seen;  // by map key: the (mesh#index) already counted
  bool forgetMap(const Target& t) {
    for (const std::string& k : g_unsaved) { const Target o = fromKey(k); if (o.chr == t.chr && o.kind == t.kind) return false; }
    auto drop = [&](auto& by_key) { for (auto it = by_key.begin(); it != by_key.end();) it = kindOfKey(it->first, t.chr, t.kind) ? by_key.erase(it) : std::next(it); };
    drop(g_maps);
    g_old_fits.clear();
    for (auto it = g_worn_failed.begin(); it != g_worn_failed.end();) { const Target o = fromKey(*it); it = o.chr == t.chr && o.kind == t.kind ? g_worn_failed.erase(it) : std::next(it); }
    for (auto it = g_map_failed.begin(); it != g_map_failed.end();) it = kindOfKey(*it, t.chr, t.kind) ? g_map_failed.erase(it) : std::next(it);
    drop(g_drawn);
    drop(g_drawn_tried);
    drop(g_drawn_seen);
    drop(g_tpl_names);
    drop(g_names);
    forgetOwn(t.chr, t.kind);
    for (auto it = g_edits_loaded.begin(); it != g_edits_loaded.end();) {
      const Target o = fromKey(*it);
      if (o.chr != t.chr || o.kind != t.kind) { ++it; continue; }
      g_edits.erase(*it);
      g_vec_saved.erase(*it);
      it = g_edits_loaded.erase(it);
    }
    for (auto* h : {&g_undo, &g_redo})  // (steps made on the old regions)
      for (auto it = h->begin(); it != h->end();) it = it->first.rfind(t.chr + "/", 0) == 0 ? h->erase(it) : std::next(it);
    g_worn_key.clear();
    return true;
  }
  bool isVisible(UObject* comp);
  // Which fighters wear a color material now: one target per character + color + kind.
  void discoverNow();
  void discover() {  // (timed: it walks every mesh and material, so it's logged when it's slow)
    const auto t0 = Clock::now();
    discoverNow();
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    if (ms >= 30) COMBO_LOG("color editor: finding the fighters took " + std::to_string(ms) + " ms");
  }
  void discoverNow() {
    std::vector<Seen> seen;
    std::vector<Seen> extra;  // color materials of other names (their texture checked below)
    for (UObject* comp : fighterParts()) {
      const int n = numMaterials(comp);
      for (int i = 0; i < n; i++) {
        UObject* m = material(comp, i);
        std::string name, kind, chr, color;
        UObject* mic = m ? colorSource(m, name, kind) : nullptr;
        if (!mic) {
          UObject* xm = m && getTexture(m, STR("BaseMap")) && getTexture(m, STR("SssMap")) ? extraSource(m, name, kind) : nullptr;
          if (xm && slotOfPath(name, chr, color)) extra.push_back({comp, m, xm, comp->GetOuterPrivate(), chr, color, kind});
          continue;
        }
        if (!getTexture(m, STR("BaseMap"))) continue;  // Some have no color texture, so there's nothing to edit
        if (!slotOfPath(name, chr, color)) continue;
        seen.push_back({comp, m, mic, comp->GetOuterPrivate(), chr, color, kind});
      }
    }
    std::vector<Target> found = groupParts(seen);
    if (!extra.empty()) {  // Ones whose color texture no part uses become parts of their own (Sol's DI wing)
      std::set<UObject*> used;
      for (const Target& t : found)
        for (UObject* mid : t.mids) used.insert(getTexture(mid, STR("BaseMap")));
      for (const Target& old : g_targets)  // Ours may be on them now, so their own textures come from the old record
        for (const auto& [b, s] : old.orig) used.insert(b);
      std::vector<Seen> own;
      for (const Seen& x : extra) {
        UObject* tex = getTexture(x.m, STR("BaseMap"));
        for (const Target& old : g_targets)
          for (size_t k = 0; k < old.mids.size(); k++)
            if (old.mids[k] == x.m) tex = old.orig[k].first;
        if (!tex || used.count(tex)) continue;  // The same texture as a part means an effect copy, edited with it
        own.push_back(x);
      }
      if (!own.empty()) {
        for (const Seen& x : own) {
          const std::string key = extraKey(narrow(x.mic->GetFullName())), nm = narrow(x.mic->GetName());
          if (key.empty() || !g_extra_mi.emplace(key, x.kind).second) continue;
          g_extra_read[x.chr + "/" + x.kind] = std::wstring(nm.begin(), nm.end());
          std::error_code ec;
          std::filesystem::create_directories(g_dir / "cache" / x.chr, ec);
          std::ofstream f(g_dir / "cache" / x.chr / "extra_parts.txt", std::ios::app);  // Remembered for the menus and later sessions
          f << nm << '\t' << x.kind << '\n';
          COMBO_LOG("color editor: " + x.chr + " " + x.kind + " is a part of its own (" + nm + ")");
        }
        for (Target& t : groupParts(own)) found.push_back(std::move(t));
      }
    }
    std::set<UObject*> on_meshes;  // (every color material on a fighter's mesh, part or not)
    for (const Seen& x : seen) on_meshes.insert(x.m);
    // Other materials on the same meshes drawn from the same color texture (Sol's Dragon Install skin
    // next to its outfit) take the edits too.
    for (Target& t : found) {
      std::set<UObject*> bases;
      for (UObject* mid : t.mids) {
        bases.insert(getTexture(mid, STR("BaseMap")));
        for (const Target& old : g_targets)
          for (size_t i = 0; i < old.mids.size(); i++)
            if (old.mids[i] == mid) bases.insert(old.orig[i].first);  // (ours may be on it now)
      }
      bases.erase(nullptr);
      for (UObject* comp : t.comps)
        for (int i = 0, n = numMaterials(comp); i < n; i++) {
          UObject* m = material(comp, i);
          if (!m || std::find(t.mids.begin(), t.mids.end(), m) != t.mids.end()) continue;
          if (std::any_of(found.begin(), found.end(), [&](const Target& o) { return std::find(o.mids.begin(), o.mids.end(), m) != o.mids.end(); })) continue;
          bool same = bases.count(getTexture(m, STR("BaseMap"))) > 0;
          for (const Target& old : g_targets)  // Edited before, so its own texture is in the old record
            for (size_t k = 0; !same && k < old.mids.size(); k++)
              if (old.mids[k] == m) same = bases.count(old.orig[k].first) > 0;
          if (same) t.mids.push_back(m);
        }
    }
    for (Target& t : found)
      addOutlines(t, [&] {
        std::vector<UObject*> all;  // (the fighter's meshes: every part's)
        for (const Target& o : found)
          if (o.pawn == t.pawn) all.insert(all.end(), o.comps.begin(), o.comps.end());
        return all;
      }(), [&](UObject* m) { return std::any_of(found.begin(), found.end(), [&](const Target& o) { return std::find(o.mids.begin(), o.mids.end(), m) != o.mids.end(); }); });
    // The fight's other live copies of the same color materials, on no mesh (a smear's "sol_wep" is one of
    // them), take the edits too.
    {
      std::vector<UObject*> pool;
      UObjectGlobals::FindAllOf(STR("REDMaterialInstanceDynamic"), pool);
      int added = 0;
      for (Target& t : found) t.own_mids = t.mids.size();  // (what follows are the pool's copies)
      for (Target& t : found) {
        std::map<std::string, int> by;  // (what the copies are of, for the log)
        for (UObject* m : poolCopies(t, pool, on_meshes, [&](UObject* x) {
               return std::any_of(found.begin(), found.end(), [&](const Target& o) { return std::find(o.mids.begin(), o.mids.end(), x) != o.mids.end(); });
             })) {
          t.mids.push_back(m);
          by[narrow(parentOf(m)->GetName())]++;
          added++;
        }
        if (!by.empty()) {
          std::string l;
          for (const auto& [n, c] : by) l += " " + n + " x" + std::to_string(c);
          COMBO_LOG("color editor: " + keyOf(t) + " live copies:" + l);
        }
      }
      if (added) COMBO_LOG("color editor: " + std::to_string(added) + " more live copies of the color materials (effects)");
    }
    // A model kept hidden is decided when first found (a preview of ours shows it later, and the body or weapon
    // may just be hidden for a moment).
    for (Target& t : found) {
      t.kept_hidden = t.kind != "MAIN" && t.kind != "WEP" && std::none_of(t.comps.begin(), t.comps.end(), isVisible);
      for (const Target& old : g_targets)
        if (keyOf(old) == keyOf(t) && old.pawn == t.pawn) t.kept_hidden = old.kept_hidden;
    }
    // The textures the game had for each material (from before any of ours).
    std::set<UObject*> ours_now;  // (our textures: never a material's own)
    for (const auto& [k, sh] : g_shown) { ours_now.insert(sh.base); ours_now.insert(sh.sss); }
    for (const Shown& sh : scanTextures()) { ours_now.insert(sh.base); ours_now.insert(sh.sss); }
    ours_now.insert(g_hl.base); ours_now.insert(g_hl.sss); ours_now.insert(g_hl_off.base); ours_now.insert(g_hl_off.sss);
    ours_now.erase(nullptr);
    for (Target& t : found)
      for (UObject* mid : t.mids) {
        std::pair<UObject*, UObject*> o{getTexture(mid, STR("BaseMap")), getTexture(mid, STR("SssMap"))};
        if (ours_now.count(o.first)) o.first = chainTexture(parentOf(mid), "BaseMap");  // (a copy made while ours was on: the game's from its instances)
        if (ours_now.count(o.second)) o.second = chainTexture(parentOf(mid), "SssMap");
        for (const Target& old : g_targets)
          for (size_t i = 0; i < old.mids.size(); i++)
            if (old.mids[i] == mid) o = old.orig[i];
        if (outlineOnly(mid)) o = {nullptr, gameSss(mid)};  // An outline has no color texture. Its shadow one comes from its instance
        t.orig.push_back(o);
      }
    {  // Coverage check, once per part this session: every slot on its meshes and whether it takes the recolor. MISSED: it
       // reads one of this fighter's color textures (any part's) but won't take the edit.
      static std::set<std::string> said;
      auto texOf = [](UObject* m, const char* param) -> UObject* {
        if (isDynamic(m)) return getTexture(m, param[0] == 'B' ? STR("BaseMap") : STR("SssMap"));
        return isMatInstance(m) ? chainTexture(m, param) : nullptr;
      };
      for (const Target& t : found) {
        if (!said.insert(keyOf(t)).second) continue;
        std::map<UObject*, std::string> owner_tex;  // (the fighter's color textures, by part)
        for (const Target& o : found)
          if (o.pawn == t.pawn)
            for (const auto& [b0, s0] : o.orig) { if (b0) owner_tex[b0] = o.kind; if (s0) owner_tex[s0] = o.kind; }
        int slots = 0, taken = 0, missed = 0;
        for (UObject* comp : t.comps) {
          UObject* mesh = meshOf(comp);
          for (int i = 0, n = numMaterials(comp); i < n && i < 32; i++) {
            UObject* m = material(comp, i);
            if (!m) continue;
            slots++;
            const bool ours = std::find(t.mids.begin(), t.mids.end(), m) != t.mids.end();
            bool other = false;  // (another part's own: listed there)
            for (const Target& o : found) other |= &o != &t && o.pawn == t.pawn && std::find(o.mids.begin(), o.mids.end(), m) != o.mids.end();
            UObject* base = texOf(m, "BaseMap");
            UObject* sss = texOf(m, "SssMap");
            std::string reads;
            if (auto f = owner_tex.find(base); base && f != owner_tex.end()) reads = f->second;
            else if (auto g = owner_tex.find(sss); sss && g != owner_tex.end()) reads = g->second;
            const bool miss = !ours && !other && !reads.empty();
            if (ours) taken++;
            if (miss) missed++;
            UObject* p = parentOf(m);
            COMBO_LOG("color editor: coverage " + keyOf(t) + " " + (mesh ? narrow(mesh->GetName()) : std::string("?")) + " slot " + std::to_string(i) + ": " +
                      (p && isDynamic(m) ? narrow(p->GetName()) + " (dynamic)" : narrow(m->GetName()) + (isDynamic(m) ? " (dynamic)" : " (shared asset)")) +
                      " BaseMap " + (base ? narrow(base->GetName()) : std::string("-")) + " SssMap " + (sss ? narrow(sss->GetName()) : std::string("-")) +
                      (ours ? ": takes the recolor" : other ? ": another part's" : miss ? ": MISSED, reads " + reads + "'s texture but isn't recolored" : ""));
          }
        }
        COMBO_LOG("color editor: coverage " + keyOf(t) + ": " + std::to_string(slots) + " slots, " + std::to_string(taken) + " take the recolor, " +
                  std::to_string(missed) + " MISSED");
      }
    }
    const std::string was = target() ? keyOf(*target()) : std::string();
    g_targets = std::move(found);
    for (Target& t : g_targets) {  // Where each material and mesh sits in the object array now, checked before the smears use them
      t.items.clear();
      for (UObject* o : t.mids) t.items.push_back(o ? reinterpret_cast<const uint8_t*>(o->GetObjectItem()) : nullptr);
      for (UObject* o : t.comps) t.items.push_back(o ? reinterpret_cast<const uint8_t*>(o->GetObjectItem()) : nullptr);
    }
    g_smear_stale = true;  // The fighters' materials may be new ones, so the smears' live copies are looked up again
    g_target = 0;
    g_setup_home.clear();  // (the first-time setup's part was in the old fight)
    g_fight = asw_engine::get();
    g_pawns = fighters();
    g_bones_for.clear();  // the meshes may be new ones
    g_vecs_for.clear();
    g_vecs.clear();
    // The one edited before, else the player's character's body.
    static const char* kCodes[] = {"SOL", "KYK", "MAY", "AXL", "CHP", "POT", "FAU", "MLL", "ZAT", "RAM", "LEO", "NAG", "GIO", "ANJ", "INO", "GLD",
                                   "JKO", "COS", "BKN", "TST", "BGT", "SIN", "BED", "ASK", "JHN", "ELP", "ABA", "SLY", "DZY", "VEN", "UNI", "LUC",
                                   "USG", "ABS"};  // (Jam's and Robo-Ky's models are under USG and ABS)
    const int me = combo_trials::playerChara();
    int mine = -1, same = -1;
    for (int i = 0; i < static_cast<int>(g_targets.size()); i++) {
      if (keyOf(g_targets[i]) == was) same = i;
      if (mine < 0 && me >= 0 && me < static_cast<int>(std::size(kCodes)) && codeOf(g_targets[i].chr) == kCodes[me] && g_targets[i].kind == "MAIN") mine = i;
    }
    if (mine < 0)  // No "MAIN", so the player's character's first part
      for (int i = 0; mine < 0 && i < static_cast<int>(g_targets.size()); i++)
        if (me >= 0 && me < static_cast<int>(std::size(kCodes)) && codeOf(g_targets[i].chr) == kCodes[me]) mine = i;
    g_target = same >= 0 ? same : mine >= 0 ? mine : 0;
    COMBO_LOG("color editor: " + std::to_string(g_targets.size()) + " fighter parts/colors" + (target() ? ", editing " + keyOf(*target()) : std::string()));
    g_view_dirty = true;
  }

  // ---- free camera -------------------------------------------------------------------
  // The level's spare CameraActor, made the view target and moved after each battle update. It orbits a pivot kept
  // relative to the fighter's feet, so it follows them. It always eases toward a goal pose, and the user's input
  // moves the pose and the goal together, so the user wins.
  struct CamState {
    bool on = false, ready = false;
    float yaw = 0, pitch = 0, dist = 0, pivot[3] = {};      // Current pose (pitch is the camera above the pivot)
    float gyaw = 0, gpitch = 0, gdist = 0, gpivot[3] = {};  // the goal
  } g_cam;
  std::atomic<float> g_cam_dx{0}, g_cam_dy{0}, g_cam_wheel{0}, g_pan_dx{0}, g_pan_dy{0};
  void addTo(std::atomic<float>& a, float v) {  // The UI adds while the game thread takes, so nothing is lost
    float was = a.load();
    while (!a.compare_exchange_weak(was, was + v)) {}
  }
  std::atomic<bool> g_cam_keys{false}, g_typing{false};
  std::atomic<float> g_aspect{16.0f / 9.0f};          // the game's frame, width / height
  std::atomic<float> g_tag_x{-1.0f}, g_tag_y{-1.0f};  // the selection on screen (0..1), -1 none
  float g_cam_pose[12] = {};  // location, forward, right, up of the camera as last set
  float g_cam_fov = 65.0f;    // horizontal
  // Flying to the selection: aim (its place from the mapping), or else settle and probe (a scan of the selection
  // alone finds it on screen), then move there.
  enum class Focus { Idle, Aim, Settle, Probe, Move } g_focus = Focus::Idle;
  bool g_follow_sel = true;    // selecting flies the camera there
  bool g_focus_retry = false;  // Not found, so look again from the whole fighter
  int g_look = 0;              // Selection pixel search on arrival. 1 from here, 2 from the other side, 3 here without moving
  bool g_probe_looks = false;  // The running probe is that look (otherwise it's finding an unmapped part)
  float g_keep_dist = 0;       // The user's distance for flights, taken once (0 means none yet, the wheel changes it)
  float g_fighter_h = 200;     // the fighter's height, from their bones
  std::string g_fighter_h_for; // the target it's for
  Clock::time_point g_focus_since;
  bool g_tag_found = false;    // The selection's pixel was found on screen at g_tag_point (the only place the label goes)
  float g_tag_point[3] = {};   // from the feet
  float g_tag_yaw = 0;         // The fighter's facing when it was found (once they turn round, it's gone)
  // Which way the fighter faces, as drawn: the mesh's world yaw, plus 180 for each mirrored axis (a
  // side switch may turn the actor, the mesh, or flip the mesh's scale).
  float facing(const Target& t) {
    static UFunction* rot = fn(STR("/Script/Engine.SceneComponent:K2_GetComponentRotation"));
    static UFunction* scl = fn(STR("/Script/Engine.SceneComponent:K2_GetComponentScale"));
    const Target* body = &t;  // (a weapon's mesh turns with the hand)
    for (const Target& o : g_targets) if (o.pawn == t.pawn && o.kind == "MAIN") body = &o;
    if (body->comps.empty() || !rot || !scl) return 0.0f;
    Vec r, k;
    body->comps.front()->ProcessEvent(rot, &r);
    body->comps.front()->ProcessEvent(scl, &k);
    return r.y + (k.x < 0.0f ? 180.0f : 0.0f) + (k.y < 0.0f ? 180.0f : 0.0f);
  }
  struct FocusHit { bool ready = false, found = false; float cx = 0, cy = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0; int w = 0, h = 0; };
  double nowMs() { return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count(); }
  // Keys only count while the game's window is in front and nothing is being typed.
  bool keyDown(int vk) {
    if (g_typing || (GetAsyncKeyState(VK_CONTROL) & 0x8000)) return false;  // Ctrl+S and Ctrl+Z are shortcuts, not camera moves
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId() && (GetAsyncKeyState(vk) & 0x8000);
  }
  Vec actorVec(UObject* a, const TCHAR* f) {
    UFunction* fnc = fn(f);
    Vec v;
    if (fnc && a) a->ProcessEvent(fnc, &v);
    return v;
  }
  void setActor(UObject* a, Vec loc, Vec rot) {
    static UFunction* f = fn(STR("/Script/Engine.Actor:K2_SetActorLocationAndRotation"));
    if (!f || !a) return;
    alignas(16) uint8_t prm[0xC0] = {};
    std::memcpy(prm, &loc, 12);
    std::memcpy(prm + 0x0C, &rot, 12);
    prm[0xA4] = 1;  // teleport
    a->ProcessEvent(f, prm);
  }
  UObject* levelActor(const TCHAR* cls, const TCHAR* name_part) {
    std::vector<UObject*> all;
    UObjectGlobals::FindAllOf(cls, all);
    for (UObject* o : all) {
      const auto n = o->GetFullName();
      if (n.find(STR("PersistentLevel")) != StringType::npos && n.find(name_part) != StringType::npos) return o;
    }
    return nullptr;
  }
  std::string g_cam_info;

  // ---- scans: the regions lit in a binary code, read back by the render thread ----------
  std::mutex g_scan_m;          // shared with the render thread:
  int g_scan_id = 0;
  int g_scan_frames = 0, g_scan_groups = 0;
  bool g_scan_whole = false;  // the part is one region by itself (see scanWhole)
  int g_scan_frame = -1;        // the frame on screen now (-1: no scan)
  bool g_scan_grabbed = false;  // the render thread has it
  bool g_scan_done = false;     // the result is in
  double g_scan_ms = 0;         // when the frame went up
  int g_scan_w = 0, g_scan_h = 0;
  std::vector<ScanSample> g_scan_out;
  std::string g_scan_why;         // Why the last scan couldn't be read ("" if it could)
  bool g_scan_point_on = false;  // For a pick's scan, the point clicked (0..1)
  float g_scan_point[2] = {};
  std::vector<Shown> g_scan_tex;  // game thread: the frames' textures
  const std::vector<Shown>& scanTextures() { return g_scan_tex; }
  bool g_scan_on = false;         // game thread: a scan's frames are on the fighter
  // Makes the frames for these groups now (group[r] is the region's number, 0 for none).
  bool scanBuild(Target& t, const std::vector<int>& group, int groups, int cap = 1024) {
    g_pick_frames.clear();  // Whatever frames were kept are replaced now
    {
      auto mp = g_maps.find(mapKey(t));
      g_scan_major = mp == g_maps.end() ? 0x7FFF : mp->second->major;
    }
    for (Shown& s : g_scan_tex) unroot(s);
    const int bits = bitsFor(groups);
    g_scan_tex.assign(static_cast<size_t>(bits + 3), Shown{});
    for (size_t f = 0; f < g_scan_tex.size(); f++)
      if (!makeScanFrame(t.mids.front(), group, static_cast<int>(f), g_scan_tex[f], cap, bits)) {
        for (Shown& s : g_scan_tex) unroot(s);
        g_scan_tex.clear();
        return false;
      }
    std::lock_guard<std::mutex> lk(g_scan_m);
    g_scan_groups = groups;
    g_scan_whole = groups == 1 && group.size() == 1;  // (not a selection's scan of one region among many: that one is read as usual, moving pixels left out)
    return true;
  }
  std::vector<Kept> g_scan_hidden;  // what a scan hid (the other fighter, effects), shown again when it ends
  void scanHideOthers(const Target& t) {
    if (!g_scan_hidden.empty()) return;  // (hidden already)
    for (UObject* c : fighterParts())
      if (c && c->GetOuterPrivate() != t.pawn && keep(c).alive() && isVisible(c)) { g_scan_hidden.push_back(keep(c)); setVisible(c, false); }
    std::vector<UObject*> fx;
    UObjectGlobals::FindAllOf(STR("ParticleSystemComponent"), fx);
    for (UObject* c : fx)  // (never a template: every effect made from it would start hidden)
      if (c && keep(c).alive() && c->GetFullName().find(STR("Default__")) == StringType::npos && isVisible(c)) {
        g_scan_hidden.push_back(keep(c));
        setVisible(c, false);
      }
  }
  void scanShowOthers() {
    for (const Kept& k : g_scan_hidden) if (k.alive()) setVisible(k.obj, true);
    g_scan_hidden.clear();
  }
  void scanRun(Target& t, bool hide_others = false) {  // Puts frame 0 up, and the render thread takes it from there
    if (hide_others) scanHideOthers(t);  // A mapping sees the whole fighter from each side, so nothing may be in the way. A selection's own
                                       // scan (on every click in the list) and a pick hide nothing
    show(t, g_scan_tex[0], true);
    g_scan_on = true;
    std::lock_guard<std::mutex> lk(g_scan_m);
    g_scan_id++;
    g_scan_frames = static_cast<int>(g_scan_tex.size());
    g_scan_frame = 0;
    g_scan_grabbed = g_scan_done = false;
    g_scan_ms = nowMs();
    g_scan_out.clear();
    g_scan_why.clear();
  }
  std::string scanWhy() { std::lock_guard<std::mutex> lk(g_scan_m); return g_scan_why; }
  // Each frame, the next frame goes up once the render thread has the last one. Done means the samples are out.
  enum class ScanStep { Waiting, Done, Off };
  ScanStep scanPoll(Target& t, std::vector<ScanSample>& out, int& w, int& h) {
    int next = -1;
    {
      std::lock_guard<std::mutex> lk(g_scan_m);
      if (g_scan_frame < 0) return ScanStep::Off;
      if (g_scan_done) {
        out.swap(g_scan_out);
        w = g_scan_w;
        h = g_scan_h;
        g_scan_frame = -1;
        g_scan_done = false;
        return ScanStep::Done;
      }
      if (g_scan_grabbed && g_scan_frame + 1 < g_scan_frames) {
        next = ++g_scan_frame;
        g_scan_grabbed = false;
        g_scan_ms = nowMs();
      }
    }
    if (next >= 0) show(t, g_scan_tex[static_cast<size_t>(next)], true);
    return ScanStep::Waiting;
  }
  // Puts the fighter's own look back (on this fight's materials only). With keep, the frames stay for another run.
  void scanOff(Target* t, bool keep) {
    {
      std::lock_guard<std::mutex> lk(g_scan_m);
      g_scan_frame = -1;
      g_scan_done = false;
      g_scan_out.clear();
      g_scan_point_on = false;
    }
    if (g_scan_on && t && asw_engine::get() == g_fight) show(*t, edited(*t));
    g_scan_on = false;
    scanShowOthers();
    if (!keep) {
      for (Shown& s : g_scan_tex) unroot(s);
      g_scan_tex.clear();
      g_pick_frames.clear();
    }
    g_blink_at = Clock::now();
    g_blink_on = false;
  }

  // ---- mapping the parts: three sides, one scan each ------------------------------------
  constexpr int kViews = 3;  // front, side, back
  struct Mapping {
    std::string key;
    int view = 0;
    bool scanning = false;
    bool remap = false;  // asked for with Remap (not the first-time setup)
    float yaw0 = 0, dist = 0;
    std::vector<std::vector<std::array<float, 6>>> rays;  // per region: where it was seen from, and which way
    std::vector<std::map<std::string, int>> bone_votes;   // per region: the bones under its pixels, by name
    std::vector<float> frac;                              // per region: the most of the view it took
    Clock::time_point since;
  };
  std::unique_ptr<Mapping> g_mapping;
  std::set<std::string> g_map_tried;  // map keys mapped (or tried) this session
  std::set<std::string> g_setup_seen;  // map keys the setup has shown this session (each once, so one that never gets ready isn't gone back to)
  bool g_setup_off = false;            // the user took over (a tab, Remap, a selection or an edit): no setup until the editor is opened again
  std::atomic<bool> g_setup_busy{false};          // the setup's "please wait" is up
  std::atomic<unsigned long long> g_setup_busy_ms{0};  // when that was last worked out (an old answer doesn't count, so it can't stick)
  std::string g_setup_sig;                        // where the setup is, to tell it's moving
  Clock::time_point g_setup_moved;                // when that last changed
  std::string g_map_force;            // map this key again even though it's mapped

  // ---- Previewing a hidden model. Sol's Dragon Install outfit is only drawn during the move (its meshes
  // switched on from outside still aren't drawn), so the normal model's meshes wear it instead: the body as
  // dsl_body, the head as dsl_head, ..., with its materials, still animated ----
  bool isVisible(UObject* comp) {
    static UFunction* f = fn(STR("/Script/Engine.SceneComponent:IsVisible"));
    if (!f) return drawnLately(comp);
    alignas(8) uint8_t p[8] = {};
    comp->ProcessEvent(f, p);
    return p[0] != 0;
  }
  UObject* meshOf(UObject* c) {
    auto** m = c ? c->GetValuePtrByPropertyNameInChain<UObject*>(STR("SkeletalMesh")) : nullptr;
    return m ? *m : nullptr;
  }
  void setVisible(UObject* c, bool on) {
    static UFunction* f = fn(STR("/Script/Engine.SceneComponent:SetVisibility"));
    if (!f) return;
    alignas(8) uint8_t p[8] = {static_cast<uint8_t>(on), 0};  // bNewVisibility, bPropagateToChildren
    c->ProcessEvent(f, p);
  }
  void setMesh(UObject* c, UObject* mesh) {
    static UFunction* f = fn(STR("/Script/Engine.SkinnedMeshComponent:SetSkeletalMesh"));
    if (!f || !mesh) return;
    alignas(8) uint8_t p[16] = {};
    *reinterpret_cast<UObject**>(p) = mesh;
    p[8] = 0;  // Keep the pose, since it goes on animating
    c->ProcessEvent(f, p);
  }
  void setMaterial(UObject* c, int i, UObject* m) {
    static UFunction* f = fn(STR("/Script/Engine.PrimitiveComponent:SetMaterial"));
    if (!f || !m) return;
    struct { int32_t index; int32_t pad; UObject* mat; } p{i, 0, m};
    c->ProcessEvent(f, &p);
  }
  // Which part of a model a mesh is: "sol_body", "dsl_body" -> "body".
  std::string meshPart(UObject* mesh) {
    if (!mesh) return {};
    std::string n = narrow(mesh->GetName());
    for (auto& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const size_t u = n.find('_');
    return u == std::string::npos ? n : n.substr(u + 1);
  }
  bool samePart(const std::string& a, const std::string& b) {  // "head" and "head_low" are the same part
    if (a.empty() || b.empty()) return false;
    if (a == b) return true;
    const std::string& s = a.size() < b.size() ? a : b;
    const std::string& l = a.size() < b.size() ? b : a;
    return l.compare(0, s.size(), s) == 0 && l[s.size()] == '_';
  }
  // A material's parameters, its own and its parents' (what a slot draws with).
  // These are UMaterialInstance's arrays. Each element is an FMaterialParameterInfo (16 bytes: name, association,
  // index) followed by the value, a texture pointer (40 bytes each), a color (48) or a float (36).
  std::string paramsOf(UObject* m) {
    std::string out;
    for (int depth = 0; m && depth < 5; depth++, m = parentOf(m)) {
      out += " [" + narrow(m->GetName()) + "]";
      struct Arr { uint8_t* data; int32_t num, max; };
      if (auto* a = m->GetValuePtrByPropertyNameInChain<Arr>(STR("TextureParameterValues")))
        for (int i = 0; i < a->num && i < 32; i++) {
          const uint8_t* e = a->data + i * 40;
          UObject* tex = *reinterpret_cast<UObject* const*>(e + 16);
          out += " tex " + narrow(reinterpret_cast<const FName*>(e)->ToString()) + "=" + (tex ? narrow(tex->GetName()) : std::string("none"));
        }
      if (auto* a = m->GetValuePtrByPropertyNameInChain<Arr>(STR("VectorParameterValues")))
        for (int i = 0; i < a->num && i < 32; i++) {
          const uint8_t* e = a->data + i * 48;
          const float* c = reinterpret_cast<const float*>(e + 16);
          char buf[96];
          snprintf(buf, sizeof(buf), "=(%.2f %.2f %.2f %.2f)", c[0], c[1], c[2], c[3]);
          out += " col " + narrow(reinterpret_cast<const FName*>(e)->ToString()) + buf;
        }
    }
    return out;
  }
  // Every mesh drawn in the last half second (fighters, effects, smears, but not the stage's static meshes), with
  // its owner, mesh and each material's parameters. That's what draws something on screen. Watched for a few
  // seconds (the move is done meanwhile, since the pause can't run the editor), and each mesh and mesh asset is
  // logged once.
  Clock::time_point g_log_until;
  std::set<std::pair<UObject*, std::string>> g_logged;
  void logDrawnNow() {
    int n = 0;
    for (const TCHAR* cls : {STR("SkeletalMeshComponent"), STR("StaticMeshComponent"), STR("ParticleSystemComponent")}) {
      std::vector<UObject*> all;
      UObjectGlobals::FindAllOf(cls, all);
      for (UObject* c : all) {
        if (!drawnLately(c)) continue;
        UObject* owner = c->GetOuterPrivate();
        const std::string owner_name = owner ? narrow(owner->GetName()) : std::string("?");
        if (owner_name.find("StaticMeshActor") != std::string::npos) continue;  // the stage
        std::string mesh = "-";
        for (const TCHAR* prop : {STR("SkeletalMesh"), STR("StaticMesh"), STR("Template")})
          if (auto** m = c->GetValuePtrByPropertyNameInChain<UObject*>(prop); m && *m) { mesh = narrow((*m)->GetName()); break; }
        if (!g_logged.insert({c, mesh}).second) continue;  // (already logged)
        COMBO_LOG("drawn: " + narrow(c->GetClassPrivate() ? c->GetClassPrivate()->GetName() : StringType()) + " " + narrow(c->GetName()) + " of " +
                  owner_name + ", mesh " + mesh);
        for (int i = 0, k = numMaterials(c); i < k && i < 16; i++) {
          UObject* m = material(c, i);
          if (!m) continue;
          COMBO_LOG("drawn:   slot " + std::to_string(i) + " " + narrow(m->GetName()) + paramsOf(m));
        }
        struct A { UObject** data; int32_t num, max; };
        if (auto* em = c->GetValuePtrByPropertyNameInChain<A>(STR("EmitterMaterials")))
          for (int i = 0; i < em->num && i < 16; i++)
            if (UObject* m = em->data[i])
              COMBO_LOG("drawn:   emitter " + std::to_string(i) + " " + narrow(m->GetClassPrivate() ? m->GetClassPrivate()->GetName() : StringType()) + " " +
                        narrow(m->GetName()) + paramsOf(m));
        if (++n >= 60) return;  // The rest on the next update
      }
    }
  }
  struct Dressed {
    UObject* comp = nullptr;    // a mesh of the normal model, on screen
    UObject* mesh = nullptr;    // its own mesh
    std::vector<UObject*> mats; // and materials
    UObject* hidden = nullptr;  // the hidden model's mesh it wears now
  };
  struct Preview {
    UObject* pawn = nullptr;
    std::vector<Dressed> dressed;
    std::vector<UObject*> held;  // The normal model's materials and meshes, off every mesh meanwhile, kept from garbage collection
  } g_preview;
  void hold(UObject* o) {
    // The game's own rooted ones stay the game's. FUObjectItem's flags are at +8 (RootSet 1 << 30), read directly
    // because the installed UE4SS doesn't export IsRootSet.
    if (!o || !o->GetObjectItem() || (*reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(o->GetObjectItem()) + 8) & (1 << 30))) return;
    if (std::find(g_preview.held.begin(), g_preview.held.end(), o) != g_preview.held.end()) return;
    o->GetObjectItem()->SetRootSet();
    g_preview.held.push_back(o);
  }
  bool hiddenPart(const Target& t) { return t.kept_hidden; }
  UObject* standIn(UObject* hidden) {  // the mesh on screen wearing this hidden one, if any
    for (const Dressed& d : g_preview.dressed) if (d.hidden == hidden) return d.comp;
    return nullptr;
  }
  // The normal meshes the game hides while the hidden model is on screen (the move itself), by mesh name. Seen
  // once and kept per character. The preview swaps exactly those, and the rest stays (Ky's legs).
  std::map<std::string, std::set<std::string>> g_move_hides;  // by character
  std::set<std::string> g_move_tried;                         // characters whose file was read
  std::filesystem::path moveFile(const std::string& chr) { return g_dir / "cache" / chr / "move_hides.txt"; }
  const std::set<std::string>* moveHides(const std::string& chr) {
    if (g_move_tried.insert(chr).second) {
      std::ifstream f(moveFile(chr));
      std::string line;
      while (std::getline(f, line)) if (!line.empty()) g_move_hides[chr].insert(line);
    }
    auto it = g_move_hides.find(chr);
    return it != g_move_hides.end() && !it->second.empty() ? &it->second : nullptr;
  }
  struct Watch {  // a fighter's move being watched
    std::set<std::string> before;            // their normal meshes on screen the update before it began
    std::map<std::string, int> hidden;       // During it, how many updates each of those was hidden
    int frames = 0;
  };
  std::map<UObject*, Watch> g_watch;  // by pawn
  // Materials the game puts on a recolored part's meshes during play. Chipp's taunt draws his body dissolving with
  // one, and it showed the game's colors. One that's dynamic and draws the part's own texture is kept here, apart
  // from the part: each update, while it's alive, it wears what the part wears now (ours, or the game's own after a
  // reset), and once it's gone it's dropped. Logged when found.
  struct Swap { Kept mat; std::string key; UObject* base = nullptr; UObject* sss = nullptr; bool part_sss = true; };  // (its own textures; part_sss: it shares the part's shadow texture, as in show())
  std::vector<Swap> g_swaps;
  std::map<std::pair<std::string, UObject*>, Kept> g_swap_seen;  // (part, material) looked at, while that material lives
  std::set<std::string> g_swap_primed;                           // Parts whose materials at the start were recorded (discover's)
  void swapWatch() {
    for (auto it = g_swap_seen.begin(); it != g_swap_seen.end();) it = it->second.alive() ? std::next(it) : g_swap_seen.erase(it);  // (its address can be reused)
    g_swaps.erase(std::remove_if(g_swaps.begin(), g_swaps.end(), [](const Swap& w) { return !w.mat.alive(); }), g_swaps.end());
    for (Target& t : g_targets) {
      if (t.mids.empty() || t.orig.empty()) continue;
      bool live = t.items.size() == t.mids.size() + t.comps.size();  // (its meshes may be gone before a new fight is noticed)
      for (size_t j = 0; live && j < t.comps.size(); j++) live = Kept{t.comps[j], t.items[t.mids.size() + j]}.alive();
      if (!live) continue;
      const std::string key = keyOf(t);
      const bool prime = g_swap_primed.insert(key).second;
      for (UObject* comp : t.comps)
        for (int i = 0, n = numMaterials(comp); i < n && i < 32; i++) {
          UObject* m = material(comp, i);
          if (!m || g_swap_seen.count({key, m})) continue;
          g_swap_seen[{key, m}] = keep(m);  // (alive: it was just read off the mesh)
          if (prime || !isDynamic(m) || outlineOnly(m)) continue;
          if (std::any_of(g_targets.begin(), g_targets.end(), [&](const Target& o) { return std::find(o.mids.begin(), o.mids.end(), m) != o.mids.end(); })) continue;
          if (std::any_of(g_swaps.begin(), g_swaps.end(), [&](const Swap& w) { return w.mat.obj == m; })) continue;  // (another part's already)
          UObject* b = getTexture(m, STR("BaseMap"));
          const auto own = std::find_if(t.orig.begin(), t.orig.end(), [&](const auto& o) { return b && o.first == b; });
          if (own == t.orig.end()) continue;  // Another texture: not this part's
          g_swaps.push_back({keep(m), key, own->first, own->second, own->second == t.orig.front().second});
          COMBO_LOG("color editor: " + key + ": the game put " + narrow(m->GetName()) + " (" + (parentOf(m) ? narrow(parentOf(m)->GetName()) : std::string("?")) +
                    ") on " + narrow(comp->GetName()) + " slot " + std::to_string(i) + ", drawing the part's texture: it wears the recolor while it's there");
        }
    }
    for (const Swap& w : g_swaps) {  // What the part wears now (ours, or its own when the part is on the game's)
      auto sh = g_shown.find(w.key);
      UObject* want_b = sh != g_shown.end() && sh->second.base ? sh->second.base : w.base;
      UObject* want_s = sh != g_shown.end() && sh->second.sss ? sh->second.sss : w.sss;
      if (getTexture(w.mat.obj, STR("BaseMap")) != want_b) setTexture(w.mat.obj, STR("BaseMap"), want_b);
      if (w.part_sss && want_s && getTexture(w.mat.obj, STR("SssMap")) != want_s) setTexture(w.mat.obj, STR("SssMap"), want_s);
    }
  }
  void learnMove() {  // Each update, learns from the game showing a hidden model itself, never our preview (that's off here)
    if (g_preview.pawn) return;
    for (const Target& h : g_targets) {
      if (!h.kept_hidden || moveHides(h.chr)) continue;
      std::vector<UObject*> normal;  // The pawn's normal meshes, meaning the parts that aren't kept hidden
      for (const Target& o : g_targets)
        if (o.pawn == h.pawn && !o.kept_hidden)
          for (UObject* c : o.comps) if (meshOf(c) && std::find(normal.begin(), normal.end(), c) == normal.end()) normal.push_back(c);
      Watch& w = g_watch[h.pawn];
      const bool on = std::any_of(h.comps.begin(), h.comps.end(), drawnLately);
      if (!on) {
        if (w.frames >= 5) {  // The move is over, so record what stayed hidden for most of it
          std::set<std::string> hides;
          for (const auto& [n, k] : w.hidden) if (k * 2 > w.frames) hides.insert(n);
          if (!hides.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(moveFile(h.chr).parent_path(), ec);
            std::ofstream f(moveFile(h.chr), std::ios::trunc);
            std::string names;
            for (const std::string& n : hides) { f << n << "\n"; names += " " + n; }
            g_move_hides[h.chr] = hides;
            COMBO_LOG("color editor: learned " + h.chr + "'s move: it hides" + names);
          }
        }
        w = Watch{};
        for (UObject* c : normal) if (isVisible(c)) w.before.insert(narrow(meshOf(c)->GetName()));
        continue;
      }
      w.frames++;
      for (UObject* c : normal) {
        const std::string n = narrow(meshOf(c)->GetName());
        if (w.before.count(n) && !isVisible(c)) w.hidden[n]++;
      }
    }
  }
  void dress(const Dressed& d) {
    if (!d.hidden) { if (isVisible(d.comp)) setVisible(d.comp, false); return; }  // Left out of the preview, so it's hidden
    UObject* hm = meshOf(d.hidden);
    if (hm && meshOf(d.comp) != hm) setMesh(d.comp, hm);
    for (int i = 0, n = numMaterials(d.hidden); i < n; i++)
      if (UObject* m = material(d.hidden, i); m && material(d.comp, i) != m) setMaterial(d.comp, i, m);
  }
  void keepPreview() {  // Called each update, before and after the game's own, because the game may put its own back
    for (const Dressed& d : g_preview.dressed) dress(d);
  }
  void startPreview(const Target& t) {
    g_preview = Preview{};
    g_preview.pawn = t.pawn;
    std::vector<UObject*> shown;  // The normal model's meshes on screen that the move hides, once that's known
    const std::set<std::string>* hides = moveHides(t.chr);
    // A head's close-up and battle versions count as one. The move learned "sol_head_high" from its close-up,
    // but the battle shows "sol_head_low", which is the one Dragon Install's head goes on.
    const auto lod = [](std::string n) {
      for (const char* tail : {"_high", "_low"})
        if (n.size() > std::strlen(tail) && n.compare(n.size() - std::strlen(tail), std::string::npos, tail) == 0) { n.resize(n.size() - std::strlen(tail)); break; }
      return n;
    };
    std::set<std::string> hidden_by_move;
    if (hides) for (const std::string& n : *hides) hidden_by_move.insert(lod(n));
    for (UObject* c : fighterParts())
      if (c->GetOuterPrivate() == t.pawn && isVisible(c) && meshOf(c) && (!hides || hidden_by_move.count(lod(narrow(meshOf(c)->GetName())))))
        shown.push_back(c);
    int unmatched = 0;
    {  // What's on screen and what's hidden, every time
      std::string names;
      for (UObject* c : shown) names += " " + (meshOf(c) ? narrow(meshOf(c)->GetName()) : std::string("?"));
      COMBO_LOG("color editor: preview, on screen:" + names);
    }
    {
      std::vector<UObject*> hiddens;  // The hidden model's meshes, plus its other pieces that the game files under another part
      std::set<std::string> prefixes;  // Such as "dky_", the hidden model's name
      auto lower = [](UObject* mesh) { std::string n = narrow(mesh->GetName()); for (auto& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); return n; };
      for (const Target& o : g_targets)
        if (o.pawn == t.pawn && o.kept_hidden)
          for (UObject* h : o.comps) {
            hiddens.push_back(h);
            if (UObject* mesh = meshOf(h)) { const std::string n = lower(mesh); if (n.find('_') != std::string::npos) prefixes.insert(n.substr(0, n.find('_') + 1)); }
          }
      for (UObject* c : fighterParts())  // A prefix the normal model uses too, like "kyk_", isn't the hidden model's own name
        if (c->GetOuterPrivate() == t.pawn && meshOf(c) && isVisible(c)) {
          const std::string n = lower(meshOf(c));
          if (n.find('_') != std::string::npos) prefixes.erase(n.substr(0, n.find('_') + 1));
        }
      for (UObject* c : fighterParts())  // A head drawn with the body's own material, like Ky's dky_head (his dragon eye)
        if (c->GetOuterPrivate() == t.pawn && meshOf(c) && !isVisible(c) && std::find(hiddens.begin(), hiddens.end(), c) == hiddens.end()) {
          const std::string n = lower(meshOf(c));
          if (std::any_of(prefixes.begin(), prefixes.end(), [&](const std::string& p) { return n.rfind(p, 0) == 0; })) hiddens.push_back(c);
        }
      for (UObject* h : hiddens) {
        if (std::any_of(g_preview.dressed.begin(), g_preview.dressed.end(), [&](const Dressed& d) { return d.hidden == h; })) continue;  // Already worn
        const std::string part = meshPart(meshOf(h));
        UObject* pick = nullptr;  // Same part by name ("body"), else a variant ("head_low"), but not Ky's "body_cloth" if "body" is there
        const auto free = [&](UObject* c) { return !std::any_of(g_preview.dressed.begin(), g_preview.dressed.end(), [&](const Dressed& d) { return d.comp == c; }); };
        for (UObject* c : shown)
          if (!pick && meshPart(meshOf(c)) == part && free(c)) pick = c;
        for (UObject* c : shown)
          if (!pick && samePart(meshPart(meshOf(c)), part) && free(c)) pick = c;
        if (part.empty() || !pick) {
          unmatched++;
          std::string names;  // What's on screen to pair it with
          for (UObject* c : shown) names += " " + (meshOf(c) ? narrow(meshOf(c)->GetName()) : std::string("?"));
          COMBO_LOG("color editor: " + (meshOf(h) ? narrow(meshOf(h)->GetName()) : std::string("?")) + " has no match among:" + names);
          continue;
        }
        Dressed d;
        d.comp = pick;
        d.mesh = meshOf(pick);
        for (int i = 0, n = numMaterials(pick); i < n; i++) d.mats.push_back(material(pick, i));
        hold(d.mesh);
        for (UObject* m : d.mats) hold(m);
        d.hidden = h;
        g_preview.dressed.push_back(d);
        dress(d);
        COMBO_LOG("color editor: " + narrow(d.mesh->GetName()) + " wears " + narrow(meshOf(h)->GetName()));
        for (int i = 0, n = numMaterials(h); i < n; i++) {  // What each slot of the hidden model is
          UObject* m = material(h, i);
          std::string src, kind;
          UObject* base = m ? colorSource(m, src, kind) : nullptr;
          if (m) COMBO_LOG("color editor:   slot " + std::to_string(i) + " params:" + paramsOf(m));
          COMBO_LOG("color editor:   slot " + std::to_string(i) + ": " + (m ? narrow(m->GetFullName()) : std::string("none")) +
                    (base ? " (color material " + kind + ")" : std::string(" (not a color material)")) +
                    (getTexture(m, STR("BaseMap")) ? " BaseMap " + narrow(getTexture(m, STR("BaseMap"))->GetName()) : std::string()));
        }
      }
    }
    if (unmatched) COMBO_LOG("color editor: " + std::to_string(unmatched) + " hidden meshes have no match on screen");
    if (hides)
      for (UObject* c : shown)
        if (std::none_of(g_preview.dressed.begin(), g_preview.dressed.end(), [&](const Dressed& d) { return d.comp == c; })) {
          Dressed d;  // Wears nothing. It's hidden while previewing, as the move does
          d.comp = c;
          g_preview.dressed.push_back(d);
          setVisible(c, false);
        }
    g_bones_for.clear();  // The bones belong to the stand-ins now
  }
  // Puts everything back as the game had it. Pass on_stage = false when the meshes may be gone (another fight),
  // and then it only lets go.
  void stopPreview(bool on_stage) {
    if (!g_preview.pawn) return;
    if (on_stage)
      for (const Dressed& d : g_preview.dressed) {
        if (!d.hidden) { setVisible(d.comp, true); continue; }  // It was on screen when the preview began
        setMesh(d.comp, d.mesh);
        for (size_t i = 0; i < d.mats.size(); i++) setMaterial(d.comp, static_cast<int>(i), d.mats[i]);
      }
    for (UObject* o : g_preview.held)  // Back on their meshes now, or let go with the fight
      if (o->GetObjectItem()) o->GetObjectItem()->UnsetRootSet();
    g_preview = Preview{};
    g_bones_for.clear();
  }

  // A fighter's parts, as target indices.
  // ---- Where the model draws. The meshes' UV triangles (cache/<CHR>/uv.bin, made from FModel's mesh exports
  // by make_uv.py). A region the model never draws is left out of the list. --------------------------------
  struct UvSlot { std::string mesh, name; int index = 0; std::vector<float> tri; };  // u,v x3 per triangle
  std::vector<UvSlot> readUv(const std::filesystem::path& file) {  // (worker) Empty if there's none or it can't be read
    std::vector<UvSlot> out;
    const std::vector<uint8_t> b = readBytes(file);
    size_t o = 0;
    auto take = [&](void* d, size_t n) { if (n > b.size() - o) return false; std::memcpy(d, b.data() + o, n); o += n; return true; };
    auto str = [&](std::string& s) {
      uint16_t n = 0;
      if (!take(&n, 2) || n > b.size() - o) return false;
      s.assign(reinterpret_cast<const char*>(b.data() + o), n);
      o += n;
      return true;
    };
    uint32_t magic = 0, ver = 0, meshes = 0;
    if (!take(&magic, 4) || magic != 0x56554C53 /* "SLUV" */ || !take(&ver, 4) || ver != 1 || !take(&meshes, 4) || meshes > 4096) return {};
    for (uint32_t m = 0; m < meshes; m++) {
      std::string mesh;
      uint32_t slots = 0;
      if (!str(mesh) || !take(&slots, 4) || slots > 256) return {};
      for (uint32_t s = 0; s < slots; s++) {
        UvSlot u;
        u.mesh = mesh;
        int32_t idx = 0;
        uint32_t n = 0;
        if (!str(u.name) || !take(&idx, 4) || !take(&n, 4) || n > (b.size() - o) / 24) return {};
        u.index = idx;
        u.tri.resize(static_cast<size_t>(n) * 6);
        if (!take(u.tri.data(), static_cast<size_t>(n) * 24)) return {};
        out.push_back(std::move(u));
      }
    }
    return out;
  }
  struct Drawn { bool ok = false; std::vector<uint32_t> count, shown; std::set<std::string> slots; std::vector<int> parent; std::vector<std::pair<int, int>> touch; int variety = 99; };
  // How many different textures a part's color slots give, base and shadow together. Compared by pixels, since
  // the engine's PNGs differ in bytes for the same picture.
  int textureVariety(const std::vector<std::filesystem::path>& files) {  // (worker) files go base, shadow, base, shadow...
    std::set<uint64_t> seen;
    for (size_t i = 0; i + 1 < files.size(); i += 2) {
      uint64_t h = 1469598103934665603ull;
      for (size_t k = i; k < i + 2; k++) {
        Image im;
        if (!decodePng(files[k], im)) return 99;
        h = (h ^ (static_cast<uint64_t>(im.w) << 32 | static_cast<uint32_t>(im.h))) * 1099511628211ull;
        for (size_t p = 0; p < static_cast<size_t>(im.w) * im.h; p++) h = (h ^ rgbAt(im, p)) * 1099511628211ull;
      }
      seen.insert(h);
    }
    return seen.empty() ? 99 : static_cast<int>(seen.size());
  }
  // (worker) Counts the texels the part's material slots draw, per region. slots holds the names known to wear
  // the part. seen holds the (mesh, material index) pairs wearing it now, and their names join slots, so props
  // not shown now count too. hidden holds the mesh name prefixes of a model the game keeps hidden ("dky_").
  // What only that model draws is counted separately.
  Drawn countDrawn(std::filesystem::path file, std::set<std::string> slots, std::vector<std::pair<std::string, int>> seen, std::shared_ptr<CharMap> m,
                    std::vector<std::filesystem::path> pngs, std::filesystem::path kept, std::vector<std::string> hidden) {
    Drawn d;
    if (!pngs.empty()) {  // Worked out now and kept for next time
      d.variety = textureVariety(pngs);
      if (d.variety != 99) { std::ofstream f(kept, std::ios::trunc); f << d.variety << '\n'; }
    } else {
      std::ifstream f(kept);
      if (!(f >> d.variety) || d.variety < 1 || d.variety > 99) d.variety = 99;  // Unreadable or half written, so treat it as many
    }
    const std::vector<UvSlot> uv = readUv(file);
    for (const auto& [mesh, i] : seen)
      for (const UvSlot& u : uv)
        if (u.index == i && _stricmp(u.mesh.c_str(), mesh.c_str()) == 0) slots.insert(u.name);
    d.slots = slots;
    if (!m || m->w <= 0 || m->h <= 0 || m->owner.size() != static_cast<size_t>(m->w) * m->h) return d;
    const int W = m->w, H = m->h;
    {  // Each small region's parent is the big region it touches most, searched 1, 2, 4, then 8 texels away
      const int maj = m->major, RR = m->regions;
      d.parent.assign(static_cast<size_t>((std::max)(0, RR)), -1);
      if (maj >= 0 && maj < RR) {
        std::vector<std::unordered_map<int, uint32_t>> touch(static_cast<size_t>(RR - maj));
        for (int rad : {1, 2, 4, 8}) {
          for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
              const int o = m->owner[static_cast<size_t>(y) * W + x];
              if (o < maj || o >= RR || d.parent[static_cast<size_t>(o)] >= 0) continue;
              const int nx[4] = {x - rad, x + rad, x, x}, ny[4] = {y, y, y - rad, y + rad};
              for (int k = 0; k < 4; k++) {
                if (nx[k] < 0 || ny[k] < 0 || nx[k] >= W || ny[k] >= H) continue;
                const int b = m->owner[static_cast<size_t>(ny[k]) * W + nx[k]];
                if (b < maj) touch[static_cast<size_t>(o - maj)][b]++;
              }
            }
          for (int o = maj; o < RR; o++) {
            auto& tc = touch[static_cast<size_t>(o - maj)];
            if (d.parent[static_cast<size_t>(o)] >= 0 || tc.empty()) continue;
            int best = -1;
            uint32_t most = 0;
            for (const auto& [b, n] : tc) if (n > most || (n == most && b < best)) { most = n; best = b; }
            d.parent[static_cast<size_t>(o)] = best;
            tc.clear();
          }
        }
      }
    }
    {  // Regions touching each other within 2 texels, like a gradient's steps or rows side by side
      const int maj = m->major, RR = m->regions;
      std::set<uint32_t> pairs;
      for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
          const int o = m->owner[static_cast<size_t>(y) * W + x];
          if (o >= RR) continue;
          const int nx[4] = {x + 1, x + 2, x, x}, ny[4] = {y, y, y + 1, y + 2};
          for (int k = 0; k < 4; k++) {
            if (nx[k] >= W || ny[k] >= H) continue;
            const int b = m->owner[static_cast<size_t>(ny[k]) * W + nx[k]];
            if (b < RR && b != o) pairs.insert(static_cast<uint32_t>((std::min)(o, b)) << 16 | static_cast<uint32_t>((std::max)(o, b)));
          }
        }
      for (uint32_t p : pairs) d.touch.push_back({static_cast<int>(p >> 16), static_cast<int>(p & 0xFFFF)});
    }
    if (uv.empty() || slots.empty()) return d;
    std::vector<uint8_t> mask(static_cast<size_t>(W) * H, 0);  // Bit 0 means drawn, bit 1 means drawn by a mesh that isn't the hidden model's
    for (const UvSlot& u : uv) {
      if (!slots.count(u.name)) continue;
      std::string lm = u.mesh;
      for (auto& ch : lm) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      const uint8_t bit = std::any_of(hidden.begin(), hidden.end(), [&](const std::string& p) { return lm.rfind(p, 0) == 0; }) ? 1 : 3;
      for (size_t k = 0; k + 5 < u.tri.size(); k += 6) {
        float x[3], y[3];
        bool ok = true;
        for (int j = 0; j < 3; j++) {
          x[j] = u.tri[k + 2 * j] * W;
          y[j] = u.tri[k + 2 * j + 1] * H;
          ok = ok && std::isfinite(x[j]) && std::isfinite(y[j]) && std::fabs(x[j]) < 1e6f && std::fabs(y[j]) < 1e6f;
        }
        if (!ok) continue;
        {  // UVs past 0..1 repeat. A triangle fully in another tile moves back, one across an edge stays clipped
          const float sx = std::floor((std::min)({x[0], x[1], x[2]}) / W), sy = std::floor((std::min)({y[0], y[1], y[2]}) / H);
          if (sx == std::floor((std::max)({x[0], x[1], x[2]}) / W)) for (int j = 0; j < 3; j++) x[j] -= sx * W;
          if (sy == std::floor((std::max)({y[0], y[1], y[2]}) / H)) for (int j = 0; j < 3; j++) y[j] -= sy * H;
        }
        for (int j = 0; j < 3; j++) {  // A triangle smaller than a texel still marks the texel it's in
          const int px = static_cast<int>(std::floor(x[j])), py = static_cast<int>(std::floor(y[j]));
          if (px >= 0 && py >= 0 && px < W && py < H) mask[static_cast<size_t>(py) * W + px] |= bit;
        }
        const int x0 = (std::max)(0, static_cast<int>(std::floor((std::min)({x[0], x[1], x[2]})))), x1 = (std::min)(W - 1, static_cast<int>(std::ceil((std::max)({x[0], x[1], x[2]}))));
        const int y0 = (std::max)(0, static_cast<int>(std::floor((std::min)({y[0], y[1], y[2]})))), y1 = (std::min)(H - 1, static_cast<int>(std::ceil((std::max)({y[0], y[1], y[2]}))));
        const float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
        if (x0 > x1 || y0 > y1 || area == 0) continue;
        const float sg = area < 0 ? -1.0f : 1.0f;
        for (int py = y0; py <= y1; py++)
          for (int px = x0; px <= x1; px++) {  // texel centres inside the triangle
            const float cx = px + 0.5f, cy = py + 0.5f;
            const float e0 = sg * ((x[1] - x[0]) * (cy - y[0]) - (y[1] - y[0]) * (cx - x[0]));
            const float e1 = sg * ((x[2] - x[1]) * (cy - y[1]) - (y[2] - y[1]) * (cx - x[1]));
            const float e2 = sg * ((x[0] - x[2]) * (cy - y[2]) - (y[0] - y[2]) * (cx - x[2]));
            if (e0 >= 0 && e1 >= 0 && e2 >= 0) mask[static_cast<size_t>(py) * W + px] |= bit;
          }
      }
    }
    {  // Two texels wider, because filtering reads a little past a triangle's edge
      std::vector<uint8_t> tmp(mask.size(), 0);
      for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
          if (const uint8_t v = mask[static_cast<size_t>(y) * W + x])
            for (int dx = (std::max)(0, x - 2); dx <= (std::min)(W - 1, x + 2); dx++) tmp[static_cast<size_t>(y) * W + dx] |= v;
      std::fill(mask.begin(), mask.end(), 0);
      for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
          if (const uint8_t v = tmp[static_cast<size_t>(y) * W + x])
            for (int dy = (std::max)(0, y - 2); dy <= (std::min)(H - 1, y + 2); dy++) mask[static_cast<size_t>(dy) * W + x] |= v;
    }
    d.count.assign(static_cast<size_t>(m->regions), 0);
    d.shown.assign(static_cast<size_t>(m->regions), 0);
    for (size_t p = 0; p < mask.size(); p++) {
      const uint16_t r = m->owner[p];
      if (r >= m->regions) continue;
      if (mask[p] & 1) d.count[r]++;
      if (mask[p] & 2) d.shown[r]++;
    }
    d.ok = true;
    return d;
  }
  std::future<Drawn> g_drawn_job;
  std::string g_drawn_job_key;
  const CharMap* g_drawn_job_map = nullptr;
  std::filesystem::path g_drawn_job_slots;
  std::filesystem::path g_drawn_job_variety;  // The part's variety_<KIND>.txt
  std::string g_drawn_job_only;               // The hidden model's part name, or "" for none
  const std::vector<uint32_t>* drawnOf(const Target& t) {  // Null unless known for the map in use
    auto mp = g_maps.find(mapKey(t));
    auto dr = g_drawn.find(mapKey(t));
    if (mp == g_maps.end() || dr == g_drawn.end() || dr->second.map != mp->second.get() || dr->second.count.size() != static_cast<size_t>(mp->second->regions)) return nullptr;
    return &dr->second.count;
  }
  const std::vector<int>* parentsOf(const Target& t) {  // Null unless worked out for the map in use
    auto mp = g_maps.find(mapKey(t));
    auto dr = g_drawn.find(mapKey(t));
    if (mp == g_maps.end() || dr == g_drawn.end() || dr->second.map != mp->second.get() || dr->second.parent.size() != static_cast<size_t>(mp->second->regions)) return nullptr;
    return &dr->second.parent;
  }
  // The mesh name prefixes of a model the game keeps hidden that shares this part's meshes ("dky_", not the
  // character's own "kyk_"), and that hidden part's name. Returns none for a hidden part itself.
  std::vector<std::string> hiddenPrefixes(const Target& t, std::string* only) {
    std::set<std::string> pre;
    std::string name;
    if (!t.kept_hidden)
      for (const Target& o : g_targets)
        if (o.pawn == t.pawn && o.kept_hidden && o.kind != t.kind)
          for (UObject* h : o.comps)
            if (UObject* mesh = meshOf(h)) {
              std::string n = narrow(mesh->GetName());
              for (auto& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
              if (n.find('_') != std::string::npos && pre.insert(n.substr(0, n.find('_') + 1)).second && name.empty()) name = partName(o.kind);
            }
    std::string own = codeOf(t.chr) + "_";
    for (auto& ch : own) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    pre.erase(own);
    if (only) *only = pre.empty() ? std::string() : name;
    return {pre.begin(), pre.end()};
  }
  uint32_t drawnMin(const CharMap& m) { return (std::max)(8u, static_cast<uint32_t>(static_cast<size_t>(m.w) * m.h / 131072)); }  // 32 texels at 2048
  void drawnTick(const Target& t) {  // (game thread) Works out the part's drawn texels once per map, on a worker
    if (g_drawn_job.valid()) {
      if (g_drawn_job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
      Drawn d = g_drawn_job.get();
      if (!d.slots.empty()) {  // The learned slot names are kept, so props seen once count from then on
        std::error_code ec;
        std::filesystem::create_directories(g_drawn_job_slots.parent_path(), ec);
        std::ofstream f(g_drawn_job_slots, std::ios::trunc);
        for (const std::string& s : d.slots) f << s << "\n";
      }
      auto now_map = g_maps.find(g_drawn_job_key);
      if (now_map != g_maps.end() && now_map->second.get() == g_drawn_job_map) {  // Not for a map that was dropped meanwhile
        if (!d.ok) { d.count.clear(); d.shown.clear(); }  // Parents only, since there's no UV file or nothing has been seen wearing the part yet
        g_drawn[g_drawn_job_key] = {g_drawn_job_map, std::move(d.count), std::move(d.parent), std::move(d.touch), d.variety, std::move(d.shown), g_drawn_job_only};
        g_view_dirty = true;
      }
      return;
    }
    const std::string key = mapKey(t);
    auto mp = g_maps.find(key);
    if (mp == g_maps.end() || !mp->second || mp->second->regions <= 0) return;
    auto tr = g_drawn_tried.find(key);
    if (tr != g_drawn_tried.end() && Clock::now() - tr->second < std::chrono::seconds(10)) return;
    g_drawn_tried[key] = Clock::now();
    std::vector<std::pair<std::string, int>> seen;  // (mesh, material index) wearing the part now
    for (UObject* c : t.comps) {
      UObject* mesh = meshOf(c);
      if (!mesh) continue;
      const int n = numMaterials(c);
      for (int i = 0; i < n && i < 64; i++)
        if (std::string n0, k0; std::find(t.mids.begin(), t.mids.end(), material(c, i)) != t.mids.end() &&
                                !(outlineOnly(material(c, i)) && !colorSource(material(c, i), n0, k0)))  // A true outline's own triangles are a placeholder
          seen.push_back({narrow(mesh->GetName()), i});
    }
    std::set<std::string> seen_keys;
    for (const auto& [m, i] : seen) seen_keys.insert(m + "#" + std::to_string(i));
    auto& counted = g_drawn_seen[key];
    const bool fresh = std::any_of(seen_keys.begin(), seen_keys.end(), [&](const std::string& s) { return !counted.count(s); });
    std::error_code ec;
    const std::filesystem::path file = g_dir / "cache" / t.chr / "uv.bin";
    const bool have_file = std::filesystem::exists(file, ec);  // Looked for again after a while, since make_uv.py may make it later
    auto had = g_drawn.find(key);
    const bool have = had != g_drawn.end() && had->second.map == mp->second.get();  // This map's parents, and maybe its counts
    if (have && !(have_file && (fresh || !drawnOf(t)))) return;  // Done. Only redo it for a new mesh wearing it, or once the UV file shows up
    if (have_file) counted.insert(seen_keys.begin(), seen_keys.end());
    const std::filesystem::path slots_file = g_dir / "cache" / t.chr / ("uvslots_" + t.kind + ".txt");
    std::set<std::string> slots;
    {
      std::ifstream f(slots_file);
      std::string line;
      while (std::getline(f, line)) if (!line.empty()) slots.insert(line);
    }
    g_drawn_job_key = key;
    g_drawn_job_map = mp->second.get();
    g_drawn_job_slots = slots_file;
    std::vector<std::filesystem::path> pngs;  // How many of the part's color slot textures differ. Worked out once, then kept in a file
    {
      const std::filesystem::path colors = g_dir / "cache" / t.chr / ("colors_" + t.kind + ".txt"), kept = g_dir / "cache" / t.chr / ("variety_" + t.kind + ".txt");
      std::error_code e1, e2;
      const auto tc = std::filesystem::last_write_time(colors, e1), tk = std::filesystem::last_write_time(kept, e2);
      if (e2 || e1 || tk < tc) {
        std::ifstream f(colors);
        std::string pre;
        while (std::getline(f, pre))
          if (!pre.empty()) { pngs.push_back(g_dir / "cache" / t.chr / (pre + "_BaseMap.png")); pngs.push_back(g_dir / "cache" / t.chr / (pre + "_SssMap.png")); }
      }
      g_drawn_job_variety = kept;
    }
    g_drawn_job_only.clear();
    std::vector<std::string> hidden = hiddenPrefixes(t, &g_drawn_job_only);
    g_drawn_job = std::async(std::launch::async, countDrawn, file, std::move(slots), std::move(seen), mp->second, std::move(pngs), g_drawn_job_variety, std::move(hidden));
  }
  // ---- Rows. Regions that look exactly the same in the shown color (and carry the same edit) are one row, and
  // an edit to it goes to all of them (they differ only in other colors). Drawn ones lead. ------------------
  bool g_group_same = true;
  std::vector<int> leadersOf(const Target& t, int R) {
    std::vector<int> lead(static_cast<size_t>((std::max)(0, R)));
    for (int r = 0; r < R; r++) lead[static_cast<size_t>(r)] = r;
    if (!g_group_same || !g_worn || g_worn_key != keyOf(t)) return lead;
    const auto& ed = g_edits[keyOf(t)];
    const std::vector<uint32_t>* drawn = drawnOf(t);
    if (drawn && drawn->size() < static_cast<size_t>(R)) drawn = nullptr;  // Not used if counted for a map with fewer regions than this view's
    auto mp = g_maps.find(mapKey(t));
    const uint32_t least = mp != g_maps.end() ? drawnMin(*mp->second) : 32;
    std::map<std::tuple<uint32_t, uint32_t, int, uint32_t, uint32_t>, int> first;  // look and edit -> leading region
    const int n = (std::min)(R, static_cast<int>((std::min)(g_worn->lit.size(), g_worn->shadow.size())));
    for (int pass = 0; pass < 2; pass++)
      for (int r = 0; r < n; r++) {
        const bool on_model = !drawn || (*drawn)[static_cast<size_t>(r)] >= least;
        if (on_model != (pass == 0)) continue;
        const Edit e = r < static_cast<int>(ed.size()) ? ed[static_cast<size_t>(r)] : Edit{};
        const auto k = std::make_tuple(g_worn->lit[static_cast<size_t>(r)], g_worn->shadow[static_cast<size_t>(r)],
                                       (e.has_lit ? 1 : 0) | (e.has_shadow ? 2 : 0) | (e.derive ? 4 : 0), e.has_lit ? e.lit : 0u, e.has_shadow ? e.shadow : 0u);
        lead[static_cast<size_t>(r)] = first.emplace(k, r).first->second;
      }
    // A gradient painted in small steps (Sol's iris) is one row too. Small regions that touch, each within 48 of
    // the next in this color (lit and shadow), join up. Only small regions in small rows count, so a big region
    // never joins.
    auto dr = g_drawn.find(mapKey(t));
    if (drawn && mp != g_maps.end() && dr != g_drawn.end() && dr->second.map == mp->second.get()) {  // Only once the drawn texels are known, otherwise it would chain past the model
      const int maj = mp->second->major;
      std::vector<int> up(static_cast<size_t>(R));
      for (int r = 0; r < R; r++) up[static_cast<size_t>(r)] = r;
      auto root = [&](int r) {
        while (up[static_cast<size_t>(r)] != r) { up[static_cast<size_t>(r)] = up[static_cast<size_t>(up[static_cast<size_t>(r)])]; r = up[static_cast<size_t>(r)]; }
        return r;
      };
      auto close_to = [](uint32_t a, uint32_t b) {  // Not "near", which is a Windows macro
        for (int sh : {16, 8, 0}) if (std::abs(static_cast<int>(a >> sh & 0xFF) - static_cast<int>(b >> sh & 0xFF)) > 48) return false;
        return true;
      };
      for (int r = 0; r < R; r++) up[static_cast<size_t>(r)] = lead[static_cast<size_t>(r)];  // The exact twins first
      bool any = false;
      // When a part's slots are nearly all one picture, its regions are that picture's own shades, big ones too.
      const bool one_picture = dr->second.variety <= 2;
      for (const auto& [a, b] : dr->second.touch) {
        if (a >= n || b >= n) continue;
        if (!one_picture && (a < maj || b < maj)) continue;
        if (!one_picture && (lead[static_cast<size_t>(a)] < maj || lead[static_cast<size_t>(b)] < maj)) continue;  // Skipped when either is in a big region's row
        if ((*drawn)[static_cast<size_t>(a)] < least || (*drawn)[static_cast<size_t>(b)] < least) continue;
        const Edit ea = a < static_cast<int>(ed.size()) ? ed[static_cast<size_t>(a)] : Edit{}, eb = b < static_cast<int>(ed.size()) ? ed[static_cast<size_t>(b)] : Edit{};
        if (ea.has_lit != eb.has_lit || ea.has_shadow != eb.has_shadow || ea.derive != eb.derive) continue;  // A region recolored on its own stays apart
        if (!close_to(g_worn->lit[static_cast<size_t>(a)], g_worn->lit[static_cast<size_t>(b)]) || !close_to(g_worn->shadow[static_cast<size_t>(a)], g_worn->shadow[static_cast<size_t>(b)])) continue;
        const int ra = root(a), rb = root(b);
        if (ra != rb) { up[static_cast<size_t>((std::max)(ra, rb))] = (std::min)(ra, rb); any = true; }
      }
      if (any) for (int r = 0; r < R; r++) lead[static_cast<size_t>(r)] = root(r);  // The lowest number leads, which is the biggest step
    }
    // Rows only a few shades apart in this color look the same (texture compression, soft edges), so they become
    // one row. Each joins the biggest row within 10 of it, lit and shadow, measured against that row itself (no
    // chains), with the same kind of edit. An edit then moves them alike (shifted).
    {
      std::vector<double> size(static_cast<size_t>(R), 0.0);  // A row's size is its drawn texels, or else its share
      for (int r = 0; r < n; r++)
        size[static_cast<size_t>(lead[static_cast<size_t>(r)])] += drawn ? static_cast<double>((*drawn)[static_cast<size_t>(r)])
                                                                         : mp != g_maps.end() && r < static_cast<int>(mp->second->share.size()) ? mp->second->share[static_cast<size_t>(r)] : 0.0;
      std::vector<int> rows;
      for (int r = 0; r < n; r++) if (lead[static_cast<size_t>(r)] == r) rows.push_back(r);
      std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) { return size[static_cast<size_t>(a)] > size[static_cast<size_t>(b)]; });
      auto within = [](uint32_t a, uint32_t b, int lim) {
        for (int sh : {16, 8, 0}) if (std::abs(static_cast<int>(a >> sh & 0xFF) - static_cast<int>(b >> sh & 0xFF)) > lim) return false;
        return true;
      };
      std::set<uint64_t> side;  // Rows touching on the texture (any of their regions) join within 16, otherwise within 10
      if (dr != g_drawn.end() && mp != g_maps.end() && dr->second.map == mp->second.get())
        for (const auto& [a, b] : dr->second.touch) {
          if (a >= n || b >= n) continue;
          const int x = lead[static_cast<size_t>(a)], y = lead[static_cast<size_t>(b)];
          if (x != y) side.insert(static_cast<uint64_t>((std::min)(x, y)) << 32 | static_cast<uint32_t>((std::max)(x, y)));
        }
      std::vector<int> roots, to(static_cast<size_t>(R), -1);
      const int big_n = mp != g_maps.end() ? mp->second->major : R;
      for (int r : rows) {
        const Edit er = r < static_cast<int>(ed.size()) ? ed[static_cast<size_t>(r)] : Edit{};
        int best = -1;
        for (int q : roots) {  // Biggest first, so the first that fits wins
          const Edit eq = q < static_cast<int>(ed.size()) ? ed[static_cast<size_t>(q)] : Edit{};
          if (er.has_lit != eq.has_lit || er.has_shadow != eq.has_shadow || er.derive != eq.derive) continue;
          const int lim = side.count(static_cast<uint64_t>((std::min)(r, q)) << 32 | static_cast<uint32_t>((std::max)(r, q))) ? 16 : 10;
          if ((er.has_lit && !within(er.lit, eq.lit, lim)) || (er.has_shadow && !within(er.shadow, eq.shadow, lim))) continue;  // Recolored apart, so they stay apart
          if (r < big_n && q >= big_n) continue;  // A big region never goes under a small one's row
          if (within(g_worn->lit[static_cast<size_t>(r)], g_worn->lit[static_cast<size_t>(q)], lim) && within(g_worn->shadow[static_cast<size_t>(r)], g_worn->shadow[static_cast<size_t>(q)], lim)) { best = q; break; }
        }
        if (best < 0) roots.push_back(r); else to[static_cast<size_t>(r)] = best;
      }
      for (int r = 0; r < R; r++) {
        const int l = lead[static_cast<size_t>(r)];
        if (to[static_cast<size_t>(l)] >= 0) lead[static_cast<size_t>(r)] = to[static_cast<size_t>(l)];
      }
    }
    return lead;
  }
  // Moves a color the way a row's own color moved (from -> to), for a twin that started at another color.
  // Per channel, clamped.
  uint32_t shifted(uint32_t orig, uint32_t from, uint32_t to) {
    uint32_t out = 0;
    for (int sh : {16, 8, 0}) {
      const int c = static_cast<int>(orig >> sh & 0xFF) + static_cast<int>(to >> sh & 0xFF) - static_cast<int>(from >> sh & 0xFF);
      out |= static_cast<uint32_t>(std::clamp(c, 0, 255)) << sh;
    }
    return out;
  }
  uint32_t wornLit(int r) { return g_worn && r >= 0 && r < static_cast<int>(g_worn->lit.size()) ? g_worn->lit[static_cast<size_t>(r)] : 0; }
  uint32_t wornShadow(int r) { return g_worn && r >= 0 && r < static_cast<int>(g_worn->shadow.size()) ? g_worn->shadow[static_cast<size_t>(r)] : 0; }
  std::vector<int> withTwins(const std::vector<int>& lead, const std::vector<int>& rs) {  // rs and the regions sharing their rows
    std::vector<int> out;
    std::vector<char> in(lead.size(), 0);
    const int R = static_cast<int>(lead.size());
    for (int r : rs) {
      if (r < 0 || r >= R) continue;
      const int l = lead[static_cast<size_t>(r)];
      if (!in[static_cast<size_t>(r)]) { in[static_cast<size_t>(r)] = 1; out.push_back(r); }
      for (int s = 0; s < R; s++)
        if (lead[static_cast<size_t>(s)] == l && !in[static_cast<size_t>(s)]) { in[static_cast<size_t>(s)] = 1; out.push_back(s); }
    }
    return out;
  }
  std::vector<int> partsOf(const Target& t) {
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(g_targets.size()); i++)
      if (g_targets[static_cast<size_t>(i)].pawn == t.pawn) out.push_back(i);
    return out;
  }

  void publish() {
    View v;
    v.open = g_open;
    v.shadow_follows = g_follow;
    v.blackout = g_blackout;
    v.free_cam = g_cam.on;
    v.follow = g_follow_sel;
    v.cam_info = g_cam_info;
    v.selected = g_selected;
    v.status = g_status;
    v.exporting = g_export && g_export->mode != 3;  // Not when the other parts are being made again. That's quiet, and a new change drops it
    v.mapping = g_mapping ? std::clamp((g_mapping->view + (g_mapping->scanning ? 0.5f : 0.0f)) / kViews, 0.0f, 1.0f) : -1.0f;
    v.setup = g_setup_busy;
    v.setup_part = target() ? partName(target()->kind) : std::string();
    if (Target* t = target()) {
      v.title = t->chr + "  " + slotLabel(t->color);
      for (int i : partsOf(*t)) {
        if (i == g_target) v.part = static_cast<int>(v.parts.size());
        v.parts.push_back(g_targets[static_cast<size_t>(i)].kind);
        v.part_ids.push_back(i);
        v.part_hidden.push_back(hiddenPart(g_targets[static_cast<size_t>(i)]) ? 1 : 0);
      }
      if (t->kept_hidden)  // Another part's texture colors some of this model. Ky's DI glow and eye come from the Body's
        for (const Target& o : g_targets)
          if (o.pawn == t->pawn && o.kind != t->kind && !o.kept_hidden && !hiddenPrefixes(o, nullptr).empty() &&
              std::any_of(o.comps.begin(), o.comps.end(), [&](UObject* c) { return std::find(t->comps.begin(), t->comps.end(), c) != t->comps.end(); })) {
            v.drawn_from = partName(o.kind);
            break;
          }
      for (const Target& o : g_targets)
        if (o.pawn != t->pawn) v.can_switch = true;
      if (g_reading) v.progress = std::clamp(g_reading->next / 99.0f, 0.0f, 1.0f);
      auto mp = g_maps.find(mapKey(*t));
      if (g_worn && g_worn_key == keyOf(*t) && mp != g_maps.end()) {
        const auto& ed = g_edits[keyOf(*t)];
        const std::vector<Part>* parts = partsFor(*t);
        const auto* places = placesFor(*t);
        std::string hidden_part;  // a hidden model sharing this part's meshes ("DI"), and its mesh name prefixes ("dky_")
        const std::vector<std::string> hidden_pre = places && !t->kept_hidden ? hiddenPrefixes(*t, &hidden_part) : std::vector<std::string>{};
        auto hiddens = [&](const std::string& mesh) { return std::any_of(hidden_pre.begin(), hidden_pre.end(), [&](const std::string& pre) { return mesh.rfind(pre, 0) == 0; }); };
        // The meshes seen drawn this session, by character and part. Not the hidden model's (its preview and its move put
        // its mesh on these components), and not during a preview. ponytail: the close-up head is learned too if a super
        // plays with the editor open, and then its regions lose the tag until the game restarts
        static std::map<std::string, std::set<std::string>> seen_drawn;
        std::set<std::string>& drawn_now = seen_drawn[t->chr + "/" + t->kind];
        if (places && !g_preview.pawn)
          for (const std::string& m : drawnMeshes(*t))
            if (!hiddens(m)) drawn_now.insert(m);
        const std::vector<uint32_t>* drawn = drawnOf(*t);
        const std::vector<int>* parents = parentsOf(*t);
        const uint32_t least = drawnMin(*mp->second);
        const std::vector<int> lead = leadersOf(*t, mp->second->regions);
        for (int r = 0; r < mp->second->regions && r < static_cast<int>(g_worn->lit.size()); r++) {
          RegionView rv;
          const auto pr = pairOf(r, ed);
          rv.lit = pr.first; rv.shadow = pr.second;
          rv.orig_lit = g_worn->lit[r]; rv.orig_shadow = g_worn->shadow[r];
          rv.share = mp->second->share[r];
          rv.edited = r < static_cast<int>(ed.size()) && (ed[r].has_lit || ed[r].has_shadow);
          {  // Neither the atlas nor the scan has a place for it
            const bool placed = places && r < static_cast<int>(places->size()) && !(*places)[static_cast<size_t>(r)].empty();
            rv.unseen = !placed && (parts ? (*parts)[static_cast<size_t>(r)].bone.empty() : places != nullptr);
          }
          rv.detail = r >= mp->second->major;
          rv.name = namesOf(*t, mp->second->regions)[static_cast<size_t>(r)];
          if (rv.name.empty()) {  // The template's name, when the user hasn't named it
            const auto& tn = templateNames(*t);
            if (r < static_cast<int>(tn.size())) rv.name = tn[static_cast<size_t>(r)];
          }
          rv.lead = lead[static_cast<size_t>(r)];
          rv.undrawn = drawn && (*drawn)[static_cast<size_t>(r)] < least;
          if (drawn && !rv.undrawn)
            if (auto df = g_drawn.find(mapKey(*t)); df != g_drawn.end() && !df->second.only.empty() && df->second.shown.size() == drawn->size() &&
                                                    df->second.shown[static_cast<size_t>(r)] < least)
              rv.only_on = df->second.only;  // Only the hidden model draws it, like Ky's Dragon Install glow
          if (rv.only_on.empty() && places && !t->kept_hidden && !drawn_now.empty() && r < static_cast<int>(places->size()) && !(*places)[static_cast<size_t>(r)].empty()) {
            const auto& pr = (*places)[static_cast<size_t>(r)];
            if (std::none_of(pr.begin(), pr.end(), [&](const Place& pl) { return drawn_now.count(pl.mesh) > 0; })) {  // Only meshes not seen drawn have it
              const bool hidden_model = !hidden_pre.empty() && std::all_of(pr.begin(), pr.end(), [&](const Place& pl) { return hiddens(pl.mesh); });
              rv.only_on = hidden_model ? hidden_part : "Close-up";  // The hidden model's (its move), else the close-up head of the super moves
            }
          }
          if (parents && r >= mp->second->major) {
            const int pr = (*parents)[static_cast<size_t>(r)];
            rv.parent = pr >= 0 && pr < static_cast<int>(lead.size()) ? lead[static_cast<size_t>(pr)] : -1;  // Its row
          }
          v.regions.push_back(rv);
        }
        const int RV = static_cast<int>(v.regions.size());
        for (int r = 0; r < RV; r++) {  // A row stands for its twins. It takes their shares, and their name if it has none
          RegionView& rv = v.regions[static_cast<size_t>(r)];
          if (rv.lead < 0 || rv.lead >= RV) rv.lead = r;
          if (rv.lead == r) continue;
          RegionView& l = v.regions[static_cast<size_t>(rv.lead)];
          if (!rv.undrawn) l.share += rv.share;  // None of an undrawn one shows
          if (!rv.undrawn && rv.only_on.empty()) l.only_on.clear();  // If the shown model draws a twin, the row isn't the hidden model's alone
          l.twins++;
          if (l.name.empty()) l.name = rv.name;
        }
        std::vector<uint64_t> row_drawn(static_cast<size_t>(RV), 0);  // Per row, the texels the model draws, its twins' included
        if (drawn)
          for (int r = 0; r < RV && r < static_cast<int>(drawn->size()); r++) row_drawn[static_cast<size_t>(v.regions[static_cast<size_t>(r)].lead)] += (*drawn)[static_cast<size_t>(r)];
        const uint64_t tiny = (std::max)(static_cast<uint64_t>(least), static_cast<uint64_t>(mp->second->w) * mp->second->h / 20000);  // About 200 texels at 2048
        const uint64_t small_row = static_cast<uint64_t>(mp->second->w) * mp->second->h / 2000;  // 0.05% of the texture, about 2100 texels at 2048
        for (RegionView& rv : v.regions) {
          const int r = static_cast<int>(&rv - v.regions.data());
          const uint64_t rd = r < static_cast<int>(row_drawn.size()) ? row_drawn[static_cast<size_t>(r)] : 0;
          const bool on_model = rd >= tiny;
          if (drawn && rv.lead == r && !rv.detail && rd < small_row) rv.detail = true;  // A big region that's barely drawn goes with the small details
          rv.listed = rv.lead == r && (drawn ? on_model : !(rv.detail && rv.unseen));
        }
        v.group_same = g_group_same;
        v.groups.clear();  // Only regions listed in this view. A map bigger than the worn color's leaves some out
        for (const GroupView& g : mp->second->groups) {
          GroupView in = g;
          in.regions.erase(std::remove_if(in.regions.begin(), in.regions.end(), [&](int r) { return r < 0 || r >= static_cast<int>(v.regions.size()); }), in.regions.end());
          if (!in.regions.empty()) v.groups.push_back(std::move(in));
        }
        v.capturing = g_cap_until != Clock::time_point{};
        v.slot = t->color;
        v.unsaved = !g_unsaved.empty() || !g_unsaved_slots.empty() || g_cap_unsaved || !g_cap_restore.empty();
        v.confirm_close = g_confirm_close;
        v.offer_recovery = g_offer_recovery;
        v.picked = g_picked >= 0 && g_picked < RV ? v.regions[static_cast<size_t>(g_picked)].lead : g_picked;  // Its row
        for (int c : g_pick_choices) {
          const int l = c >= 0 && c < RV ? v.regions[static_cast<size_t>(c)].lead : c;
          if (std::find(v.pick_choices.begin(), v.pick_choices.end(), l) == v.pick_choices.end()) v.pick_choices.push_back(l);
        }
        v.pick_serial = g_pick_serial;
        v.picking = g_pick_wanted || g_pick_scanning;
        v.decal_lines = g_vecs_for == keyOf(*t) && std::any_of(g_vecs.begin(), g_vecs.end(), [](const VecGroup& g) { return g.label == kDecalLabel; });
        v.tex_w = g_worn ? g_worn->base.w : 0;
        v.tex_h = g_worn ? g_worn->base.h : 0;
        v.painted = hasPaint(*t);
        v.can_undo = !g_undo[slotOf(*t)].empty();
        v.can_redo = !g_redo[slotOf(*t)].empty();
        v.slots = g_slots_cache;
        v.recolor_files = g_recolor_files_cache;
        v.palettes = g_palettes_cache;
        v.palette_author = g_palette_author;
        for (const CapColor& c : g_cap) {
          CapturedColor cc;
          cc.effect = c.effect;
          cc.label = c.label;
          std::memcpy(cc.rgb, c.rgb, sizeof(cc.rgb));
          cc.edited = c.edited;
          v.captured.push_back(cc);
        }
        for (const MatColor& m : g_matc) {
          CapturedColor cc;
          cc.effect = m.effect;
          cc.label = m.label;
          std::memcpy(cc.rgb, m.rgb, sizeof(cc.rgb));
          cc.edited = m.edited;
          v.captured_mats.push_back(cc);
        }
        v.captured_named = g_cap_named;
        v.hidden_effects.assign(g_hidden.begin(), g_hidden.end());
        v.on_screen = g_on_screen;
        if (g_fx_for == t->chr + "/" + t->color)
          for (const FxColor& f : fxList()) {
            EffectColor e;
            e.label = f.label;
            e.uses = f.uses;
            std::memcpy(e.rgb, f.rgb, sizeof(e.rgb));
            e.edited = f.edited;
            {  // The effects that take it by name, while they're loaded
              std::set<std::string> by;
              for (const FxCopy& c : f.copies) if (!c.effect.empty() && c.k.alive()) by.insert(c.effect);
              for (const std::string& n : by) e.used_by += (e.used_by.empty() ? "" : ", ") + n;
              by.clear();  // Own color only for an effect holding the color itself. By name, it spawns from the fighter's palette
              for (const FxCopy& c : f.copies) if (!c.effect.empty() && c.k.alive() && !c.named) by.insert(c.effect);
              for (const auto& [n, c] : f.per)  // One saved for an effect that takes it by name can't do anything, so it isn't listed
                if (std::none_of(f.copies.begin(), f.copies.end(), [&](const FxCopy& x) { return x.named && x.effect == n; })) by.insert(n);
              for (const std::string& n : by) {
                EffectUse u;
                u.effect = n;
                u.says = fxEffectSays(t->chr, n);
                if (u.says.empty()) u.says = fxMoves(t->chr, "@" + n);  // (the moves that play it, from the move scripts)
                const float* rgb = fxRgbFor(f, n);
                std::memcpy(u.rgb, rgb ? rgb : f.rgb, sizeof(u.rgb));
                u.edited = f.per.count(n) > 0;
                e.per.push_back(u);
              }
              std::stable_sort(e.per.begin(), e.per.end(), [](const EffectUse& a, const EffectUse& b) { return !a.says.empty() && b.says.empty(); });
            }
            v.effect_colors.push_back(e);
          }
        if (g_vecs_for == keyOf(*t))
          for (const VecGroup& g : g_vecs) {
            MaterialColor mc;
            mc.label = g.label;
            std::memcpy(mc.rgba, g.cur, sizeof(mc.rgba));
            mc.edited = g.edited;
            v.material_colors.push_back(mc);
          }
      }
    }
    v.cap_gen = g_cap_gen;  // The lists this view shows. A row's edit names them, and an older list's edit is dropped
    v.fx_slot = g_fx_for;
    std::lock_guard<std::mutex> lock(g_m);
    g_view = std::move(v);
  }
}

View view() { std::lock_guard<std::mutex> lock(g_m); return g_view; }
bool isOpen() { std::lock_guard<std::mutex> lock(g_m); return g_view.open; }
// Game thread. While the editor scans the fighter (mapping, a pick, a selection's own scan) or runs the first-time
// setup, the fight is held (as the pause holds it with the editor open): a scan compares frames, and a fighter
// moving through its idle animation reads as a picture that kept changing (without it, most selection scans were
// refused in game).
bool setupRunning() { return g_setup_busy && GetTickCount64() - g_setup_busy_ms < 500; }
bool setupShown() { return g_setup_busy && GetTickCount64() - g_setup_busy_ms < 1500; }
bool holdsFight() { return g_open && (g_mapping || g_scan_on || g_pick_scanning || setupRunning()); }
void toggle() { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kToggle, -1, 0}); }
void setLit(int region, uint32_t rgb) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kLit, region, rgb}); }
void setShadow(int region, uint32_t rgb) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kShadow, region, rgb}); }
void select(int region) { selectMany(region < 0 ? std::vector<int>{} : std::vector<int>{region}); }
void selectMany(const std::vector<int>& regions) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kSelect, -1, 0, {}, regions}); }
void setPairs(const std::vector<Pick>& picks, int mode) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kPairs, -1, static_cast<uint32_t>(std::clamp(mode, 0, 2)), picks, {}}); }
void reset(int region) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kReset, region, 0}); }
void resetColor() { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kResetColor, -1, 0}); }
void nextTarget() { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kNext, -1, 0}); }
void showPart(int id) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kPart, id, 0}); }
void remap() { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kRemap, -1, 0}); }
// A part's name for people, like "MAIN" -> "Body", "DI_FLARE_UNIQUEC" -> "DI wing" (Sol's),
// "BURGERSET_FRIP" -> "Burgerset frip".
std::string partName(const std::string& k) {
  if (k == "MAIN") return "Body";
  if (k == "WEP") return "Weapon";
  if (k == "DI") return "DI";
  if (k == "DI_WEP") return "DI weapon";
  std::string n = k;
  if (n.find("FLARE") != std::string::npos) return n.rfind("DI_", 0) == 0 ? "DI wing" : "Flare";
  const size_t u = n.find("_UNIQUE");  // A material's own suffix, which tells people nothing
  if (u != std::string::npos) n.resize(u);
  std::string out;
  for (size_t i = 0; i < n.size(); i++) {
    const char c = n[i];
    out += c == '_' ? ' ' : i == 0 ? c : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (out.rfind("Wep", 0) == 0) out = "Weapon" + std::string(out.size() > 3 && std::isdigit(static_cast<unsigned char>(out[3])) ? " " : "") + out.substr(3);  // ("WEP2" -> "Weapon 2")
  if (out.rfind("Main ", 0) == 0) out = out.substr(5);           // ("MAIN_SHIRT" -> "Shirt")
  if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
  return out;
}
void exportTextures() { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kExport, -1, 0}); }
void groupSame(bool on) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kGroupSame, -1, on ? 1u : 0u}); }
// Import's own window (the editor's, not Windows' picker): where it opens, and the files ticked in it
std::wstring textureDir() {
  std::lock_guard<std::mutex> lk(g_tex_pick_m);
  return g_tex_pick_dir.empty() ? (g_dir / "export").wstring() : g_tex_pick_dir;
}
std::wstring exportsDir() { return (g_dir / "export").wstring(); }
void importTextureFiles(const std::vector<std::wstring>& files) {
  if (files.empty()) return;
  std::vector<std::filesystem::path> picked(files.begin(), files.end());
  std::sort(picked.begin(), picked.end());
  {
    std::lock_guard<std::mutex> lk(g_tex_pick_m);
    g_tex_pick.insert(g_tex_pick.end(), picked.begin(), picked.end());
    g_tex_added += static_cast<int>(picked.size());
  }
  std::lock_guard<std::mutex> lock(g_m);
  g_reqs.push_back({kImportTex, -1, 0});
}
void logDrawn() { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kLogDrawn, -1, 0}); }
void renameRegion(int region, const std::string& name) {
  std::string n;
  for (char c : name) if (c != '\t' && c != '\n' && c != '\r') n += c;  // Kept to one line in the file
  while (!n.empty() && n.back() == ' ') n.pop_back();
  if (n.size() > 48) n.resize(48);
  std::lock_guard<std::mutex> lock(g_m);
  g_reqs.push_back({kRename, region, 0, {}, {}, n});
}
void setMaterialColor(int i, const float rgba[4]) {
  Req q{kVec, i, 0};
  std::memcpy(q.f, rgba, sizeof(q.f));
  std::lock_guard<std::mutex> lock(g_m);
  g_reqs.push_back(q);
}
void resetMaterialColor(int i) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kVecReset, i, 0}); }
void setEffectColor(int i, const float rgb[3], const std::string& slot) {
  Req q{kFx, i, 0};
  std::memcpy(q.f, rgb, sizeof(float) * 3);
  q.text = slot;
  std::lock_guard<std::mutex> lock(g_m);
  g_reqs.push_back(q);
}
void resetEffectColor(int i, const std::string& slot) { std::lock_guard<std::mutex> lock(g_m); Req q{kFxReset, i, 0}; q.text = slot; g_reqs.push_back(q); }
void setEffectColorFor(int i, const std::string& effect, const float rgb[3], const std::string& slot) {
  Req q{kFxOne, i, 0};
  std::memcpy(q.f, rgb, sizeof(float) * 3);
  q.text = slot + "\t" + effect;
  std::lock_guard<std::mutex> lock(g_m);
  g_reqs.push_back(q);
}
void resetEffectColorFor(int i, const std::string& effect, const std::string& slot) {
  std::lock_guard<std::mutex> lock(g_m);
  Req q{kFxOneReset, i, 0};
  q.text = slot + "\t" + effect;
  g_reqs.push_back(q);
}
void captureEffects() { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kCapture, -1, 0}); }
void listRecolors() { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kLists, -1, 0}); }
void saveRecolor() { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kSave, -1, 0}); }
void discardAndClose() { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kDiscardClose, -1, 0}); }
void cancelClose() { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kCancelClose, -1, 0}); }
void restoreRecovery(bool restore) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kRecovery, -1, restore ? 1u : 0u}); }
void undo() { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kUndo, -1, 0}); }
int textureRegionAt(float u, float v) {
  std::lock_guard<std::mutex> lk(g_prev_m);
  if (!g_prev_ds || g_prev_px.empty()) return -1;  // Uses the regions as the picture shows them, so a thin line widened there is picked there
  const DsCache& d = *g_prev_ds;
  if (d.w <= 0 || d.h <= 0 || u < 0 || v < 0 || u >= 1 || v >= 1) return -1;
  const int x = std::clamp(static_cast<int>(u * d.w), 0, d.w - 1), y = std::clamp(static_cast<int>(v * d.h), 0, d.h - 1);
  const uint16_t r = d.owner[static_cast<size_t>(y) * d.w + x];
  return r == 0xFFFF ? -1 : r;
}
void listAllEffects() { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kListEffects, -1, 0}); }
void onScreen(bool on) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kOnScreen, on ? 1 : 0, 0}); }
void hideEffect(const std::string& effect, bool hide) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kHide, hide ? 1 : 0, 0, {}, {}, effect}); }

void textureViewOpen(bool open) {
  g_texview_on = open;
  g_texview_ms = open ? std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count() : 0;
}
void textureViewMap(int map) { g_texview_map = std::clamp(map, 0, 2); }
bool textureView(int& serial, std::vector<uint8_t>& rgba, int& w, int& h, float box[4]) {  // Empty rgba means nothing to show
  std::lock_guard<std::mutex> lk(g_prev_m);
  if (serial == g_prev_serial) return false;
  serial = g_prev_serial;
  rgba = g_prev_px;
  w = g_prev_w;
  h = g_prev_h;
  std::memcpy(box, g_prev_box, sizeof(g_prev_box));
  return true;
}
void textureViewPlain(bool plain) { g_texview_plain = plain; }
namespace {
  void paintReq(int kind, float u, float v, const PaintBrush* b, std::vector<float> extra = {}) {
    std::lock_guard<std::mutex> lock(g_m);
    Req q{kind, -1, 0};
    q.nums = {u, v};
    if (b) {
      q.nums.insert(q.nums.end(), {static_cast<float>(b->map), static_cast<float>(b->blend), b->rgb[0], b->rgb[1], b->rgb[2], b->size, b->hardness,
                                   b->opacity, b->flow, b->eraser ? 1.0f : 0.0f, b->keep_region ? 1.0f : 0.0f});
    }
    q.nums.insert(q.nums.end(), extra.begin(), extra.end());
    g_reqs.push_back(q);
  }
}
void paintBegin(float u, float v, const PaintBrush& b) { paintReq(kPaintBegin, u, v, &b); }
void paintTo(float u, float v) { paintReq(kPaintTo, u, v, nullptr); }
void paintEnd() { paintReq(kPaintEnd, 0, 0, nullptr); }
void paintFill(float u, float v, const PaintBrush& b, int tolerance, bool contiguous) {
  paintReq(kPaintFill, u, v, &b, {static_cast<float>(tolerance), contiguous ? 1.0f : 0.0f});
}
void paintClear(int map) { paintReq(kPaintClear, static_cast<float>(map), 0, nullptr); }
void paintShape(float u0, float v0, float u1, float v1, const PaintBrush& b, int kind, bool filled) {
  paintReq(kPaintShape, u0, v0, &b, {static_cast<float>(kind), filled ? 1.0f : 0.0f, u1, v1});
}
void pickAt(float x, float y) {
  std::lock_guard<std::mutex> lock(g_m);
  Req q{kPickAt, -1, 0};
  q.f[0] = x;
  q.f[1] = y;
  g_reqs.push_back(q);
}
void redo() { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kRedo, -1, 0}); }
void exportRecolor() { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kExportRecolor, -1, 0}); }
void importRecolor(const std::string& file) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kImportRecolor, -1, 0, {}, {}, file}); }
void savePalette(const std::string& file, const std::string& name, const std::string& author, const std::string& note) {
  std::lock_guard<std::mutex> lock(g_m);
  g_reqs.push_back({kPaletteSave, -1, 0, {}, {}, file + '\x1f' + name + '\x1f' + author + '\x1f' + note});
}
void loadPalette(const std::string& file) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kPaletteLoad, -1, 0, {}, {}, file}); }
void editPalette(const std::string& file, const std::string& name, const std::string& author, const std::string& note) {
  std::lock_guard<std::mutex> lock(g_m);
  g_reqs.push_back({kPaletteEdit, -1, 0, {}, {}, file + '\x1f' + name + '\x1f' + author + '\x1f' + note});
}
void deletePalette(const std::string& file) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kPaletteDelete, -1, 0, {}, {}, file}); }
void copyToSlot(const std::string& slot) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kCopySlot, -1, 0, {}, {}, slot}); }
void readColorsAgain() { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kReread, -1, 0}); }
void setCapturedColor(int i, const float rgb[3], uint32_t gen) {
  std::lock_guard<std::mutex> lock(g_m);
  Req q{kCap, i, gen};
  std::memcpy(q.f, rgb, sizeof(float) * 3);
  g_reqs.push_back(q);
}
void setCapturedMatColor(int i, const float rgb[3], uint32_t gen) {
  std::lock_guard<std::mutex> lock(g_m);
  Req q{kMat, i, gen};
  std::memcpy(q.f, rgb, sizeof(float) * 3);
  g_reqs.push_back(q);
}
void resetCapturedMatColor(int i, uint32_t gen) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kMatReset, i, gen}); }
void resetCapturedColor(int i, uint32_t gen) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kCapReset, i, gen}); }
void menuUpdate(bool in_fight) { menuUpdateNow(in_fight); }
void menuRelease() { menuRelease_(); }
void battleGone() {
  // The same training fight, only stalled: nothing's over. (The engine's address can be reused by a new fight, so the
  // fighters' meshes must still be the ones found.)
  if (asw_engine::get() && asw_engine::get() == g_fight && !g_targets.empty() && std::all_of(g_targets.begin(), g_targets.end(), [](const Target& t) {
        if (t.items.size() != t.mids.size() + t.comps.size()) return false;
        for (size_t j = 0; j < t.comps.size(); j++)
          if (!Kept{t.comps[j], t.items[t.mids.size() + j]}.alive()) return false;
        return true;
      }))
    return;
  {  // Textures ticked for this fight's fighter don't go on the next one's
    std::lock_guard<std::mutex> lk(g_tex_pick_m);
    g_tex_pick.clear();
    g_tex_added = 0;
  }
  g_tex_front.clear();
  g_tex_home.clear();
  g_tex_all = g_tex_done = 0;
  g_setup_home.clear();
  scanShowOthers();  // Left the fight during a scan: what it hid is shown again while it's still there
  for (RxPart& p : g_rx.parts) {  // The fight's over. Only its file is kept, for saving, and unfinished textures are dropped when done
    unroot(p.shown); p.shown = Shown{}; p.failed = true;
    if (p.job.valid()) g_png_orphans.push_back(std::move(p.job));
  }
  g_rx.live = false;
  g_my_chr.clear(); g_my_color.clear();
  capStage();  // Only live modules are written. Before the slot's, as a capture may hold a slot edit as its original
  fxRestore();
  if (!g_smear_pool.empty()) g_pool_last_fight = g_smear_pool;  // (its material copies wait until it's gone)
  for (const std::string& k : g_hold) reloadSlot(k.substr(0, k.find('/')), k.substr(k.find('/') + 1));  // Saved during the fight, so read them again now
  g_hold.clear();
  g_smeared.clear();  // The effect pool goes with the fight, so these are let go without being touched
  g_matdressed.clear();
  g_hid.clear();
  g_hidden.clear();
  g_on_screen.clear();
  g_smear_pool.clear();
    g_smear_scanned = false;
  { std::lock_guard<std::mutex> lk(g_smear_new_m); g_smear_new.clear(); }
  g_smear_live.clear();
  g_named.clear();
  g_tpl_mats.clear();
}
// ---- recolor sharing ----
namespace {
  const char* const kShareCodes[] = {"SOL", "KYK", "MAY", "AXL", "CHP", "POT", "FAU", "MLL", "ZAT", "RAM", "LEO", "NAG", "GIO", "ANJ", "INO", "GLD",
                                     "JKO", "COS", "BKN", "TST", "BGT", "SIN", "BED", "ASK", "JHN", "ELP", "ABA", "SLY", "DZY", "VEN", "UNI", "LUC",
                                     "USG", "ABS"};  // The game's character numbers. Jam's and Robo-Ky's models are under USG and ABS
  std::string codeOfChara(int c) { return c >= 0 && c < static_cast<int>(std::size(kShareCodes)) ? kShareCodes[c] : std::string(); }
  bool plainName(const std::string& v, size_t lo, size_t hi) {
    if (v.size() < lo || v.size() > hi) return false;
    for (char c : v) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    return true;
  }
  bool slotName(const std::string& v) { return v.size() == 7 && v.compare(0, 5, "Color") == 0 && std::isdigit(static_cast<unsigned char>(v[5])) && std::isdigit(static_cast<unsigned char>(v[6])); }
  float screenX(UObject* pawn) {  // A fighter's x position on screen, or -1 if it can't be told
    static UObject* gs = cdo(STR("/Script/Engine.Default__GameplayStatics"));
    static UFunction* get_pc = fn(STR("/Script/Engine.GameplayStatics:GetPlayerController"));
    static UFunction* project = fn(STR("/Script/Engine.GameplayStatics:ProjectWorldToScreen"));
    if (!gs || !get_pc || !project || !pawn) return -1;
    alignas(8) uint8_t a[24] = {};
    *reinterpret_cast<UObject**>(a) = pawn;
    gs->ProcessEvent(get_pc, a);
    UObject* pc = *reinterpret_cast<UObject**>(a + 16);
    if (!pc) return -1;
    const Vec at = actorVec(pawn, STR("/Script/Engine.Actor:K2_GetActorLocation"));
    alignas(8) uint8_t b[32] = {};
    *reinterpret_cast<UObject**>(b) = pc;
    std::memcpy(b + 8, &at, sizeof(at));
    gs->ProcessEvent(project, b);
    return b[0x1D] ? *reinterpret_cast<float*>(b + 0x14) : -1;
  }
  void put32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; i++) o.push_back(static_cast<uint8_t>(v >> (8 * i))); }
  void putStr(std::vector<uint8_t>& o, const std::string& v) { put32(o, static_cast<uint32_t>(v.size())); o.insert(o.end(), v.begin(), v.end()); }
  struct Reader {  // Bounds checked, since these are the opponent's bytes
    const std::vector<uint8_t>& d; size_t at = 0; bool ok = true;
    uint32_t u32() { if (at + 4 > d.size()) { ok = false; return 0; } uint32_t v = 0; for (int i = 0; i < 4; i++) v |= uint32_t(d[at + i]) << (8 * i); at += 4; return v; }
    std::string str(size_t max) { const uint32_t n = u32(); if (!ok || n > max || at + n > d.size()) { ok = false; return {}; } std::string v(d.begin() + at, d.begin() + at + n); at += n; return v; }
    const uint8_t* bytes(size_t n) { if (at + n > d.size()) { ok = false; return nullptr; } const uint8_t* p = d.data() + at; at += n; return p; }
  };
  void shareSaveNow() {
    std::string slot;
    { std::lock_guard<std::mutex> lk(g_share_m); slot.swap(g_share_save_slot); }
    if (slot.empty()) return;
    auto say = [](const std::string& m) { std::lock_guard<std::mutex> lk(g_share_m); g_share_status = m; COMBO_LOG("share: " + m); };
    if (g_rx.chr.empty() || g_rx.stpl.empty()) { say("Nothing received to save."); return; }
    if (!slotName(slot)) { say("Pick a slot."); return; }
    const std::string file = "received_" + g_rx.chr + "_" + g_rx.color + "_" + nowStamp() + ".stpl";  // Kept as a palette too
    std::error_code ec;
    std::filesystem::create_directories(paletteDir(g_rx.chr), ec);
    {
      std::ofstream f(paletteDir(g_rx.chr) / file, std::ios::binary | std::ios::trunc);
      f.write(g_rx.stpl.data(), static_cast<std::streamsize>(g_rx.stpl.size()));
      if (!f.good()) { say("Couldn't save."); return; }
    }
    Target to;
    to.chr = g_rx.chr; to.color = slot; to.kind = "MAIN";
    if (g_rx.live) g_hold.insert(to.chr + "/" + slot);  // A fight is running, so it keeps its look. The slot's new recolor applies from the next fight
    // Your recolor of that slot is kept first, with unsaved changes saved before that. If nothing is saved for
    // it, nothing is written.
    (void)writeRecolor(to, trashFile(to.chr, slot + "_before_received.stpl"), paletteMeta(slot + " before a received recolor", "", ""), true);
    const std::string r = importRecolorPath(to, paletteDir(to.chr) / file);
    COMBO_LOG("share: " + r + " (palette " + file + ")");
    say(r.rfind("Imported", 0) == 0 ? "Saved to " + slotLabel(slot) + "." : r);
  }
}

void shareNoteSides(int local_side) {
  g_my_chr.clear(); g_my_color.clear();
  // The fighters on show, meaning pawns with a visible mesh wearing a character's color (the Park keeps hidden
  // ones around).
  std::vector<UObject*> pawns = fighters(), comps;
  UObjectGlobals::FindAllOf(STR("SkeletalMeshComponent"), comps);
  std::map<UObject*, std::map<std::string, int>> slots;  // pawn -> "chr/color" -> materials. The color with the most is its own
  for (UObject* c : comps) {
    if (!c || !whole(c) || !isVisible(c)) continue;
    UObject* owner = c->GetOuterPrivate();
    if (std::find(pawns.begin(), pawns.end(), owner) == pawns.end()) continue;
    for (int i = 0, n = numMaterials(c); i < n && i < 32; i++) {
      std::string name, kind, chr, color;
      UObject* m = material(c, i);
      if (isDynamic(m) && colorSource(m, name, kind) && slotOfPath(name, chr, color)) slots[owner][chr + "/" + color]++;
    }
  }
  auto slotOf = [&](UObject* p) {
    std::string best;
    int most = 0;
    for (const auto& [k, n] : slots[p]) if (n > most) { most = n; best = k; }
    return best;
  };
  std::vector<UObject*> shown;
  for (const auto& [p, m] : slots) if (!m.empty()) shown.push_back(p);
  const std::string me = codeOfChara(combo_trials::playerChara()), them = codeOfChara(combo_trials::opponentChara());
  UObject* mine = nullptr;
  if (!me.empty() && !them.empty() && me != them) {  // Different characters, so ours is the one playing our character
    for (UObject* p : shown) if (slotOf(p).substr(0, 3) == me) mine = mine ? reinterpret_cast<UObject*>(-1) : p;
    if (mine == reinterpret_cast<UObject*>(-1)) mine = nullptr;  // Two matched, so we can't tell
  }
  if (!mine && shown.size() == 2) {  // A mirror match. P1 starts on the left of the screen
    const float x0 = screenX(shown[0]), x1 = screenX(shown[1]);
    if (x0 >= 0 && x1 >= 0 && x0 != x1) {
      UObject* p1 = x0 < x1 ? shown[0] : shown[1];
      mine = local_side == 0 ? p1 : (p1 == shown[0] ? shown[1] : shown[0]);
    }
  }
  if (!mine) { COMBO_LOG("share: " + std::to_string(shown.size()) + " fighters on show (" + me + " vs " + them + "), ours not told"); return; }
  const std::string k = slotOf(mine);
  g_my_chr = k.substr(0, k.find('/'));
  g_my_color = k.substr(k.find('/') + 1);
  COMBO_LOG("share: we're P" + std::to_string(local_side + 1) + " in " + g_my_chr + " " + g_my_color + " (" + std::to_string(shown.size()) + " fighters on show)");
}

int sharePayload(std::vector<uint8_t>& out, std::string& why) {
  out.clear();
  if (g_my_chr.empty()) { why = "our fighter's slot isn't known"; return 2; }
  const std::string pre = g_my_chr + "/" + g_my_color + "/";
  std::vector<std::pair<std::string, const TexKept*>> parts;
  for (const auto& [key, k] : g_menu_tex) {
    if (key.rfind(pre, 0) != 0 || !k.shown.base) continue;
    auto on = g_menu.find(key);
    if (on != g_menu.end() && on->second.shown.base == k.shown.base) parts.push_back({key.substr(pre.size()), &k});  // The one on our fighter
  }
  if (parts.empty()) {
    std::error_code ec;
    if (slotFiles(g_my_chr, g_my_color).empty()) { why = "no recolor saved for " + g_my_chr + " " + g_my_color; return 2; }
    why = "the recolor's textures aren't made yet";
    return 1;
  }
  for (const auto& [kind, k] : parts)
    if (!k->packed.valid() || k->packed.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { why = "still packing"; return 1; }
  Target t;
  t.chr = g_my_chr; t.color = g_my_color; t.kind = "MAIN";
  std::string stpl;
  if (writeRecolor(t, g_dir / "share_out.stpl", "", true).empty()) {  // (what's on disk, even if a save just failed)
    const auto b = readBytes(g_dir / "share_out.stpl");
    stpl.assign(b.begin(), b.end());
  }
  out.insert(out.end(), {'S', 'R', 'C', '1'});
  putStr(out, g_my_chr);
  putStr(out, g_my_color);
  putStr(out, stpl);
  put32(out, static_cast<uint32_t>(parts.size()));
  for (const auto& [kind, k] : parts) {
    const Packed& p = k->packed.get();
    if (!p.ok) { out.clear(); why = underWine() ? "recolors aren't sent from Linux / Proton" : "a texture couldn't be packed"; return 2; }
    putStr(out, kind);
    std::string vecs;
    if (const auto* v = menuVecs(pre + kind))
      for (const auto& [label, c] : *v) {
        char b[96];
        snprintf(b, sizeof(b), "\t%g %g %g %g\n", c[0], c[1], c[2], c[3]);
        vecs += oneLine(label) + b;
      }
    putStr(out, vecs);
    for (int i = 0; i < 2; i++) {
      put32(out, static_cast<uint32_t>(p.w[i]));
      put32(out, static_cast<uint32_t>(p.h[i]));
      put32(out, static_cast<uint32_t>(p.z[i].size()));
      out.insert(out.end(), p.z[i].begin(), p.z[i].end());
    }
  }
  why = std::to_string(parts.size()) + " parts, " + std::to_string(out.size() / 1024) + " KB";
  return 0;
}

bool shareTake(const std::vector<uint8_t>& d, std::string& why) {
  Reader r{d};
  const uint8_t* magic = r.bytes(4);
  if (!magic || std::memcmp(magic, "SRC1", 4) != 0) { why = "not a recolor"; return false; }
  Rx rx;
  rx.chr = r.str(16);
  rx.color = r.str(16);
  rx.stpl = r.str(24 * 1024 * 1024);
  const uint32_t n = r.u32();
  if (!r.ok || !plainName(rx.chr, 3, 8) || !slotName(rx.color) || n > 8) { why = "bad header"; return false; }
  const std::string theirs = codeOfChara(combo_trials::opponentChara());
  if (rx.chr.substr(0, 3) != theirs) { why = "it's for " + rx.chr + ", the opponent plays " + (theirs.empty() ? "?" : theirs); return false; }
  if (rx.chr == g_my_chr && rx.color == g_my_color) { why = "it's our own slot"; return false; }
  if (g_my_chr.empty() && rx.chr.substr(0, 3) == codeOfChara(combo_trials::playerChara())) { why = "a mirror with our side not known"; return false; }
  if (rx.stpl.rfind("STPL ", 0) != 0) rx.stpl.clear();  // No recolor file, so it's shown but not saved
  struct Pic { int w, h; std::vector<uint8_t> z; };
  std::vector<std::shared_ptr<std::array<Pic, 2>>> pics_of;
  for (uint32_t i = 0; i < n; i++) {
    RxPart part;
    part.kind = r.str(16);
    const std::string vecs = r.str(64 * 1024);
    if (!r.ok || !plainName(part.kind, 2, 24)) { why = "bad part"; return false; }
    std::istringstream vs(vecs);
    std::string line;
    while (std::getline(vs, line)) {
      const size_t tab = line.find('\t');
      std::array<float, 4> c{};
      std::istringstream nums(tab == std::string::npos ? std::string() : line.substr(tab + 1));
      if (tab != std::string::npos && (nums >> c[0] >> c[1] >> c[2] >> c[3]) && part.vecs.size() < 64) {
        for (float& f : c) f = std::isfinite(f) ? std::clamp(f, -100.0f, 100.0f) : 0.0f;
        part.vecs[line.substr(0, tab)] = c;
      }
    }
    auto pics = std::make_shared<std::array<Pic, 2>>();
    for (int k = 0; k < 2; k++) {
      Pic& p = (*pics)[k];
      p.w = static_cast<int>(r.u32());
      p.h = static_cast<int>(r.u32());
      const uint32_t zn = r.u32();
      const uint8_t* z = r.bytes(zn);
      if (!r.ok || !z || p.w <= 0 || p.h <= 0 || p.w > 4096 || p.h > 4096) { why = "bad picture"; return false; }
      p.z.assign(z, z + zn);
    }
    rx.parts.push_back(std::move(part));
    pics_of.push_back(pics);
  }
  {  // All read and checked. The parts now run one by one on a single worker, each handed over when done
    auto proms = std::make_shared<std::vector<std::promise<Pngs>>>(rx.parts.size());
    for (size_t i = 0; i < rx.parts.size(); i++) rx.parts[i].job = (*proms)[i].get_future();
    g_png_orphans.push_back(std::async(std::launch::async, [pics_of, proms]() {  // (worker) Unpacks, checks, and makes them into our own PNGs
      for (size_t i = 0; i < pics_of.size(); i++) {
        Pngs out;
        try {
          for (int k = 0; k < 2; k++) {
            Image im;
            im.w = (*pics_of[i])[k].w; im.h = (*pics_of[i])[k].h;
            im.px = lzmsUnpack((*pics_of[i])[k].z.data(), (*pics_of[i])[k].z.size(), static_cast<size_t>(im.w) * im.h * 4);
            if (im.px.empty()) { out = Pngs{}; break; }
            (k ? out.sss : out.base) = encodePng(im, false);  // Already in the form the texture takes
          }
        } catch (...) {
          out = Pngs{};  // Out of memory or similar, so it isn't shown
        }
        try { (*proms)[i].set_value(std::move(out)); } catch (...) {}
      }
      return Pngs{};
    }));
  }
  for (RxPart& p : g_rx.parts) {  // An earlier one's textures are let go. Its unfinished work is dropped when done, never waited on
    unroot(p.shown);
    if (p.job.valid()) g_png_orphans.push_back(std::move(p.job));
  }
  g_rx = std::move(rx);
  g_rx.live = true;
  { std::lock_guard<std::mutex> lk(g_share_m); g_share_info = g_rx.chr + " " + g_rx.color; g_share_status.clear(); }
  why = g_rx.chr + " " + g_rx.color + ", " + std::to_string(n) + " parts" + (g_rx.stpl.empty() ? " (no recolor file: can't be saved)" : "");
  return true;
}

std::string shareInfo() { std::lock_guard<std::mutex> lk(g_share_m); return g_share_info; }
std::string shareSaveStatus() { std::lock_guard<std::mutex> lk(g_share_m); return g_share_status; }
bool slotHasRecolor(const std::string& chr, const std::string& color) { return !chr.empty() && !slotFiles(chr, color).empty(); }
void shareSave(const std::string& slot) { std::lock_guard<std::mutex> lk(g_share_m); g_share_save_slot = slot; g_share_status = "Saving..."; }

void setShadowFollows(bool on) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kFollow, -1, on ? 1u : 0u}); }
void setBlackout(bool on) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kBlackout, -1, on ? 1u : 0u}); }
void setFreeCamera(bool on) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kCamera, -1, on ? 1u : 0u}); }
void cameraPan(float dx, float dy) { addTo(g_pan_dx, dx); addTo(g_pan_dy, dy); }
void setFollow(bool on) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kFollowSel, -1, on ? 1u : 0u}); }
bool tagAt(float& x, float& y) {
  x = g_tag_x;
  y = g_tag_y;
  return x >= 0.0f && y >= 0.0f;
}
bool scanWanted(int& id, int& frame, int& frames, int& groups) {
  std::lock_guard<std::mutex> lk(g_scan_m);
  if (g_scan_frame < 0 || g_scan_grabbed || g_scan_done || nowMs() - g_scan_ms < 50.0) return false;
  id = g_scan_id;
  frame = g_scan_frame;
  frames = g_scan_frames;
  groups = g_scan_groups;
  return true;
}
bool scanWhole() { std::lock_guard<std::mutex> lk(g_scan_m); return g_scan_whole; }
void scanGrabbed(int id, int frame) {
  std::lock_guard<std::mutex> lk(g_scan_m);
  if (id == g_scan_id && frame == g_scan_frame) g_scan_grabbed = true;
}
bool scanPoint(float& x, float& y) {
  std::lock_guard<std::mutex> lk(g_scan_m);
  if (!g_scan_point_on) return false;
  x = g_scan_point[0];
  y = g_scan_point[1];
  return true;
}
void scanFailed(int id, const std::string& why) {
  std::lock_guard<std::mutex> lk(g_scan_m);
  if (id != g_scan_id || g_scan_frame < 0) return;  // From an old scan
  g_scan_out.clear();
  g_scan_w = g_scan_h = 0;
  g_scan_why = why;
  g_scan_done = true;
}
void scanResult(int id, int screen_w, int screen_h, std::vector<ScanSample>& samples) {
  std::lock_guard<std::mutex> lk(g_scan_m);
  if (id != g_scan_id || g_scan_frame < 0) return;  // From an old scan
  g_scan_out.swap(samples);
  g_scan_w = screen_w;
  g_scan_h = screen_h;
  g_scan_done = true;
}
bool freeCameraKeys() { return g_cam_keys; }
void overlayFrame(bool typing, int screen_w, int screen_h) {
  g_typing = typing;
  if (screen_w > 0 && screen_h > 0) g_aspect = static_cast<float>(screen_w) / static_cast<float>(screen_h);
}
void cameraInput(float dx, float dy, float wheel) {
  addTo(g_cam_dx, dx); addTo(g_cam_dy, dy); addTo(g_cam_wheel, wheel);
}
// The free camera's view goes in where the engine hands the view to the renderer, ULocalPlayer::GetViewPoint.
// It's found by its opening bytes and was checked in the game's code, where it copies the camera manager's
// view (player controller at +0x30, camera manager at +0x2C0). After the game's own view, ours replaces
// location, rotation and field of view, with no black bars and none of the game camera's post process. The
// game's camera keeps running underneath, untouched, so turning ours off just means not replacing. Without
// the hook (a game update moved it), the level's spare CameraActor becomes the view target instead (below).
using GetViewPointFn = void(__fastcall*)(void* self, uint8_t* view, int pass);
GetViewPointFn g_get_view_point = nullptr;
bool g_view_hooked = false;
std::atomic<bool> g_view_on{false};  // When set, ours replaces the game's
std::atomic<double> g_view_ms{0};    // When it was last set. It ends once the battle update stops setting it
float g_view_pose[7] = {};           // location, pitch yaw roll, field of view (FMinimalViewInfo's first fields)
void __fastcall viewPointHook(void* self, uint8_t* view, int pass) {
  g_get_view_point(self, view, pass);
  if (!view || !g_view_on || nowMs() - g_view_ms > 250.0) return;
  std::memcpy(view, g_view_pose, sizeof(g_view_pose));
  view[0x30] &= ~1;                               // Clear bConstrainAspectRatio for no black bars
  *reinterpret_cast<float*>(view + 0x38) = 0.0f;  // PostProcessBlendWeight 0, so none of the game camera's post process
}
// The view comes from the player's camera manager. Found live, its view target's POV is at +0xEA0+0x10 and
// its camera cache at +0x1B00, rebuilt every frame from the view target. So the free camera makes the battle
// level's spare CameraActor the view target and moves that actor, and the camera manager reads it in its own
// update, later in the frame. Turning it off brings the game's own view target back.
UObject* g_cam_pc = nullptr;      // the battle's player controller
UObject* g_cam_actor = nullptr;   // the level's spare CameraActor, our camera
UObject* g_cam_orig = nullptr;    // the view target the game had
void setViewTarget(UObject* pc, UObject* target) {
  static UFunction* f = fn(STR("/Script/Engine.PlayerController:SetViewTargetWithBlend"));
  if (!f || !pc || !target) return;
  alignas(8) uint8_t prm[0x20] = {};
  *reinterpret_cast<UObject**>(prm) = target;  // Blend time 0, so it switches at once
  pc->ProcessEvent(f, prm);
}
UObject* cameraManager() { return g_cam_pc ? *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(g_cam_pc) + 0x2C0) : nullptr; }
UObject* viewTarget() { UObject* m = cameraManager(); return m ? *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(m) + 0xEA0) : nullptr; }
float angleTo(float from, float to) { return std::remainder(to - from, 360.0f); }  // the short way round, in degrees
// Screen <-> world through the engine's own projection, the player controller's. That goes through
// GetViewPoint, so it's the view as drawn (ours included), with the engine's own rules for the field of view
// and the frame's shape. Screen positions are in the viewport's pixels. g_vw x g_vh is the viewport, read
// once per camera update (0 when not available, and then the camera's own math is used instead).
float g_vw = 0.0f, g_vh = 0.0f;
void readViewport() {
  static UFunction* f = fn(STR("/Script/Engine.PlayerController:GetViewportSize"));
  g_vw = g_vh = 0.0f;
  if (!f || !g_cam_pc) return;
  alignas(8) uint8_t p[16] = {};  // SizeX at 0, SizeY at 4
  g_cam_pc->ProcessEvent(f, p);
  const int x = *reinterpret_cast<int*>(p), y = *reinterpret_cast<int*>(p + 4);
  if (x > 0 && y > 0) { g_vw = static_cast<float>(x); g_vh = static_cast<float>(y); }
}
bool toScreen(const Vec& q, float s[2]) {
  static UFunction* f = fn(STR("/Script/Engine.PlayerController:ProjectWorldLocationToScreen"));
  if (!f || !g_cam_pc) return false;
  alignas(8) uint8_t p[0x20] = {};  // WorldLocation at 0, ScreenLocation at 0xC, bPlayerViewportRelative 0x14, ReturnValue 0x15
  std::memcpy(p, &q, sizeof(q));
  g_cam_pc->ProcessEvent(f, p);
  if (!p[0x15]) return false;
  std::memcpy(s, p + 0xC, sizeof(float) * 2);
  return true;
}
bool toRay(float sx, float sy, std::array<float, 6>& ray) {
  static UFunction* f = fn(STR("/Script/Engine.PlayerController:DeprojectScreenPositionToWorld"));
  if (!f || !g_cam_pc) return false;
  alignas(8) uint8_t p[0x30] = {};  // ScreenX 0, ScreenY 4, WorldLocation 8, WorldDirection 0x14, ReturnValue 0x20
  const float at[2] = {sx, sy};
  std::memcpy(p, at, sizeof(at));
  g_cam_pc->ProcessEvent(f, p);
  if (!p[0x20]) return false;
  std::memcpy(ray.data(), p + 8, sizeof(float) * 6);
  return true;
}
// The tangent of the view's vertical half-angle. Measured through the engine once our view is up, otherwise
// worked out from the horizontal field of view and the frame's shape.
float halfTanV() {
  std::array<float, 6> mid{}, top{};
  if (g_vw > 0.0f && g_view_on && toRay(g_vw * 0.5f, g_vh * 0.5f, mid) && toRay(g_vw * 0.5f, 0.0f, top)) {
    const float c = mid[3] * top[3] + mid[4] * top[4] + mid[5] * top[5];
    if (c > 0.1f && c < 0.9999f) return std::sqrt(1.0f - c * c) / c;
  }
  return std::tan(g_cam_fov * 0.5f / 57.2958f) / g_aspect;
}
// Sets the goal to the whole fighter from where the camera looks now, with their height and some room in
// view for the frame's shape (the field of view is horizontal).
void portraitGoal() {
  const float half_h = halfTanV();
  g_cam.gpivot[0] = 0.0f; g_cam.gpivot[1] = 0.0f; g_cam.gpivot[2] = g_fighter_h * 0.5f;
  g_cam.gdist = std::clamp(g_fighter_h * 0.625f / (std::max)(half_h, 0.05f), 150.0f, 1500.0f);
  g_cam.gpitch = 5.0f;
  g_cam.gyaw = g_cam.yaw;
}
// Projects a world point to the screen with the camera as last set, giving 0..1 across and down, and its
// depth.
bool project(const Vec& q, float aspect, float& sx, float& sy, float& depth) {
  const float* L = g_cam_pose; const float* F = g_cam_pose + 3; const float* Rv = g_cam_pose + 6; const float* U = g_cam_pose + 9;
  const float v[3] = {q.x - L[0], q.y - L[1], q.z - L[2]};
  depth = v[0] * F[0] + v[1] * F[1] + v[2] * F[2];
  if (depth < 1.0f) return false;
  if (g_vw > 0.0f) {  // The engine's projection, which is the view as drawn
    float px[2];
    if (!toScreen(q, px)) return false;
    sx = px[0] / g_vw;
    sy = px[1] / g_vh;
    return true;
  }
  const float th = std::tan(g_cam_fov * 0.5f / 57.2958f), tv = th / aspect;
  sx = ((v[0] * Rv[0] + v[1] * Rv[1] + v[2] * Rv[2]) / (depth * th) + 1.0f) * 0.5f;
  sy = (1.0f - (v[0] * U[0] + v[1] * U[1] + v[2] * U[2]) / (depth * tv)) * 0.5f;
  return true;
}
// The line of sight through a screen point (pixels of a w x h frame), as origin and direction.
std::array<float, 6> rayAt(float px, float py, int w, int h) {
  std::array<float, 6> ray{};
  if (g_vw > 0.0f && w > 0 && h > 0 && toRay(px * g_vw / w, py * g_vh / h, ray)) return ray;  // the engine's
  const float* L = g_cam_pose; const float* F = g_cam_pose + 3; const float* Rv = g_cam_pose + 6; const float* U = g_cam_pose + 9;
  const float th = std::tan(g_cam_fov * 0.5f / 57.2958f), tv = th * h / w;
  const float nx = px / w * 2.0f - 1.0f, ny = 1.0f - py / h * 2.0f;
  float d[3];
  for (int i = 0; i < 3; i++) d[i] = F[i] + Rv[i] * nx * th + U[i] * ny * tv;
  const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  return {L[0], L[1], L[2], d[0] / n, d[1] / n, d[2] / n};
}
// Where a line of sight passes closest to the fighter's upright axis (from the feet); false if it
// doesn't pass by the fighter.
bool onFighter(const std::array<float, 6>& ray, const Vec& feet, float out[3]) {
  const float ox = ray[0] - feet.x, oy = ray[1] - feet.y, dxy = ray[3] * ray[3] + ray[4] * ray[4];
  if (dxy < 1e-4f) return false;  // looking straight down
  const float s = -(ox * ray[3] + oy * ray[4]) / dxy;
  if (s <= 0.0f) return false;  // the fighter is behind the camera
  out[0] = ox + ray[3] * s; out[1] = oy + ray[4] * s; out[2] = ray[2] + ray[5] * s - feet.z;
  return std::hypot(out[0], out[1]) <= 150.0f && out[2] >= -50.0f && out[2] <= 350.0f;
}
// The point closest to all these lines of sight (least squares), from the feet; false if they
// don't pin one down near the fighter (seen from one side only, or along the same line).
bool nearestToRays(const std::vector<std::array<float, 6>>& rays, const Vec& feet, float out[3]) {
  double A[3][3] = {}, b[3] = {};
  for (const auto& ry : rays)
    for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++) {
        const double m = (i == j ? 1.0 : 0.0) - static_cast<double>(ry[3 + i]) * ry[3 + j];
        A[i][j] += m;
        b[i] += m * ry[j];
      }
  auto det = [](const double M[3][3]) {
    return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
           M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
  };
  const double d = det(A);
  if (std::fabs(d) < 0.05) return false;
  for (int k = 0; k < 3; k++) {
    double M[3][3];
    for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++) M[i][j] = j == k ? b[i] : A[i][j];
    out[k] = static_cast<float>(det(M) / d);
  }
  out[0] -= feet.x; out[1] -= feet.y; out[2] -= feet.z;
  return std::hypot(out[0], out[1]) <= 150.0f && out[2] >= -50.0f && out[2] <= 350.0f;
}
// The view's pixels, made on a worker from what's copied here. One job at a time, and a change during one
// waits.
std::shared_ptr<const DsCache> g_ds;
std::future<void> g_prev_job;
std::string g_prev_key;   // the part shown in the view (game thread)
std::string g_prev_want;  // The same for the workers (under g_prev_m), so a job for another part doesn't publish
void previewPublish(std::vector<uint8_t> px, int w, int h, const float box[4], const std::string& key = std::string(),
                    std::shared_ptr<const DsCache> regions = nullptr) {
  std::lock_guard<std::mutex> lk(g_prev_m);
  if (!key.empty() && key != g_prev_want) return;  // Made for a part that's no longer shown
  g_prev_ds = std::move(regions);  // None for the decal or for nothing, since there are no regions to pick there
  g_prev_px.swap(px);
  g_prev_w = w;
  g_prev_h = h;
  std::memcpy(g_prev_box, box, sizeof(g_prev_box));
  g_prev_serial++;
}
void previewClear() {  // Nothing to show (another part, not ready yet), so the old picture goes
  if (g_prev_job.valid() && g_prev_job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  if (g_prev_job.valid()) g_prev_job.get();
  bool had;
  { std::lock_guard<std::mutex> lk(g_prev_m); had = !g_prev_px.empty(); }
  if (had) { const float none[4] = {}; previewPublish({}, 0, 0, none); }
  g_prev_sig = 0;
}
// The part's region colors at full size, for the view and the fill's reference, made again when its edits
// change. The paint is composed over them, either whole or just what changed since.
uint64_t regionSig(const Target& t) {
  uint64_t sig = 1469598103934665603ull;
  auto mix = [&](uint64_t v) { sig = (sig ^ v) * 1099511628211ull; };
  mix(static_cast<uint64_t>(g_worn_serial));
  mix(g_follow ? 1 : 2);
  const auto& ed = g_edits[keyOf(t)];
  for (size_t r = 0; r < ed.size(); r++)
    if (ed[r].has_lit || ed[r].has_shadow) {
      mix(r);
      mix(static_cast<uint64_t>(ed[r].lit) | (static_cast<uint64_t>(ed[r].has_lit) << 32) | (static_cast<uint64_t>(ed[r].derive) << 33));
      mix(static_cast<uint64_t>(ed[r].shadow) | (static_cast<uint64_t>(ed[r].has_shadow) << 32));
    }
  return sig;
}
bool paintRefresh(PaintPart& p, const Target& t, bool now = false) {  // now gives the fill's reference as it is. False means it was put off (the colors are still moving)
  const uint64_t sig = regionSig(t);
  const paint::Map maps[2] = {paint::Map::Lit, paint::Map::Shadow};
  static uint64_t seen = 0;
  static Clock::time_point since;
  if (sig != seen) { seen = sig; since = Clock::now(); }
  const bool settled = now || Clock::now() - since > std::chrono::milliseconds(250);  // While region colors are being dragged, the last picture stays
  if ((sig != p.base_sig && settled) || p.comp[0].w == 0 || p.comp[1].w == 0) {
    Image ob, os;
    buildW(*g_worn, g_follow, g_edits[keyOf(t)], {}, ob, os);
    p.base[0] = paint::Image{ob.w, ob.h, std::move(ob.px)};
    p.base[1] = paint::Image{os.w, os.h, std::move(os.px)};
    for (int m = 0; m < 2; m++) { p.canvas.compose(maps[m], p.base[m], p.comp[m]); p.canvas.take_dirty(maps[m]); }
    p.base_sig = sig;
    return true;
  }
  for (int m = 0; m < 2; m++) {
    const paint::Rect r = p.canvas.take_dirty(maps[m]);
    if (!r.empty()) p.canvas.compose_rect(maps[m], p.base[m], p.comp[m], r.x0, r.y0, r.x1, r.y1);
  }
  return sig == p.base_sig;
}
void paintChanged(PaintPart& p, const Target& t) {  // The view picks it up. The fighter's textures update at most every 300 ms while stroking
  p.gen++;
  p.snap_stale = true;
  static Clock::time_point live;
  if (!p.stroking || Clock::now() - live > std::chrono::milliseconds(300)) {
    g_dirty = true;
    g_last_change = Clock::now() - std::chrono::milliseconds(100);
    live = Clock::now();
  }
  (void)t;
}
void paintStep(const Target& t, int steps) {  // One undo step made of the canvas's own last `steps`
  if (steps <= 0) return;
  auto& u = g_undo[slotOf(t)];
  u.push_back(Snap{keyOf(t), {}, {}, {}, steps});
  if (u.size() > 100) u.erase(u.begin());
  g_redo[slotOf(t)].clear();
  g_snap_kind = -1;
  markUnsaved(t);
}
void finishStroke(PaintPart& p, const Target& t) {
  if (!p.stroking) return;
  p.canvas.end_stroke();
  int steps = p.canvas.last_changed() ? 1 : 0;  // A stroke that changed nothing makes no step
  if (p.both && p.map == paint::Map::Lit &&
      p.canvas.begin_stroke(paint::Map::Shadow, p.blend, p.rgb, p.size, p.hardness, p.opacity, p.flow, p.eraser)) {  // The same stroke on the shadow
    for (const auto& [px, py] : p.pts) p.canvas.stroke_to(px, py);
    p.canvas.end_stroke();
    steps += p.canvas.last_changed() ? 1 : 0;
  }
  p.canvas.set_mask(nullptr);
  p.stroking = false;
  p.pts.clear();
  paintStep(t, steps);
  paintChanged(p, t);
}
void finishStrokes() {  // Finishes every part's open stroke, on a switch, a close or an undo
  for (auto& [k, p] : g_paint) if (p.stroking) finishStroke(p, fromKey(k));
}
void paintRequest(Target& t, const Req& q) {
  const Worn& w = *g_worn;
  PaintPart* pp = paintFor(t, w.base.w, w.base.h);
  if (!pp) { g_status = "Couldn't paint on this part."; return; }
  PaintPart& p = *pp;
  const float x = (q.nums.size() > 1 ? q.nums[0] : 0) * w.base.w, y = (q.nums.size() > 1 ? q.nums[1] : 0) * w.base.h;
  auto readBrush = [&]() {
    if (q.nums.size() < 13) return false;
    const int map = static_cast<int>(q.nums[2]);
    p.map = map == 1 ? paint::Map::Shadow : paint::Map::Lit;
    p.both = map == 2;
    p.blend = static_cast<paint::Blend>(std::clamp(static_cast<int>(q.nums[3]), 0, 3));
    p.rgb = {static_cast<uint8_t>(std::lround(std::clamp(q.nums[4], 0.0f, 1.0f) * 255)), static_cast<uint8_t>(std::lround(std::clamp(q.nums[5], 0.0f, 1.0f) * 255)),
             static_cast<uint8_t>(std::lround(std::clamp(q.nums[6], 0.0f, 1.0f) * 255)), 255};
    p.size = std::clamp(q.nums[7], 0.5f, 256.0f);
    p.hardness = std::clamp(q.nums[8], 0.0f, 1.0f);
    p.opacity = std::clamp(q.nums[9], 0.0f, 1.0f);
    p.flow = std::clamp(q.nums[10], 0.01f, 1.0f);
    p.eraser = q.nums[11] != 0;
    const bool keep = q.nums[12] != 0;  // only inside the region under the first point
    const int ix = std::clamp(static_cast<int>(x), 0, w.base.w - 1), iy = std::clamp(static_cast<int>(y), 0, w.base.h - 1);
    const uint16_t r = w.owner[static_cast<size_t>(iy) * w.base.w + ix];
    if (keep && r != 0xFFFF) {
      auto mp = g_maps.find(mapKey(t));
      const std::vector<int> lead = leadersOf(t, mp != g_maps.end() ? mp->second->regions : 0);
      const int lr = r < lead.size() ? lead[r] : r;
      p.mask.assign(w.owner.size(), 0);
      for (size_t i = 0; i < w.owner.size(); i++) {
        const uint16_t o = w.owner[i];
        p.mask[i] = o == r || (o < lead.size() && lead[o] == lr);
      }
      p.canvas.set_mask(&p.mask);
    } else {
      p.canvas.set_mask(nullptr);
    }
    return true;
  };
  switch (q.kind) {
    case kPaintBegin:
      finishStrokes();  // A stroke left open gets its own step first
      if (!readBrush()) return;
      if (p.canvas.begin_stroke(p.map, p.blend, p.rgb, p.size, p.hardness, p.opacity, p.flow, p.eraser)) {
        p.canvas.stroke_to(x, y);
        p.pts.assign(1, {x, y});
        p.stroking = true;
        paintChanged(p, t);
      }
      break;
    case kPaintTo:
      if (!p.stroking) return;
      p.canvas.stroke_to(x, y);
      p.pts.push_back({x, y});
      paintChanged(p, t);
      break;
    case kPaintEnd:
      finishStroke(p, t);
      break;
    case kPaintFill: {
      finishStrokes();
      if (!readBrush() || q.nums.size() < 15) return;
      paintRefresh(p, t, true);  // The reference is the picture as it is now
      const int tol = std::clamp(static_cast<int>(q.nums[13]), 0, 255);
      const bool contiguous = q.nums[14] != 0;
      const int ix = std::clamp(static_cast<int>(x), 0, w.base.w - 1), iy = std::clamp(static_cast<int>(y), 0, w.base.h - 1);
      const paint::Image& ref = p.comp[p.map == paint::Map::Shadow ? 1 : 0];
      int steps = p.canvas.fill(p.map, p.blend, p.rgb, p.opacity, ix, iy, ref, tol, contiguous) && p.canvas.last_changed() ? 1 : 0;
      if (p.both && p.map == paint::Map::Lit && p.canvas.fill(paint::Map::Shadow, p.blend, p.rgb, p.opacity, ix, iy, ref, tol, contiguous) && p.canvas.last_changed()) steps++;
      p.canvas.set_mask(nullptr);
      paintStep(t, steps);
      paintChanged(p, t);
      break;
    }
    case kPaintShape: {  // The mask's region is the one under the press point
      finishStrokes();
      if (!readBrush() || q.nums.size() < 17) return;
      const int kind = std::clamp(static_cast<int>(q.nums[13]), 0, 2);
      const float x1 = std::clamp(q.nums[15], -4.0f, 5.0f) * w.base.w, y1 = std::clamp(q.nums[16], -4.0f, 5.0f) * w.base.h;
      if (q.nums[14] != 0 && kind != 0) {  // Filled shapes take the texels of the box from the press texel to the release texel
        const int W = w.base.w, H = w.base.h;
        const int bx0 = static_cast<int>(std::floor((std::min)(x, x1))), bx1 = static_cast<int>(std::floor((std::max)(x, x1))) + 1;
        const int by0 = static_cast<int>(std::floor((std::min)(y, y1))), by1 = static_cast<int>(std::floor((std::max)(y, y1))) + 1;
        const float cx = (bx0 + bx1) * 0.5f, cy = (by0 + by1) * 0.5f, rx = (bx1 - bx0) * 0.5f, ry = (by1 - by0) * 0.5f;
        std::vector<uint8_t> cov(static_cast<size_t>(W) * H, 0);
        for (int yy = (std::max)(by0, 0); yy < (std::min)(by1, H); yy++)
          for (int xx = (std::max)(bx0, 0); xx < (std::min)(bx1, W); xx++) {
            const float dx = (xx + 0.5f - cx) / rx, dy = (yy + 0.5f - cy) / ry;  // Texel centres inside the ellipse
            cov[static_cast<size_t>(yy) * W + xx] = kind == 1 || dx * dx + dy * dy <= 1.0f;
          }
        int steps = p.canvas.fill_shape(p.map, p.blend, p.rgb, p.opacity, cov) && p.canvas.last_changed() ? 1 : 0;
        if (p.both && p.map == paint::Map::Lit && p.canvas.fill_shape(paint::Map::Shadow, p.blend, p.rgb, p.opacity, cov) && p.canvas.last_changed()) steps++;
        p.canvas.set_mask(nullptr);
        paintStep(t, steps);
        paintChanged(p, t);
        break;
      }
      std::vector<std::pair<float, float>> pts;  // An outline is one closed stroke along the edge
      if (kind == 0) {
        pts = {{x, y}, {x1, y1}};
      } else if (kind == 1) {
        pts = {{x, y}, {x1, y}, {x1, y1}, {x, y1}, {x, y}};
      } else {
        const float cx = (x + x1) * 0.5f, cy = (y + y1) * 0.5f, rx = std::fabs(x1 - x) * 0.5f, ry = std::fabs(y1 - y) * 0.5f;
        const int n = std::clamp(static_cast<int>(std::ceil(6.2832f * std::sqrt((rx * rx + ry * ry) * 0.5f) / 2.0f)), 24, 4096);  // A segment about every 2 texels
        for (int i = 0; i <= n; i++) {
          const float a = 6.2831853f * static_cast<float>(i % n) / static_cast<float>(n);
          pts.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a)});
        }
      }
      p.eraser = false;
      if (p.canvas.begin_stroke(p.map, p.blend, p.rgb, p.size, p.hardness, p.opacity, p.flow, false)) {
        for (const auto& [px, py] : pts) p.canvas.stroke_to(px, py);
        p.pts = std::move(pts);
        p.stroking = true;
        finishStroke(p, t);  // One step. The shadow takes the same points for "both"
      } else {
        p.canvas.set_mask(nullptr);
      }
      break;
    }
    case kPaintClear: {
      finishStrokes();
      const int map = q.nums.empty() ? 2 : static_cast<int>(q.nums[0]);
      int steps = 0;
      if (map != 1 && p.canvas.clear(paint::Map::Lit) && p.canvas.last_changed()) steps++;
      if (map != 0 && p.canvas.clear(paint::Map::Shadow) && p.canvas.last_changed()) steps++;
      paintStep(t, steps);
      paintChanged(p, t);
      break;
    }
    default:
      break;
  }
}
// A texture edited elsewhere (Photoshop...), or any image of the part's size, ticked in Import textures.
// Whatever differs from its baseline becomes Normal paint at full strength (one undo step), so region colors
// still work everywhere else. Its name says the map. "_Shadow", "sss" or "shadow" in it means the shadow
// color, else lit. The baseline for an exported file is the export's own copy (.exported/), so colors changed
// in the editor since aren't put back. For anything else it's the picture as shown now. A new export's
// "<Part>_Lit" / "<Part>_Shadow" name says its part too, and another part's file is turned away (show that
// part first).
// The part a new export's file name says ("Body_Lit.png"), of this fighter, and which color (0 lit, 1 shadow). Null
// for a name that says none; *many is set when it fits more than one.
const Target* texPartOf(const Target& t, const std::filesystem::path& file, int& m, bool& many) {
  const std::wstring stem = file.stem().wstring();
  const Target* named = nullptr;
  many = false;
  m = 0;
  for (const auto& [tail, mm] : {std::pair<const wchar_t*, int>{L"_Lit", 0}, {L"_Shadow", 1}, {L"_material_colors", 2}}) {
    const size_t n = wcslen(tail);
    if (named || stem.size() <= n || _wcsicmp(stem.c_str() + stem.size() - n, tail) != 0) continue;
    const std::wstring part = stem.substr(0, stem.size() - n);
    std::set<std::string> kinds;
    for (const Target& o : g_targets)
      if (o.pawn == t.pawn && o.chr == t.chr && o.color == t.color && _wcsicmp(wide(exportStem(o)).c_str(), part.c_str()) == 0 && kinds.insert(o.kind).second)
        named = o.kind == t.kind ? &t : &o;
    if (kinds.size() > 1) many = true;
    m = mm;
  }
  return named;
}
// An export's effects.txt onto the shown slot's effect colors (by label). True if any changed.
bool importFxNow(const Target& t, const std::filesystem::path& file) {
  const std::string fname = narrow(file.filename().wstring());
  const std::string slot = t.chr + "/" + t.color;
  if (g_fx_for != slot) return false;
  std::map<std::string, std::array<float, 3>> rows;
  for (const std::string& l : readLines(file)) {
    const size_t tab = l.find('\t');
    if (l.empty() || l[0] == '#' || tab == std::string::npos) continue;
    std::array<float, 3> c{};
    if (sscanf_s(l.c_str() + tab + 1, "%f %f %f", &c[0], &c[1], &c[2]) != 3) continue;
    if (std::all_of(c.begin(), c.end(), [](float f) { return std::isfinite(f) && f >= 0.0f && f <= 1000.0f; }) && c[0] + c[1] + c[2] > 0.0f)
      rows[l.substr(0, tab)] = c;  // (not a black one: it would black out every color of that table)
  }
  int set = 0;
  for (FxColor& f : fxList()) {
    auto it = rows.find(f.label);
    if (it == rows.end() || std::equal(it->second.begin(), it->second.end(), f.rgb, [](float a, float b) { return std::fabs(a - b) <= 1e-4f * (std::max)(1.0f, std::fabs(b)); })) continue;  // (the file has 6 digits)
    if (!set) { pushUndo(t, kFx, -1); g_snap_kind = -1; }
    std::memcpy(f.rgb, it->second.data(), sizeof(f.rgb));
    f.edited = true;
    g_fx_saved[slot][f.label] = it->second;
    set++;
  }
  if (set) {
    takeAllByName("");  // (every loaded effect taking any of them, once)
    for (FxColor& f : fxList()) if (rows.count(f.label)) fxApply(f);
    markUnsaved(t);
    g_unsaved_slots.insert(slot);
  }
  g_status = set ? "Imported " + fname + " (" + std::to_string(set) + " effect colors)." : fname + " has no changes to import (the effect colors).";
  COMBO_LOG("color editor: " + g_status);
  return set > 0;
}
// An export's <Part>_material_colors.txt onto the shown part: each row's color on the material color with its label
// (the part's material colors have to be found already). True if any changed.
bool importMatColorsNow(const Target& t, const std::filesystem::path& file) {
  const std::string fname = narrow(file.filename().wstring());
  std::map<std::string, std::array<float, 4>> rows;  // label -> color
  for (const std::string& l : readLines(file)) {
    if (l.empty() || l[0] == '#') continue;
    std::vector<std::string> col;
    for (size_t a = 0, b; a <= l.size(); a = b + 1) { b = l.find('\t', a); if (b == std::string::npos) b = l.size(); col.push_back(l.substr(a, b - a)); }
    if (col.size() < 4) continue;
    std::array<float, 4> c{};
    if (sscanf_s(col[3].c_str(), "%f %f %f %f", &c[0], &c[1], &c[2], &c[3]) != 4) continue;
    if (std::all_of(c.begin(), c.end(), [](float f) { return std::isfinite(f) && f >= 0.0f && f <= 1000.0f; })) rows[col[1]] = c;
  }
  if (rows.empty()) { g_status = fname + " has no material colors in it."; COMBO_LOG("color editor: " + g_status); return false; }
  int set = 0;
  bool undo = false;
  for (VecGroup& g : g_vecs) {
    auto it = rows.find(g.label);
    if (it == rows.end() || g.label == kDecalLabel) continue;
    if (std::equal(it->second.begin(), it->second.end(), g.cur, [](float a, float b) { return std::fabs(a - b) <= 1e-4f * (std::max)(1.0f, std::fabs(b)); })) continue;  // (the file has 6 digits)
    if (!undo) { pushUndo(t, kVec, -1); g_snap_kind = -1; undo = true; }  // (its own step, never merged with the next)
    std::memcpy(g.cur, it->second.data(), sizeof(g.cur));
    g.edited = true;
    g_vec_saved[keyOf(t)][g.label] = it->second;
    for (size_t k = 0; k < g.mids.size(); k++) vecSet(g.mids[k], g.paramOf(k), g.cur);
    set++;
  }
  if (set) markUnsaved(t);
  g_status = set ? "Imported " + fname + " (" + std::to_string(set) + " material colors)." : fname + " has no changes to import (the material colors).";
  COMBO_LOG("color editor: " + g_status);
  return set > 0;
}
bool importTexturesNow(const Target& t, const std::filesystem::path& file) {  // (game thread) True if it changed the part
  if (file.empty()) return false;
  const std::string fname = narrow(file.filename().wstring());
  const std::wstring stem = file.stem().wstring();
  int named_m = 0;
  bool many = false;
  const Target* named = texPartOf(t, file, named_m, many);  // The part a new export's name says
  if (many) { g_status = fname + ": which part? Its name fits more than one."; COMBO_LOG("color editor: " + g_status); return false; }
  if (named && named != &t) { g_status = fname + " is " + partName(named->kind) + "'s texture: show that part first."; COMBO_LOG("color editor: " + g_status); return false; }
  finishStrokes();
  const Worn& w = *g_worn;
  PaintPart* pp = paintFor(t, w.base.w, w.base.h);
  if (!pp) { g_status = "Couldn't import on this part."; return false; }
  PaintPart& p = *pp;
  paintRefresh(p, t, true);  // The picture as it is now
  std::wstring lower = file.filename().wstring();
  for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
  const bool says_base = lower.find(L"basemap") != std::wstring::npos, says_sss = lower.find(L"sssmap") != std::wstring::npos;  // An exported name decides
  const int m = named ? named_m : says_sss ? 1 : says_base ? 0 : lower.find(L"sss") != std::wstring::npos || lower.find(L"shadow") != std::wstring::npos ? 1 : 0;
  const char* names[2] = {"BaseMap", "SssMap"};
  const char* says[2] = {"the lit color", "the shadow color"};
  Image im;
  if (!decodePng(file, im)) { g_status = "Couldn't read " + fname + " (PNG, BMP or TIFF)."; return false; }
  const paint::Image& now = p.comp[m];
  if (im.w != now.w || im.h != now.h) {
    g_status = fname + " is the wrong size (needs " + std::to_string(now.w) + "x" + std::to_string(now.h) + ").";
    return false;
  }
  const std::filesystem::path dir = g_dir / "export" / t.chr / t.color;
  const std::string prefix = t.kind == "MAIN" ? std::string() : t.kind + "_";
  std::error_code ec;
  Image exported;
  bool from_export = false;
  std::filesystem::path root = file.parent_path();  // A new export's file sits in its dated folder or that folder's alpha/
  if (_wcsicmp(root.filename().wstring().c_str(), L"alpha") == 0) root = root.parent_path();
  if (named && std::filesystem::equivalent(root.parent_path(), dir, ec)) {  // This slot's export, so use that folder's copy of it
    from_export = decodePng(root / ".exported" / (stem + L".png"), exported) && exported.w == now.w && exported.h == now.h;
  } else if (std::filesystem::equivalent(file.parent_path(), dir, ec)) {  // An old export's file is compared with what was exported
    const std::string own_case = prefix + names[m];
    std::string own = own_case;
    for (auto& c : own) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool own_name = lower.rfind(std::wstring(own.begin(), own.end()), 0) == 0;
    if (!own_name && (says_base || says_sss)) {  // An exported name, but another part's, like WEP_BaseMap with the body shown
      g_status = fname + " is another part's texture: show that part first.";
      return false;
    }
    const std::wstring stem = file.stem().wstring();
    const bool plain = _wcsicmp(stem.c_str(), std::wstring(own_case.begin(), own_case.end()).c_str()) == 0 || _wcsicmp(stem.c_str(), (std::wstring(own_case.begin(), own_case.end()) + L"_view").c_str()) == 0;
    const std::filesystem::path copy = plain ? exportedCopy(dir, prefix + names[m] + ".png") : dir / ".exported" / file.filename();  // (a kept edit: its own)
    from_export = own_name && decodePng(copy, exported) && exported.w == now.w && exported.h == now.h;  // (a name of your own: the picture as shown)
  }
  const paint::Image exported_px = from_export ? paint::Image{exported.w, exported.h, std::move(exported.px)} : paint::Image{};
  const paint::Image& base = from_export ? exported_px : now;
  // See-through texels. An editor that drops their colors leaves them all one color, and then they don't count
  // as edits.
  bool any_clear = false, one_color = true;
  uint8_t first[3] = {};
  for (size_t i = 0; i + 3 < im.px.size(); i += 4) {
    if (im.px[i + 3] != 0) continue;
    if (!any_clear) { std::memcpy(first, &im.px[i], 3); any_clear = true; }
    else if (std::memcmp(first, &im.px[i], 3) != 0) { one_color = false; break; }
  }
  if (any_clear && one_color)
    for (size_t i = 0; i + 3 < im.px.size(); i += 4)
      if (im.px[i + 3] == 0) std::memcpy(&im.px[i], &base.px[i], 3);
  const paint::Image want{im.w, im.h, std::move(im.px)};
  const paint::Map map = m ? paint::Map::Shadow : paint::Map::Lit;
  // The regions take the picture's colors (this map's: lit or shadow), so the list shows the imported look
  bool regions = false;
  paint::Image shown;  // the picture with the new region colors and the paint as it is
  std::vector<Edit> ed_before, ed = g_edits[keyOf(t)];  // (the new colors go in only once the paint has taken the file)
  if (!from_export) {  // (an edited copy of this slot's own export keeps its compare: only what was changed in it comes in)
    const int R = static_cast<int>(w.lit.size());
    if (static_cast<int>(ed.size()) < R) ed.resize(static_cast<size_t>(R));
    if (w.owner.size() == static_cast<size_t>(want.w) * want.h && w.base.w == want.w && w.base.h == want.h) {
      std::vector<std::unordered_map<uint32_t, uint32_t>> counts(static_cast<size_t>(R));
      std::vector<std::array<uint64_t, 3>> sums(static_cast<size_t>(R), {0, 0, 0});
      std::vector<uint32_t> n(static_cast<size_t>(R), 0);
      const Image& own = m ? w.sss : w.base;
      for (size_t i = 0; i < w.owner.size(); i += 2) {  // (every other texel is plenty) its own-color texels only
        const uint16_t r = w.owner[i];
        if (r >= R || rgbAt(own, i) != (m ? w.shadow[r] : w.lit[r])) continue;
        const uint32_t c = uint32_t(want.px[i * 4]) << 16 | uint32_t(want.px[i * 4 + 1]) << 8 | want.px[i * 4 + 2];
        counts[r][c]++;
        sums[r][0] += want.px[i * 4]; sums[r][1] += want.px[i * 4 + 1]; sums[r][2] += want.px[i * 4 + 2];
        n[r]++;
      }
      std::vector<std::pair<int, uint32_t>> set;
      for (int r = 0; r < R; r++) {
        if (!n[r]) continue;
        const uint32_t c = regionColor(counts[r], sums[r].data(), n[r]);
        const std::pair<uint32_t, uint32_t> now_pair = pairOf(r, ed);
        if (c != (m ? now_pair.second : now_pair.first)) set.push_back({r, c});
      }
      if (!set.empty()) {
        ed_before = ed;
        for (const auto& [r, c] : set) {
          Edit& e = ed[static_cast<size_t>(r)];
          if (m) { e.has_shadow = true; e.shadow = c; }
          else { e.has_lit = true; e.lit = c; e.derive = false; }
        }
        regions = true;
        Image ob, os;
        buildW(w, g_follow, ed, {}, ob, os);
        Image& im2 = m ? os : ob;
        const paint::Image fresh{im2.w, im2.h, std::move(im2.px)};
        if (!p.canvas.compose(map, fresh, shown)) shown = fresh;
      }
    }
  }
  if (!p.canvas.set_texels(map, regions ? shown : base, want)) { g_status = "Couldn't take in " + fname + "."; return false; }
  const bool changed = p.canvas.last_changed();
  if (regions) {  // The colors and the paint are one undo step
    g_edits[keyOf(t)] = ed;
    auto& u = g_undo[slotOf(t)];
    if (changed) u.push_back(Snap{keyOf(t), ed_before, {}, {}, 1, true});
    else { Snap sn = snapOf(t); sn.ed = ed_before; u.push_back(std::move(sn)); }
    if (u.size() > 100) u.erase(u.begin());
    g_redo[slotOf(t)].clear();
    g_snap_kind = -1;
    markUnsaved(t);
    g_dirty = true;
    g_last_change = Clock::now() - std::chrono::seconds(1);
  } else {
    paintStep(t, changed ? 1 : 0);
  }
  if (changed) paintChanged(p, t);
  g_status = changed || regions ? "Imported " + fname + "." : fname + " has no changes to import (" + says[m] + ").";
  COMBO_LOG("color editor: " + g_status + (regions ? " The regions took its colors." : ""));
  return changed || regions;
}
void previewTick(const Target& t) {
  if (g_prev_job.valid()) {
    if (g_prev_job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    g_prev_job.get();
  }
  if (g_prev_key != keyOf(t)) {
    g_prev_key = keyOf(t);
    { std::lock_guard<std::mutex> lk(g_prev_m); g_prev_want = g_prev_key; }
    previewClear();
  }
  const Worn& w = *g_worn;
  auto mp = g_maps.find(mapKey(t));
  const int major = mp == g_maps.end() ? 0x7FFF : mp->second->major;
  if (g_ds_for != g_worn_serial || !g_ds) {  // the texture at about 1024, once per worn color
    auto ds = std::make_shared<DsCache>();
    const int f = (std::max)(1, (std::max)(w.base.w, w.base.h) / 1024), W = w.base.w / f, H = w.base.h / f;
    ds->owner.assign(static_cast<size_t>(W) * H, 0xFFFF);
    ds->base.assign(static_cast<size_t>(W) * H, 0);
    ds->sss.assign(static_cast<size_t>(W) * H, 0);
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        const size_t mid = static_cast<size_t>(y * f + f / 2) * w.base.w + (x * f + f / 2);
        uint16_t best = w.owner[mid];
        for (int by = 0; by < f && f > 1; by++)
          for (int bx = 0; bx < f; bx++) {
            const uint16_t r = w.owner[static_cast<size_t>(y * f + by) * w.base.w + (x * f + bx)];
            if (r != 0xFFFF && r >= major && (best == 0xFFFF || best < major || r > best)) best = r;
          }
        ds->owner[static_cast<size_t>(y) * W + x] = best;
        ds->base[static_cast<size_t>(y) * W + x] = rgbAt(w.base, mid);
        ds->sss[static_cast<size_t>(y) * W + x] = rgbAt(w.sss, mid);
      }
    ds->w = W;
    ds->h = H;
    g_ds = ds;
    g_ds_for = g_worn_serial;
    g_prev_sig = 0;
  }
  const int map = g_texview_map;
  const DecalTint* tint = nullptr;
  for (const auto& [key, d] : g_decals) if (key.second == keyOf(t)) { tint = &d; break; }
  const DecalPix* dpx = tint ? &g_decal_pix[tint->tex] : nullptr;
  const bool decal = map == 2 && dpx && dpx->img.w > 0;
  const auto& ed = g_edits[keyOf(t)];
  uint64_t sig = 1469598103934665603ull;
  auto mix = [&](uint64_t v) { sig = (sig ^ v) * 1099511628211ull; };
  mix(static_cast<uint64_t>(g_worn_serial));
  mix(0x500000000ull + static_cast<uint64_t>(decal ? 2 : map == 1 ? 1 : 0));
  if (decal) {
    mix(reinterpret_cast<uintptr_t>(tint->tex));
    mix(tint->edited ? 1 : 0);
    for (float c : tint->want) mix(static_cast<uint64_t>(std::lround(c * 1000.0f)));
  }
  mix(g_follow ? 1 : 2);
  for (int r : g_selected) mix(0x100000000ull + static_cast<uint64_t>(r));
  for (size_t r = 0; r < ed.size(); r++)
    if (ed[r].has_lit || ed[r].has_shadow) {
      mix(r);
      mix(static_cast<uint64_t>(ed[r].lit) | (static_cast<uint64_t>(ed[r].has_lit) << 32) | (static_cast<uint64_t>(ed[r].derive) << 33));
      mix(static_cast<uint64_t>(ed[r].shadow) | (static_cast<uint64_t>(ed[r].has_shadow) << 32));
    }
  const bool plain = g_texview_plain;
  mix(plain ? 11 : 12);
  PaintPart* pp = hasPaint(t) || g_paint.count(keyOf(t)) ? paintFor(t, w.base.w, w.base.h) : nullptr;
  const bool painted = pp && !pp->canvas.empty();
  if (painted) mix(0x700000000ull + pp->gen);
  if (sig == g_prev_sig || Clock::now() - g_prev_at < std::chrono::milliseconds(painted && pp->stroking ? 40 : 100)) return;
  g_prev_sig = sig;
  g_prev_at = Clock::now();
  if (decal) {  // the decal at about 1024, tinted as it's worn
    const bool edited = tint->edited;
    const float smax = dpx->smax, T[3] = {std::clamp(tint->want[0], 0.0f, 1.0f), std::clamp(tint->want[1], 0.0f, 1.0f), std::clamp(tint->want[2], 0.0f, 1.0f)};
    g_prev_job = std::async(std::launch::async, [im = dpx->img, edited, smax, T, key = keyOf(t)]() {
      const int f = (std::max)(1, (std::max)(im.w, im.h) / 1024), W = im.w / f, H = im.h / f;
      std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 255);
      for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
          const uint32_t c = rgbAt(im, static_cast<size_t>(y * f + f / 2) * im.w + (x * f + f / 2));
          uint8_t* o = &px[(static_cast<size_t>(y) * W + x) * 4];
          for (int k = 0; k < 3; k++) o[k] = static_cast<uint8_t>(c >> (16 - 8 * k) & 0xFF);
          if (edited && smax > 0) {
            const float st = 1.0f - static_cast<float>(o[0] + o[1] + o[2]) / 765.0f, kk = std::clamp(st / smax, 0.0f, 1.0f);
            for (int k = 0; k < 3; k++) o[k] = static_cast<uint8_t>(std::lround(255.0f - kk * (255.0f - 255.0f * T[k])));
          }
        }
      const float none[4] = {};
      previewPublish(std::move(px), W, H, none, key);
    });
    return;
  }
  const bool shadow = map == 1;
  const int R = static_cast<int>(w.lit.size());
  std::vector<std::array<int, 3>> off(static_cast<size_t>(R), {0, 0, 0});
  for (int r = 0; r < R; r++) {
    const auto pr = pairOfW(w, g_follow, r, ed);
    const uint32_t want = shadow ? pr.second : pr.first, own = shadow ? w.shadow[r] : w.lit[r];
    for (int c = 0; c < 3; c++) off[r][c] = int(want >> (16 - 8 * c) & 0xFF) - int(own >> (16 - 8 * c) & 0xFF);
  }
  std::vector<uint8_t> sel(static_cast<size_t>(R), 0);
  for (int r : g_selected) if (r >= 0 && r < R) sel[r] = 1;
  const bool any = !g_selected.empty() && !plain;
  std::shared_ptr<const paint::Image> comp;  // the part with its paint, sampled at the view's size (for the worker)
  if (painted) {
    if (!paintRefresh(*pp, t)) g_prev_sig = 0;  // Put off while the region colors move, and asked again next time
    const paint::Image& full = pp->comp[shadow ? 1 : 0];
    if (full.w > 0 && g_ds && g_ds->w > 0 && full.w / g_ds->w > 0) {
      const int cf = full.w / g_ds->w;
      auto view_img = std::make_shared<paint::Image>();
      view_img->w = g_ds->w;
      view_img->h = g_ds->h;
      view_img->px.resize(static_cast<size_t>(view_img->w) * view_img->h * 4);
      for (int yy = 0; yy < view_img->h; yy++)
        for (int xx = 0; xx < view_img->w; xx++)
          std::memcpy(&view_img->px[(static_cast<size_t>(yy) * view_img->w + xx) * 4],
                      &full.px[(static_cast<size_t>(yy * cf + cf / 2) * full.w + (xx * cf + cf / 2)) * 4], 4);
      comp = view_img;
    }
  }
  g_prev_job = std::async(std::launch::async, [ds = g_ds, off = std::move(off), sel = std::move(sel), any, shadow, R, key = keyOf(t), comp]() {
    const int cf = comp && comp->w == ds->w ? 1 : 0;  // The composed picture, at the view's size
    std::vector<uint8_t> px(static_cast<size_t>(ds->w) * ds->h * 4, 255);
    int x0 = ds->w, y0 = ds->h, x1 = -1, y1 = -1;
    for (int y = 0; y < ds->h; y++)
      for (int x = 0; x < ds->w; x++) {
        const size_t q = static_cast<size_t>(y) * ds->w + x;
        const uint16_t r = ds->owner[q];
        const uint32_t c = shadow ? ds->sss[q] : ds->base[q];
        const bool in = r != 0xFFFF && r < R;
        const bool lit = in && sel[r];
        const uint8_t* cp = cf > 0 ? &comp->px[q * 4] : nullptr;  // Painted texels use the composed picture
        for (int k = 0; k < 3; k++) {
          int v = cp ? cp[k] : std::clamp(int(c >> (16 - 8 * k) & 0xFF) + (in ? off[r][k] : 0), 0, 255);
          if (any && !lit) v = v * 3 / 10;  // The rest is dimmed
          px[q * 4 + k] = static_cast<uint8_t>(v);
        }
        if (lit) { x0 = (std::min)(x0, x); y0 = (std::min)(y0, y); x1 = (std::max)(x1, x); y1 = (std::max)(y1, y); }
      }
    float box[4] = {};
    if (x1 >= 0) {
      box[0] = static_cast<float>(x0) / ds->w; box[1] = static_cast<float>(y0) / ds->h;
      box[2] = static_cast<float>(x1 + 1) / ds->w; box[3] = static_cast<float>(y1 + 1) / ds->h;
    }
    previewPublish(std::move(px), ds->w, ds->h, box, key, ds);
  });
}
void stopFocus(Target* t) {
  if (g_pick_scanning) scanOff(t, true);  // A pick is cut off, and its frames are kept a while
  if (g_pick_wanted || g_pick_scanning) { g_status = "Pick cancelled."; g_view_dirty = true; }
  g_pick_wanted = g_pick_scanning = false;
  if (g_focus == Focus::Probe) scanOff(t, false);
  g_focus = Focus::Idle;
  g_look = 0;
  g_probe_looks = false;
}
void stopMapping(Target* t) {
  if (!g_mapping) return;
  scanOff(t, false);
  g_mapping.reset();
  g_cam_info = kCamHelp;
  g_view_dirty = true;
}
// A mapping cut off by a new fight or new materials (its frames sat on the old ones) is let go, and maps
// again when the editor is next on this fighter.
void dropMapping() {
  if (!g_mapping) return;
  g_map_tried.erase(g_mapping->key);
  g_mapping.reset();
  g_cam_info = g_cam.on ? std::string(kCamHelp) : std::string();
}
void cameraOff() {
  g_view_on = false;
  if (!g_view_hooked && g_cam_pc && g_cam_orig && asw_engine::get() == g_fight && viewTarget() == g_cam_actor) setViewTarget(g_cam_pc, g_cam_orig);
  g_cam_pc = g_cam_actor = g_cam_orig = nullptr;
  g_cam.ready = false;
  g_keep_dist = 0.0f;
  stopFocus(target());
  stopMapping(target());
}
// The selection's middle and box from a probe's samples, using the middle 90% so strays are left out.
FocusHit hitOf(const std::vector<ScanSample>& s, int w, int h) {
  FocusHit hit;
  hit.ready = true;
  std::vector<int> xs, ys;
  for (const ScanSample& p : s) if (p.id == 1) { xs.push_back(p.x); ys.push_back(p.y); }
  if (xs.size() < 4 || w <= 0 || h <= 0) return hit;
  std::sort(xs.begin(), xs.end());
  std::sort(ys.begin(), ys.end());
  const size_t n = xs.size(), a = n * 5 / 100, b = n - 1 - n * 5 / 100;
  hit = {true, true, xs[n / 2] + 1.0f, ys[n / 2] + 1.0f, static_cast<float>(xs[a]), static_cast<float>(ys[a]), xs[b] + 2.0f, ys[b] + 2.0f, w, h};
  return hit;
}
// Sets the goal on a point of the fighter (from the feet), keeping the user's distance. It backs off a
// little only when the part, frac of the view at distance at, wouldn't fit.
void goTo(const float p[3], float frac, float at) {
  std::memcpy(g_cam.gpivot, p, sizeof(float) * 3);
  if (g_keep_dist <= 0.0f) g_keep_dist = g_cam.gdist;  // Set once, so a flight's own backing off doesn't add up
  const float keep = g_keep_dist;
  const float want = frac * at / keep;  // of the view, at that distance
  g_cam.gdist = want > 0.8f ? (std::min)(keep * want / 0.7f, keep * 1.3f) : keep;
}
// Aims at what the probe found. The ray through its middle meets the fighter where it passes closest to
// their upright axis. Returns false when nothing is usable, and then nothing moves.
bool aimAt(const FocusHit& hit, const Vec& feet) {
  if (!hit.ready || !hit.found || hit.w <= 0 || hit.h <= 0) return false;
  const float bw = (hit.x1 - hit.x0) / hit.w, bh = (hit.y1 - hit.y0) / hit.h;
  if (bw > 0.9f || bh > 0.95f) return false;  // Most of the screen, so not a part
  float p[3];
  if (!onFighter(rayAt(hit.cx, hit.cy, hit.w, hit.h), feet, p)) return false;
  goTo(p, (std::max)(bw, bh), g_cam.dist);
  g_look = 1;  // Once there, find its own pixels for the label
  COMBO_LOG("color editor: found at " + std::to_string(p[0]) + " " + std::to_string(p[1]) + " " + std::to_string(p[2]) + " from the feet");
  return true;
}
// The selection's pixel nearest the middle of the screen whose line of sight passes the fighter (a real
// pixel of it, even when it's in pieces), as that point from the feet. False if none does, meaning it's not
// the fighter.
bool spotOf(const std::vector<ScanSample>& s, int w, int h, const Vec& feet, float out[3]) {
  if (w <= 0 || h <= 0) return false;
  std::vector<std::pair<long long, const ScanSample*>> by;  // by distance from the middle
  for (const ScanSample& q : s) {
    if (q.id != 1) continue;
    const long long dx = 2LL * q.x + 2 - w, dy = 2LL * q.y + 2 - h;
    by.push_back({dx * dx + dy * dy, &q});
  }
  if (by.size() < 4) return false;
  std::sort(by.begin(), by.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [d, q] : by)  // A weapon's tip can be off the fighter, so try the next one in
    if (onFighter(rayAt(q->x + 1.0f, q->y + 1.0f, w, h), feet, out)) return true;
  return false;
}
// Turns the camera where it stands to face a point (from the feet). It lands in the middle of the screen at
// the size it is now, and the camera orbits it from then on.
void face(const float p[3], const Vec& feet) {
  const float d[3] = {g_cam_pose[0] - (feet.x + p[0]), g_cam_pose[1] - (feet.y + p[1]), g_cam_pose[2] - (feet.z + p[2])};  // to the camera
  const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  if (n < 1.0f) return;
  std::memcpy(g_cam.gpivot, p, sizeof(float) * 3);
  g_cam.gdist = n;
  g_cam.gpitch = std::clamp(std::asin(std::clamp(d[2] / n, -1.0f, 1.0f)) * 57.2958f, -85.0f, 85.0f);
  g_cam.gyaw = g_cam.yaw + angleTo(g_cam.yaw, std::atan2(d[1], d[0]) * 57.2958f);
}
namespace {
  // ---- The bone atlas (StriveLabs_colors/atlas/<CHR>.bin, made from the game's own mesh files by make_atlas.py).
  // For each mesh and material slot, a grid over the texture whose cells hold where the surface drawn there is: the
  // bone it moves with, the point in that bone's space and the way it faces. A mirrored part (both legs on one
  // texture) has a point per side. Joined with a part's regions, every region the model draws has places. --------
  struct AtlasPoint { uint16_t cx = 0, cy = 0, bone = 0; int16_t p[3] = {}; int8_t n[3] = {}; };
  struct AtlasMesh {
    int grid = 0;
    std::vector<std::string> bones, slot_names;
    std::vector<int16_t> parent;               // (v2) each bone's parent, -1 for none
    std::vector<std::array<float, 7>> rest;    // (v2) each bone's rest location x y z and rotation x y z w, in the mesh's space
    std::vector<std::vector<AtlasPoint>> slots;
  };
  std::map<std::string, std::map<std::string, AtlasMesh>> g_atlas;  // by character ("SOL", "ANJ_C02"), then mesh name in lower case
  std::string lowered(std::string s) { for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); return s; }
  const std::map<std::string, AtlasMesh>& atlasOf(const std::string& chr) {  // Read once a session (empty if there's none or it's bad)
    auto it = g_atlas.find(chr);
    if (it != g_atlas.end()) return it->second;
    std::map<std::string, AtlasMesh>& out = g_atlas[chr];
    const std::vector<uint8_t> b = readBytes(g_dir / "atlas" / (chr + ".bin"));
    if (b.empty()) return out;
    size_t o = 0;
    auto take = [&](void* d, size_t n) { if (n > b.size() - o) return false; std::memcpy(d, b.data() + o, n); o += n; return true; };
    auto str = [&](std::string& v) {
      uint16_t n = 0;
      if (!take(&n, 2) || n > b.size() - o) return false;
      v.assign(reinterpret_cast<const char*>(b.data() + o), n);
      o += n;
      return true;
    };
    uint32_t magic = 0, ver = 0, grid = 0, meshes = 0;
    bool ok = take(&magic, 4) && magic == 0x54414C53 /* "SLAT" */ && take(&ver, 4) && (ver == 1 || ver == 2) && take(&grid, 4) && grid >= 8 && grid <= 1024 &&
              take(&meshes, 4) && meshes <= 4096;
    size_t points = 0;
    for (uint32_t m = 0; ok && m < meshes; m++) {
      std::string name;
      uint16_t nb = 0;
      uint32_t ns = 0;
      AtlasMesh am;
      am.grid = static_cast<int>(grid);
      ok = str(name) && take(&nb, 2);
      am.bones.resize(ok ? nb : 0);
      if (ver == 2) { am.parent.assign(am.bones.size(), -1); am.rest.assign(am.bones.size(), {}); }
      for (size_t k = 0; ok && k < am.bones.size(); k++) {
        ok = str(am.bones[k]);
        if (ver == 2) ok = ok && take(&am.parent[k], 2) && am.parent[k] < static_cast<int>(nb) && take(am.rest[k].data(), 28) &&
                           std::all_of(am.rest[k].begin(), am.rest[k].end(), [](float f) { return std::isfinite(f); });
      }
      ok = ok && take(&ns, 4) && ns <= 256;
      for (uint32_t k = 0; ok && k < ns; k++) {
        std::string slot;
        uint32_t n = 0;
        ok = str(slot) && take(&n, 4) && n <= (b.size() - o) / 15;
        std::vector<AtlasPoint> pts(ok ? n : 0);
        for (AtlasPoint& q : pts)
          ok = ok && take(&q.cx, 2) && take(&q.cy, 2) && take(&q.bone, 2) && take(q.p, 6) && take(q.n, 3) && q.bone < nb && q.cx < grid && q.cy < grid;
        points += pts.size();
        am.slots.push_back(std::move(pts));
        am.slot_names.push_back(slot);
      }
      if (ok) out.emplace(lowered(name), std::move(am));  // (the first of a name)
    }
    if (!ok) out.clear();  // A bad file is never trusted: the part is scanned as before
    COMBO_LOG("color editor: bone atlas for " + chr + ": " + (ok ? std::to_string(out.size()) + " meshes, " + std::to_string(points) + " points" : std::string("couldn't be read")));
    return out;
  }
  const AtlasMesh* atlasMesh(const std::string& chr, const std::string& mesh) {  // (a second costume borrows the first's meshes)
    for (const std::string& c : {chr, codeOf(chr)}) {
      const auto& a = atlasOf(c);
      auto it = a.find(mesh);
      if (it != a.end()) return &it->second;
    }
    return nullptr;
  }
  UObject* shownComp(UObject* c) { return standIn(c) ? standIn(c) : c; }  // A hidden model being previewed is on screen in another component
  std::string meshNameOf(UObject* c) { UObject* m = c ? meshOf(shownComp(c)) : nullptr; return m ? lowered(narrow(m->GetName())) : std::string(); }
  // The part's mesh components that are still there (a mesh the game let go mid-round is never asked anything)
  std::vector<UObject*> liveComps(const Target& t) {
    std::vector<UObject*> out;
    if (t.items.size() != t.mids.size() + t.comps.size()) return out;
    for (size_t j = 0; j < t.comps.size(); j++)
      if (t.comps[j] && Kept{t.comps[j], t.items[t.mids.size() + j]}.alive()) out.push_back(t.comps[j]);
    return out;
  }
  // The meshes and material slots wearing the part now, as drawnTick finds them
  struct Wear { UObject* comp = nullptr; std::string mesh; int slot = 0; };
  std::vector<Wear> wearing(const Target& t) {
    std::vector<Wear> out;
    for (UObject* c : liveComps(t)) {
      const std::string mesh = meshNameOf(c);
      if (mesh.empty()) continue;
      const int n = numMaterials(c);
      for (int i = 0; i < n && i < 64; i++)
        if (std::string n0, k0; std::find(t.mids.begin(), t.mids.end(), material(c, i)) != t.mids.end() &&
                                !(outlineOnly(material(c, i)) && !colorSource(material(c, i), n0, k0)))
          out.push_back({c, mesh, i});
    }
    return out;
  }
  int slotIndex(UObject* comp, const std::string& slot) {  // The mesh's material index for a slot name, -1 if it hasn't one
    static UFunction* f = fn(STR("/Script/Engine.PrimitiveComponent:GetMaterialIndex"));
    if (!f || !comp) return -2;  // (can't tell)
    const std::wstring w(slot.begin(), slot.end());
    const FName name(w.c_str(), FNAME_Add);
    alignas(8) uint8_t p[16] = {};  // MaterialSlotName at 0, ReturnValue at 8
    std::memcpy(p, &name, sizeof(FName));
    comp->ProcessEvent(f, p);
    return *reinterpret_cast<int32_t*>(p + 8);
  }
  // A part mapped by hand with Remap goes by that mapping from then on, not the atlas (a mod's own model can keep the
  // stock names): a marker beside its parts file says so
  std::map<std::string, bool> g_by_hand;  // by map key, looked at once a session
  std::filesystem::path byHandFile(const std::string& key, const std::string& chr, const std::string& kind) {
    std::filesystem::path f = partsFile(key, chr, kind);
    f.replace_extension(".remapped");
    return f;
  }
  bool byHand(const Target& t) {
    const std::string key = mapKey(t);
    auto it = g_by_hand.find(key);
    if (it == g_by_hand.end()) {
      std::error_code ec;
      it = g_by_hand.emplace(key, std::filesystem::exists(byHandFile(key, t.chr, t.kind), ec)).first;
    }
    return it->second;
  }
  // Whether the atlas has every mesh and slot wearing the part, at that slot. Why not, for the log
  bool coveredBy(const Target& t, const std::vector<Wear>& wear, std::string* why) {
    if (byHand(t)) { if (why) *why = "it was mapped with Remap"; return false; }
    if (wear.empty()) { if (why) *why = "nothing wears it now"; return false; }
    for (const Wear& w : wear) {
      const AtlasMesh* am = atlasMesh(t.chr, w.mesh);
      if (!am) { if (why) *why = w.mesh + " isn't in it"; return false; }
      if (w.slot >= static_cast<int>(am->slots.size())) { if (why) *why = w.mesh + " has no slot " + std::to_string(w.slot) + " in it"; return false; }
      const int at = slotIndex(w.comp, am->slot_names[static_cast<size_t>(w.slot)]);
      if (at != -2 && at != w.slot) {
        if (why) *why = w.mesh + "'s slot " + std::to_string(w.slot) + " isn't " + am->slot_names[static_cast<size_t>(w.slot)] + " in the game";
        return false;
      }
    }
    return true;
  }
  struct PlacesFor {
    std::weak_ptr<CharMap> map;  // (watched, not held: a map made again is never taken for this one, and an old one is freed)
    std::string wear;
    Clock::time_point checked;
    int grid = 0;
    std::vector<std::vector<std::pair<uint16_t, int>>> cells;  // per atlas cell, the regions in it and their texels
    std::vector<uint16_t> centre;                              // per cell, the region at its middle texel (where the atlas has its point)
    std::vector<std::vector<Place>> by_region;
  };
  std::map<std::string, PlacesFor> g_places;  // by map key
// Each region's places, worked out when the part's regions or what wears it change. Null when the part isn't all
// in an atlas, and then it's mapped by scanning as before.
const std::vector<std::vector<Place>>* placesFor(const Target& t) {
  auto mp = g_maps.find(mapKey(t));
  if (mp == g_maps.end() || !mp->second || mp->second->regions <= 0) return nullptr;
  PlacesFor& pf = g_places[mapKey(t)];
  const bool same_map = pf.map.lock() == mp->second;
  if (same_map && Clock::now() - pf.checked < std::chrono::milliseconds(250))
    return pf.by_region.empty() ? nullptr : &pf.by_region;
  pf.checked = Clock::now();
  const auto wear = wearing(t);
  std::string sig;
  for (const Wear& w : wear) sig += w.mesh + "#" + std::to_string(w.slot) + ";";
  if (same_map && pf.wear == sig) return pf.by_region.empty() ? nullptr : &pf.by_region;
  const auto t0 = Clock::now();
  if (!same_map) { pf.cells.clear(); pf.centre.clear(); pf.grid = 0; }
  pf.map = mp->second;
  pf.wear = sig;
  pf.by_region.clear();
  std::string why;
  if (!coveredBy(t, wear, &why)) {
    COMBO_LOG("color editor: " + mapKey(t) + " isn't placed from a bone atlas (" + why + "), so it's mapped by scanning");
    return nullptr;
  }
  const CharMap& m = *mp->second;
  const int G = atlasMesh(t.chr, wear.front().mesh)->grid;
  if (pf.grid != G) {  // The regions in each cell, once per map
    pf.grid = G;
    pf.cells.assign(static_cast<size_t>(G) * G, {});
    pf.centre.assign(static_cast<size_t>(G) * G, 0xFFFF);
    for (int cy = 0; cy < G; cy++)
      for (int cx = 0; cx < G; cx++) {
        const int x0 = cx * m.w / G, x1 = (cx + 1) * m.w / G, y0 = cy * m.h / G, y1 = (cy + 1) * m.h / G;
        auto& in = pf.cells[static_cast<size_t>(cy) * G + cx];
        for (int y = y0; y < y1; y++)
          for (int x = x0; x < x1; x++) {
            const uint16_t r = m.owner[static_cast<size_t>(y) * m.w + x];
            if (r >= m.regions) continue;
            auto c = std::find_if(in.begin(), in.end(), [&](const auto& e) { return e.first == r; });
            if (c == in.end()) in.push_back({r, 1});
            else c->second++;
          }
        const int mx = (std::min)(m.w - 1, (x0 + x1) / 2), my = (std::min)(m.h - 1, (y0 + y1) / 2);
        pf.centre[static_cast<size_t>(cy) * G + cx] = m.owner[static_cast<size_t>(my) * m.w + mx];
      }
  }
  // Each region keeps up to 128 places, spread over all it has: counted first, then every k-th kept
  std::vector<int> total(static_cast<size_t>(m.regions), 0), seen(static_cast<size_t>(m.regions), 0);
  for (int pass = 0; pass < 2; pass++)
    for (const Wear& w : wear) {
      const AtlasMesh* am = atlasMesh(t.chr, w.mesh);
      if (am->grid != G) continue;
      for (const AtlasPoint& q : am->slots[static_cast<size_t>(w.slot)]) {
        const size_t cell = static_cast<size_t>(q.cy) * G + q.cx;
        for (const auto& [r, n] : pf.cells[cell]) {
          if (pass == 0) { total[r]++; continue; }
          const int every = (total[r] + 127) / 128;
          if (seen[r]++ % every) continue;
          if (pf.by_region.empty()) pf.by_region.resize(static_cast<size_t>(m.regions));
          Place pl;
          pl.mesh = w.mesh;
          pl.bone = am->bones[q.bone];
          pl.am = am;
          pl.bi = q.bone;
          for (int k = 0; k < 3; k++) { pl.p[k] = q.p[k] * 0.1f; pl.n[k] = q.n[k] / 127.0f; }
          pl.own = pf.centre[cell] == r;
          pf.by_region[r].push_back(std::move(pl));
        }
      }
    }
  int placed = 0;
  for (const auto& l : pf.by_region) placed += l.empty() ? 0 : 1;
  COMBO_LOG("color editor: " + mapKey(t) + " placed from the bone atlas: " + std::to_string(placed) + " of " + std::to_string(m.regions) + " regions (" +
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()) + " ms)");
  if (!placed) pf.by_region.clear();
  return pf.by_region.empty() ? nullptr : &pf.by_region;
}
bool atlasCovers(const Target& t) { return coveredBy(t, wearing(t), nullptr); }
  // A bone's place in the world now (its rotation x y z w, location and scale), or false if the mesh hasn't that bone
  bool boneWorld(UObject* comp, const std::string& bone, float q[4], float loc[3], float scale[3]) {
    static UFunction* gi = fn(STR("/Script/Engine.SkinnedMeshComponent:GetBoneIndex"));
    static UFunction* gst = fn(STR("/Script/Engine.SceneComponent:GetSocketTransform"));
    if (!comp || !gi || !gst) return false;
    const std::wstring w(bone.begin(), bone.end());
    const FName name(w.c_str(), FNAME_Add);
    alignas(8) uint8_t pi[16] = {};  // BoneName at 0, ReturnValue at 8
    std::memcpy(pi, &name, sizeof(FName));
    comp->ProcessEvent(gi, pi);
    if (*reinterpret_cast<int32_t*>(pi + 8) < 0) return false;
    alignas(16) uint8_t p[96] = {};  // InSocketName at 0, TransformSpace at 8 (RTS_World), ReturnValue (FTransform) at 16
    std::memcpy(p, &name, sizeof(FName));
    comp->ProcessEvent(gst, p);
    const float* x = reinterpret_cast<const float*>(p + 16);  // rotation x y z w, location x y z _, scale x y z _
    for (int k = 0; k < 4; k++) q[k] = x[k];
    for (int k = 0; k < 3; k++) { loc[k] = x[4 + k]; scale[k] = x[8 + k]; }
    return std::isfinite(loc[0]) && std::isfinite(loc[1]) && std::isfinite(loc[2]);
  }
  void rotateBy(const float q[4], const float v[3], float out[3]) {  // v + 2 u x (u x v + w v)
    const float u[3] = {q[0], q[1], q[2]}, w = q[3];
    const float c1[3] = {u[1] * v[2] - u[2] * v[1] + w * v[0], u[2] * v[0] - u[0] * v[2] + w * v[1], u[0] * v[1] - u[1] * v[0] + w * v[2]};
    out[0] = v[0] + 2.0f * (u[1] * c1[2] - u[2] * c1[1]);
    out[1] = v[1] + 2.0f * (u[2] * c1[0] - u[0] * c1[2]);
    out[2] = v[2] + 2.0f * (u[0] * c1[1] - u[1] * c1[0]);
  }
  // The places' components: those drawn now, by mesh name (a hidden mesh keeps its bones wherever they were left),
  // or all of them if none is
  struct Comps { std::map<std::string, UObject*> by_mesh; std::vector<UObject*> drawn; };
  Comps compsByMesh(const Target& t) {
    Comps all, drawn;
    for (UObject* c : liveComps(t))
      if (const std::string mesh = meshNameOf(c); !mesh.empty()) {
        if (all.by_mesh.emplace(mesh, shownComp(c)).second) all.drawn.push_back(shownComp(c));
        if (drawnLately(shownComp(c)) && drawn.by_mesh.emplace(mesh, shownComp(c)).second) drawn.drawn.push_back(shownComp(c));
      }
    return drawn.drawn.empty() ? all : drawn;
  }
  std::set<std::string> drawnMeshes(const Target& t) {  // (really drawn: none if none is)
    std::set<std::string> out;
    for (UObject* c : liveComps(t))
      if (const std::string mesh = meshNameOf(c); !mesh.empty() && drawnLately(shownComp(c))) out.insert(mesh);
    return out;
  }
  // A place in the world now, with the way it faces. A place on a mesh not drawn now (the close-up head) goes by a
  // drawn one with the same bone (the head on screen)
  bool placeAt(const Place& pl, const Comps& comps, float pos[3], float nrm[3]) {
    float q[4], loc[3], sc[3];
    auto real = [&](UObject* comp) {  // the bone there, at a real size (a bone scaled to nothing isn't shown)
      return boneWorld(comp, pl.bone, q, loc, sc) && std::fabs(sc[0]) >= 0.01f && std::fabs(sc[1]) >= 0.01f && std::fabs(sc[2]) >= 0.01f;
    };
    auto c = comps.by_mesh.find(pl.mesh);
    bool have = c != comps.by_mesh.end() && real(c->second);
    for (size_t i = 0; !have && c == comps.by_mesh.end() && i < comps.drawn.size(); i++) have = real(comps.drawn[i]);
    float lp[3] = {pl.p[0], pl.p[1], pl.p[2]}, ln[3] = {pl.n[0], pl.n[1], pl.n[2]};
    if (!have && c == comps.by_mesh.end() && pl.am && pl.bi >= 0 && pl.bi < static_cast<int>(pl.am->rest.size())) {
      // A bone only a hidden mesh has (the close-up head's): the nearest bone above it that a drawn mesh has, with the
      // point moved into that bone's space by their rest places
      const auto& r = pl.am->rest[static_cast<size_t>(pl.bi)];
      float mp[3], mn[3];
      rotateBy(&r[3], pl.p, mp);
      for (int k = 0; k < 3; k++) mp[k] += r[k];
      rotateBy(&r[3], pl.n, mn);
      // (not the root: a point placed by it alone doesn't move with the head)
      for (int a = pl.am->parent[static_cast<size_t>(pl.bi)], hops = 0; !have && a >= 0 && pl.am->parent[static_cast<size_t>(a)] >= 0 && hops < 64;
           a = pl.am->parent[static_cast<size_t>(a)], hops++) {
        const std::string& up = pl.am->bones[static_cast<size_t>(a)];
        auto real_up = [&](UObject* comp) {
          return boneWorld(comp, up, q, loc, sc) && std::fabs(sc[0]) >= 0.01f && std::fabs(sc[1]) >= 0.01f && std::fabs(sc[2]) >= 0.01f;
        };
        for (size_t i = 0; !have && i < comps.drawn.size(); i++) have = real_up(comps.drawn[i]);
        if (!have) continue;
        const auto& ra = pl.am->rest[static_cast<size_t>(a)];
        const float inv[4] = {-ra[3], -ra[4], -ra[5], ra[6]};
        const float d[3] = {mp[0] - ra[0], mp[1] - ra[1], mp[2] - ra[2]};
        rotateBy(inv, d, lp);
        rotateBy(inv, mn, ln);
      }
    }
    if (!have) return false;
    const float sp[3] = {lp[0] * sc[0], lp[1] * sc[1], lp[2] * sc[2]};
    const float sn[3] = {sc[0] < 0 ? -ln[0] : ln[0], sc[1] < 0 ? -ln[1] : ln[1], sc[2] < 0 ? -ln[2] : ln[2]};  // (a mirrored fighter)
    rotateBy(q, sp, pos);
    for (int k = 0; k < 3; k++) pos[k] += loc[k];
    rotateBy(q, sn, nrm);
    return true;
  }
  // The selection's places to look at: its own ones first, at most cap, spread over all of them
  // (only those on the meshes in drawn, when given)
  std::vector<const Place*> placesOfSel(const std::vector<std::vector<Place>>& pl, const std::vector<int>& sel, size_t cap, const Comps* drawn = nullptr) {
    std::vector<const Place*> own, rest, out;
    for (int r : sel)
      if (r >= 0 && r < static_cast<int>(pl.size()))
        for (const Place& p : pl[static_cast<size_t>(r)])
          if (!drawn || drawn->by_mesh.count(p.mesh)) (p.own ? own : rest).push_back(&p);
    const auto& from = own.empty() ? rest : own;
    const size_t step = (std::max<size_t>)(1, (from.size() + cap - 1) / cap);
    for (size_t i = 0; i < from.size() && out.size() < cap; i += step) out.push_back(from[i]);
    return out;
  }
}  // namespace
// Where the selection is on the fighter now, from the atlas (from the feet): the middle of its places on the bone
// most of them move with, and how much of the view it takes. False if it has no places.
bool anchorFromAtlas(const Target& t, const std::vector<std::vector<Place>>& pl, const std::vector<int>& sel, const Vec& feet, float out[3], float& frac, float& dist) {
  const auto comps = compsByMesh(t);
  std::map<std::pair<std::string, std::string>, std::vector<const Place*>> groups;  // by (mesh, bone), on meshes drawn now if any
  for (const Place* p : placesOfSel(pl, sel, 4096, &comps)) groups[{p->mesh, p->bone}].push_back(p);
  if (groups.empty())
    for (const Place* p : placesOfSel(pl, sel, 4096)) groups[{p->mesh, p->bone}].push_back(p);
  std::string drawn_names;
  for (const auto& [m, c] : comps.by_mesh) drawn_names += " " + m;
  if (groups.empty()) {
    COMBO_LOG("color editor: atlas: the selection has no places (meshes drawn now:" + drawn_names + ")");
    return false;
  }
  const auto most = std::max_element(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return a.second.size() < b.second.size(); });
  Place mid = *most->second.front();
  float sum[3] = {};
  for (const Place* p : most->second) for (int k = 0; k < 3; k++) sum[k] += p->p[k];
  for (int k = 0; k < 3; k++) mid.p[k] = sum[k] / static_cast<float>(most->second.size());
  float reach = 5.0f;  // its size around the middle, in cm (not less than 5)
  for (const Place* p : most->second) {
    const float dx = p->p[0] - mid.p[0], dy = p->p[1] - mid.p[1], dz = p->p[2] - mid.p[2];
    reach = (std::max)(reach, std::sqrt(dx * dx + dy * dy + dz * dz));
  }
  float pos[3], nrm[3];
  if (!placeAt(mid, comps, pos, nrm)) {
    std::string have;
    for (UObject* c : comps.drawn) {
      float q[4], loc[3], sc[3];
      const std::string mesh = c && meshOf(c) ? lowered(narrow(meshOf(c)->GetName())) : std::string("?");
      have += " " + mesh + (boneWorld(c, mid.bone, q, loc, sc) ? ": scale " + std::to_string(sc[0]) + " " + std::to_string(sc[1]) + " " + std::to_string(sc[2])
                                                                : std::string(": no such bone"));
    }
    COMBO_LOG("color editor: atlas: " + std::to_string(groups.size()) + " groups, the biggest on " + mid.mesh + " bone " + mid.bone + " (" +
              std::to_string(most->second.size()) + " places) couldn't be placed. Meshes drawn now:" + drawn_names + ";" + have);
    return false;
  }
  out[0] = pos[0] - feet.x;
  out[1] = pos[1] - feet.y;
  out[2] = pos[2] - feet.z;
  dist = (std::max)(g_cam.dist, 40.0f);
  frac = std::clamp(2.0f * reach / (2.0f * dist * std::tan(g_cam_fov * 0.5f / 57.2958f)), 0.02f, 1.0f);
  return true;
}
// The selection's place for its label (from the feet): the one facing the camera most, on screen. 1 found, 0 none
// of them faces it (it's round the other side), -1 none of them could be found in the game now.
int spotFromAtlas(const Target& t, const std::vector<int>& sel, const Vec& feet, float out[3]) {
  const auto* pl = placesFor(t);
  if (!pl) return -1;
  const auto comps = compsByMesh(t);
  float best = 0.2f;  // (facing it at least a little)
  int found = -1;
  std::vector<const Place*> look = placesOfSel(*pl, sel, 96, &comps);  // (on meshes drawn now if it has any there)
  if (look.empty()) look = placesOfSel(*pl, sel, 96);
  for (const Place* p : look) {
    float pos[3], nrm[3];
    if (!placeAt(*p, comps, pos, nrm)) continue;
    if (found < 0) found = 0;
    const float to[3] = {g_cam_pose[0] - pos[0], g_cam_pose[1] - pos[1], g_cam_pose[2] - pos[2]};
    const float tl = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]), nl = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
    if (tl < 1.0f || nl < 0.5f) continue;
    const float facing_cam = (to[0] * nrm[0] + to[1] * nrm[1] + to[2] * nrm[2]) / (tl * nl);
    float sx, sy, d;
    if (facing_cam <= best || !project(Vec{pos[0], pos[1], pos[2]}, g_aspect, sx, sy, d) || sx < 0.02f || sx > 0.98f || sy < 0.02f || sy > 0.98f) continue;
    best = facing_cam;
    out[0] = pos[0] - feet.x;
    out[1] = pos[1] - feet.y;
    out[2] = pos[2] - feet.z;
    found = 1;
  }
  return found;
}
// Where the selection is on the fighter now, from the mapping (from the feet). False if any of it isn't
// mapped.
bool anchorOf(const Target& t, const std::vector<int>& sel, const Vec& feet, float out[3], float& frac, float& dist) {
  if (const auto* pl = placesFor(t); pl && anchorFromAtlas(t, *pl, sel, feet, out, frac, dist)) return true;
  std::vector<Part>* parts = partsFor(t);
  if (!parts || sel.empty()) return false;
  const auto& bones = bonesOf(t);
  float sum[3] = {};
  frac = 0;
  dist = 0;
  for (int r : sel) {
    if (r < 0 || r >= static_cast<int>(parts->size()) || (*parts)[static_cast<size_t>(r)].bone.empty()) return false;
    const Part& p = (*parts)[static_cast<size_t>(r)];
    const auto b = std::find_if(bones.begin(), bones.end(), [&](const Bone& x) { return x.text == p.bone; });
    if (b == bones.end()) return false;
    const Vec q = boneAt(*b);
    sum[0] += q.x + p.off[0] - feet.x;
    sum[1] += q.y + p.off[1] - feet.y;
    sum[2] += q.z + p.off[2] - feet.z;
    frac = (std::max)(frac, p.frac);
    dist = p.dist;
  }
  for (int i = 0; i < 3; i++) out[i] = sum[i] / static_cast<float>(sel.size());
  return true;
}
// One side of the mapping. Each region's pixels give a line of sight (through their middle) and a size,
// and each pixel votes for the bone under it (the front-most one close by).
void mapView(Mapping& m, const Target& t, const std::vector<ScanSample>& s, int w, int h, const Vec& feet) {
  if (w <= 0 || h <= 0) return;
  const auto& bones = bonesOf(t);
  const float aspect = static_cast<float>(w) / static_cast<float>(h);
  std::vector<std::array<float, 3>> at;  // bones on screen: x, y (pixels), depth
  std::vector<const std::string*> name;
  for (size_t i = 0; i < bones.size(); i++) {
    const Bone& b = bones[i];
    if (b.root) continue;
    float sx, sy, d;
    if (!project(boneAt(b), aspect, sx, sy, d)) continue;
    at.push_back({sx * w, sy * h, d});
    name.push_back(&b.text);
  }
  const int R = static_cast<int>(m.rays.size());
  std::vector<std::vector<std::pair<int, int>>> pts(static_cast<size_t>(R));
  for (const ScanSample& p : s)
    if (p.id >= 1 && p.id <= R) pts[p.id - 1].push_back({p.x, p.y});
  const float near2 = (0.04f * h) * (0.04f * h), far2 = 4.0f * near2;
  int seen = 0;
  for (int r = 0; r < R; r++) {
    const auto& q = pts[static_cast<size_t>(r)];
    if (q.size() < 4) continue;
    seen++;
    std::vector<int> xs, ys;
    for (const auto& [x, y] : q) { xs.push_back(x); ys.push_back(y); }
    std::sort(xs.begin(), xs.end());
    std::sort(ys.begin(), ys.end());
    const size_t n = xs.size(), a = n * 5 / 100, b = n - 1 - n * 5 / 100;
    m.frac[r] = (std::max)(m.frac[r], (std::max)(static_cast<float>(xs[b] - xs[a]) / w, static_cast<float>(ys[b] - ys[a]) / h));
    auto ray = rayAt(xs[n / 2] + 1.0f, ys[n / 2] + 1.0f, w, h);
    ray[0] -= feet.x; ray[1] -= feet.y; ray[2] -= feet.z;  // Relative to the feet, since the fighter may move between sides
    m.rays[r].push_back(ray);
    for (const auto& [x, y] : q) {
      int pick = -1, nearest = -1;
      float front = 1e30f, best2 = 1e30f;
      for (size_t i = 0; i < at.size(); i++) {
        const float dx = at[i][0] - x, dy = at[i][1] - y, d2 = dx * dx + dy * dy;
        if (d2 < near2 && at[i][2] < front) { front = at[i][2]; pick = static_cast<int>(i); }
        if (d2 < best2) { best2 = d2; nearest = static_cast<int>(i); }
      }
      if (pick < 0 && best2 < far2) pick = nearest;
      if (pick < 0) continue;
      m.bone_votes[r][*name[static_cast<size_t>(pick)]]++;
    }
  }
  COMBO_LOG("color editor: mapping " + m.key + " side " + std::to_string(m.view + 1) + ": " + std::to_string(seen) + " regions seen, " +
            std::to_string(at.size()) + " bones on screen");
}
// The mapping's results. Each region's place is the bone most of its pixels sit on, so it follows the
// fighter. With no pixel near a bone, it's the point nearest its lines of sight, as a bone and an offset.
void finishMapping(Target& t, const Vec& feet) {
  Mapping& m = *g_mapping;
  const int R = static_cast<int>(m.rays.size());
  std::vector<Part> parts(static_cast<size_t>(R));
  const auto& bones = bonesOf(t);
  std::vector<Vec> where(bones.size());
  for (size_t i = 0; i < bones.size(); i++)
    if (!bones[i].root) where[i] = boneAt(bones[i]);
  int placed = 0;
  for (int r = 0; r < R; r++) {
    Part& p = parts[static_cast<size_t>(r)];
    if (!m.bone_votes[r].empty()) {  // The bone most of its pixels sit on. That's on the part itself, even when it's in pieces
      int most = 0;
      for (const auto& [b, v] : m.bone_votes[r])
        if (v > most) { most = v; p.bone = b; }
      p.frac = m.frac[r];
      p.dist = m.dist;
      placed++;
      continue;
    }
    float at[3];  // No pixel near a bone, so use the point nearest its lines of sight
    const Vec from{};  // the rays were kept from the feet
    bool have = m.rays[r].size() >= 2 && nearestToRays(m.rays[r], from, at);
    for (size_t k = 0; !have && k < m.rays[r].size(); k++) have = onFighter(m.rays[r][k], from, at);
    if (!have) continue;
    const Vec q{feet.x + at[0], feet.y + at[1], feet.z + at[2]};
    int closest = -1;
    float best2 = 1e30f;
    for (size_t i = 0; i < bones.size(); i++) {
      if (bones[i].root) continue;
      const float dx = where[i].x - q.x, dy = where[i].y - q.y, dz = where[i].z - q.z, d2 = dx * dx + dy * dy + dz * dz;
      if (d2 < best2) { best2 = d2; closest = static_cast<int>(i); }
    }
    if (closest < 0) continue;
    p.bone = bones[static_cast<size_t>(closest)].text;  // No offset, as world axes would be wrong once the fighter turns. The look on arrival finds the pixel
    p.frac = m.frac[r];
    p.dist = m.dist;
    placed++;
  }
  if (placed > 0) {  // Otherwise nothing was seen (every side timed out). What was there stays, and it maps again next time
    g_parts[m.key] = std::move(parts);
    saveParts(m.key, t.chr, t.kind);
    if (m.remap) {  // Mapped by hand: this part goes by it from now on, not the atlas
      std::ofstream(byHandFile(m.key, t.chr, t.kind), std::ios::trunc) << "mapped with Remap\n";
      g_by_hand[m.key] = true;
      g_places.erase(m.key);
    }
  }
  COMBO_LOG("color editor: mapped " + m.key + ": " + std::to_string(placed) + " of " + std::to_string(R) + " regions placed" +
            (placed ? "" : " (not kept)"));
  const float yaw0 = m.yaw0;
  scanOff(&t, false);
  g_mapping.reset();
  portraitGoal();  // back to the front
  g_cam.gyaw = yaw0;
  g_cam_info = kCamHelp;
  if (!findable(t, selRows())) g_blinks_left = 0;  // selected while mapping, and it turned out unseen
  else if (g_selected.size() < g_worn->lit.size()) {  // Not for everything
    if (g_follow_sel) g_focus = Focus::Aim;
    else { g_look = 3; g_focus = Focus::Move; }  // Just find its pixel for the label
  }
  g_view_dirty = true;
}
void startMapping(Target& t) {
  const int R = (std::min)(g_maps[mapKey(t)]->regions, static_cast<int>(g_worn->lit.size()));
  g_map_tried.insert(mapKey(t));
  const bool remap = g_map_force == mapKey(t);
  g_map_force.clear();
  if (R <= 0) return;
  stopFocus(&t);
  std::vector<int> group(static_cast<size_t>(R));
  for (int r = 0; r < R; r++) group[static_cast<size_t>(r)] = r + 1;
  if (!scanBuild(t, group, R, 2048)) { COMBO_LOG("color editor: couldn't make the mapping's frames"); return; }  // Full size, so thin lines get seen
  g_mapping = std::make_unique<Mapping>();
  Mapping& m = *g_mapping;
  m.remap = remap;
  if (!remap && !g_setup_off && g_setup_home.empty()) g_setup_home = keyOf(t);  // The first-time setup starts here (no gap before the next part)
  m.key = mapKey(t);
  m.yaw0 = g_cam.gyaw;
  m.rays.resize(static_cast<size_t>(R));
  m.bone_votes.resize(static_cast<size_t>(R));
  m.frac.assign(static_cast<size_t>(R), 0.0f);
  portraitGoal();
  m.since = Clock::now();
  g_cam_info = "Mapping, camera back shortly.";
  g_view_dirty = true;
  COMBO_LOG("color editor: mapping " + m.key + ", " + std::to_string(R) + " regions in " + std::to_string(g_scan_tex.size()) + " frames");
}
void afterBattleUpdate() {
  Timed timed{g_time.after};
  if (g_preview.pawn && asw_engine::get() == g_fight) keepPreview();
  if (!g_targets.empty() && asw_engine::get() == g_fight) { dressSmears(); if (!g_scan_on) effCopiesSync(); }  // A scan's coded frames on the part aren't copied onto effects  // For an effect started in the game's update, before it's drawn
  g_cam_keys = g_cam.on;
  if (!g_cam.on) {
    g_view_on = false;
    if (g_cam_pc) cameraOff();
    else { stopFocus(target()); stopMapping(target()); }  // nothing of ours outlives the camera
    if (!g_pick_frames.empty() && !g_scan_on) {  // A pick's kept frames are let go with the camera
      for (Shown& s : g_scan_tex) unroot(s);
      g_scan_tex.clear();
      g_pick_frames.clear();
    }
    g_tag_x = -1.0f;
    return;
  }
  Target* t = target();
  if (!t || !t->pawn || t->mids.empty() || asw_engine::get() != g_fight) {
    g_view_on = false;
    if (g_cam_pc && asw_engine::get() == g_fight) cameraOff();  // the view handed back while it's ours to hand
    g_cam_pc = g_cam_actor = g_cam_orig = nullptr;
    g_cam.ready = false;
    stopFocus(t);
    stopMapping(t);
    g_tag_x = -1.0f;
    return;
  }
  if (!g_cam_pc) {
    g_cam_pc = levelActor(STR("REDPlayerController_Battle"), STR("REDPlayerController_Battle_"));
    g_cam_actor = g_view_hooked ? nullptr : levelActor(STR("CameraActor"), STR(".CameraActor_"));
    if (!g_cam_pc || (!g_view_hooked && !g_cam_actor) || !cameraManager()) {
      g_cam_info = "Couldn't find the battle's camera.";
      g_cam.on = false;
      g_cam_pc = g_cam_actor = nullptr;
      stopFocus(t);
      stopMapping(t);
      g_map_tried.insert(mapKey(*t));  // No mapping without it, so don't turn the camera back on for one
      g_map_force.clear();
      g_view_dirty = true;
      return;
    }
    g_cam_orig = g_view_hooked ? nullptr : viewTarget();
    g_cam.ready = false;
  }
  UObject* mgr = cameraManager();
  if (!mgr) return;
  if (g_view_on) g_view_ms = nowMs();  // Refreshed after a long pause, so projections below see our view, not the game's
  readViewport();
  const Vec feet = actorVec(t->pawn, STR("/Script/Engine.Actor:K2_GetActorLocation"));
  {  // How tall they are. The body's highest bone plus a bit for the hair, whichever part is shown
    const Target* body = t;
    for (const Target& o : g_targets) if (o.pawn == t->pawn && o.kind == "MAIN") body = &o;
    if (g_fighter_h_for != keyOf(*body)) {
      float top = 0.0f;
      for (const Bone& b : bonesOf(*body)) if (!b.root) top = (std::max)(top, boneAt(b).z - feet.z);
      g_fighter_h = top > 50.0f ? std::clamp(top * 1.1f, 120.0f, 450.0f) : 200.0f;
      g_fighter_h_for = keyOf(*body);
      COMBO_LOG("color editor: " + keyOf(*body) + " is " + std::to_string(static_cast<int>(g_fighter_h)) + " tall");
    }
  }
  if (!g_cam.ready) {  // start exactly where the game's camera is and looks, then ease to the whole fighter
    const float* pov = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(mgr) + 0x1B00);  // location, pitch yaw roll, fov
    const float pr = pov[3] / 57.2958f, yr = pov[4] / 57.2958f;
    const float fwd[3] = {std::cos(pr) * std::cos(yr), std::cos(pr) * std::sin(yr), std::sin(pr)};
    const float depth = (std::max)((feet.x - pov[0]) * fwd[0] + (feet.y - pov[1]) * fwd[1] + (feet.z + 100.0f - pov[2]) * fwd[2], 100.0f);
    g_cam.pitch = std::clamp(-std::remainder(pov[3], 360.0f), -85.0f, 85.0f);
    g_cam.yaw = pov[4] + 180.0f;
    g_cam.dist = depth;
    g_cam.pivot[0] = pov[0] + fwd[0] * depth - feet.x;
    g_cam.pivot[1] = pov[1] + fwd[1] * depth - feet.y;
    g_cam.pivot[2] = pov[2] + fwd[2] * depth - feet.z;
    g_cam_fov = pov[6] > 5.0f && pov[6] < 140.0f ? pov[6] : 65.0f;
    if (!g_view_hooked)
      if (auto* comp = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(g_cam_actor) + 0x230)) {
        *reinterpret_cast<float*>(comp + 0x208) = g_cam_fov;
        comp[0x21C] &= ~1;  // no black bars
      }
    portraitGoal();
    g_keep_dist = 0.0f;
    g_cam.ready = true;
    COMBO_LOG("free camera: from " + std::to_string(pov[0]) + " " + std::to_string(pov[1]) + " " + std::to_string(pov[2]) + ", fov " +
              std::to_string(g_cam_fov) + ", fighter at " + std::to_string(feet.x) + " " + std::to_string(feet.y) + " " + std::to_string(feet.z) +
              ", whole fighter at distance " + std::to_string(g_cam.gdist));
  }
  // ---- The user's input moves the pose and its goal together, so it always wins ----
  const float mdx = g_cam_dx.exchange(0.0f), mdy = g_cam_dy.exchange(0.0f), wheel = g_cam_wheel.exchange(0.0f);
  const float pdx = g_pan_dx.exchange(0.0f), pdy = g_pan_dy.exchange(0.0f);
  const bool still = g_scan_on;                    // held still while a scan's frames are grabbed
  const bool locked = still || g_mapping != nullptr || setupRunning();  // the mapping steers the camera itself (and the setup's between parts)
  if (!locked) {
    const float fast = keyDown(VK_SHIFT) ? 3.0f : 1.0f, step = 3.0f * fast, turn = 1.2f * fast;
    float dyaw = mdx * 0.3f, dpitch = mdy * 0.3f;
    if (keyDown(VK_LEFT)) dyaw -= turn;
    if (keyDown(VK_RIGHT)) dyaw += turn;
    if (keyDown(VK_UP)) dpitch += turn;
    if (keyDown(VK_DOWN)) dpitch -= turn;
    g_cam.yaw += dyaw;
    g_cam.gyaw += dyaw;
    g_cam.pitch = std::clamp(g_cam.pitch + dpitch, -85.0f, 85.0f);
    g_cam.gpitch = std::clamp(g_cam.gpitch + dpitch, -85.0f, 85.0f);
    if (wheel != 0.0f) {
      const float z = std::pow(0.88f, wheel);
      g_cam.dist = std::clamp(g_cam.dist * z, 40.0f, 3000.0f);
      g_cam.gdist = std::clamp(g_cam.gdist * z, 40.0f, 3000.0f);
      if (g_keep_dist > 0.0f) g_keep_dist = std::clamp(g_keep_dist * z, 40.0f, 3000.0f);  // Zoomed while finding a part, so that's the distance now
    }
    const float yr = g_cam.yaw / 57.2958f;
    const float fx = -std::cos(yr), fy = -std::sin(yr), rx = -fy, ry = fx;  // forward on the ground, and right of it
    float mv[3] = {0.0f, 0.0f, 0.0f};
    if (keyDown('W')) { mv[0] += fx * step; mv[1] += fy * step; }
    if (keyDown('S')) { mv[0] -= fx * step; mv[1] -= fy * step; }
    if (keyDown('D')) { mv[0] += rx * step; mv[1] += ry * step; }
    if (keyDown('A')) { mv[0] -= rx * step; mv[1] -= ry * step; }
    if (keyDown('Q')) mv[2] += step;
    if (keyDown('E')) mv[2] -= step;
    const float k = g_cam.dist * 0.0015f;  // right drag slides the pivot across the view, faster when far
    mv[0] -= rx * pdx * k; mv[1] -= ry * pdx * k; mv[2] += pdy * k;
    for (int i = 0; i < 3; i++) { g_cam.pivot[i] += mv[i]; g_cam.gpivot[i] += mv[i]; }
    if (keyDown('R')) { stopFocus(t); portraitGoal(); g_keep_dist = 0.0f; }  // flights take their distance from here again
  }
  if (!still) {
    const float e = 0.14f;  // easing toward the goal
    g_cam.yaw += angleTo(g_cam.yaw, g_cam.gyaw) * e;
    g_cam.pitch += (g_cam.gpitch - g_cam.pitch) * e;
    g_cam.dist += (g_cam.gdist - g_cam.dist) * e;
    for (int i = 0; i < 3; i++) g_cam.pivot[i] += (g_cam.gpivot[i] - g_cam.pivot[i]) * e;
  }
  // Whether the camera has reached its goal. Ask it after any goal set below, since a goal set this frame
  // isn't reached yet.
  const auto settled = [] {
    return std::fabs(angleTo(g_cam.yaw, g_cam.gyaw)) < 0.3f && std::fabs(g_cam.gpitch - g_cam.pitch) < 0.3f &&
           std::fabs(g_cam.gdist - g_cam.dist) < 0.01f * g_cam.gdist + 0.5f &&
           std::fabs(g_cam.gpivot[0] - g_cam.pivot[0]) + std::fabs(g_cam.gpivot[1] - g_cam.pivot[1]) + std::fabs(g_cam.gpivot[2] - g_cam.pivot[2]) < 1.5f;
  };
  const bool ready = g_worn && g_worn_key == keyOf(*t);
  // ---- The parts of the model, mapped once (three sides, a scan each) ----
  if (!g_mapping && !g_pick_wanted && !g_pick_scanning && g_open && ready &&
      (g_map_force == mapKey(*t) || (!g_map_tried.count(mapKey(*t)) && !partsFor(*t) && !placesFor(*t))))
    startMapping(*t);
  if (g_mapping) {
    Mapping& m = *g_mapping;
    if (!m.scanning) {
      g_cam.gyaw = m.yaw0 + 90.0f * m.view;
      if (settled() || Clock::now() - m.since > std::chrono::milliseconds(1500)) {
        m.dist = g_cam.dist;
        scanRun(*t, true);
        m.scanning = true;
        m.since = Clock::now();
      }
    } else {
      std::vector<ScanSample> s;
      int w = 0, h = 0;
      const ScanStep st = scanPoll(*t, s, w, h);
      if (st == ScanStep::Done && (w <= 0 || h <= 0)) {  // The screen couldn't be read, so stop rather than guess
        const std::string why = scanWhy(), key = m.key;
        COMBO_LOG("color editor: mapping stopped: " + why);
        stopMapping(t);
        g_map_tried.insert(key);  // Not retried by itself. Remap tries again
        g_status = "Mapping stopped: " + why + " Then press Remap.";
      } else if (st != ScanStep::Waiting || Clock::now() - m.since > std::chrono::seconds(6)) {
        if (st == ScanStep::Done) mapView(m, *t, s, w, h, feet);
        else COMBO_LOG("color editor: mapping side " + std::to_string(m.view + 1) + " timed out");
        scanOff(t, true);  // the fighter's own look while the camera turns
        m.scanning = false;
        m.since = Clock::now();
        if (++m.view >= kViews) finishMapping(*t, feet);
        else g_cam.gyaw = m.yaw0 + 90.0f * m.view;  // on to the next side
      }
    }
    g_view_dirty = true;
  }
  // ---- A pick lights every region in code for a few frames and reads them at the point clicked ----
  if (g_pick_wanted && ready && !g_mapping && !g_pick_scanning && !g_scan_on) {
    g_pick_wanted = false;
    stopFocus(t);
    g_cam.gyaw = g_cam.yaw;  // Held where the user clicked from
    g_cam.gpitch = g_cam.pitch;
    g_cam.gdist = g_cam.dist;
    std::memcpy(g_cam.gpivot, g_cam.pivot, sizeof(g_cam.pivot));
    const int R = static_cast<int>(g_worn->lit.size());
    const std::string frames_for = keyOf(*t) + "/" + std::to_string(R) + "/" + std::to_string(g_worn_serial);
    bool have = g_pick_frames == frames_for && !g_scan_tex.empty();
    if (!have && R > 0) {
      std::vector<int> group(static_cast<size_t>(R));
      for (int r = 0; r < R; r++) group[static_cast<size_t>(r)] = r + 1;
      have = scanBuild(*t, group, R, 2048);  // Full size, so thin lines stay thin and in place
    }
    if (have) {
      g_pick_frames = frames_for;
      g_pick_frames_until = Clock::now() + std::chrono::seconds(60);
      scanRun(*t);
      {
        std::lock_guard<std::mutex> lk(g_scan_m);
        g_scan_point_on = true;
        g_scan_point[0] = g_pick_xy[0];
        g_scan_point[1] = g_pick_xy[1];
      }
      g_pick_scanning = true;
      g_pick_since = Clock::now();
    } else {
      g_status = "Couldn't look there.";
    }
    g_view_dirty = true;
  }
  if (g_pick_scanning) {
    std::vector<ScanSample> s;
    int w = 0, h = 0;
    const ScanStep st = scanPoll(*t, s, w, h);
    if (st != ScanStep::Waiting || Clock::now() - g_pick_since > std::chrono::seconds(6)) {
      g_pick_scanning = false;
      scanOff(t, true);  // The frames are kept a minute for the next pick
      // every region with a pixel right around the click (the dense pixels sent back there), nearest first
      std::map<int, std::pair<float, int>> around;  // region -> (nearest distance, pixels)
      const float cx = g_pick_xy[0] * w, cy = g_pick_xy[1] * h, reach = 18.0f;  // In back buffer pixels
      if (st == ScanStep::Done && w > 0 && h > 0)
        for (const ScanSample& p : s) {
          const float dx = p.x - cx, dy = p.y - cy, d = std::sqrt(dx * dx + dy * dy);
          if (p.id < 1 || d > reach) continue;
          auto [it, fresh] = around.emplace(p.id - 1, std::make_pair(d, 0));
          it->second.first = (std::min)(it->second.first, d);
          it->second.second++;
        }
      std::vector<std::pair<float, int>> order;
      for (const auto& [r, dn] : around) order.push_back({dn.first, r});
      std::sort(order.begin(), order.end());
      if (order.size() > 8) order.resize(8);
      {  // What was read there, in full
        std::string l = "color editor: pick at " + std::to_string(static_cast<int>(cx)) + "," + std::to_string(static_cast<int>(cy)) + " of " +
                        std::to_string(w) + "x" + std::to_string(h) + (st == ScanStep::Done ? "" : " (timed out)") + ", " + std::to_string(s.size()) +
                        " pixels read; around it:";
        for (const auto& [d, r] : order) l += " " + std::to_string(r) + "(" + std::to_string(static_cast<int>(d)) + "px x" + std::to_string(around[r].second) + ")";
        if (order.empty()) l += " nothing";
        COMBO_LOG(l);
      }
      g_pick_choices.clear();
      for (const auto& [d, r] : order) g_pick_choices.push_back(r);
      const int best = g_pick_choices.empty() ? -1 : g_pick_choices.front();
      if (best >= 0) {
        g_picked = best;
        g_pick_serial++;
        g_pick_quiet = true;
        g_status = g_pick_choices.size() > 1 ? "Several parts there: choose one." : "Picked region " + std::to_string(best) + ".";
      } else if (st == ScanStep::Done && (w <= 0 || h <= 0) && !scanWhy().empty()) {
        g_status = "Pick couldn't read the screen: " + scanWhy();
      } else {
        g_status = "Not on this part. Switch parts first.";
      }
      g_view_dirty = true;
    }
  }
  if (!g_pick_frames.empty() && !g_pick_scanning && !g_scan_on && Clock::now() > g_pick_frames_until) {  // Not picked again, so let go
    for (Shown& s : g_scan_tex) unroot(s);
    g_scan_tex.clear();
    g_pick_frames.clear();
  }
  // ---- Flying to the selection. Aim if mapped, else settle, probe and move ----
  const bool sel_ready = ready && !g_selected.empty();
  if (g_focus == Focus::Aim && !g_mapping) {
    float p[3], frac = 0, at = 0;
    if (!sel_ready) {
      g_focus = Focus::Idle;
    } else if (anchorOf(*t, selRows(), feet, p, frac, at)) {
      goTo(p, frac, at);
      g_focus = Focus::Move;
      g_look = 1;  // Once there, find its pixels and put the label and the camera on them
      g_cam_info = kCamHelp;
      g_view_dirty = true;
    } else {  // Not mapped (or its places can't be found in the game now), so find it on screen
      if (placesFor(*t)) COMBO_LOG("color editor: the selection's atlas places couldn't be found in the game now, so it's looked for on screen");
      g_focus = Focus::Settle;
      g_focus_retry = false;
      g_focus_since = Clock::now();
    }
  }
  auto startProbe = [&](bool looks) {  // a scan of the selection alone
    if (g_pick_scanning) { g_focus = Focus::Idle; g_look = 0; return; }  // A pick has the scan
    std::vector<int> group(g_worn->lit.size(), 0);
    for (int r : selRows()) if (r >= 0 && r < static_cast<int>(group.size())) group[static_cast<size_t>(r)] = 1;
    g_probe_looks = looks;
    if (scanBuild(*t, group, 1)) {
      scanRun(*t);
      g_focus = Focus::Probe;
      g_focus_since = Clock::now();
    } else {
      g_focus = Focus::Idle;
      g_look = 0;
    }
  };
  if (g_focus == Focus::Settle && !g_mapping) {
    if (!sel_ready) {
      g_focus = Focus::Idle;
    } else if (settled() || Clock::now() - g_focus_since > std::chrono::milliseconds(1200)) {
      startProbe(false);
    }
  }
  if (g_focus == Focus::Probe) {
    std::vector<ScanSample> s;
    int w = 0, h = 0;
    const ScanStep st = scanPoll(*t, s, w, h);
    if (st == ScanStep::Done && (w <= 0 || h <= 0) && !scanWhy().empty()) {  // The screen couldn't be read. Say so, don't retry
      g_cam_info = "Couldn't read the screen: " + scanWhy();
      scanOff(t, false);
      g_probe_looks = false;
      g_look = 0;
      g_focus = Focus::Idle;
    } else if (st != ScanStep::Waiting || Clock::now() - g_focus_since > std::chrono::milliseconds(2000)) {
      scanOff(t, false);
      float spot[3];
      if (g_probe_looks) {  // Arrived. Put the label and the camera on its pixels, else look from the other side
        g_probe_looks = false;
        if (st == ScanStep::Done && spotOf(s, w, h, feet, spot)) {
          std::memcpy(g_tag_point, spot, sizeof(spot));
          g_tag_found = true;
          g_tag_yaw = facing(*t);
          if (g_look != 3) face(spot, feet);  // At 3 the camera stays where the user has it
          g_look = 0;
          g_focus = Focus::Move;
        } else if (g_look == 3) {
          g_look = 0;
          g_focus = Focus::Idle;
        } else if (g_look == 1 && sel_ready) {
          g_cam.gyaw += 180.0f;
          g_look = 2;
          g_focus = Focus::Move;
        } else {
          g_look = 0;
          g_focus = Focus::Idle;
          g_cam_info = "Not visible. Turn the camera.";
        }
      } else if (aimAt(st == ScanStep::Done ? hitOf(s, w, h) : FocusHit{}, feet)) {
        g_focus = Focus::Move;
        g_cam_info = kCamHelp;
      } else if (!g_focus_retry && sel_ready) {  // maybe it was out of view: look again at the whole fighter
        g_focus_retry = true;
        portraitGoal();
        g_focus = Focus::Settle;
        g_focus_since = Clock::now();
      } else {
        g_focus = Focus::Idle;
        g_cam_info = "Not found here. Turn the camera and retry.";
      }
      g_view_dirty = true;
    }
  }
  if (g_focus == Focus::Move && !g_mapping && settled()) {
    float spot[3];
    const int atlas_spot = g_look && sel_ready && placesFor(*t) ? spotFromAtlas(*t, selRows(), feet, spot) : -1;
    if (atlas_spot >= 0) {  // Arrived. The label on its place facing the camera, else look from the other side
      if (atlas_spot == 1) {
        std::memcpy(g_tag_point, spot, sizeof(spot));
        g_tag_found = true;
        g_tag_yaw = facing(*t);
        const bool stay = g_look == 3;  // At 3 the camera stays where the user has it
        if (!stay) face(spot, feet);
        g_look = 0;
        g_focus = stay ? Focus::Idle : Focus::Move;
      } else if (g_look == 1) {
        g_cam.gyaw += 180.0f;
        g_look = 2;
        g_focus = Focus::Move;
      } else {
        if (g_look == 2) g_cam_info = "Not visible. Turn the camera.";
        g_look = 0;
        g_focus = Focus::Idle;
      }
      g_view_dirty = true;
    } else if (g_look && sel_ready) {
      if (placesFor(*t)) COMBO_LOG("color editor: the selection's atlas places couldn't be found in the game now, so its label is looked for on screen");
      startProbe(true);  // Arrived, so look for its pixels
    } else {
      g_focus = Focus::Idle;
      g_look = 0;
    }
  }
  // ---- the camera ----
  const Vec pivot{feet.x + g_cam.pivot[0], feet.y + g_cam.pivot[1], feet.z + g_cam.pivot[2]};
  const float y = g_cam.yaw / 57.2958f, pt = g_cam.pitch / 57.2958f;
  const Vec loc{pivot.x + g_cam.dist * std::cos(pt) * std::cos(y), pivot.y + g_cam.dist * std::cos(pt) * std::sin(y), pivot.z + g_cam.dist * std::sin(pt)};
  const Vec rot{-g_cam.pitch, g_cam.yaw + 180.0f, 0.0f};  // Pitch, yaw, roll, looking at the pivot
  if (g_view_hooked) {  // the view hook puts it on screen, this frame
    const float view[7] = {loc.x, loc.y, loc.z, rot.x, rot.y, rot.z, g_cam_fov};
    std::memcpy(g_view_pose, view, sizeof(view));
    g_view_ms = nowMs();
    g_view_on = true;
  } else {
    setActor(g_cam_actor, loc, rot);
  }
  {  // the pose, for turning screen points into lines of sight and back
    const float fwd[3] = {-std::cos(pt) * std::cos(y), -std::cos(pt) * std::sin(y), -std::sin(pt)};
    const float rgt[3] = {-fwd[1], fwd[0], 0.0f};
    const float rl = std::sqrt(rgt[0] * rgt[0] + rgt[1] * rgt[1]);
    const float r3[3] = {rl > 1e-4f ? rgt[0] / rl : 1.0f, rl > 1e-4f ? rgt[1] / rl : 0.0f, 0.0f};
    const float up[3] = {fwd[1] * r3[2] - fwd[2] * r3[1], fwd[2] * r3[0] - fwd[0] * r3[2], fwd[0] * r3[1] - fwd[1] * r3[0]};
    const float pose[12] = {loc.x, loc.y, loc.z, fwd[0], fwd[1], fwd[2], r3[0], r3[1], r3[2], up[0], up[1], up[2]};
    std::memcpy(g_cam_pose, pose, sizeof(pose));
  }
  if (!g_view_hooked && viewTarget() != g_cam_actor) setViewTarget(g_cam_pc, g_cam_actor);
  // ---- The selection's tag, where its part is on screen (a few parts at most) ----
  float tx = -1.0f, ty = -1.0f;
  if (g_tag_found && std::fabs(angleTo(g_tag_yaw, facing(*t))) > 30.0f)
    g_tag_found = false;  // The fighter turned round (a side switch), so the point is on the wrong side now
  if (sel_ready && selRows().size() <= 8 && !g_mapping && !g_scan_on) {
    float d = 0;
    const float* p = g_tag_point;  // on its pixel, as seen (never a guess)
    if (g_tag_found && project(Vec{feet.x + p[0], feet.y + p[1], feet.z + p[2]}, g_aspect, tx, ty, d) && (tx < 0.0f || tx > 1.0f || ty < 0.0f || ty > 1.0f))
      tx = ty = -1.0f;  // off screen
  }
  g_tag_x = tx;
  g_tag_y = ty;
}

namespace {
  bool sameFile(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    if (std::filesystem::file_size(a, ec) != std::filesystem::file_size(b, ec) || ec) return false;
    std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
    return x && y && std::equal(std::istreambuf_iterator<char>(x), std::istreambuf_iterator<char>(), std::istreambuf_iterator<char>(y), std::istreambuf_iterator<char>());
  }
  // Moves what's in from into to, folder by folder. Returns how many entries were left behind (in use, or a
  // different file already there); from is removed when nothing is.
  int mergeInto(const std::filesystem::path& from, const std::filesystem::path& to, int& moved) {
    int left = 0;
    std::error_code ec;
    if (std::filesystem::equivalent(from, to, ec)) return 1;  // (one folder by two names: nothing to move)
    std::filesystem::create_directories(to, ec);
    for (const auto& e : listDir(from)) {
      std::error_code mec;
      const auto dst = to / e.path().filename();
      const auto type = e.symlink_status(mec).type();
      if (type != std::filesystem::file_type::directory && type != std::filesystem::file_type::regular) { left++; continue; }  // Links and junctions stay as they are
      if (std::filesystem::exists(dst, mec)) {
        if (std::filesystem::equivalent(e.path(), dst, mec) || mec) { left++; continue; }  // (the same file by two names is never "a copy")
        if (e.is_directory(mec) && std::filesystem::is_directory(dst, mec)) left += mergeInto(e.path(), dst, moved);
        else if (!e.is_directory(mec) && sameFile(e.path(), dst)) { std::filesystem::remove(e.path(), mec); left += mec ? 1 : 0; }
        else left++;
        continue;
      }
      std::filesystem::rename(e.path(), dst, mec);
      if (mec) left++; else moved++;
    }
    if (!left) std::filesystem::remove(from, ec);  // (only removes it if it's empty; a skipped non-ASCII name keeps it)
    return left;
  }
}  // namespace

void init(const std::filesystem::path& ue4ss_dir) {
  g_dir = ue4ss_dir / "StriveLabs_colors";
  {  // The test builds' folder, merged into the release one (saves, palettes and cache keep working). A file the
     // release zip put there already (the effect list, the layouts) is only dropped from the old folder if it's the
     // same; anything else stays there. The old folder goes once it's empty.
    std::error_code ec;
    const auto old = ue4ss_dir / "StriveLabs_recolor_test";
    if (std::filesystem::is_directory(old, ec)) {
      int moved = 0;
      const int left = mergeInto(old, g_dir, moved);
      if (moved || left) COMBO_LOG("color editor: StriveLabs_recolor_test merged into StriveLabs_colors (" + std::to_string(moved) + " moved" +
                                   (left ? ", " + std::to_string(left) + " left in the old folder: different from or in use)" : ")"));
    }
  }
  {  // Parts of their own found before (Sol's DI wing), so the menus know them from the start
    std::error_code ec;
    std::filesystem::directory_iterator it(g_dir / "cache", ec), end;
    for (; !ec && it != end; it.increment(ec)) {  // No throwing. A folder that can't be read is skipped
      const auto& d = *it;
      std::ifstream f(d.path() / "extra_parts.txt");
      const std::string chr = d.path().filename().string();
      std::string line;
      while (std::getline(f, line)) {
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) continue;
        const std::string nm = line.substr(0, tab), kind = line.substr(tab + 1);
        std::string key = chr + "/" + nm;
        for (auto& ch : key) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (kind.size() > 24 || !std::all_of(kind.begin(), kind.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; })) continue;
        g_extra_mi[key] = kind;
        g_extra_read[chr + "/" + kind] = std::wstring(nm.begin(), nm.end());
      }
    }
  }
  try {  // ULocalPlayer::GetViewPoint (the same opening bytes as in other UE4 4.2x games)
    const uint64_t at = sigscan::get().scan(
        "\x4C\x8B\xDC\x49\x89\x5B\x20\x55\x56\x57\x48\x81\xEC\x00\x00\x00\x00\x48\x8B\x05\x00\x00\x00\x00\x48\x33\xC4\x48\x89\x84\x24\x00\x00\x00\x00\x41\x0F",
        "xxxxxxxxxxxxx????xxx????xxxxxxx????xx");
    static PLH::x64Detour* detour = nullptr;
    detour = new PLH::x64Detour(at, reinterpret_cast<uint64_t>(&viewPointHook), reinterpret_cast<uint64_t*>(&g_get_view_point));
    g_view_hooked = detour->hook() && g_get_view_point;
  } catch (...) {}
  COMBO_LOG(g_view_hooked ? "free camera: view hook in" : "free camera: view hook not found, using the level's camera actor");
  g_psc_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.ParticleSystemComponent"));  // Found once, here
}
// The effect creation hook, registered once from the game thread in the first fight. Registering installs
// UE4SS's detour, and doing that at UE4SS's start, on its thread, while the game was loading at full speed
// crashed once inside UE4SS (Nov 1, bad_function_call in its dispatch). A fight's pool scan finds the rest.
void hookEffects() {
  static bool done = false;
  if (done) return;
  done = true;
  if (!g_psc_class) g_psc_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.ParticleSystemComponent"));
  try {  // New effect components as they're made. Any thread, so they're only noted here
    Hook::RegisterStaticConstructObjectPostCallback([](const FStaticConstructObjectParameters& params, UObject* obj) -> UObject* {
      // Only ones in the game's effect pool (they last the fight). Checked now, while the outer surely exists.
      UClass* psc_class = g_psc_class;
      if (obj && psc_class && obj->GetClassPrivate() == psc_class && params.Outer && params.Outer->GetClassPrivate() &&
          params.Outer->GetClassPrivate()->GetName().find(STR("PSCManager")) != StringType::npos &&
          params.Outer->GetName().rfind(STR("Default__"), 0) != 0) {  // Not the pool's template, since that would change every later one
        const Kept k = keep(obj);
        std::lock_guard<std::mutex> lk(g_smear_new_m);
        if (g_smear_new.size() < 4096) g_smear_new.push_back(k);
      }
      return obj;
    });
    COMBO_LOG("color editor: effect creation hook in");
  } catch (...) {}
}

void update(bool round_started) {
  hookEffects();  // Once, in the first fight. Each fight's own pool scan finds what was made before it
  Timed timed{g_time.update};
  Laps laps;
  struct ExportLast { Laps& l; ~ExportLast() { l.mark("before export"); try { exportTick(); } catch (...) { COMBO_LOG("color editor: export step failed"); } l.mark("export"); } } export_last{laps};  // An export carries on, editor open or not. Last, so the editor's own encodes take the free slot first
  timingTick();
  static bool key_was_down = false;
  const bool key = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState('E') & 0x8000);
  if (key && !key_was_down) toggle();
  key_was_down = key;

  // Another fight (or none). The materials found belong to the old one and may be gone, so they're dropped
  // without being touched. Ours are let go, and edits go on the new ones. A new fight can sit where the old
  // one was, so a gap in the updates (a loading screen, the pause menu) counts as one too.
  // ponytail: after a gap, the fighters' addresses stand in for a fight id (a new fight reusing both is missed).
  static Clock::time_point last_update = Clock::now();
  bool gap = Clock::now() - last_update > std::chrono::seconds(2);
  last_update = Clock::now();
  if (gap && !g_targets.empty()) gap = fighters() != g_pawns;  // A pause holds the updates too. Same fighters means nothing to do
  if (!g_targets.empty() && (gap || asw_engine::get() != g_fight)) {
    // Materials still on stage get the game's own textures back first, so ours can go and the next
    // discover() reads true originals. Only ones found on stage now are touched.
    if (asw_engine::get() == g_fight) {
      std::set<UObject*> on_stage;
      const std::vector<UObject*> comps_now = fighterParts();
      const bool preview_here = g_preview.pawn && std::all_of(g_preview.dressed.begin(), g_preview.dressed.end(), [&](const Dressed& d) {
        return std::find(comps_now.begin(), comps_now.end(), d.comp) != comps_now.end();
      });
      stopPreview(preview_here);
      for (UObject* comp : comps_now)
        for (int i = 0, n = numMaterials(comp); i < n; i++) on_stage.insert(material(comp, i));
      std::set<UObject*> ours;  // only a texture we put there is taken off (a new material at an old address has its own)
      for (const auto& [k, sh] : g_shown) { ours.insert(sh.base); ours.insert(sh.sss); }
      for (const Shown& sh : g_scan_tex) ours.insert(sh.base);
      ours.insert(g_hl.base); ours.insert(g_hl.sss);
      ours.insert(g_hl_off.base); ours.insert(g_hl_off.sss);
      ours.erase(nullptr);
      for (Target& t : g_targets)
        for (size_t i = 0; i < t.mids.size(); i++) {
          if (!on_stage.count(t.mids[i])) continue;
          if (!t.orig[i].first) {  // An outline has only its shadow texture
            if (ours.count(getTexture(t.mids[i], STR("SssMap")))) setTexture(t.mids[i], STR("SssMap"), t.orig[i].second);
            continue;
          }
          if (ours.count(getTexture(t.mids[i], STR("BaseMap")))) {
            setTexture(t.mids[i], STR("BaseMap"), t.orig[i].first);
            setTexture(t.mids[i], STR("SssMap"), t.orig[i].second);
          }
        }
      decalOriginals(on_stage);
    }
    stopPreview(false);  // Already put back above, when its meshes were still on stage
    capStage();
    fxRestore();
    if (asw_engine::get() == g_fight) undressSmears();
    g_smeared.clear();
    g_matdressed.clear();
    g_worn_failed.clear();
    g_smear_pool.clear();
    g_smear_live.clear();
    g_named.clear();
    g_tpl_mats.clear();
    effCopiesForget();
    { std::lock_guard<std::mutex> lk(g_smear_new_m); g_smear_new.clear(); }
    g_smear_scanned = false;
    g_vec_orig.clear();
    g_menu_rows.clear();
    g_vec_owner.clear();
    decalForget();
    for (auto it = g_paint.begin(); it != g_paint.end();) {  // Parts with nothing unsaved are read again when painted or shown
      finishStroke(it->second, fromKey(it->first));
      it = g_unsaved.count(it->first) ? std::next(it) : g_paint.erase(it);
    }
    g_watch.clear();
    g_swaps.clear();
    g_swap_seen.clear();
    g_swap_primed.clear();
    g_auto_done = false;
    g_auto_queue.clear();
    g_auto_paused.clear();
    refreshDrop();
    g_targets.clear();
    for (auto& [k, s] : g_shown) unroot(s);
    g_shown.clear();
    unroot(g_hl);
    unroot(g_hl_off);
    g_selected.clear(); g_selected_rows.clear();
    g_cam.ready = false;
    g_cam_pc = g_cam_actor = g_cam_orig = nullptr;  // the old fight's
    g_view_on = false;
    scanOff(nullptr, false);  // Let go, not taken off, since the materials may be gone
    dropMapping();
    g_focus = Focus::Idle;
    g_look = 0;
    g_tag_found = false;
    g_keep_dist = 0.0f;
    g_bones_for.clear();
    g_fighter_h_for.clear();
    g_worn_key.clear();  // made again for the new fight's color (it may differ)
    g_worn.reset();      // Its images take a few hundred MB at 4K
    g_dirty = false;
    g_view_dirty = true;
    if (!asw_engine::get()) { if (g_view_dirty) { publish(); g_view_dirty = false; } return; }
    discover();
  }

  std::vector<Req> reqs;
  { std::lock_guard<std::mutex> lock(g_m); reqs.swap(g_reqs); }
  if (!reqs.empty()) g_smear_stale = true;
  laps.mark("start");
  for (const Req& q : reqs) {
    struct SlowReq { int kind; Clock::time_point t0 = Clock::now(); ~SlowReq() {
      const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
      if (ms >= 100) COMBO_LOG("color editor: request " + std::to_string(kind) + " took " + std::to_string(ms) + " ms"); } } slow_req{q.kind};
    Target* t = target();
    const bool ready = t && g_worn && g_worn_key == keyOf(*t);
    std::vector<Edit>* ed = ready ? &g_edits[keyOf(*t)] : nullptr;
    const int R = ready ? static_cast<int>(g_worn->lit.size()) : 0;
    if (ed && static_cast<int>(ed->size()) < R) ed->resize(R);
    // Leaving this target (another fighter, or the body / weapon). Nothing of ours stays on it.
    auto leave = [&](int to) {
      if (g_export && g_export->mode == 3 && to >= 0 && to < static_cast<int>(g_targets.size()))  // (the editor makes the part it shows: an older background result for it is dropped)
        for (auto& [k, pngs] : g_export->made) if (k == keyOf(g_targets[static_cast<size_t>(to)])) pngs.reset();
      finishStrokes();
      if (t) { auto pp = g_paint.find(keyOf(*t)); if (pp != g_paint.end()) { pp->second.base[0] = pp->second.base[1] = pp->second.comp[0] = pp->second.comp[1] = paint::Image{}; } }
      stopFocus(t);
      g_picked = -1;
      g_pick_quiet = false;
      g_pick_choices.clear();
      if (g_mapping) g_map_tried.erase(g_mapping->key);  // Cut off, so it's mapped again when this target is back
      stopMapping(t);
      unroot(g_hl);
      unroot(g_hl_off);
      g_selected.clear(); g_selected_rows.clear();
      g_blinks_left = 0;
      g_tag_found = false;
      if (t) show(*t, edited(*t));  // stop a blink half way
      g_target = to;
    };
    if (g_export && g_export->mode == 3 && (q.kind == kReread || q.kind == kRemap || q.kind == kImportTex || q.kind == kCopySlot ||
                                             q.kind == kPaletteLoad || q.kind == kImportRecolor || q.kind == kExport || q.kind == kResetColor ||
                                             q.kind == kPaletteSave || q.kind == kPaletteEdit || q.kind == kExportRecolor)) {  // If the other parts are being made again, that's dropped and this goes first
      std::set<std::string> slots;
      for (const auto& [key, pngs] : g_export->made) { const Target o = fromKey(key); slots.insert(o.chr + "/" + o.color); }
      for (const std::string& k : g_export->keys) { const Target o = fromKey(k); slots.insert(o.chr + "/" + o.color); }
      if (g_export->job.valid()) g_bool_orphans.push_back(std::move(g_export->job));
      g_export.reset();
      for (const std::string& sl : slots) g_refresh_wait.insert(sl);
    }
    const bool look_on = g_export && g_export->mode == 2;  // A palette's look is being put on, and its result writes the slot's paint
    const bool edit = q.kind == kPairs || q.kind == kLit || q.kind == kShadow || q.kind == kReset || q.kind == kVec || q.kind == kVecReset ||
                      q.kind == kPaintBegin || q.kind == kPaintTo || q.kind == kPaintEnd || q.kind == kPaintFill || q.kind == kPaintClear ||
                      q.kind == kPaintShape || q.kind == kUndo || q.kind == kRedo || q.kind == kSave || q.kind == kDiscardClose ||
                      q.kind == kFx || q.kind == kFxReset || q.kind == kFxOne || q.kind == kFxOneReset || q.kind == kCap || q.kind == kCapReset || q.kind == kMat || q.kind == kMatReset ||
                      q.kind == kRecovery;
    if (g_export && ((look_on && edit) || q.kind == kReread || q.kind == kRemap || q.kind == kImportTex || q.kind == kCopySlot || q.kind == kPaletteLoad ||
                     q.kind == kImportRecolor || q.kind == kExport || q.kind == kResetColor || q.kind == kPaletteSave || q.kind == kPaletteEdit ||
                     q.kind == kExportRecolor)) {  // They change what the export reads, and an exported recolor's look is made by the same workers
      g_status = g_export->mode == 0 ? "Exporting, wait a moment." : "Working on the palette's look, wait a moment.";
      g_view_dirty = true;
      continue;
    }
    if (q.kind == kNext) {  // The other fighter gets its own setup (Switch can't be pressed during one: its window is up)
      g_setup_home.clear();
      g_setup_off = false;
    }
    if ((q.kind == kPart && q.rgb != 1) || q.kind == kRemap || q.kind == kLit || q.kind == kShadow || q.kind == kPairs ||
        q.kind == kPaintBegin || q.kind == kPaintFill || q.kind == kPaintShape || q.kind == kPickAt || (q.kind == kSelect && !q.regions.empty()) ||
        q.kind == kReset || q.kind == kUndo || q.kind == kRedo || q.kind == kReread || q.kind == kImportTex || q.kind == kCopySlot ||
        q.kind == kPaletteLoad || q.kind == kImportRecolor || q.kind == kResetColor || q.kind == kVec || q.kind == kVecReset || q.kind == kFx ||
        q.kind == kFxReset || q.kind == kFxOne || q.kind == kFxOneReset || q.kind == kCap || q.kind == kCapReset || q.kind == kMat ||
        q.kind == kMatReset || q.kind == kCamera) {
      g_setup_off = true;
      g_setup_home.clear();
    }
    switch (q.kind) {
      case kToggle:
        if (g_open && (!g_unsaved.empty() || !g_unsaved_slots.empty() || g_cap_unsaved || !g_cap_restore.empty())) { g_confirm_close = true; break; }  // Asks Save / Discard / Cancel
        g_open = !g_open;
        g_confirm_close = false;
        g_setup_off = false;
        if (g_open) g_setup_seen.clear();  // (a part cut off last time gets its visit again)
        if (!g_open && !g_setup_home.empty()) {  // Closed mid-way through the first-time setup: the part it started on is shown again
          const int home = targetIndex(g_setup_home);
          if (home >= 0 && home != g_target && t && g_targets[static_cast<size_t>(home)].pawn == t->pawn) { leave(home); t = target(); }
        }
        if (!g_open) refreshDrop();  // Closed, so the saved recolors' queue makes what's left
        if (g_open) autoStop();
        else if (!g_auto_paused.empty()) {  // Closed, so the leftover saved recolors go on now
          g_auto_was = target() ? keyOf(*target()) : std::string();
          g_auto_queue.swap(g_auto_paused);
          g_auto_paused.clear();
          autoNext();
        }
        if (g_open && g_targets.empty()) discover();
        else if (g_open) g_find_due = true;  // (searched again below if the last search is 5 s old or more)

        if (!g_open) {  // Closed. No selection, no flashing, and the fighter's own look comes back
          g_setup_home.clear();  // (the first-time setup goes on when it's opened again, from the part shown then)
          g_setup_busy = false;
          { std::lock_guard<std::mutex> lk(g_tex_pick_m); g_tex_pick.clear(); g_tex_added = 0; }  // (textures picked and not in yet are dropped)
          g_tex_home.clear();
          g_tex_front.clear();
          g_tex_all = g_tex_done = 0;
          finishStrokes();
          hideShowAll(true);  // Effects hidden to find one are shown again
          g_on_screen.clear();
          for (auto& [k, pp] : g_paint) pp.base[0] = pp.base[1] = pp.comp[0] = pp.comp[1] = paint::Image{};  // The view's full-size pictures
          g_cam.on = false;  // The game's camera comes back in the next camera update
          stopFocus(t);
          if (g_mapping) g_map_tried.erase(g_mapping->key);  // Cut off, so it's mapped again when the editor is next open
          stopMapping(t);
          g_selected.clear(); g_selected_rows.clear();
          g_blinks_left = 0;
          unroot(g_hl);
          unroot(g_hl_off);
          if (t) show(*t, edited(*t));
          g_blink_on = false;
        }
        break;
      case kLit:
      case kShadow:
        if (ed && q.region >= 0 && q.region < R) {
          pushUndo(*t, q.kind, q.region);
          for (int r : withTwins(leadersOf(*t, R), {q.region})) {  // The row's twins too, so a gradient's steps move alike
            if (r >= static_cast<int>(ed->size())) continue;
            Edit& e = (*ed)[static_cast<size_t>(r)];
            if (q.kind == kLit) { e.has_lit = true; e.lit = shifted(wornLit(r), wornLit(q.region), q.rgb); e.derive = false; }
            else { e.has_shadow = true; e.shadow = shifted(wornShadow(r), wornShadow(q.region), q.rgb); }
          }
          g_dirty = true;
          g_last_change = Clock::now();
          markUnsaved(*t);
        }
        break;
      case kGroupSame:
        g_group_same = q.rgb != 0;
        break;
      case kSelect:
        g_selected = t ? withTwins(leadersOf(*t, R), q.regions) : std::vector<int>{};  // A row's twins show with it
        g_selected_rows.clear();
        for (int r : q.regions) if (r >= 0 && r < R) g_selected_rows.push_back(r);
        unroot(g_hl);  // The blackout frame stays, since it's the same for every selection
        if (t && !g_scan_on) show(*t, edited(*t));  // A mapping scan keeps its frames up
        g_blink_on = false;
        g_blink_at = Clock::now();
        g_tag_found = false;
        {
          const bool all = R > 0 && static_cast<int>(g_selected.size()) >= R;
          // Regions the mapping never saw can't be shown, so no flashing and no flight. They're still editable.
          const bool seen = t && findable(*t, selRows());
          g_blinks_left = g_selected.empty() || all || !seen ? 0 : 6;  // a few flashes show where it is (not for everything)
          stopFocus(t);
          const std::vector<int> lead = t ? leadersOf(*t, R) : std::vector<int>{};
          const auto row = [&](int r) { return r >= 0 && r < static_cast<int>(lead.size()) ? lead[static_cast<size_t>(r)] : r; };
          const bool quiet = q.regions.size() == 1 && std::any_of(g_pick_choices.begin(), g_pick_choices.end(), [&](int c) { return row(c) == row(q.regions[0]); });
          g_pick_quiet = false;  // Picked on screen or chosen from the pick's list, so the camera stays
          if (!quiet) g_pick_choices.clear();
          if (!g_selected.empty() && !all && seen) {
            if (g_follow_sel && !quiet) {  // fly there, at the distance the user has
              if (!g_cam.on) { g_cam.on = true; g_cam.ready = false; }
              g_focus = Focus::Aim;
            } else if (g_cam.on && g_cam.ready && !g_mapping) {  // No flight. Just find its pixel for the label from here
              g_look = 3;
              g_focus = Focus::Move;
            }
          }
        }
        break;
      case kReset:
        if (ed) {
          pushUndo(*t, kReset, q.region);
          if (q.region < 0) {
            ed->assign(ed->size(), Edit{});
            g_vec_saved[keyOf(*t)].clear();
            restoreVecs(keyOf(*t));
            if (g_fx_for == t->chr + "/" + t->color) {
              for (FxColor& f : fxList()) if (fxChanged(f)) { f.edited = false; f.per.clear(); fxApply(f); }
              g_fx_saved[g_fx_for].clear();
            }
            if (g_vecs_for == keyOf(*t))
              for (VecGroup& g : g_vecs) {
                g.edited = false;
                if (!g.mids.empty() && vecOrig(g.mids.front(), g.param)) std::memcpy(g.cur, vecOrig(g.mids.front(), g.param), sizeof(g.cur));
              }
          }
          else
            for (int r : withTwins(leadersOf(*t, R), {q.region}))
              if (r < static_cast<int>(ed->size())) (*ed)[static_cast<size_t>(r)] = Edit{};
          g_dirty = true;
          g_last_change = Clock::now() - std::chrono::seconds(1);
          markUnsaved(*t);
        }
        break;
      case kNext:  // the other fighter's body
        {  // A texture import going on stops (what's in stays)
          std::lock_guard<std::mutex> lk(g_tex_pick_m);
          if (!g_tex_pick.empty()) { g_tex_pick.clear(); g_tex_added = 0; g_status = "Import stopped (the other fighter was picked)."; }
          g_tex_front.clear();
          g_tex_home.clear();
          g_tex_all = g_tex_done = 0;
        }
        if (t)
          for (size_t k = 1; k < g_targets.size(); k++) {
            const int i = static_cast<int>((g_target + k) % g_targets.size());
            if (g_targets[static_cast<size_t>(i)].pawn != t->pawn && g_targets[static_cast<size_t>(i)].kind == "MAIN") { leave(i); break; }
          }
        if (t && target() == t)  // No "MAIN" on the other fighter, so take their first part
          for (size_t i = 0; i < g_targets.size(); i++)
            if (g_targets[i].pawn != t->pawn) { leave(static_cast<int>(i)); break; }
        break;
      case kPart:  // another of this fighter's color materials (its target index, as published)
        if (q.rgb == 1 && (!g_open || g_setup_off)) break;  // The first-time setup's, queued before the editor closed or the user took over
        if (q.rgb == 0) {  // The user's own: a texture import going on stops (what's in stays)
          std::lock_guard<std::mutex> lk(g_tex_pick_m);
          if (!g_tex_pick.empty()) { g_tex_pick.clear(); g_tex_added = 0; g_status = "Import stopped (another part was picked)."; }
          g_tex_front.clear();
          g_tex_home.clear();
          g_tex_all = g_tex_done = 0;
        }
        if (t && q.region >= 0 && q.region < static_cast<int>(g_targets.size()) && q.region != g_target &&
            g_targets[static_cast<size_t>(q.region)].pawn == t->pawn)
          leave(q.region);
        break;
      case kRemap:
        if (t) {
          g_map_force = mapKey(*t);
          if (!g_cam.on) { g_cam.on = true; g_cam.ready = false; }
        }
        break;
      case kFollow: g_follow = q.rgb != 0; g_dirty = true; g_last_change = Clock::now(); break;
      case kFollowSel: g_follow_sel = q.rgb != 0; break;
      case kSave:
        saveAllNow();
        g_status = g_save_failed ? "Couldn't save everything (a file in use?): try Save again." : "Saved.";
        if (g_confirm_close && !g_save_failed) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kToggle, -1, 0}); }
        g_confirm_close = false;  // A failed save keeps the editor open, and it asks again on the next close
        break;
      case kDiscardClose:
        discardAllNow();
        g_confirm_close = false;
        { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kToggle, -1, 0}); }
        break;
      case kCancelClose:
        g_confirm_close = false;
        break;
      case kPaintBegin:
      case kPaintTo:
      case kPaintEnd:
      case kPaintFill:
      case kPaintClear:
      case kPaintShape:
        if (ready && g_worn) paintRequest(*t, q);
        break;
      case kPickAt:
        if (!g_open || !t) break;
        if (!g_cam.on) { g_cam.on = true; g_cam.ready = false; }
        g_pick_wanted = true;
        g_pick_xy[0] = std::clamp(q.f[0], 0.0f, 1.0f);
        g_pick_xy[1] = std::clamp(q.f[1], 0.0f, 1.0f);
        g_status = "Picking...";
        break;
      case kRecovery:
        if (!g_offer_recovery.empty()) {
          if (q.rgb) {
            stageRecovery(g_offer_recovery);
            g_status = "Unsaved changes restored.";
          } else {
            std::error_code ec;
            std::filesystem::remove_all(recoveryDir(g_offer_recovery), ec);
          }
          g_offer_recovery.clear();
        }
        break;
      case kUndo:
      case kRedo:
        finishStrokes();  // A stroke still open gets its own step before stepping back
        if (ready) {
          auto& from = (q.kind == kUndo ? g_undo : g_redo)[slotOf(*t)];
          auto& to = (q.kind == kUndo ? g_redo : g_undo)[slotOf(*t)];
          if (!from.empty()) {
            const Snap s = from.back();
            from.pop_back();
            if (s.paint > 0) {  // the part's canvas steps back (or forward) that many
              auto pp = g_paint.find(s.key);
              if (pp != g_paint.end() && pp->second.ready) {
                for (int i = 0; i < s.paint; i++) { if (q.kind == kUndo) pp->second.canvas.undo(); else pp->second.canvas.redo(); }
                if (s.with_ed) {  // An import's region colors go back (or forward) with its paint, as one step
                  Snap back = s;
                  back.ed = g_edits[s.key];
                  g_edits[s.key] = s.ed;
                  to.push_back(std::move(back));
                } else {
                  to.push_back(s);
                }
                pp->second.snap_stale = true;
                pp->second.gen++;  // The texture view's picture is made again
                g_unsaved.insert(s.key);
                g_recovery_due = true;
                g_recovery_change = Clock::now();
                if (s.key == keyOf(*t)) { g_dirty = true; g_last_change = Clock::now() - std::chrono::seconds(1); }
                else if (g_worn_key == s.key) g_worn_key.clear();
              } else {
                to.push_back(s);
              }
              break;
            }
            to.push_back(snapOf(fromKey(s.key)));  // The same part as it is now
            applySnap(*t, s);
          }
        }
        break;
      case kLists:
        if (t) {
          g_slots_cache = slotsOf(t->chr);
          g_recolor_files_cache.clear();
          std::error_code ec;
          for (const auto& e : listDir(recolorDir())) {
            const std::string n = e.path().filename().string();
            if (e.path().extension() == ".stpl" && n.rfind(t->chr + "_Color", 0) == 0) g_recolor_files_cache.push_back(n);
          }
          std::sort(g_recolor_files_cache.rbegin(), g_recolor_files_cache.rend());  // Newest first
          listPalettes(t->chr);
          if (g_palette_author.empty()) { std::ifstream f(authorFile()); std::getline(f, g_palette_author); }
        }
        break;
      case kExportRecolor:
        g_look_note.clear();
        if (t) g_status = exportRecolorFile(*t) + g_look_note;
        g_look_note.clear();
        break;
      case kPaletteSave:
      case kPaletteEdit:
        g_look_note.clear();
        if (t) {
          std::string file, name, author, note;
          splitMeta(q.text, file, name, author, note);
          if (name.empty()) { g_status = "A palette needs a name."; break; }
          if (!author.empty()) {  // Remembered for the next one
            g_palette_author = oneLine(author);
            std::ofstream(authorFile(), std::ios::trunc) << g_palette_author << "\n";
          }
          const bool fresh = file.empty();
          if (!fresh && !paletteName(file)) { g_status = "Not a palette file."; break; }
          if (fresh) file = fileForName(t->chr, name);
          const std::filesystem::path path = paletteDir(t->chr) / file;
          std::string err;
          if (q.kind == kPaletteSave) {
            std::error_code pec;
            if (!fresh && std::filesystem::exists(path, pec)) {  // The palette it replaces is kept
              std::filesystem::create_directories(paletteDir(t->chr) / ".trash", pec);
              std::filesystem::copy_file(path, trashFile(t->chr, file), pec);
            }
            err = writeRecolor(*t, path, paletteMeta(name, author, note));
            if (err.empty()) lookSaveStart(*t, path);  // The parts' finished look goes into it, on the workers
          } else {  // Only its name, author and note change. The colors stay
            std::ifstream in(path);
            std::vector<std::string> lines;
            std::string line;
            bool head = true, ok = static_cast<bool>(in);
            while (ok && std::getline(in, line)) {
              if (head && line.rfind("file\t", 0) == 0) {
                head = false;
                std::istringstream m(paletteMeta(name, author, note));
                for (std::string ml; std::getline(m, ml);) lines.push_back(ml);
              }
              if (head && (line.rfind("name\t", 0) == 0 || line.rfind("author\t", 0) == 0 || line.rfind("note\t", 0) == 0)) continue;
              lines.push_back(line);
            }
            if (!ok || lines.empty()) err = "Couldn't read " + file + ".";
            else {
              std::error_code ec;
              { std::ofstream out(withSuffix(path, ".new"), std::ios::trunc); for (const auto& l : lines) out << l << "\n"; out.flush(); ok = out.good(); }
              if (ok) std::filesystem::rename(withSuffix(path, ".new"), path, ec);
              else std::filesystem::remove(withSuffix(path, ".new"), ec);
              if (!ok || ec) err = "Couldn't write " + file + ".";
            }
          }
          g_status = !err.empty() ? err : q.kind == kPaletteEdit ? "Palette " + oneLine(name) + " updated." : fresh ? "Saved palette " + oneLine(name) + "." : "Palette " + oneLine(name) + " overwritten.";
          if (err.empty()) g_status += g_look_note;
          g_look_note.clear();
          if (err.empty()) COMBO_LOG("color editor: palette " + file + (q.kind == kPaletteEdit ? " edited" : " saved"));
          listPalettes(t->chr);
        }
        break;
      case kPaletteLoad:
        if (t) {
          if (!paletteName(q.text)) { g_status = "Not a palette file."; break; }
          {  // The slot's recolor is kept first, since a load replaces it. Nothing saved yet means nothing to keep
            const std::string kept = writeRecolor(*t, trashFile(t->chr, t->color + "_before_load.stpl"), paletteMeta(t->color + " before loading " + q.text, "", ""), true);
            (void)kept;
          }
          g_look_note.clear();
          g_status = importRecolorPath(*t, paletteDir(t->chr) / q.text, true) + g_look_note;
          g_look_note.clear();
          COMBO_LOG("color editor: palette " + q.text + " loaded onto " + keyOf(*t) + ": " + g_status);
        }
        break;
      case kPaletteDelete:
        if (t) {
          if (!paletteName(q.text)) { g_status = "Not a palette file."; break; }
          std::error_code ec;
          const std::filesystem::path trash = paletteDir(t->chr) / ".trash";
          std::filesystem::create_directories(trash, ec);
          std::filesystem::rename(paletteDir(t->chr) / q.text, trashFile(t->chr, q.text), ec);  // Kept, not deleted
          g_status = ec ? "Couldn't delete " + q.text + "." : "Palette deleted.";
          listPalettes(t->chr);
        }
        break;
      case kImportRecolor:
        if (t) {  // The slot's recolor is kept first, as a palette load does
          (void)writeRecolor(*t, trashFile(t->chr, t->color + "_before_import.stpl"), paletteMeta(t->color + " before importing " + q.text, "", ""), true);
          g_look_note.clear();
          g_status = importRecolorFile(*t, q.text) + g_look_note;
          g_look_note.clear();
        }
        break;
      case kCopySlot:
        if (t) g_status = copyToSlotNow(*t, q.text);
        break;
      case kResetColor:  // Every part, paint and effect color of the slot back to the game's, kept first as a load does
        if (t) {
          dropLookWait(t->chr + "/" + t->color);
          (void)writeRecolor(*t, trashFile(t->chr, t->color + "_before_reset.stpl"), paletteMeta(t->color + " before Reset whole color", "", ""), true);
          std::error_code ec;
          {
            std::lock_guard<std::mutex> lk(g_files_m);
            for (const auto& [suffix, p] : slotFiles(t->chr, t->color)) std::filesystem::rename(p, withSuffix(p, ".before_reset"), ec);
          }
          reloadSlot(t->chr, t->color);
          g_undo.erase(slotOf(*t));
          g_redo.erase(slotOf(*t));
          g_status = t->color + " is back to the game's colors. The old recolor is kept in recolors/palettes/" + t->chr + "/.trash.";
          COMBO_LOG("color editor: " + t->chr + " " + t->color + " reset (whole color)");
        }
        break;
      case kReread:  // After a game patch changed the textures, read them from the game again next time
        if (t && !g_reading && !g_map_job.valid() && !g_worn_job.valid() && !g_tex_job.valid()) {
          // Every part of the character (body, DI, weapons...), not only the shown one. A part left out would keep
          // its recolor made on the old colors.
          std::set<std::string> kinds;
          for (const Target& o : g_targets) if (o.chr == t->chr) kinds.insert(o.kind);
          kinds.insert(t->kind);
          if (std::any_of(g_unsaved.begin(), g_unsaved.end(), [&](const std::string& k) { return fromKey(k).chr == t->chr; })) {
            g_status = "Save or discard the changes first.";
            break;
          }
          std::error_code ec;
          for (const std::string& kind : kinds) {
            Target x = *t;
            x.kind = kind;
            const auto cdir = g_dir / "cache" / t->chr;  // The maps the saves were made on are kept, and moved onto the new one when read
            if (auto mp = g_maps.find(mapKey(x)); mp != g_maps.end()) archiveMap(cdir, kind, *mp->second);
            archiveMapFile(cdir, kind);
            forgetMap(x);
            std::filesystem::remove(g_dir / "cache" / t->chr / ("colors_" + kind + ".txt"), ec);
            std::filesystem::remove(g_dir / "cache" / t->chr / ("map_" + kind + ".bin"), ec);
          }
          for (const auto& e : listDir(g_dir / "cache" / t->chr))  // The decals' pixels too
            if (e.path().filename().string().rfind("decal_" + t->color + "_", 0) == 0) std::filesystem::remove(e.path(), ec);
          for (Target& o : g_targets) {  // The other parts get the game's own textures now. A saved recolor is rebuilt when the editor closes
            if (o.chr != t->chr || keyOf(o) == keyOf(*t)) continue;
            show(o, Shown{});
            auto sh = g_shown.find(keyOf(o));
            if (sh != g_shown.end()) { unroot(sh->second); g_shown.erase(sh); }
            if (std::filesystem::exists(editsFile(o), ec) || std::filesystem::exists(paintFile(o), ec))
              if (std::find(g_auto_paused.begin(), g_auto_paused.end(), keyOf(o)) == g_auto_paused.end()) g_auto_paused.push_back(keyOf(o));
          }
          g_smear_stale = true;  // Effect copies of the old textures are looked up again
          g_dirty = true;  // The shown part is put on again from what's saved, on the new colors
          g_last_change = Clock::now() - std::chrono::seconds(1);
          g_status = "Reading " + t->chr + "'s colors again...";
        }
        break;
      case kListEffects:
        if (t) {  // every effect of this character the game has loaded (its own folder), as a capture lists them
          std::vector<UObject*> all;
          UObjectGlobals::FindAllOf(STR("ParticleSystem"), all);
          const std::string own = "/Chara/" + codeOf(t->chr) + "/";
          takeAllByName("");
          {  // Every material instance of the character, and the shared ones its effects use, by folder
            std::set<UObject*> mics;
            std::vector<UObject*> found;
            UObjectGlobals::FindAllOf(STR("MaterialInstanceConstant"), found);
            for (UObject* m : found)
              if (m && keep(m).alive() && narrow(m->GetPathName()).find(own) != std::string::npos) mics.insert(m);
            for (UObject* ps : all)
              if (ps && keep(ps).alive() && narrow(ps->GetPathName()).find(own) != std::string::npos)
                for (const auto& [e, mic] : emitterMats(ps)) mics.insert(mic);
            size_t rows = 0;
            for (UObject* mic : mics) {
              std::string path = narrow(mic->GetPathName());
              std::string group = path.substr(0, path.rfind('/'));  // "/Game/Chara/SOL/Costume01/Material/Color01"
              const size_t at = group.find(own);
              group = "Materials: " + (at != std::string::npos ? group.substr(at + own.size()) : group.substr(group.find('/', 1) + 1));
              for (const std::string& param : matColorParams(mic)) {
                if (std::any_of(g_matc.begin(), g_matc.end(), [&](const MatColor& x) { return x.effect == group && x.mic.obj == mic && x.param == param; })) continue;
                MatColor mc;
                mc.effect = group;
                mc.label = narrow(mic->GetName()) + "  " + param;
                mc.path = path;
                mc.param = param;
                mc.mic = keep(mic);
                if (!vecDefault(mic, param, mc.orig)) continue;
                const float mx = (std::max)({mc.orig[0], mc.orig[1], mc.orig[2], 1e-6f});
                for (int k = 0; k < 3; k++) mc.rgb[k] = (std::max)(mc.orig[k], 0.0f) / mx;
                auto sv = g_cap_saved.find(mc.path + "\t@" + param);
                if (sv != g_cap_saved.end()) { std::memcpy(mc.rgb, sv->second.data(), sizeof(mc.rgb)); mc.edited = true; }
                g_matc.push_back(mc);
                rows++;
              }
            }
            COMBO_LOG("color editor: listed " + std::to_string(rows) + " material colors in " + std::to_string(mics.size()) + " materials");
            g_view_dirty = true;
          }
          const size_t before = g_cap_tpls.size();
          for (UObject* ps : all)
            if (ps && keep(ps).alive() && narrow(ps->GetPathName()).find(own) != std::string::npos) capAdd(ps);  // Not ones being destroyed
          for (UObject* ps : all) {  // Diagnostic. The HMC effects' colors and materials as the game holds them now
            if (!ps || !keep(ps).alive() || narrow(ps->GetPathName()).find(own) == std::string::npos || narrow(ps->GetName()).find("502") == std::string::npos) continue;
            auto* emitters = ps->GetValuePtrByPropertyNameInChain<UArr>(STR("Emitters"));
            for (int e = 0; emitters && e < emitters->num && e < 32; e++) {
              UObject* em = emitters->data[e];
              auto* lods = em ? em->GetValuePtrByPropertyNameInChain<UArr>(STR("LODLevels")) : nullptr;
              if (!lods || lods->num < 1 || !lods->data[0]) continue;
              auto* mods = lods->data[0]->GetValuePtrByPropertyNameInChain<UArr>(STR("Modules"));
              std::string line = narrow(ps->GetName()) + " emitter " + std::to_string(e + 1) + ":";
              if (auto** rq = lods->data[0]->GetValuePtrByPropertyNameInChain<UObject*>(STR("RequiredModule")); rq && *rq)
                if (auto** mat = (*rq)->GetValuePtrByPropertyNameInChain<UObject*>(STR("Material")); mat && *mat) {
                  line += " material " + narrow((*mat)->GetName());
                  float nc[4] = {};
                  if (vecDefault(*mat, "noise_color", nc)) line += " noise_color " + std::to_string(nc[0]) + " " + std::to_string(nc[1]) + " " + std::to_string(nc[2]);
                }
              for (int m = 0; mods && m < mods->num && m < 64; m++) {
                UObject* mod = mods->data[m];
                if (!mod || !mod->GetClassPrivate()) continue;
                const std::string cls = narrow(mod->GetClassPrivate()->GetName());
                if (cls.find("Color") == std::string::npos && cls.find("Material") == std::string::npos) continue;
                line += " | " + cls;
                if (!fxPattern(mod).empty()) line += " by name " + fxPattern(mod);
                for (const TCHAR* prop : {STR("ColorOverLife"), STR("StartColor"), STR("ColorScaleOverLife")}) {
                  uint8_t* d = fxDist(mod, prop);
                  if (!d) continue;
                  FArr* v = reinterpret_cast<FArr*>(d + 8);
                  if (v->num < 3 || v->num > 4096) continue;
                  line += " " + narrow(prop) + " " + std::to_string(v->data[0]) + " " + std::to_string(v->data[1]) + " " + std::to_string(v->data[2]) + " (" + std::to_string(v->num / 3) + " keys)";
                }
              }
              COMBO_LOG("color editor: " + line);
            }
          }
          g_status = "Listed " + std::to_string(g_cap_tpls.size() - before) + " more effects.";
          COMBO_LOG("color editor: " + g_status);
        }
        break;
      case kCapture:
        if (t) {
          g_cap_until = Clock::now() + std::chrono::seconds(5);
          g_status = "Capturing effects for 5 seconds: do the move now.";
        }
        break;
      case kCap:
      case kCapReset:
        if (q.rgb == g_cap_gen && q.region >= 0 && q.region < static_cast<int>(g_cap.size())) {  // rgb carries the list's version, as shown
          CapColor& c = g_cap[static_cast<size_t>(q.region)];
          const std::string key = c.path + "\t" + std::to_string(c.index);
          if (q.kind == kCap) { std::memcpy(c.rgb, q.f, sizeof(c.rgb)); c.edited = true; g_cap_saved[key] = {c.rgb[0], c.rgb[1], c.rgb[2]}; }
          else { c.edited = false; g_cap_saved.erase(key); }
          capApply(c);
          g_cap_unsaved = true;  // Saved with Save
          g_recovery_due = true;
          g_recovery_change = Clock::now();
        }
        break;
      case kOnScreen:
        g_on_screen.clear();
        if (q.region) {  // The pool's components playing now. Their effects get listed (rows made) and the list narrows to them
          std::set<std::string> names;
          for (const Kept& k : g_smear_pool) {
            if (!k.alive() || !isActive(k.obj)) continue;
            auto** tp = k.obj->GetValuePtrByPropertyNameInChain<UObject*>(STR("Template"));
            if (!tp || !*tp) continue;
            const std::string path = narrow((*tp)->GetPathName());  // Another character's isn't this one's to edit. Shared and common ones are
            if (t && path.find("/Chara/") != std::string::npos && path.find("/Chara/" + codeOf(t->chr) + "/") == std::string::npos &&
                path.find("/Chara/CMN/") == std::string::npos)
              continue;
            capAdd(*tp);
            names.insert(narrow((*tp)->GetName()));
          }
          g_on_screen.assign(names.begin(), names.end());
          std::string all;
          for (const std::string& n : g_on_screen) all += " " + n;
          COMBO_LOG("color editor: on screen now:" + all);
          g_status = std::to_string(g_on_screen.size()) + " effects playing";
        }
        break;
      case kHide:
        if (q.region) g_hidden.insert(q.text);
        else g_hidden.erase(q.text);
        break;
      case kMat:
      case kMatReset:
        if (q.rgb == g_cap_gen && q.region >= 0 && q.region < static_cast<int>(g_matc.size())) {
          const MatColor src = g_matc[static_cast<size_t>(q.region)];
          if (!src.mic.alive()) break;
          const std::string key = src.path + "\t@" + src.param;
          for (MatColor& m : g_matc)  // Every effect's row for this material's parameter
            if (m.mic.obj == src.mic.obj && m.param == src.param) {
              if (q.kind == kMat) { std::memcpy(m.rgb, q.f, sizeof(m.rgb)); m.edited = true; }
              else m.edited = false;
            }
          if (q.kind == kMat) g_cap_saved[key] = {q.f[0], q.f[1], q.f[2]};
          else g_cap_saved.erase(key);
          matSet(src.mic.obj, src.param, src.orig, q.kind == kMat ? q.f : nullptr);
          if (g_mat_mid.count(src.mic.obj)) g_mat_done.insert(key);  // No copy yet, so matPending tries again
          g_cap_unsaved = true;  // Saved with Save
          g_recovery_due = true;
          g_recovery_change = Clock::now();
        }
        break;
      case kFx:
      case kFxReset:
        if (q.text != g_fx_for) break;  // The list it was sent from isn't the current one
        if (t && ready) pushUndo(*t, q.kind, q.region);
        if (q.region >= 0 && q.region < static_cast<int>(fxList().size())) {
          FxColor& f = fxList()[static_cast<size_t>(q.region)];
          if (q.kind == kFx) { std::memcpy(f.rgb, q.f, sizeof(f.rgb)); f.edited = true; g_fx_saved[g_fx_for][f.label] = {f.rgb[0], f.rgb[1], f.rgb[2]}; }
          else { f.edited = false; g_fx_saved[g_fx_for].erase(f.label); }
          {  // Picks up copies of effects loaded since. A drag sends many edits, so once a second is enough
            static Clock::time_point last;
            if (q.kind == kFxReset || Clock::now() - last > std::chrono::seconds(1)) { takeAllByName(f.label); last = Clock::now(); }
          }
          fxApply(f);
          if (t && t->chr + "/" + t->color == g_fx_for) markUnsaved(*t);
          g_unsaved_slots.insert(g_fx_for);  // Saved with Save
        }
        break;
      case kFxOne:  // one effect's own color (text: "<slot>\t<effect>")
      case kFxOneReset: {
        const size_t tab = q.text.find('\t');
        if (tab == std::string::npos || q.text.substr(0, tab) != g_fx_for) break;
        const std::string eff = q.text.substr(tab + 1);
        if (q.region < 0 || q.region >= static_cast<int>(fxList().size()) || eff.empty()) break;
        if (t && ready) pushUndo(*t, q.kind, q.region * 4096 + static_cast<int>(std::hash<std::string>{}(eff) % 4096));
        FxColor& f = fxList()[static_cast<size_t>(q.region)];
        if (q.kind == kFxOne) { f.per[eff] = {q.f[0], q.f[1], q.f[2]}; g_fx_saved[g_fx_for][f.label + "@" + eff] = f.per[eff]; }
        else { f.per.erase(eff); g_fx_saved[g_fx_for].erase(f.label + "@" + eff); }
        {  // Picks up copies of effects loaded since. Once a second is enough for a drag
          static Clock::time_point last;
          if (q.kind == kFxOneReset || Clock::now() - last > std::chrono::seconds(1)) { takeAllByName(f.label); last = Clock::now(); }
        }
        fxApply(f);
        if (t && t->chr + "/" + t->color == g_fx_for) markUnsaved(*t);
        g_unsaved_slots.insert(g_fx_for);
        break;
      }
      case kLogDrawn:
        g_log_until = Clock::now() + std::chrono::seconds(5);
        g_logged.clear();
        COMBO_LOG("drawn: watching for 5 s");
        g_status = "Logging what's drawn for 5 seconds: do the move now.";
        break;
      case kRename:
        if (ready && q.region >= 0 && q.region < R) {
          auto& names = namesOf(*t, R);
          for (int r : withTwins(leadersOf(*t, R), {q.region}))  // The whole row, twins too, so a cleared name clears
            if (r < static_cast<int>(names.size())) names[static_cast<size_t>(r)] = q.text;
          saveNames(*t, names);
        }
        break;
      case kVec:
      case kVecReset:
        if (ready) pushUndo(*t, q.kind, q.region);
        if (ready && g_vecs_for == keyOf(*t))
          for (int i = 0; i < static_cast<int>(g_vecs.size()); i++) {
            if (q.region >= 0 && i != q.region) continue;
            VecGroup& g = g_vecs[static_cast<size_t>(i)];
            if (q.kind == kVec) {
              std::memcpy(g.cur, q.f, sizeof(g.cur));
              g.edited = true;
              g_vec_saved[keyOf(*t)][g.label] = {g.cur[0], g.cur[1], g.cur[2], g.cur[3]};
              for (size_t k = 0; k < g.mids.size(); k++) vecSet(g.mids[k], g.paramOf(k), g.cur);
            } else {
              g.edited = false;
              g_vec_saved[keyOf(*t)].erase(g.label);
              for (size_t k = 0; k < g.mids.size(); k++) vecRestore(g.mids[k], g.paramOf(k));
              if (!g.mids.empty() && vecOrig(g.mids.front(), g.param)) std::memcpy(g.cur, vecOrig(g.mids.front(), g.param), sizeof(g.cur));
            }
          }
        if (ready) markUnsaved(*t);  // Saved with Save
        break;
      case kImportTex:  // (the picked files go in below, each on its part)
        break;
      case kExport:
        if (t) exportStart(*t);  // Every part of the slot. Ones not ready yet are named
        break;
      case kCamera:
        g_cam.on = q.rgb != 0;
        g_cam.ready = false;
        g_cam_info = g_cam.on ? std::string(kCamHelp) : std::string();
        COMBO_LOG(std::string("free camera: ") + (g_cam.on ? "on" : "off"));
        break;
      case kBlackout:
        g_blackout = q.rgb != 0;
        unroot(g_hl);
        unroot(g_hl_off);
        if (t && !g_scan_on) show(*t, edited(*t));
        g_blink_on = false;
        g_blinks_left = g_selected.empty() || static_cast<int>(g_selected.size()) >= R || !t || !findable(*t, selRows()) ? 0 : 6;  // show the new look
        break;
      case kPairs:
        if (ed) {
          pushUndo(*t, kPairs, -1);  // A gradient or auto colors is one step
          const std::vector<int> lead = leadersOf(*t, R);  // Taken before, so the rows are as the picks were made
          std::set<int> done;
          for (const Pick& k : q.picks) {
            if (k.region < 0 || k.region >= R) continue;
            if (q.rgb && !done.insert(lead[static_cast<size_t>(k.region)]).second) continue;  // By rows, each row once, with its first pick
            for (int r : q.rgb ? withTwins(lead, {k.region}) : std::vector<int>{k.region}) {  // For auto colors, each region gets its own part's
              if (r >= static_cast<int>(ed->size())) continue;
              Edit& e = (*ed)[static_cast<size_t>(r)];
              if (k.shadow_mode == 3) { e = Edit{}; continue; }  // Back to the game's own, with no edit left
              e.has_lit = true;
              const bool same = q.rgb == 2;  // A chroma key sets exactly that color everywhere. Otherwise a gradient's steps move alike
              e.lit = same ? k.lit & 0xFFFFFF : shifted(wornLit(r), wornLit(k.region), k.lit & 0xFFFFFF);
              e.has_shadow = k.shadow_mode == 1;
              e.shadow = same ? k.shadow & 0xFFFFFF : shifted(wornShadow(r), wornShadow(k.region), k.shadow & 0xFFFFFF);
              e.derive = k.shadow_mode == 2;
            }
          }
          g_dirty = true;
          g_last_change = Clock::now() - std::chrono::seconds(1);  // At once, with one update for all of them
          markUnsaved(*t);
        }
        break;
    }
    g_view_dirty = true;
  }

  laps.mark("requests");
  // A position reset keeps the fighters' meshes and materials. They're found again if any of them is gone, else
  // at most every 5 s, which picks up effect copies of the color materials that appeared since.
  static Clock::time_point last_find;
  const bool all_alive = round_started && std::all_of(g_targets.begin(), g_targets.end(), [](const Target& t) {
    if (t.items.size() != t.mids.size() + t.comps.size()) return false;
    for (size_t i = 0; i < t.items.size(); i++) {
      UObject* o = i < t.mids.size() ? t.mids[i] : t.comps[i - t.mids.size()];
      if (o && !Kept{o, t.items[i]}.alive()) return false;
    }
    return true;
  });
  // The every-5-s search is for effect copies of a recolored part; with the editor closed and no fighter wearing a
  // recolor there's nothing for it to find, so a reset costs nothing
  const bool recolor_on = g_open || std::any_of(g_targets.begin(), g_targets.end(), [](const Target& t) {
    auto sh = g_shown.find(keyOf(t));
    return sh != g_shown.end() && sh->second.base;
  });
  const bool targets_there = all_alive && (Clock::now() - last_find < std::chrono::seconds(5) || !recolor_on);
  const bool find_on_open = g_find_due && Clock::now() - last_find >= std::chrono::seconds(5);  // (the editor just opened)
  g_find_due = false;
  if ((round_started && !g_targets.empty() && !(targets_there && asw_engine::get() == g_fight)) || (find_on_open && !g_targets.empty())) {
    last_find = Clock::now();
    stopPreview(asw_engine::get() == g_fight);  // Started again below if still wanted
    if (asw_engine::get() == g_fight) undressSmears();  // Only the live ones. They're dressed again next update
    g_smear_pool.clear();
    g_smeared.clear();
    g_smear_tick = 0;  // Scan next update
    g_smear_scanned = false;
    // A new fight replaces the game's materials, so find them again and put the edits back on.
    // (only the parts' own materials count: the pool's live copies can be made again on a reset)
    auto own = [](const Target& t) { return t.mids.begin() + static_cast<std::ptrdiff_t>((std::min)(t.own_mids, t.mids.size())); };
    std::vector<UObject*> before;
    for (const Target& t : g_targets) before.insert(before.end(), t.mids.begin(), own(t));
    discover();
    std::vector<UObject*> after;
    for (const Target& t : g_targets) after.insert(after.end(), t.mids.begin(), own(t));
    if (before != after) {
      COMBO_LOG("color editor: the fighters' own materials changed on a reset: the selection and view start over");
      for (auto& [k, s] : g_shown) unroot(s);
      g_shown.clear();
      unroot(g_hl);
      unroot(g_hl_off);
      scanOff(nullptr, false);  // On the old materials, so let go
      dropMapping();
      g_focus = Focus::Idle;
      g_selected.clear(); g_selected_rows.clear();
      g_dirty = true;
      g_last_change = Clock::now() - std::chrono::seconds(1);
    }
  }
  if (!g_targets.empty() && asw_engine::get() == g_fight) {
    learnMove();
    swapWatch();
    dressSmears();
    capTick();
  } else {
    takeNewEffects();  // Not kept, since they'd be stale by the time they're used
  }
  if (g_log_until != Clock::time_point{}) {
    if (Clock::now() < g_log_until) logDrawnNow();
    else {
      g_log_until = Clock::time_point{};
      COMBO_LOG("drawn: done, " + std::to_string(g_logged.size()) + " meshes");
      g_status = "Logged " + std::to_string(g_logged.size()) + " meshes to combo_log.txt.";
      g_view_dirty = true;
    }
  }
  {  // a hidden model is shown in place of the normal one while it's the part being edited
    Target* cur = target();
    const bool want = g_open && cur && asw_engine::get() == g_fight && hiddenPart(*cur);
    if (g_preview.pawn && (!want || g_preview.pawn != cur->pawn)) { stopPreview(true); g_view_dirty = true; }
    if (want && !g_preview.pawn) { startPreview(*cur); g_view_dirty = true; }
    keepPreview();
  }
  if (!g_open && !g_auto_done && asw_engine::get()) {
    g_auto_done = true;
    if (g_targets.empty()) discover();
    g_auto_queue.clear();
    g_auto_was = target() ? keyOf(*target()) : std::string();
    std::error_code ec;
    for (const Target& o : g_targets)
      if (std::filesystem::exists(editsFile(o), ec) || std::filesystem::exists(fxFile(o.chr, o.color), ec) || std::filesystem::exists(paintFile(o), ec))
        g_auto_queue.push_back(keyOf(o));
    if (!g_auto_queue.empty()) {
      COMBO_LOG("color editor: putting on " + std::to_string(g_auto_queue.size()) + " saved recolors");
      autoNext();
    }
  }
  if (g_open && !g_auto_done && asw_engine::get()) {  // A fight loaded with the editor open: the shown part is the editor's, the rest are made in the background
    g_auto_done = true;
    if (g_targets.empty()) discover();
    std::error_code ec;
    std::set<std::string> slots;
    for (const Target& o : g_targets) {
      const bool has = (g_edits_loaded.count(keyOf(o)) ? anyEdit(g_edits[keyOf(o)]) || hasPaint(o) || (g_vec_saved.count(keyOf(o)) && !g_vec_saved[keyOf(o)].empty())
                                                        : std::filesystem::exists(editsFile(o), ec) || std::filesystem::exists(paintFile(o), ec)) ||
                       std::filesystem::exists(fxFile(o.chr, o.color), ec);  // (effect colors: on when the editor closes)
      if (!has) continue;
      if (std::find(g_auto_paused.begin(), g_auto_paused.end(), keyOf(o)) == g_auto_paused.end()) g_auto_paused.push_back(keyOf(o));  // (and when the editor closes, if not made by then)
      slots.insert(o.chr + "/" + o.color);
    }
    for (const std::string& sl : slots) g_refresh_wait.insert(sl);
    {  // Their saved effect colors go on now too (the shown slot's are the editor's), as a match does
      const Target* shown = target();
      for (const Target& o : g_targets)
        if (auto fs = g_fx_saved.find(o.chr + "/" + o.color);
            !(shown && o.chr == shown->chr && o.color == shown->color) && !g_fx_slots.count(o.chr + "/" + o.color) &&
            (std::filesystem::exists(fxFile(o.chr, o.color), ec) || (fs != g_fx_saved.end() && !fs->second.empty())))  // (saved, or edited and not saved yet)
          fxBuild(o);
      g_fx_for.clear();  // (the editor takes the shown slot's again, next update)
    }
    if (!slots.empty()) COMBO_LOG("color editor: the editor is open, so " + std::to_string(slots.size()) + " slots' saved recolors are made in the background");
  }
  if (!g_open && !g_auto_queue.empty()) {  // A part that can't get ready (its colors couldn't be read) is skipped
    const Target* front = targetIndex(g_auto_queue.front()) >= 0 ? &g_targets[static_cast<size_t>(targetIndex(g_auto_queue.front()))] : nullptr;
    const bool busy = g_reading || g_map_job.valid() || g_worn_job.valid() || g_tex_job.valid();  // A slow map being made isn't given up on
    if (!front || g_map_failed.count(mapKey(*front)) || (!busy && ++g_auto_updates > 60 * 20)) {  // About 20 s of play
      if (front) COMBO_LOG("color editor: couldn't put the saved recolor on " + keyOf(*front));
      g_auto_queue.erase(g_auto_queue.begin());
      autoNext();
      if (g_auto_queue.empty()) g_dirty = false;
    }
  }
  if (g_open && g_offer_recovery.empty() && target() && !g_recovery_offered.count(target()->chr)) {  // Asked once per session per character
    g_recovery_offered.insert(target()->chr);
    if (hasRecovery(target()->chr)) g_offer_recovery = target()->chr;
  }
  if (g_recovery_due && Clock::now() - g_recovery_change > std::chrono::seconds(2)) {
    g_recovery_due = false;
    if (!g_unsaved.empty() || !g_unsaved_slots.empty() || g_cap_unsaved || !g_cap_restore.empty()) writeRecovery();
  }
  if (!g_decals.empty() && asw_engine::get() == g_fight)  // Finished decal tints go on, editor open or not
    if (Target* dt = target(); dt && !dt->mids.empty()) decalTick(dt->mids.front());
  if (!g_open && !g_dirty && !g_tex_job.valid()) { if (g_view_dirty) { publish(); g_view_dirty = false; } return; }  // A job in progress carries on so it can be put on
  if (g_open && g_targets.empty()) discover();
  Target* t = target();
  if (!t) { g_status = "No fighter wearing a color found."; publish(); return; }
  UObject* world = t->mids.empty() ? nullptr : t->mids.front();

  laps.mark("fight");
  // 1. Read the character's colors (of the body or the weapon), one per frame, then build the map on a worker.
  std::string mkey = mapKey(*t);
  const auto cached = cachedPrefixesNow(t->chr, t->kind);
  {  // The first-time setup's "please wait": while a part is mapped (not for Remap), between parts, and while the part
     // shown, never mapped, gets ready to be
    bool pending = false;
    if (g_open && !g_setup_off && g_map_force != mkey && !g_map_tried.count(mkey) && !g_map_failed.count(mkey) && !g_worn_failed.count(keyOf(*t))) {
      if (g_worn && g_worn_key == keyOf(*t)) pending = !partsFor(*t) && !placesFor(*t);
      else {  // (it'll be placed from the atlas: looked at 4 times a second)
        static Clock::time_point when;
        static std::string what;
        static bool covers = false;
        if (what != keyOf(*t) || Clock::now() - when > std::chrono::milliseconds(250)) { what = keyOf(*t); when = Clock::now(); covers = atlasCovers(*t); }
        if (!covers) {
          std::error_code ec;
          const auto pa = g_parts.find(mkey);  // (an empty list: not on disk)
          pending = pa != g_parts.end() ? pa->second.empty() : !std::filesystem::exists(partsFile(mkey, t->chr, t->kind), ec);
        }
      }
    }
    bool busy = g_open && ((g_mapping && !g_mapping->remap) || (!g_setup_off && (!g_setup_home.empty() || pending)));
    const std::string sig = keyOf(*t) + "/" + std::to_string(g_mapping ? g_mapping->view * 2 + (g_mapping->scanning ? 1 : 0) : -1) + "/" +
                            std::to_string(g_map_tried.size()) + "/" + std::to_string(g_reading ? g_reading->next : -1) + "/" + g_worn_key;
    if (!busy || sig != g_setup_sig || g_map_job.valid() || g_worn_job.valid()) { g_setup_sig = sig; g_setup_moved = Clock::now(); }
    else if (Clock::now() - g_setup_moved > std::chrono::seconds(30)) {  // Stuck: the editor is given back
      COMBO_LOG("color editor: first-time setup stopped, nothing moved for 30 s on " + keyOf(*t));
      g_setup_off = true;
      g_setup_home.clear();
      g_map_tried.insert(mkey);  // (not again by itself this session: Remap maps it)
      if (g_mapping) { g_map_tried.insert(g_mapping->key); stopMapping(t); }
      g_status = "The setup stopped here. Press Remap to scan this part.";
      g_setup_moved = Clock::now();
      busy = false;
    }
    if (busy != g_setup_busy) { g_setup_busy = busy; g_view_dirty = true; }
    g_setup_busy_ms = GetTickCount64();
  }
  if (world && !g_export && !g_reading && !g_map_job.valid() && !g_worn_job.valid() && !g_tex_job.valid() &&
      std::find(cached.begin(), cached.end(), prefixOf(srcOf(*t), t->kind)) != cached.end()) {  // 0. If its textures aren't the ones read, read that slot again
    const std::string tried = t->chr + "/" + t->kind + "/" + prefixOf(srcOf(*t), t->kind);
    const int st = g_reread_tried.count(tried) ? 0 : texState(*t);
    if (st == 0) g_reread_tried.insert(tried);  // Checked once a session, since skin mods don't change while the game runs
    const std::string& c = srcOf(*t);
    const int nn = colorName(c) && c.size() == 7 ? std::atoi(c.c_str() + 5) : 0;
    if (st != 0 && nn > 0 && !forgetMap(*t)) {  // With unsaved changes on the part, wait until after Save
      static std::set<std::string> told;  // Said once, not over every other status each frame
      if (g_open && told.insert(tried).second) {
        g_status = st == 1 ? "This color's textures changed: save to read them again." : "Save to finish setting up this color.";
        g_view_dirty = true;
      }
    } else if (st != 0 && nn > 0) {
      g_reread_tried.insert(tried);
      g_reading = std::make_unique<Reading>();
      g_reading->chr = t->chr;
      g_reading->kind = t->kind;
      g_reading->only = g_reading->next = nn;
      COMBO_LOG("color editor: " + tried + (st == 1 ? "'s textures changed since they were read (a skin mod added or removed): read again"
                                                    : ": read again once (its textures' ids weren't kept yet)"));
      g_status = st == 1 ? "This color's textures changed: reading them again..." : "Reading this color once...";
      g_view_dirty = true;
      mkey = mapKey(*t);
    }
  }
  if (auto have = g_maps.find(mkey); have != g_maps.end() && !g_reading && !g_map_job.valid() && !g_worn_job.valid() && !g_tex_job.valid() &&
      !have->second->prefixes.empty() &&
      std::find(have->second->prefixes.begin(), have->second->prefixes.end(), prefixOf(srcOf(*t), t->kind)) == have->second->prefixes.end() &&
      !g_remapped.count(mkey)) {
    if (forgetMap(*t)) { g_remapped.insert(mkey); COMBO_LOG("color editor: " + t->color + " is new to " + mkey + "'s regions: found again with it"); }
    else g_status = "Save first to map this color.";
  }
  if (!g_maps.count(mkey) && !g_map_failed.count(mkey) && !g_map_job.valid() && !g_reading &&
      std::find(cached.begin(), cached.end(), prefixOf(srcOf(*t), t->kind)) != cached.end()) {
    g_map_job_key = mkey;  // Read in an earlier session, so it comes from disk with no reading from the game
    g_map_job = std::async(std::launch::async, mapAndMove, g_dir / "cache" / t->chr, t->kind, cached, ownOfKey(mkey));
    g_status = "Finding the color regions...";
    g_view_dirty = true;
  }
  if (!g_maps.count(mkey) && !g_map_failed.count(mkey) && !g_map_job.valid() && !g_reading) {
    g_reading = std::make_unique<Reading>();
    g_reading->chr = t->chr;
    g_reading->kind = t->kind;
    g_status = "Reading " + t->chr + (t->kind == "MAIN" ? std::string() : t->kind == "WEP" ? std::string("'s weapon") : " " + t->kind) + " colors...";
    g_view_dirty = true;
  }
  if (g_reading && world) {
    Reading& rd = *g_reading;
    std::error_code ec;
    const auto dir = g_dir / "cache" / rd.chr;
    std::filesystem::create_directories(dir, ec);
    for (int tries = 0; tries < 30 && rd.next <= 99; tries++) {
      const int nn = rd.next++;
      if (rd.only && nn != rd.only) { rd.next = 100; break; }  // Only that color when one is read again
      wchar_t col[16];
      swprintf_s(col, L"Color%02d", nn);
      const auto xr = g_extra_read.find(rd.chr + "/" + rd.kind);  // A part of its own uses its material's own name
      const std::wstring mi = xr != g_extra_read.end() ? xr->second : L"MI_" + std::wstring(rd.kind.begin(), rd.kind.end()) + L"_BASE";
      char name[16];
      snprintf(name, sizeof(name), "Color%02d", nn);
      UObject* mic = loadAsset(materialDir(rd.chr, name) + L"/" + mi + L"." + mi);
      if (!mic) continue;
      const std::string prefix = prefixOf(name, rd.kind);
      // What was there is pinned first, if nothing is yet, and never written through a pinned link. The pins are
      // held until it's drawn, so a worker comparing them or making a map from them waits.
      std::unique_lock<std::mutex> pk(g_pin_m, std::try_to_lock);
      if (!pk.owns_lock()) { rd.next = nn; break; }  // The pins are in use on a worker, so this color waits for next frame
      bool clear = true;  // Nothing of ours in the way of the draw
      for (const char* param : {"BaseMap", "SssMap"}) {
        const auto cur = dir / (prefix + "_" + param + ".png"), pin = dir / "pinned" / (prefix + "_" + param + ".png");
        if (std::filesystem::exists(cur, ec)) {
          if (!std::filesystem::exists(pin, ec)) { std::filesystem::create_directories(pin.parent_path(), ec); std::filesystem::rename(cur, pin, ec); }
          else if (std::filesystem::equivalent(cur, pin, ec)) std::filesystem::remove(cur, ec);
          ec.clear();
          clear = clear && (!std::filesystem::exists(cur, ec) || (std::filesystem::exists(pin, ec) && !std::filesystem::equivalent(cur, pin, ec)));
        }
        ec.clear();
      }
      if (!clear) {  // A file held open somewhere is retried for a while, then this color's old textures stay as they are
        if (++rd.stuck < 600) { rd.next = nn; break; }
        rd.stuck = 0;
        if (std::filesystem::exists(dir / (prefix + "_BaseMap.png"), ec) && std::filesystem::exists(dir / (prefix + "_SssMap.png"), ec)) rd.found.push_back(prefix);
        COMBO_LOG("color editor: " + prefix + "'s textures are in use, kept as they were");
        break;
      }
      rd.stuck = 0;
      bool ok = true;
      std::string ids;
      for (const char* param : {"BaseMap", "SssMap"}) {
        UObject* tex = micTexture(mic, param);
        const int w = tex ? sizeOf(tex, STR("Blueprint_GetSizeX")) : 0, h = tex ? sizeOf(tex, STR("Blueprint_GetSizeY")) : 0;
        ok = ok && w > 0 && h > 0 && w <= 8192 && h <= 8192 && drawToFile(world, tex, w, h, dir / (prefix + "_" + param + ".png"));
        ids += (ids.empty() ? "" : " ") + texId(tex);
      }
      if (ok) rd.ids[prefix] = ids;
      if (!ok)  // Not drawn, so restore the textures it had from the pins
        for (const char* param : {"BaseMap", "SssMap"}) {
          const auto cur = dir / (prefix + "_" + param + ".png"), pin = dir / "pinned" / (prefix + "_" + param + ".png");
          if (!std::filesystem::exists(cur, ec) && std::filesystem::exists(pin, ec)) {
            std::filesystem::create_hard_link(pin, cur, ec);
            if (ec) { ec.clear(); std::filesystem::copy_file(pin, cur, ec); }
          }
          ec.clear();
        }
      if (ok) rd.found.push_back(prefix);
      break;  // one color per frame
    }
    if (rd.only && rd.next > rd.only) rd.next = 100;  // The one color is done
    g_view_dirty = true;
    if (rd.next > 99) {
      COMBO_LOG("color editor: " + rd.chr + "/" + rd.kind + " colors read: " + std::to_string(rd.found.size()) + (rd.only ? " (one color)" : ""));
      g_map_job_key = rd.chr + "/" + rd.kind + "/P";  // The shared map first, since it finds which slots changed
      std::vector<std::string> list = rd.found;
      if (rd.only) {  // One color read again. The list is written again as it was, so the pins are compared
        list = cachedPrefixes(rd.chr, rd.kind);
        for (const std::string& p : rd.found) if (std::find(list.begin(), list.end(), p) == list.end()) list.push_back(p);
      }
      {  // the colors read, for later sessions (and the menus)
        std::ofstream f(dir / ("colors_" + rd.kind + ".txt"), std::ios::trunc);
        for (const auto& p : list) f << p << "\n";
      }
      {  // Their textures' ids. A full read keeps only these, a single color replaces just its own
        auto& ids = texIds(rd.chr, rd.kind);
        if (!rd.only) ids.clear();
        for (const auto& [p, id] : rd.ids) ids[p] = id;
        std::ofstream f(withSuffix(idsFile(rd.chr, rd.kind), ".new"), std::ios::trunc);
        for (const auto& [p, id] : ids) f << p << '\t' << id << "\n";
        f.flush();
        const bool good = f.good();
        f.close();
        if (good) std::filesystem::rename(withSuffix(idsFile(rd.chr, rd.kind), ".new"), idsFile(rd.chr, rd.kind), ec);
      }
      g_map_job = std::async(std::launch::async, mapAndMove, dir, rd.kind, list, std::string());
      if (!rd.only) g_remapped.insert(g_map_job_key);  // Only one color was read again, so a new color can still be read in
      g_reading.reset();
      g_status = "Finding the color regions...";
    }
  }
  if (g_map_job.valid() && g_map_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    auto m = g_map_job.get();
    if (m && m->regions > 0) {
      g_maps[g_map_job_key] = m;
      g_worn_failed.clear();  // A new map, so those that failed get another go
      g_old_fits.clear();  // Worked out for the map it replaces
      g_drawn.erase(g_map_job_key);  // Counted for the map it replaces
      g_drawn_tried.erase(g_map_job_key);
      g_drawn_seen.erase(g_map_job_key);
      COMBO_LOG("color editor: " + g_map_job_key + " has " + std::to_string(m->regions) + " regions");
    } else {
      g_map_failed.insert(g_map_job_key);
      g_status = "Couldn't read these colors.";
    }
    if (ownOfKey(g_map_job_key).empty()) {  // The shared map's job compared the slots. Those with their own map are read again
      const size_t a = g_map_job_key.find('/'), b = g_map_job_key.rfind('/');
      forgetOwn(g_map_job_key.substr(0, a), g_map_job_key.substr(a + 1, b - a - 1));
    }
    g_view_dirty = true;
    mkey = mapKey(*t);  // This slot's map may be its own now, with its job next update
  }
  laps.mark("colors");
  // 2. The worn color, ready to edit (worker).
  auto mp = g_maps.find(mkey);
  if (mp != g_maps.end() && g_edits_loaded.insert(keyOf(*t)).second) {  // The map is in, so its stamp checks the file
    {
      auto st = g_edit_restore.find(keyOf(*t));
      if (st != g_edit_restore.end()) {  // A recovered copy is read as unsaved changes
        if (loadEdits(*t, g_edits[keyOf(*t)], mp->second->regions, false, st->second)) {
          g_unsaved.insert(keyOf(*t));
        } else {  // Made for another region map, so it goes to the recovery folder and the real save is read instead
          std::error_code ec;
          std::filesystem::rename(st->second, withSuffix(st->second, ".old"), ec);
          if (!g_paint_restore.count(keyOf(*t))) g_unsaved.erase(keyOf(*t));
          loadEdits(*t, g_edits[keyOf(*t)], mp->second->regions);
        }
        g_edit_restore.erase(st);
      } else {
        loadEdits(*t, g_edits[keyOf(*t)], mp->second->regions);
      }
    }
    if (g_worn && g_worn_key == keyOf(*t)) { g_dirty = true; g_last_change = Clock::now() - std::chrono::seconds(1); }  // Read again on the same worn color, so it's made now
  }
  if (mp != g_maps.end() && g_worn_key != keyOf(*t) && !g_worn_job.valid() && !g_worn_failed.count(keyOf(*t))) {
    const auto dir = g_dir / "cache" / t->chr;
    const std::string prefix = prefixOf(srcOf(*t), t->kind);
    g_worn_job_key = keyOf(*t);
    g_worn_job = std::async(std::launch::async, prepareWorn, mp->second, dir / (prefix + "_BaseMap.png"), dir / (prefix + "_SssMap.png"));
    g_status = "Loading " + slotLabel(t->color) + "...";
    g_view_dirty = true;
  }
  if (g_worn_job.valid() && g_worn_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    auto w = g_worn_job.get();
    if (w) {
      g_worn = w;
      g_worn_serial++;
      g_worn_key = g_worn_job_key;
      if (g_export && g_export->mode == 0) g_status = "Exporting...";  // Still going
      else { g_status = g_aside_note; g_aside_note.clear(); }
      { std::lock_guard<std::mutex> lk(g_tex_pick_m); const Target k = fromKey(g_worn_key); g_tex_pick_dir = newestExport(k.chr, k.color); }  // Import opens there
      if (anyEdit(g_edits[g_worn_key]) || hasPaint(targetOf(g_worn_key))) { g_dirty = true; g_last_change = Clock::now() - std::chrono::seconds(1); }  // Paint alone counts too
    } else {
      g_status = "Couldn't get this color ready.";
      g_worn_failed.insert(g_worn_job_key);  // Not tried every frame. It's tried again once its map is found again
    }
    g_view_dirty = true;
  }
  const bool ready = g_worn && g_worn_key == keyOf(*t);
  if (ready && g_vecs_for != keyOf(*t)) { buildVecs(*t); g_view_dirty = true; }
  {  // the texture view, while the UI draws it
    const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    if (t && g_worn && g_worn_key == keyOf(*t)) drawnTick(*t);  // Which regions the model draws, once per map
    static bool was_on = false;
    const bool on = g_texview_on && now_ms - g_texview_ms < 250;
    if (was_on && !on)  // Closed, so the full-size pictures are let go
      for (auto& [k, pp] : g_paint) { finishStroke(pp, fromKey(k)); pp.base[0] = pp.base[1] = pp.comp[0] = pp.comp[1] = paint::Image{}; }
    was_on = on;
    if (on) {
      if (ready) previewTick(*t);
      else {
        if (g_prev_key != keyOf(*t)) { g_prev_key = keyOf(*t); std::lock_guard<std::mutex> lk(g_prev_m); g_prev_want = g_prev_key; }
        previewClear();
      }
    }
  }
  // This character's saved shared-effect colors. They're read first, so an emitter with your own saved color
  // stays yours when the palette takes its copies, and put on their effects after the slot's colors are set up.
  bool cap_loaded = false;
  if (ready && g_cap_slot != t->chr + "/" + t->color) {
    capStage();  // The slot shown before keeps its unsaved captured colors for Save
    capLoad(t->chr + "/" + t->color);
    cap_loaded = true;
  }
  if (ready && g_fx_for != t->chr + "/" + t->color) { fxBuild(*t); g_view_dirty = true; }
  if (cap_loaded) {
    std::set<std::string> paths;
    for (const auto& [k, c] : g_cap_saved)
      if (k.find("\t@") == std::string::npos) paths.insert(k.substr(0, k.find('\t')));  // A material's, put on by matPending
    for (const std::string& path : paths) {
      const std::wstring w(path.begin(), path.end());
      if (UObject* tpl = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, w.c_str())) capAdd(tpl);
    }
    g_view_dirty = true;
  }
  if (false) {  // Saving is by hand now, with Save
  }
  {  // Picked textures go in one by one, each on the part its name says (shown first, so it's ready to take it)
    std::filesystem::path next;
    size_t queued = 0;
    {
      std::lock_guard<std::mutex> lk(g_tex_pick_m);
      if (!g_open && !g_tex_pick.empty()) { g_tex_pick.clear(); g_tex_added = 0; }  // (picked after the editor closed)
      if (g_tex_added) {  // New files: a new batch if none was going
        if (g_tex_all == 0) { g_tex_done = 0; g_tex_home = keyOf(*t); }
        g_tex_all += g_tex_added;
        g_tex_added = 0;
      }
      if (!g_tex_pick.empty()) next = g_tex_pick.front();
      queued = g_tex_pick.size();
    }
    if (!next.empty() && (g_export || g_mapping || g_scan_on)) g_tex_front_at = Clock::now();  // (its time doesn't run while the editor is busy)
    if (!next.empty() && g_open && !g_export && !g_confirm_close) {
      if (next != g_tex_front) { g_tex_front = next; g_tex_front_at = Clock::now(); g_tex_switched = false; }
      const std::string fname = narrow(next.filename().wstring());
      auto pop = [&](bool ok, const std::string& why) {
        if (!ok && !why.empty()) { g_status = why; COMBO_LOG("color editor: " + why); }
        if (ok) g_tex_done++;
        std::lock_guard<std::mutex> lk(g_tex_pick_m);
        if (!g_tex_pick.empty() && g_tex_pick.front() == next) g_tex_pick.pop_front();
        queued = g_tex_pick.size();
        g_tex_front.clear();
        g_view_dirty = true;
      };
      int m = 0;
      bool many = false;
      const Target* named = texPartOf(*t, next, m, many);
      const std::wstring stem = next.stem().wstring();
      const bool export_name = named || m == 2 || (stem.size() > 4 && (_wcsicmp(stem.c_str() + stem.size() - 4, L"_Lit") == 0 || (stem.size() > 7 && _wcsicmp(stem.c_str() + stem.size() - 7, L"_Shadow") == 0)));
      const bool settled_here = !ready || (!g_dirty && !g_tex_job.valid());  // this part's new textures are made and on (a part not ready has none to make)
      if (Clock::now() - g_tex_front_at > std::chrono::seconds(30)) {
        pop(false, fname + ": couldn't import it (its part didn't get ready).");
      } else if (many) {
        pop(false, fname + ": which part? Its name fits more than one.");
      } else if (!named && export_name && g_tex_all > 1) {
        pop(false, fname + ": no part of this fighter has that name, so it's left out.");
      } else if (named && named != t) {  // Its part first, once this one's textures are on
        if (!g_tex_switched && settled_here && !g_mapping && !g_scan_on) {
          g_tex_switched = true;
          std::lock_guard<std::mutex> lock(g_m);
          g_reqs.push_back({kPart, targetIndex(keyOf(*named)), 2});
        }
      } else if (_wcsicmp(next.filename().wstring().c_str(), L"effects.txt") == 0) {  // The slot's effect colors, once they're read
        if (ready && g_fx_for == t->chr + "/" + t->color) {
          const bool ok = importFxNow(*t, next);
          pop(ok, ok ? std::string() : g_status);
        }
      } else if (m == 2) {  // Material colors, once the part's are found
        if (ready && g_vecs_for == keyOf(*t)) {
          const bool ok = importMatColorsNow(*t, next);
          pop(ok, ok ? std::string() : g_status);
        }
      } else if (ready && g_worn && !g_mapping && !g_scan_on) {
        const bool ok = importTexturesNow(*t, next);
        pop(ok, ok ? std::string() : g_status);
      }
    }
    if (g_tex_all > 0 && queued == 0 && g_tex_front.empty() && (!ready || (!g_dirty && !g_tex_job.valid()))) {  // Done, and the last part's textures are on
      if (g_tex_all > 1) {
        g_status = "Imported " + std::to_string(g_tex_done) + " of " + std::to_string(g_tex_all) + " textures (the rest had no changes or didn't fit: see the log).";
        COMBO_LOG("color editor: " + g_status);
        const int home = targetIndex(g_tex_home);
        if (g_open && home >= 0 && home != g_target) { std::lock_guard<std::mutex> lock(g_m); g_reqs.push_back({kPart, home, 2}); }
      }
      g_tex_home.clear();
      g_tex_all = g_tex_done = 0;
      g_view_dirty = true;
    }
  }
  laps.mark("worn and effects");
  // The parts of the model aren't mapped yet, so the free camera does it (once per character).
  if (g_open && ready && !g_cam.on && !g_mapping && (g_map_force == mkey || (!g_map_tried.count(mkey) && !partsFor(*t) && !placesFor(*t)))) {
    g_cam.on = true;
    g_cam.ready = false;
    g_view_dirty = true;
  }
  // Once this part is mapped (or can't be), the fighter's next part that isn't mapped yet is shown, so it's mapped
  // too. When none is left, the part it started on comes back. (Looked at once a second: it checks files.)
  if (g_open && !g_setup_off && !g_mapping && !g_pick_wanted && !g_pick_scanning && !g_scan_on && !g_reading && !g_map_job.valid() &&
      !g_worn_job.valid() && !g_confirm_close && !g_export && g_map_force != mkey &&
      (g_map_failed.count(mkey) || g_worn_failed.count(keyOf(*t)) || (ready && (g_map_tried.count(mkey) || partsFor(*t) || placesFor(*t))))) {
    static Clock::time_point checked;
    if (Clock::now() - checked >= std::chrono::seconds(1)) {
      checked = Clock::now();
      int next = -1;
      for (int i = 0; next < 0 && i < static_cast<int>(g_targets.size()); i++) {
        const Target& o = g_targets[static_cast<size_t>(i)];
        if (i == g_target || o.pawn != t->pawn) continue;
        const std::string k = mapKey(o);
        if (g_map_tried.count(k) || g_map_failed.count(k) || g_setup_seen.count(k) || atlasCovers(o)) continue;
        std::error_code ec;
        const auto pa = g_parts.find(k);  // (an empty list: not on disk)
        if (pa != g_parts.end() ? !pa->second.empty() : std::filesystem::exists(partsFile(k, o.chr, o.kind), ec)) continue;
        next = i;
      }
      int to = -1;
      if (next >= 0) {
        if (g_setup_home.empty()) g_setup_home = keyOf(*t);
        to = next;
        g_setup_seen.insert(mapKey(g_targets[static_cast<size_t>(next)]));
        COMBO_LOG("color editor: first-time setup goes on to " + keyOf(g_targets[static_cast<size_t>(next)]));
      } else if (!g_setup_home.empty()) {
        to = targetIndex(g_setup_home);
        if (to == g_target) to = -1;
        g_setup_home.clear();
        COMBO_LOG("color editor: first-time setup done");
      }
      if (to >= 0) {
        std::lock_guard<std::mutex> lock(g_m);
        g_reqs.push_back({kPart, to, 1});  // (1: the setup's, not the user's)
      }
    }
  }

  laps.mark("setup");
  // 3. Edits go into the game's material shortly after the last change (not during a scan). The textures are
  // made on a worker (images and PNGs are the slow part) and put on here when they're done.
  std::future<Pngs>& tex_job = g_tex_job;
  std::string& tex_key = g_tex_key;
  dropPngOrphans();
  auto partDone = [&] {  // While putting saved recolors on, move to the next part
    g_blink_at = Clock::now();
    g_blink_on = false;
    g_blinks_left = 0;  // The new color shows, so no more flashing over it
    g_view_dirty = true;
    if (!g_open && !g_auto_queue.empty() && keyOf(*t) == g_auto_queue.front()) {
      g_auto_queue.erase(g_auto_queue.begin());
      autoNext();
    }
  };
  static std::set<std::string> owed;  // parts whose textures were dropped half made: made again when they're back
  if (tex_job.valid() && (tex_key != keyOf(*t) || !ready || g_worn_key != tex_key)) {  // It's for another part now
    owed.insert(tex_key);
    if (g_open && std::find(g_auto_paused.begin(), g_auto_paused.end(), tex_key) == g_auto_paused.end()) g_auto_paused.push_back(tex_key);  // (and when the editor closes)
    g_png_orphans.push_back(std::move(tex_job));
  }
  if (owed.erase(keyOf(*t))) { g_dirty = true; g_last_change = Clock::now() - std::chrono::seconds(1); }
  if (ready && world && g_dirty && !g_scan_on && !tex_job.valid() && g_encoding == 0 && Clock::now() - g_last_change > std::chrono::milliseconds(120)) {
    g_dirty = false;
    auto& ed = g_edits[keyOf(*t)];
    if (!anyEdit(ed) && !hasPaint(*t)) {
      unroot(g_hl);
      show(*t, Shown{});  // the game's own
      unroot(edited(*t));
      partDone();
    } else {
      tex_job = startEncode(g_worn, ed, g_follow, paintBytes(*t));
      tex_key = keyOf(*t);
      if (!tex_job.valid()) g_dirty = true;  // No thread, so try again next update
    }
  }
  if (tex_job.valid() && tex_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    Pngs png = tex_job.get();
    Shown fresh;
    if (world && !g_scan_on && importPngs(world, png, fresh)) {
      unroot(g_hl);  // the highlight is made again from the new colors
      show(*t, fresh);
      unroot(edited(*t));
      edited(*t) = fresh;
      g_smear_stale = true;  // Effects wearing a copy of this part's material get the new textures now
    } else if (g_scan_on) {
      g_dirty = true;  // a scan's frames are up, so it's made again after the scan
    } else {
      g_status = "Couldn't make the new textures.";
      show(*t, edited(*t));  // takes off whatever was showing, such as a highlight
    }
    if (!g_dirty) partDone();
  }
  // The selection flashes on the fighter a few times (its highlight, then the normal look or black),
  // then the normal look stays. Held while the camera finds the part or maps the model.
  const bool blinking = ready && world && g_open && !g_selected.empty() && g_blinks_left > 0 && !g_scan_on && !g_mapping &&
                        g_focus != Focus::Aim && g_focus != Focus::Settle && g_focus != Focus::Probe;
  if (g_hl_job.valid() && g_hl_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {  // a highlight is done, so turn it on if it's still wanted
    Pngs png = g_hl_job.get();
    const bool wanted = ready && world && g_hl_job_key == keyOf(*t) && (g_hl_job_off ? g_blackout && !g_hl_off.base : !g_hl.base && g_hl_job_sel == g_selected);
    if (wanted) importPngs(world, png, g_hl_job_off ? g_hl_off : g_hl);
  }
  if (blinking && !g_hl_job.valid() && g_encoding == 0) {  // make the flash's textures on a worker thread so the game doesn't wait
    const bool off = g_hl.base && g_blackout && !g_hl_off.base;
    if (!g_hl.base || off) {
      g_hl_job_off = off;
      g_hl_job_sel = off ? std::vector<int>{} : g_selected;
      g_hl_job_key = keyOf(*t);
      g_encoding++;
      try { g_hl_job = std::async(std::launch::async, encodeHighlight, g_worn, g_edits[keyOf(*t)], g_hl_job_sel, g_follow, off || g_blackout); }
      catch (...) { g_encoding--; }  // no thread available, so it's tried again next update
    }
  }
  if (blinking && g_hl.base && (!g_blackout || g_hl_off.base) && Clock::now() - g_blink_at > std::chrono::milliseconds(350)) {
    const bool last = --g_blinks_left == 0;
    g_blink_on = !last && !g_blink_on;
    const Shown& off = g_blackout && g_hl_off.base && !last ? g_hl_off : edited(*t);
    show(*t, g_blink_on && g_hl.base ? g_hl : off);
    g_blink_at = Clock::now();
  }
  if (g_view_dirty) { publish(); g_view_dirty = false; }
}
}  // namespace recolor_test
