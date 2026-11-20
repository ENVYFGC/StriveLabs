#include "tex_upload.h"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <mutex>

namespace tex_upload {
namespace {

void (*g_log)(const std::string&) = nullptr;
void (*g_quiet)(bool) = nullptr;
void log(const std::string& s) {
  if (g_log) g_log(s);
}

enum class St { Watch, Ready, Bad };

struct Entry {
  St st = St::Watch;
  int w = 0, h = 0;
  Sample sample;
  int tries = 0;
  ID3D11Texture2D* d3d = nullptr;
  std::shared_ptr<const std::vector<uint8_t>> next;
  bool lost = false;
  int span = 0;
  void* partner = nullptr;
};

std::mutex g_m;
std::map<void*, Entry> g_tex;
std::atomic<bool> g_active{false};
int g_off[3] = {-1, -1, -1};
int g_back = -1;
int g_order = 0;
void* g_vt = nullptr;
bool g_vt_failed = false;
int g_put_count = 0;

constexpr int kObjSpan = 0x300, kResSpan = 0x100, kRhiSpan = 0x180, kMaxTries = 30, kFirstTry = 3, kLearnEvery = 10, kMaxLearns = 3;
int g_learns = 0;
int g_since = kLearnEvery - 1;

int readWord(uintptr_t p, uintptr_t* out) {
  int ok = 0;
  if (g_quiet) g_quiet(true);
  __try {
    *out = *reinterpret_cast<const volatile uintptr_t*>(p);
    ok = 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = 0;
  }
  if (g_quiet) g_quiet(false);
  return ok;
}

int descOf(ID3D11Texture2D* t, D3D11_TEXTURE2D_DESC* d) {
  int ok = 0;
  if (g_quiet) g_quiet(true);
  __try {
    t->GetDesc(d);
    ok = 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = 0;
  }
  if (g_quiet) g_quiet(false);
  return ok;
}

int deviceOf(ID3D11Texture2D* t, ID3D11Device** d) {
  int ok = 0;
  if (g_quiet) g_quiet(true);
  __try {
    t->GetDevice(d);
    ok = 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = 0;
  }
  if (g_quiet) g_quiet(false);
  return ok;
}

struct Reader {
  std::map<uintptr_t, std::pair<uintptr_t, bool>> regions;
  bool ok(uintptr_t p) {
    if (p < 0x10000 || p > 0x7FFFFFFEFFFFull || (p & 7)) return false;
    auto it = regions.upper_bound(p);
    if (it != regions.begin()) {
      --it;
      if (p + 8 <= it->second.first) return it->second.second;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) return false;
    const uintptr_t lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress), hi = lo + mbi.RegionSize;
    const DWORD pr = mbi.Protect & 0xFF;
    const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && pr != PAGE_NOACCESS && pr != PAGE_EXECUTE && pr != 0;
    regions[lo] = {hi, readable};
    return readable && p + 8 <= hi;
  }
  uintptr_t ptr(uintptr_t p) {
    uintptr_t v = 0;
    if (!ok(p) || !readWord(p, &v)) return 0;
    return v;
  }
};

int orderOf(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      return 1;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      return 2;
    default:
      return 0;
  }
}

int fits(ID3D11Texture2D* t, ID3D11Device* dev, int w, int h) {
  D3D11_TEXTURE2D_DESC d{};
  if (!descOf(t, &d)) return 0;
  if (static_cast<int>(d.Width) != w || static_cast<int>(d.Height) != h || d.MipLevels != 1 || d.ArraySize != 1 ||
      d.SampleDesc.Count != 1 || d.Usage != D3D11_USAGE_DEFAULT)
    return 0;
  ID3D11Device* owner = nullptr;
  if (!deviceOf(t, &owner) || !owner) return 0;
  const bool same = owner == dev;
  owner->Release();
  return same ? orderOf(d.Format) : 0;
}

int backAt(Reader& r, uintptr_t a, uintptr_t obj) {
  for (int o = 0; o < kResSpan; o += 8)
    if (r.ptr(a + static_cast<uintptr_t>(o)) == obj) return o;
  return -1;
}

ID3D11Texture2D* resolve(Reader& r, uintptr_t obj, const int off[3], int back) {
  if (off[0] < 0) return nullptr;
  const uintptr_t a = r.ptr(obj + static_cast<uintptr_t>(off[0]));
  if (!a || a == obj) return nullptr;
  if (back >= 0 ? r.ptr(a + static_cast<uintptr_t>(back)) != obj : backAt(r, a, obj) < 0) return nullptr;
  const uintptr_t b = r.ptr(a + static_cast<uintptr_t>(off[1]));
  if (!b) return nullptr;
  const uintptr_t c = r.ptr(b + static_cast<uintptr_t>(off[2]));
  if (!c || r.ptr(c) != reinterpret_cast<uintptr_t>(g_vt)) return nullptr;
  return reinterpret_cast<ID3D11Texture2D*>(c);
}

bool sampleMatches(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* t, const Sample& s) {
  if (s.w <= 0 || s.h <= 0 || s.rgba.size() != static_cast<size_t>(s.w) * s.h * 4) return false;
  D3D11_TEXTURE2D_DESC d{};
  if (!descOf(t, &d)) return false;
  const int ord = orderOf(d.Format);
  if (!ord || s.x < 0 || s.y < 0 || s.x + s.w > static_cast<int>(d.Width) || s.y + s.h > static_cast<int>(d.Height)) return false;
  D3D11_TEXTURE2D_DESC sd{};
  sd.Width = static_cast<UINT>(s.w);
  sd.Height = static_cast<UINT>(s.h);
  sd.MipLevels = 1;
  sd.ArraySize = 1;
  sd.Format = d.Format;
  sd.SampleDesc.Count = 1;
  sd.Usage = D3D11_USAGE_STAGING;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* st = nullptr;
  if (FAILED(dev->CreateTexture2D(&sd, nullptr, &st)) || !st) return false;
  const D3D11_BOX box{static_cast<UINT>(s.x), static_cast<UINT>(s.y), 0, static_cast<UINT>(s.x + s.w), static_cast<UINT>(s.y + s.h), 1};
  ctx->CopySubresourceRegion(st, 0, 0, 0, 0, t, 0, &box);
  D3D11_MAPPED_SUBRESOURCE m{};
  bool same = false;
  if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m)) && m.pData) {
    same = true;
    for (int y = 0; same && y < s.h; y++) {
      const uint8_t* row = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
      for (int x = 0; same && x < s.w; x++) {
        const uint8_t* want = &s.rgba[(static_cast<size_t>(y) * s.w + x) * 4];
        const uint8_t* got = row + static_cast<size_t>(x) * 4;
        const uint8_t r = ord == 1 ? got[2] : got[0], b = ord == 1 ? got[0] : got[2];
        same = r == want[0] && got[1] == want[1] && b == want[2] && got[3] == want[3];
      }
    }
    ctx->Unmap(st, 0);
  }
  st->Release();
  return same;
}

std::vector<std::array<int, 3>> shapes(Reader& r, uintptr_t obj, int span) {
  std::vector<std::array<int, 3>> out;
  for (int o0 = 0; o0 + 8 <= span; o0 += 8) {
    const uintptr_t a = r.ptr(obj + static_cast<uintptr_t>(o0));
    if (!a || a == obj || backAt(r, a, obj) < 0) continue;
    for (int o1 = 0; o1 < kResSpan; o1 += 8) {
      const uintptr_t b = r.ptr(a + static_cast<uintptr_t>(o1));
      if (!b) continue;
      for (int o2 = 0; o2 < kRhiSpan; o2 += 8) {
        const uintptr_t c = r.ptr(b + static_cast<uintptr_t>(o2));
        if (c && r.ptr(c) == reinterpret_cast<uintptr_t>(g_vt)) out.push_back({o0, o1, o2});
      }
    }
  }
  return out;
}

int learnFrom(Reader& r, ID3D11Device* dev, ID3D11DeviceContext* ctx) {
  std::vector<std::pair<void*, Entry*>> ws;
  for (auto& [o, e] : g_tex)
    if (e.st == St::Watch && e.tries >= kFirstTry) ws.push_back({o, &e});
  if (ws.size() < 2) return 0;
  std::map<std::array<int, 3>, int> seen;
  std::vector<std::array<int, 3>> order;
  for (const auto& [o, e] : ws)
    for (const auto& sh : shapes(r, reinterpret_cast<uintptr_t>(o), e->span))
      if (seen[sh]++ == 0) order.push_back(sh);
  for (const auto& sh : order) {
    if (seen[sh] < 2) continue;
    std::vector<ID3D11Texture2D*> got;
    int back = -1;
    bool bad = false;
    for (const auto& [o, e] : ws) {
      ID3D11Texture2D* t = resolve(r, reinterpret_cast<uintptr_t>(o), sh.data(), -1);
      if (!t) continue;
      const uintptr_t a = r.ptr(reinterpret_cast<uintptr_t>(o) + static_cast<uintptr_t>(sh[0]));
      const int at = backAt(r, a, reinterpret_cast<uintptr_t>(o));
      if (at < 0 || (back >= 0 && at != back) || !fits(t, dev, e->w, e->h) || std::find(got.begin(), got.end(), t) != got.end() ||
          !sampleMatches(dev, ctx, t, e->sample)) {
        bad = true;
        break;
      }
      back = at;
      got.push_back(t);
    }
    if (!bad && got.size() >= 2) {
      std::copy(sh.begin(), sh.end(), g_off);
      g_back = back;
      return 1;
    }
  }
  return -1;
}

void giveUp(Entry& e) {
  e.st = St::Bad;
  log("color editor: a texture didn't check out for in-place updates, that part makes new textures as before");
}

void checkWatch(Reader& r, ID3D11Device* dev, ID3D11DeviceContext* ctx, void* obj, Entry& e) {
  ID3D11Texture2D* t = resolve(r, reinterpret_cast<uintptr_t>(obj), g_off, g_back);
  if (t) {
    for (const auto& [o, x] : g_tex)
      if (o != obj && x.d3d == t) t = nullptr;
  }
  if (!t) {
    if (e.tries >= kMaxTries) giveUp(e);
    return;
  }
  const int ord = fits(t, dev, e.w, e.h);
  if (!ord) {
    if (e.tries >= kMaxTries) giveUp(e);
    return;
  }
  if (!sampleMatches(dev, ctx, t, e.sample)) {
    giveUp(e);
    return;
  }
  t->AddRef();
  e.d3d = t;
  e.st = St::Ready;
  e.sample = Sample{};
  g_order = ord;
}

bool learnStep(Reader& r, ID3D11Device* dev, ID3D11DeviceContext* ctx) {
  bool due = false;
  for (auto& [o, e] : g_tex)
    if (e.st == St::Watch) {
      e.tries++;
      due |= e.tries >= kFirstTry;
    }
  if (!due || g_learns >= kMaxLearns || ++g_since < kLearnEvery) return true;
  const int got = learnFrom(r, dev, ctx);
  if (got == 0) return true;
  g_since = 0;
  if (got > 0) {
    log("color editor: in-place texture updates ready (texture found at +" + std::to_string(g_off[0]) + " +" + std::to_string(g_off[1]) + " +" +
        std::to_string(g_off[2]) + ", owner at +" + std::to_string(g_back) + ")");
    return true;
  }
  if (++g_learns >= kMaxLearns) {
    log("color editor: in-place texture updates not available (the game's texture wasn't found), edits make new textures as before");
    for (auto& [o, e] : g_tex)
      if (e.st == St::Watch) e.st = St::Bad;
  }
  return true;
}

}  // namespace

void setLog(void (*fn)(const std::string&)) { g_log = fn; }
void setQuiet(void (*fn)(bool)) { g_quiet = fn; }
void setActive(bool on) { g_active.store(on, std::memory_order_relaxed); }

void watch(void* tex, int w, int h, Sample s, int span) {
  if (!tex || w <= 0 || h <= 0 || span < 8) return;
  std::lock_guard<std::mutex> lk(g_m);
  Entry& e = g_tex[tex];
  if (e.d3d) e.d3d->Release();
  e = Entry{};
  e.w = w;
  e.h = h;
  e.sample = std::move(s);
  e.span = (std::min)(span, kObjSpan);
}

bool ready(void* tex) {
  std::lock_guard<std::mutex> lk(g_m);
  const auto it = g_tex.find(tex);
  return it != g_tex.end() && it->second.st == St::Ready && g_order != 0;
}

int order() {
  std::lock_guard<std::mutex> lk(g_m);
  return g_order;
}

bool put(void* tex, int w, int h, std::shared_ptr<const std::vector<uint8_t>> px) {
  std::lock_guard<std::mutex> lk(g_m);
  const auto it = g_tex.find(tex);
  if (it == g_tex.end() || it->second.st != St::Ready) return false;
  Entry& e = it->second;
  if (e.w != w || e.h != h || !px || px->size() != static_cast<size_t>(w) * h * 4) {
    e.st = St::Bad;
    return false;
  }
  e.next = std::move(px);
  e.partner = nullptr;
  return true;
}

bool putPair(void* a, void* b, int w, int h, std::shared_ptr<const std::vector<uint8_t>> pa, std::shared_ptr<const std::vector<uint8_t>> pb) {
  std::lock_guard<std::mutex> lk(g_m);
  const auto ia = g_tex.find(a), ib = g_tex.find(b);
  if (ia == g_tex.end() || ib == g_tex.end() || ia == ib || ia->second.st != St::Ready || ib->second.st != St::Ready) return false;
  const size_t n = static_cast<size_t>(w) * h * 4;
  if (ia->second.w != w || ia->second.h != h || ib->second.w != w || ib->second.h != h) {
    ia->second.st = St::Bad;
    ib->second.st = St::Bad;
    return false;
  }
  if (!pa || !pb || pa->size() != n || pb->size() != n) return false;
  ia->second.next = std::move(pa);
  ib->second.next = std::move(pb);
  ia->second.partner = b;
  ib->second.partner = a;
  return true;
}

bool takeLost(void* tex) {
  std::lock_guard<std::mutex> lk(g_m);
  const auto it = g_tex.find(tex);
  if (it == g_tex.end() || !it->second.lost) return false;
  it->second.lost = false;
  return true;
}

void forget(void* tex) {
  if (!tex) return;
  std::lock_guard<std::mutex> lk(g_m);
  const auto it = g_tex.find(tex);
  if (it == g_tex.end()) return;
  if (it->second.d3d) it->second.d3d->Release();
  g_tex.erase(it);
}

void frame(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
  if (!dev || !ctx || !g_active.load(std::memory_order_relaxed)) return;
  std::lock_guard<std::mutex> lk(g_m);
  if (g_tex.empty()) return;
  if (!g_vt) {
    if (g_vt_failed) return;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = d.Height = 1;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* probe = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &probe)) || !probe) {
      g_vt_failed = true;
      log("color editor: in-place texture updates not available (no test texture), edits make new textures as before");
      return;
    }
    g_vt = *reinterpret_cast<void**>(probe);
    probe->Release();
  }
  Reader r;
  const bool was_learned = g_off[0] >= 0;
  if (!was_learned) {
    if (g_learns >= kMaxLearns) {
      for (auto& [o, e] : g_tex)
        if (e.st == St::Watch) e.st = St::Bad;
    } else {
      learnStep(r, dev, ctx);
    }
  }
  std::map<void*, bool> still;
  for (auto& [obj, e] : g_tex)
    if (e.st == St::Ready && e.next)
      still[obj] = resolve(r, reinterpret_cast<uintptr_t>(obj), g_off, g_back) == e.d3d && fits(e.d3d, dev, e.w, e.h) != 0;
  bool said_lost = false;
  for (auto& [obj, e] : g_tex) {
    if (e.st == St::Watch) {
      if (g_off[0] >= 0) {
        if (was_learned) e.tries++;
        if (e.tries >= kFirstTry) checkWatch(r, dev, ctx, obj, e);
      }
      continue;
    }
    if (e.st != St::Ready || !e.next) continue;
    const auto px = std::move(e.next);
    e.next.reset();
    const auto mate = e.partner ? still.find(e.partner) : still.end();
    const bool mate_ok = !e.partner || (mate != still.end() && mate->second);
    if (!still[obj] || !mate_ok) {
      e.st = St::Bad;
      e.lost = true;
      if (!said_lost) log("color editor: a texture changed under an in-place update, that part makes new textures as before");
      said_lost = true;
      continue;
    }
    ctx->UpdateSubresource(e.d3d, 0, nullptr, px->data(), static_cast<UINT>(e.w) * 4, static_cast<UINT>(e.w) * 4 * static_cast<UINT>(e.h));
    if (++g_put_count == 1) log("color editor: first in-place texture update done");
  }
}

}  // namespace tex_upload
