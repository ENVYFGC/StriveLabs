#include "bake_core.h"

#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <zlib.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#pragma comment(lib, "bcrypt")

namespace bake {
  void decodeDxt5(const uint8_t in[16], uint8_t rgba[64]);
  namespace {
    int32_t i32(const uint8_t* p) { int32_t v; std::memcpy(&v, p, 4); return v; }
    uint32_t u32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
    int64_t i64(const uint8_t* p) { int64_t v; std::memcpy(&v, p, 8); return v; }

    // ---- DXT5 ----
    struct C3 { float r, g, b; };
    uint16_t pack565(C3 c) {
      const int r = std::clamp(static_cast<int>(std::lround(c.r * 31.0f / 255.0f)), 0, 31);
      const int g = std::clamp(static_cast<int>(std::lround(c.g * 63.0f / 255.0f)), 0, 63);
      const int b = std::clamp(static_cast<int>(std::lround(c.b * 31.0f / 255.0f)), 0, 31);
      return static_cast<uint16_t>((r << 11) | (g << 5) | b);
    }
    C3 unpack565(uint16_t c) {
      const int r = c >> 11, g = (c >> 5) & 63, b = c & 31;
      return {static_cast<float>((r << 3) | (r >> 2)), static_cast<float>((g << 2) | (g >> 4)), static_cast<float>((b << 3) | (b >> 2))};
    }
    void palette(uint16_t c0, uint16_t c1, C3 pal[4]) {  // (4-colour, as BC3 always is)
      pal[0] = unpack565(c0);
      pal[1] = unpack565(c1);
      pal[2] = {(2 * pal[0].r + pal[1].r) / 3, (2 * pal[0].g + pal[1].g) / 3, (2 * pal[0].b + pal[1].b) / 3};
      pal[3] = {(pal[0].r + 2 * pal[1].r) / 3, (pal[0].g + 2 * pal[1].g) / 3, (pal[0].b + 2 * pal[1].b) / 3};
    }
    float dist(C3 a, C3 b) { const float r = a.r - b.r, g = a.g - b.g, bl = a.b - b.b; return r * r + g * g + bl * bl; }
    // selectors for the 16 pixels, and the total error
    float assign(const C3 px[16], uint16_t c0, uint16_t c1, uint8_t sel[16]) {
      C3 pal[4];
      palette(c0, c1, pal);
      float err = 0;
      for (int i = 0; i < 16; i++) {
        int best = 0;
        float bd = dist(px[i], pal[0]);
        for (int k = 1; k < 4; k++) { const float d = dist(px[i], pal[k]); if (d < bd) { bd = d; best = k; } }
        sel[i] = static_cast<uint8_t>(best);
        err += bd;
      }
      return err;
    }
    // A flat colour through the 2/3 entry (finer than 565 alone): per channel value, the two ends that give it best,
    // as decodeBlock rounds; built once
    struct Ends { uint8_t e0, e1, err; };
    const Ends* singleTable(int bits) {
      static Ends t5[256], t6[256];
      static bool built = false;
      if (!built) {
        for (int pass = 0; pass < 2; pass++) {
          Ends* t = pass ? t6 : t5;
          const int n = pass ? 64 : 32;
          auto ex = [&](int e) { return pass ? (e << 2) | (e >> 4) : (e << 3) | (e >> 2); };
          for (int v = 0; v < 256; v++) {
            Ends best{0, 0, 255};
            for (int e0 = 0; e0 < n; e0++)
              for (int e1 = 0; e1 < n; e1++) {
                const int got = static_cast<int>(static_cast<float>(2 * ex(e0) + ex(e1)) / 3);
                const int d = std::abs(got - v);
                if (d < best.err) best = {static_cast<uint8_t>(e0), static_cast<uint8_t>(e1), static_cast<uint8_t>(d)};
              }
            t[v] = best;
          }
        }
        built = true;
      }
      return bits == 6 ? t6 : t5;
    }

    void encodeColor(const uint8_t* rgba, uint8_t out[8]) {
      C3 px[16];
      C3 mean{0, 0, 0};
      for (int i = 0; i < 16; i++) {
        px[i] = {static_cast<float>(rgba[4 * i]), static_cast<float>(rgba[4 * i + 1]), static_cast<float>(rgba[4 * i + 2])};
        mean.r += px[i].r / 16; mean.g += px[i].g / 16; mean.b += px[i].b / 16;
      }
      // principal axis of the colours (power iteration on the covariance)
      float cov[6] = {};
      for (const C3& p : px) {
        const float r = p.r - mean.r, g = p.g - mean.g, b = p.b - mean.b;
        cov[0] += r * r; cov[1] += r * g; cov[2] += r * b; cov[3] += g * g; cov[4] += g * b; cov[5] += b * b;
      }
      C3 axis{1, 1, 1};
      for (int it = 0; it < 8; it++) {
        C3 n{cov[0] * axis.r + cov[1] * axis.g + cov[2] * axis.b, cov[1] * axis.r + cov[3] * axis.g + cov[4] * axis.b,
             cov[2] * axis.r + cov[4] * axis.g + cov[5] * axis.b};
        const float len = std::sqrt(n.r * n.r + n.g * n.g + n.b * n.b);
        if (len < 1e-6f) break;
        axis = {n.r / len, n.g / len, n.b / len};
      }
      float lo = 1e9f, hi = -1e9f;
      for (const C3& p : px) {
        const float t = (p.r - mean.r) * axis.r + (p.g - mean.g) * axis.g + (p.b - mean.b) * axis.b;
        lo = std::min(lo, t); hi = std::max(hi, t);
      }
      auto at = [&](float t) { return C3{mean.r + axis.r * t, mean.g + axis.g * t, mean.b + axis.b * t}; };
      uint16_t c0 = pack565(at(hi)), c1 = pack565(at(lo));
      uint8_t sel[16];
      float err = assign(px, c0, c1, sel);
      {  // the block's own colours as endpoints (blocks that were DXT already come back exact this way): every pair
        // of its distinct 565 colours when there are few, else the per-channel box corners
        C3 q[16];
        int nq = 0;
        for (const C3& p : px) {
          bool seen = false;
          for (int k = 0; k < nq; k++) seen |= q[k].r == p.r && q[k].g == p.g && q[k].b == p.b;
          if (!seen) q[nq++] = p;
        }
        auto tryPair = [&](uint16_t a, uint16_t b) {
          if (a == c0 && b == c1) return;
          uint8_t ns[16];
          const float e = assign(px, a, b, ns);
          if (e < err) { err = e; c0 = a; c1 = b; std::memcpy(sel, ns, 16); }
        };
        auto mix = [](C3 a, C3 b, float ka, float kb) { return C3{ka * a.r + kb * b.r, ka * a.g + kb * b.g, ka * a.b + kb * b.b}; };
        if (nq == 1) {  // (one colour: straight, or through the 2/3 entry)
          const Ends r = singleTable(5)[rgba[0]], g = singleTable(6)[rgba[1]], bl = singleTable(5)[rgba[2]];
          const uint16_t a = static_cast<uint16_t>((r.e0 << 11) | (g.e0 << 5) | bl.e0), b = static_cast<uint16_t>((r.e1 << 11) | (g.e1 << 5) | bl.e1);
          tryPair(a, b);
        } else if (nq <= 8) {
          // a pair of its colours can be: both ends; the two middle entries (ends 2a-b, 2b-a); an end and its near
          // middle entry (other end 3b-2a); an end and its far middle entry (other end (3b-a)/2)
          for (int i = 0; i < nq; i++)
            for (int j = 0; j < nq; j++) {
              if (i == j) continue;
              const C3 a = q[i], b = q[j];
              if (i < j) tryPair(pack565(a), pack565(b));
              if (i < j) tryPair(pack565(mix(a, b, 2, -1)), pack565(mix(b, a, 2, -1)));
              tryPair(pack565(a), pack565(mix(b, a, 3, -2)));
              tryPair(pack565(a), pack565(mix(b, a, 1.5f, -0.5f)));
            }
        } else {
          C3 mn{255, 255, 255}, mx{0, 0, 0};
          for (const C3& p : px) {
            mn = {std::min(mn.r, p.r), std::min(mn.g, p.g), std::min(mn.b, p.b)};
            mx = {std::max(mx.r, p.r), std::max(mx.g, p.g), std::max(mx.b, p.b)};
          }
          tryPair(pack565(mx), pack565(mn));
        }
      }
      // refine: least-squares endpoints for the current selectors, twice
      static const float w0[4] = {1.0f, 0.0f, 2.0f / 3, 1.0f / 3};
      for (int it = 0; it < 2 && err > 0; it++) {
        float aa = 0, ab = 0, bb = 0;
        C3 ax{0, 0, 0}, bx{0, 0, 0};
        for (int i = 0; i < 16; i++) {
          const float a = w0[sel[i]], b = 1 - a;
          aa += a * a; ab += a * b; bb += b * b;
          ax.r += a * px[i].r; ax.g += a * px[i].g; ax.b += a * px[i].b;
          bx.r += b * px[i].r; bx.g += b * px[i].g; bx.b += b * px[i].b;
        }
        const float det = aa * bb - ab * ab;
        if (std::fabs(det) < 1e-6f) break;
        const C3 e0{(ax.r * bb - bx.r * ab) / det, (ax.g * bb - bx.g * ab) / det, (ax.b * bb - bx.b * ab) / det};
        const C3 e1{(bx.r * aa - ax.r * ab) / det, (bx.g * aa - ax.g * ab) / det, (bx.b * aa - ax.b * ab) / det};
        const uint16_t n0 = pack565(e0), n1 = pack565(e1);
        uint8_t ns[16];
        const float ne = assign(px, n0, n1, ns);
        if (ne >= err) break;
        c0 = n0; c1 = n1; err = ne;
        std::memcpy(sel, ns, 16);
      }
      // c0 > c1 (as decoders expect), the selectors following; one colour: all selector 0
      if (c0 < c1) {
        std::swap(c0, c1);
        static const uint8_t flip[4] = {1, 0, 3, 2};
        for (uint8_t& s : sel) s = flip[s];
      }
      if (c0 == c1) std::memset(sel, 0, 16);
      std::memcpy(out, &c0, 2);
      std::memcpy(out + 2, &c1, 2);
      uint32_t bits = 0;
      for (int i = 0; i < 16; i++) bits |= static_cast<uint32_t>(sel[i]) << (2 * i);
      std::memcpy(out + 4, &bits, 4);
    }
    void encodeAlpha(const uint8_t* rgba, uint8_t out[8]) {
      int lo = 255, hi = 0;
      for (int i = 0; i < 16; i++) { lo = std::min<int>(lo, rgba[4 * i + 3]); hi = std::max<int>(hi, rgba[4 * i + 3]); }
      out[0] = static_cast<uint8_t>(hi);
      out[1] = static_cast<uint8_t>(lo);
      uint64_t bits = 0;
      if (hi != lo) {  // (8-value mode: a0 > a1)
        int pal[8] = {hi, lo};
        for (int k = 2; k < 8; k++) pal[k] = ((8 - k) * hi + (k - 1) * lo) / 7;
        for (int i = 0; i < 16; i++) {
          const int a = rgba[4 * i + 3];
          int best = 0, bd = 1 << 30;
          for (int k = 0; k < 8; k++) { const int d = std::abs(pal[k] - a); if (d < bd) { bd = d; best = k; } }
          bits |= static_cast<uint64_t>(best) << (3 * i);
        }
      }
      for (int k = 0; k < 6; k++) out[2 + k] = static_cast<uint8_t>(bits >> (8 * k));
    }
  }

  namespace {
    // DXT1 with transparent pixels: 3-colour mode (c0 <= c1), index 3 transparent
    void encodeDxt1Punch(const uint8_t* rgba, uint8_t out[8]) {
      int n = 0;
      C3 mn{255, 255, 255}, mx{0, 0, 0};
      for (int i = 0; i < 16; i++)
        if (rgba[4 * i + 3] >= 128) {
          const C3 p{static_cast<float>(rgba[4 * i]), static_cast<float>(rgba[4 * i + 1]), static_cast<float>(rgba[4 * i + 2])};
          mn = {std::min(mn.r, p.r), std::min(mn.g, p.g), std::min(mn.b, p.b)};
          mx = {std::max(mx.r, p.r), std::max(mx.g, p.g), std::max(mx.b, p.b)};
          n++;
        }
      uint16_t c0 = n ? pack565(mn) : 0, c1 = n ? pack565(mx) : 0;
      if (c0 > c1) std::swap(c0, c1);  // (c0 <= c1: 3-colour mode)
      C3 pal[3] = {unpack565(c0), unpack565(c1), {}};
      pal[2] = {(pal[0].r + pal[1].r) / 2, (pal[0].g + pal[1].g) / 2, (pal[0].b + pal[1].b) / 2};
      uint32_t bits = 0;
      for (int i = 0; i < 16; i++) {
        int k = 3;
        if (rgba[4 * i + 3] >= 128) {
          const C3 p{static_cast<float>(rgba[4 * i]), static_cast<float>(rgba[4 * i + 1]), static_cast<float>(rgba[4 * i + 2])};
          k = 0;
          for (int j = 1; j < 3; j++) if (dist(p, pal[j]) < dist(p, pal[k])) k = j;
        }
        bits |= static_cast<uint32_t>(k) << (2 * i);
      }
      std::memcpy(out, &c0, 2);
      std::memcpy(out + 2, &c1, 2);
      std::memcpy(out + 4, &bits, 4);
    }

    // ---- BC7, mode 6: 7 mode bits (0000001), RGBA endpoints 7 bits each (R0 R1 G0 G1 B0 B1 A0 A1), a p-bit per
    // endpoint, 16 four-bit indices (the first one 3 bits: its top bit is 0) ----
    const int kW4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
    int lerp6(int a, int b, int w) { return ((64 - w) * a + w * b + 32) >> 6; }
    struct Bits {
      uint8_t* p;
      int at = 0;
      void put(uint32_t v, int n) { for (int i = 0; i < n; i++, at++) if ((v >> i) & 1) p[at >> 3] |= static_cast<uint8_t>(1 << (at & 7)); }
    };
    struct RBits {
      const uint8_t* p;
      int at = 0;
      uint32_t get(int n) { uint32_t v = 0; for (int i = 0; i < n; i++, at++) v |= static_cast<uint32_t>((p[at >> 3] >> (at & 7)) & 1) << i; return v; }
    };
    float bc7Assign(const uint8_t* rgba, const int e0[4], const int e1[4], uint8_t idx[16]) {
      int pal[16][4];
      for (int k = 0; k < 16; k++) for (int c = 0; c < 4; c++) pal[k][c] = lerp6(e0[c], e1[c], kW4[k]);
      float err = 0;
      for (int i = 0; i < 16; i++) {
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < 16; k++) {
          int d = 0;
          for (int c = 0; c < 4; c++) { const int x = pal[k][c] - rgba[4 * i + c]; d += x * x; }
          if (d < bd) { bd = d; best = k; }
        }
        idx[i] = static_cast<uint8_t>(best);
        err += static_cast<float>(bd);
      }
      return err;
    }
    // an 8-bit endpoint as 7 bits + a p-bit shared by its four channels: the p-bit that fits best
    void bc7Quant(const float e[4], int q7[4], int& p, int out[4]) {
      float best = 1e30f;
      for (int pb = 0; pb < 2; pb++) {
        int t7[4], tv[4];
        float err = 0;
        for (int c = 0; c < 4; c++) {
          t7[c] = std::clamp(static_cast<int>(std::lround((e[c] - pb) / 2.0f)), 0, 127);
          tv[c] = (t7[c] << 1) | pb;
          err += (tv[c] - e[c]) * (tv[c] - e[c]);
        }
        if (err < best) { best = err; p = pb; std::memcpy(q7, t7, sizeof t7); std::memcpy(out, tv, sizeof tv); }
      }
    }
    void encodeBc7(const uint8_t* rgba, uint8_t out[16]) {
      // ends along the principal axis (RGBA), then least-squares passes
      float mean[4] = {};
      for (int i = 0; i < 16; i++) for (int c = 0; c < 4; c++) mean[c] += rgba[4 * i + c] / 16.0f;
      float cov[4][4] = {};
      for (int i = 0; i < 16; i++)
        for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) cov[a][b] += (rgba[4 * i + a] - mean[a]) * (rgba[4 * i + b] - mean[b]);
      float ax[4] = {1, 1, 1, 1};
      for (int it = 0; it < 8; it++) {
        float n[4] = {};
        for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) n[a] += cov[a][b] * ax[b];
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2] + n[3] * n[3]);
        if (len < 1e-6f) break;
        for (int a = 0; a < 4; a++) ax[a] = n[a] / len;
      }
      float lo = 1e9f, hi = -1e9f;
      for (int i = 0; i < 16; i++) {
        float t = 0;
        for (int c = 0; c < 4; c++) t += (rgba[4 * i + c] - mean[c]) * ax[c];
        lo = std::min(lo, t);
        hi = std::max(hi, t);
      }
      float f0[4], f1[4];
      for (int c = 0; c < 4; c++) { f0[c] = std::clamp(mean[c] + ax[c] * lo, 0.0f, 255.0f); f1[c] = std::clamp(mean[c] + ax[c] * hi, 0.0f, 255.0f); }
      int q0[4], q1[4], v0[4], v1[4], p0 = 0, p1 = 0;
      uint8_t idx[16];
      bc7Quant(f0, q0, p0, v0);
      bc7Quant(f1, q1, p1, v1);
      float err = bc7Assign(rgba, v0, v1, idx);
      for (int it = 0; it < 3 && err > 0; it++) {
        float aa = 0, ab = 0, bb = 0, xa[4] = {}, xb[4] = {};
        for (int i = 0; i < 16; i++) {
          const float w = kW4[idx[i]] / 64.0f, a = 1 - w, b = w;
          aa += a * a; ab += a * b; bb += b * b;
          for (int c = 0; c < 4; c++) { xa[c] += a * rgba[4 * i + c]; xb[c] += b * rgba[4 * i + c]; }
        }
        const float det = aa * bb - ab * ab;
        if (std::fabs(det) < 1e-6f) break;
        float g0[4], g1[4];
        for (int c = 0; c < 4; c++) {
          g0[c] = std::clamp((xa[c] * bb - xb[c] * ab) / det, 0.0f, 255.0f);
          g1[c] = std::clamp((xb[c] * aa - xa[c] * ab) / det, 0.0f, 255.0f);
        }
        int r0[4], r1[4], w0[4], w1[4], s0 = 0, s1 = 0;
        uint8_t ni[16];
        bc7Quant(g0, r0, s0, w0);
        bc7Quant(g1, r1, s1, w1);
        const float ne = bc7Assign(rgba, w0, w1, ni);
        if (ne >= err) break;
        err = ne;
        std::memcpy(q0, r0, sizeof q0); std::memcpy(q1, r1, sizeof q1); std::memcpy(v0, w0, sizeof v0); std::memcpy(v1, w1, sizeof v1);
        p0 = s0; p1 = s1;
        std::memcpy(idx, ni, 16);
      }
      if (idx[0] & 8) {  // (the first index must have its top bit clear: swap the ends)
        std::swap(q0, q1);
        std::swap(p0, p1);
        for (uint8_t& k : idx) k = static_cast<uint8_t>(15 - k);
      }
      std::memset(out, 0, 16);
      Bits w{out};
      w.put(1u << 6, 7);
      for (int c = 0; c < 4; c++) { w.put(static_cast<uint32_t>(q0[c]), 7); w.put(static_cast<uint32_t>(q1[c]), 7); }
      w.put(static_cast<uint32_t>(p0), 1);
      w.put(static_cast<uint32_t>(p1), 1);
      w.put(idx[0], 3);
      for (int i = 1; i < 16; i++) w.put(idx[i], 4);
    }
    bool decodeBc7Mode6(const uint8_t* in, uint8_t rgba[64]) {
      RBits r{in};
      if (r.get(7) != (1u << 6)) return false;
      int e[2][4];
      for (int c = 0; c < 4; c++) { e[0][c] = static_cast<int>(r.get(7)); e[1][c] = static_cast<int>(r.get(7)); }
      const int p0 = static_cast<int>(r.get(1)), p1 = static_cast<int>(r.get(1));
      for (int c = 0; c < 4; c++) { e[0][c] = (e[0][c] << 1) | p0; e[1][c] = (e[1][c] << 1) | p1; }
      for (int i = 0; i < 16; i++) {
        const int k = static_cast<int>(r.get(i == 0 ? 3 : 4));
        for (int c = 0; c < 4; c++) rgba[4 * i + c] = static_cast<uint8_t>(lerp6(e[0][c], e[1][c], kW4[k]));
      }
      return true;
    }
  }

  int blockBytes(Format f) { return f == Format::DXT1 ? 8 : 16; }

  void encodeBlock(Format f, const uint8_t rgba[64], uint8_t* out) {
    if (f == Format::BC7) { encodeBc7(rgba, out); return; }
    if (f == Format::DXT1) {
      bool punch = false;
      for (int i = 0; i < 16; i++) punch |= rgba[4 * i + 3] < 128;
      if (punch) encodeDxt1Punch(rgba, out);
      else encodeColor(rgba, out);
      return;
    }
    encodeAlpha(rgba, out);
    encodeColor(rgba, out + 8);
  }

  bool decodeBlock(Format f, const uint8_t* in, uint8_t rgba[64]) {
    if (f == Format::BC7) return decodeBc7Mode6(in, rgba);
    if (f == Format::DXT1) {
      uint16_t c0, c1;
      std::memcpy(&c0, in, 2);
      std::memcpy(&c1, in + 2, 2);
      C3 pal[4];
      palette(c0, c1, pal);
      if (c0 <= c1) pal[2] = {(pal[0].r + pal[1].r) / 2, (pal[0].g + pal[1].g) / 2, (pal[0].b + pal[1].b) / 2};
      const uint32_t bits = u32(in + 4);
      for (int i = 0; i < 16; i++) {
        const int k = (bits >> (2 * i)) & 3;
        const bool clear = c0 <= c1 && k == 3;
        rgba[4 * i] = clear ? 0 : static_cast<uint8_t>(pal[k].r);
        rgba[4 * i + 1] = clear ? 0 : static_cast<uint8_t>(pal[k].g);
        rgba[4 * i + 2] = clear ? 0 : static_cast<uint8_t>(pal[k].b);
        rgba[4 * i + 3] = clear ? 0 : 255;
      }
      return true;
    }
    decodeDxt5(in, rgba);
    return true;
  }

  void decodeDxt5(const uint8_t in[16], uint8_t rgba[64]) {
    const int a0 = in[0], a1 = in[1];
    int apal[8] = {a0, a1};
    if (a0 > a1) for (int k = 2; k < 8; k++) apal[k] = ((8 - k) * a0 + (k - 1) * a1) / 7;
    else { for (int k = 2; k < 6; k++) apal[k] = ((6 - k) * a0 + (k - 1) * a1) / 5; apal[6] = 0; apal[7] = 255; }
    uint64_t abits = 0;
    for (int k = 0; k < 6; k++) abits |= static_cast<uint64_t>(in[2 + k]) << (8 * k);
    uint16_t c0, c1;
    std::memcpy(&c0, in + 8, 2);
    std::memcpy(&c1, in + 10, 2);
    C3 pal[4];
    palette(c0, c1, pal);
    const uint32_t bits = u32(in + 12);
    for (int i = 0; i < 16; i++) {
      const C3& c = pal[(bits >> (2 * i)) & 3];
      rgba[4 * i] = static_cast<uint8_t>(c.r);
      rgba[4 * i + 1] = static_cast<uint8_t>(c.g);
      rgba[4 * i + 2] = static_cast<uint8_t>(c.b);
      rgba[4 * i + 3] = static_cast<uint8_t>(apal[(abits >> (3 * i)) & 7]);
    }
  }

  // ---- the texture ----
  bool Texture::parse(std::string& why) {
    mips.clear();
    {  // the pixel format's name, an FString in the platform data
      const char* names[3] = {"PF_DXT1", "PF_DXT5", "PF_BC7"};
      const Format fmts[3] = {Format::DXT1, Format::DXT5, Format::BC7};
      bool known = false;
      for (size_t at = 4; at + 12 < uexp.size() && !known; at++) {
        if (uexp[at] != 'P' || uexp[at + 1] != 'F' || uexp[at + 2] != '_') continue;
        const int32_t n = i32(uexp.data() + at - 4);
        const std::string name(reinterpret_cast<const char*>(uexp.data() + at), strnlen(reinterpret_cast<const char*>(uexp.data() + at), 32));
        if (n != static_cast<int32_t>(name.size() + 1)) continue;
        for (int k = 0; k < 3; k++) if (name == names[k]) { format = fmts[k]; known = true; }
        if (!known) { why = "pixel format " + name + " isn't handled"; return false; }
      }
      if (!known) { why = "no pixel format"; return false; }
    }
    const uint8_t* b = uexp.data();
    for (size_t at = 0; at + 24 <= uexp.size(); at++) {
      if (i32(b + at) != 1) continue;
      const uint32_t fl = u32(b + at + 4);
      if (fl != 0x48 && fl != 0x10501 && fl != 0x501) continue;  // (0x501: a mod's texture, its .ubulk offsets counted from past the header files)
      const int32_t n = i32(b + at + 8), sd = i32(b + at + 12);
      if (n <= 0 || n != sd) continue;
      Mip m;
      m.header = at;
      m.flags = fl;
      m.size = n;
      m.offset = i64(b + at + 16);
      m.inline_ = (fl & 0x40) != 0;
      const size_t dims = at + 24 + (m.inline_ ? n : 0);
      if (dims + 12 > uexp.size()) continue;
      m.w = i32(b + dims);
      m.h = i32(b + dims + 4);
      mips.push_back(m);
      at = dims + 11;  // (past this mip)
    }
    if (mips.empty()) { why = "no mips found"; return false; }
    {  // A mod tool's .ubulk mips (flag 0x501, no 0x10000) count their offsets from the header files' end; the game's
      // (0x10501) from 0. Theirs are moved to start at 0 and written the game's way: a bake renames the texture, which
      // changes the header files' size, so offsets from their end would point at the wrong pixels. The game's are left be.
      bool modMips = false;
      int64_t base = 0;
      for (const Mip& m : mips)
        if (!m.inline_ && m.flags == 0x501) { if (!modMips) base = m.offset; modMips = true; }
      if (modMips)
        for (Mip& m : mips)
          if (!m.inline_ && m.flags == 0x501) {
            m.offset -= base;
            m.flags = 0x10501;
            std::memcpy(uexp.data() + m.header + 4, &m.flags, 4);
            std::memcpy(uexp.data() + m.header + 16, &m.offset, 8);
          }
    }
    int64_t bulk = 0;
    for (size_t i = 0; i < mips.size(); i++) {
      const Mip& m = mips[i];
      const int w = std::max(1, mips[0].w >> i), h = std::max(1, mips[0].h >> i);
      if (m.size != ((w + 3) / 4) * ((h + 3) / 4) * blockBytes(format)) { why = "mip " + std::to_string(i) + " isn't its size in its format"; return false; }
      if (!m.inline_) {
        if (m.offset != bulk) { why = "the .ubulk mips aren't back to back"; return false; }
        bulk += m.size;
      }
    }
    if (bulk != static_cast<int64_t>(ubulk.size())) { why = "the .ubulk size doesn't match its mips"; return false; }
    return true;
  }

  uint8_t* Texture::mipData(size_t i) {
    const Mip& m = mips[i];
    return m.inline_ ? uexp.data() + m.header + 24 : ubulk.data() + m.offset;
  }

  size_t writeImage(Texture& t, const std::vector<uint8_t>& image, const std::vector<uint8_t>& was, std::string& why) {
    const int W = t.mips[0].w, H = t.mips[0].h;
    if (W % 4 || H % 4 || image.size() != static_cast<size_t>(W) * H * 4 || was.size() != image.size()) { why = "image size"; return 0; }
    const int BW = W / 4, BH = H / 4;
    std::vector<uint8_t> changed(static_cast<size_t>(BW) * BH);
    for (int by = 0; by < BH; by++)
      for (int bx = 0; bx < BW; bx++) {
        bool c = false;
        for (int y = 0; y < 4 && !c; y++) {
          const size_t row = (static_cast<size_t>(by * 4 + y) * W + bx * 4) * 4;
          c = std::memcmp(image.data() + row, was.data() + row, 16) != 0;
        }
        changed[static_cast<size_t>(by) * BW + bx] = c;
      }
    size_t encoded = 0;
    std::vector<uint8_t> level = image;
    int w = W, h = H;
    for (size_t i = 0; i < t.mips.size(); i++) {
      if (i > 0) {  // 2x2 box filter of the level above
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        std::vector<uint8_t> next(static_cast<size_t>(nw) * nh * 4);
        for (int y = 0; y < nh; y++)
          for (int x = 0; x < nw; x++)
            for (int c = 0; c < 4; c++) {
              int s = 0;
              for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                  const int sx = std::min(w - 1, 2 * x + dx), sy = std::min(h - 1, 2 * y + dy);
                  s += level[(static_cast<size_t>(sy) * w + sx) * 4 + c];
                }
              next[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<uint8_t>((s + 2) / 4);
            }
        level.swap(next);
        w = nw;
        h = nh;
      }
      const int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
      const int bb = blockBytes(t.format);
      if (static_cast<size_t>(bw) * bh * bb != static_cast<size_t>(t.mips[i].size)) { why = "mip " + std::to_string(i) + " size"; return 0; }
      const int span = 1 << i;  // (top-mip blocks per block side here)
      uint8_t* dst = t.mipData(i);
      for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
          bool c = false;
          for (int y = by * span; y < std::min(BH, (by + 1) * span) && !c; y++)
            for (int x = bx * span; x < std::min(BW, (bx + 1) * span) && !c; x++) c = changed[static_cast<size_t>(y) * BW + x];
          if (!c) continue;  // (the game's own bytes stay)
          uint8_t px[64];
          for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
              const int sx = std::min(w - 1, bx * 4 + x), sy = std::min(h - 1, by * 4 + y);
              std::memcpy(px + (y * 4 + x) * 4, level.data() + (static_cast<size_t>(sy) * w + sx) * 4, 4);
            }
          encodeBlock(t.format, px, dst + (static_cast<size_t>(by) * bw + bx) * bb);
          encoded++;
        }
    }
    return encoded;
  }

  // ---- the pak ----
  namespace {
    constexpr uint32_t kMagic = 0x5A6F12E1;
    bool sha1(const uint8_t* p, size_t n, uint8_t out[20]) {
      BCRYPT_ALG_HANDLE alg = nullptr;
      if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return false;
      const bool ok = BCryptHash(alg, nullptr, 0, const_cast<PUCHAR>(p), static_cast<ULONG>(n), out, 20) == 0;
      BCryptCloseAlgorithmProvider(alg, 0);
      return ok;
    }
    template <typename T> void put(Bytes& b, T v) { const auto* p = reinterpret_cast<const uint8_t*>(&v); b.insert(b.end(), p, p + sizeof v); }
    void putStr(Bytes& b, const std::string& s) { put<int32_t>(b, static_cast<int32_t>(s.size() + 1)); b.insert(b.end(), s.begin(), s.end()); b.push_back(0); }
    constexpr size_t kBlock = 65536;
    // A file as the pak stores it: zlib blocks of 64 KB (empty: stored as it is). blocks: each one's [start, end) from
    // the entry's own start, once the header's size is known
    struct Packed { Bytes data; std::vector<std::pair<int64_t, int64_t>> blocks; bool comp = false; uint8_t hash[20] = {}; };
    bool pack(const Bytes& in, Packed& p) {
      p = Packed{};
      if (in.empty()) { static const uint8_t none = 0; return sha1(&none, 0, p.hash); }
      p.comp = true;
      std::vector<size_t> sizes;
      for (size_t at = 0; at < in.size(); at += kBlock) {
        const size_t n = (std::min)(kBlock, in.size() - at);
        uLongf out = compressBound(static_cast<uLong>(n));
        const size_t was = p.data.size();
        p.data.resize(was + out);
        if (compress2(p.data.data() + was, &out, in.data() + at, static_cast<uLong>(n), 6) != Z_OK) return false;
        p.data.resize(was + out);
        sizes.push_back(out);
      }
      const int64_t hdr = 8 + 8 + 8 + 4 + 20 + 4 + static_cast<int64_t>(sizes.size()) * 16 + 1 + 4;
      int64_t at = hdr;
      for (size_t n : sizes) { p.blocks.push_back({at, at + static_cast<int64_t>(n)}); at += static_cast<int64_t>(n); }
      return sha1(p.data.data(), p.data.size(), p.hash);
    }
    // FPakEntry (v9): offset, stored size, size, compression (1: the footer's "Zlib"), sha1 of the stored bytes,
    // [the blocks], flags 0, block size
    void entryOf(Bytes& b, int64_t off, const Packed& p, size_t usize) {
      put<int64_t>(b, off);
      put<int64_t>(b, static_cast<int64_t>(p.comp ? p.data.size() : usize));
      put<int64_t>(b, static_cast<int64_t>(usize));
      put<uint32_t>(b, p.comp ? 1 : 0);
      b.insert(b.end(), p.hash, p.hash + 20);
      if (p.comp) {
        put<int32_t>(b, static_cast<int32_t>(p.blocks.size()));
        for (const auto& [s, e] : p.blocks) { put<int64_t>(b, s); put<int64_t>(b, e); }
      }
      put<uint8_t>(b, 0);
      put<uint32_t>(b, p.comp ? static_cast<uint32_t>((std::min)(kBlock, usize)) : 0);
    }
    // One entry of an index or a file's own header: its fields, and where it ends (false: it doesn't fit)
    struct Entry { int64_t off = 0, size = 0, usize = 0; uint32_t comp = 0; const uint8_t* hash = nullptr; std::vector<std::pair<int64_t, int64_t>> blocks; size_t end = 0; };
    bool readEntry(const uint8_t* p, size_t at, size_t len, Entry& e) {
      if (at + 48 > len) return false;
      e.off = i64(p + at); e.size = i64(p + at + 8); e.usize = i64(p + at + 16); e.comp = u32(p + at + 24); e.hash = p + at + 28;
      at += 48;
      e.blocks.clear();
      if (e.comp) {
        if (at + 4 > len) return false;
        const int32_t n = i32(p + at);
        at += 4;
        if (n < 0 || n > (1 << 20) || at + static_cast<size_t>(n) * 16 > len) return false;
        for (int32_t k = 0; k < n; k++) e.blocks.push_back({i64(p + at + k * 16), i64(p + at + k * 16 + 8)});
        at += static_cast<size_t>(n) * 16;
      }
      if (at + 5 > len) return false;
      e.end = at + 5;
      return true;
    }
    bool readAll(const std::wstring& path, Bytes& out) {
      std::ifstream f(path, std::ios::binary);
      if (!f) return false;
      out.assign(std::istreambuf_iterator<char>(f), {});
      return true;
    }
  }

  std::string sha1hex(const Bytes& d) {
    uint8_t h[20] = {};
    sha1(d.data(), d.size(), h);
    char out[41];
    for (int i = 0; i < 20; i++) snprintf(out + 2 * i, 3, "%02x", h[i]);
    return out;
  }

  bool writePak(const std::wstring& path, const std::vector<PakFile>& files, std::string& why) {
    Bytes body, index;
    putStr(index, "../../../");
    put<int32_t>(index, static_cast<int32_t>(files.size()));
    for (const PakFile& f : files) {
      const int64_t off = static_cast<int64_t>(body.size());
      Packed p;
      if (!pack(f.data, p)) { why = "compressing " + f.name; return false; }
      entryOf(body, 0, p, f.data.size());
      if (p.comp) body.insert(body.end(), p.data.begin(), p.data.end());
      else body.insert(body.end(), f.data.begin(), f.data.end());
      putStr(index, f.name);
      entryOf(index, off, p, f.data.size());
    }
    uint8_t ih[20];
    if (!sha1(index.data(), index.size(), ih)) { why = "sha1"; return false; }
    Bytes foot(16, 0);  // (no encryption key guid)
    put<uint8_t>(foot, 0);
    put<uint32_t>(foot, kMagic);
    put<int32_t>(foot, 9);
    put<int64_t>(foot, static_cast<int64_t>(body.size()));
    put<int64_t>(foot, static_cast<int64_t>(index.size()));
    foot.insert(foot.end(), ih, ih + 20);
    put<uint8_t>(foot, 0);  // (index not frozen)
    char methods[160] = "Zlib";
    foot.insert(foot.end(), methods, methods + 160);
    if (foot.size() != 222) { why = "footer size"; return false; }
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o) { why = "can't write the file"; return false; }
    o.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
    o.write(reinterpret_cast<const char*>(index.data()), static_cast<std::streamsize>(index.size()));
    o.write(reinterpret_cast<const char*>(foot.data()), static_cast<std::streamsize>(foot.size()));
    if (!o.good()) { why = "write failed"; return false; }
    return true;
  }

  bool pakNames(const std::wstring& path, std::vector<std::string>& names) {
    names.clear();
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const int64_t len = static_cast<int64_t>(f.tellg());
    if (len < 222) return false;
    uint8_t foot[222];
    f.seekg(len - 222);
    f.read(reinterpret_cast<char*>(foot), 222);
    if (!f || foot[16] != 0 || u32(foot + 17) != kMagic || i32(foot + 21) != 9) return false;
    const int64_t io = i64(foot + 25), isz = i64(foot + 33);
    if (io < 0 || isz <= 0 || io + isz > len - 222 || isz > (64 << 20)) return false;
    Bytes idx(static_cast<size_t>(isz));
    f.seekg(io);
    f.read(reinterpret_cast<char*>(idx.data()), isz);
    if (!f) return false;
    size_t at = 0;
    auto str = [&](std::string& s) {
      if (at + 4 > idx.size()) return false;
      const int32_t n = i32(idx.data() + at);
      at += 4;
      if (n <= 0 || at + static_cast<size_t>(n) > idx.size()) return false;
      s.assign(reinterpret_cast<const char*>(idx.data() + at), static_cast<size_t>(n) - 1);
      at += static_cast<size_t>(n);
      return true;
    };
    std::string mount;
    if (!str(mount) || at + 4 > idx.size()) return false;
    const int32_t count = i32(idx.data() + at);
    at += 4;
    for (int32_t i = 0; i < count; i++) {
      std::string n;
      Entry e;
      if (!str(n) || !readEntry(idx.data(), at, idx.size(), e)) return false;
      at = e.end;
      names.push_back(mount + n);
    }
    return true;
  }

  bool checkPak(const std::wstring& path, const std::vector<PakFile>& files, std::string& why) {
    Bytes b;
    if (!readAll(path, b) || b.size() < 222) { why = "can't read it back"; return false; }
    const uint8_t* f = b.data() + b.size() - 222;
    if (f[16] != 0 || u32(f + 17) != kMagic || i32(f + 21) != 9) { why = "footer"; return false; }
    const int64_t io = i64(f + 25), isz = i64(f + 33);
    if (io < 0 || isz < 0 || static_cast<size_t>(io + isz) > b.size() - 222) { why = "index place"; return false; }
    const uint8_t* idx = b.data() + io;
    uint8_t h[20];
    if (!sha1(idx, static_cast<size_t>(isz), h) || std::memcmp(h, f + 41, 20) != 0) { why = "index hash"; return false; }
    size_t at = 0;
    auto str = [&](std::string& s) {
      const int32_t n = i32(idx + at);
      at += 4;
      if (n <= 0 || at + n > static_cast<size_t>(isz)) return false;
      s.assign(reinterpret_cast<const char*>(idx + at), n - 1);
      at += n;
      return true;
    };
    std::string mount;
    if (!str(mount) || mount != "../../../") { why = "mount point"; return false; }
    const int32_t count = i32(idx + at);
    at += 4;
    if (count != static_cast<int32_t>(files.size())) { why = "file count"; return false; }
    for (int32_t i = 0; i < count; i++) {
      std::string name;
      Entry e;
      if (!str(name) || !readEntry(idx, at, static_cast<size_t>(isz), e)) { why = "index entry"; return false; }
      at = e.end;
      const PakFile* want = nullptr;
      for (const PakFile& pf : files) if (pf.name == name) want = &pf;
      if (!want || e.usize != static_cast<int64_t>(want->data.size()) || e.comp > 1) { why = "entry for " + name; return false; }
      Entry fh;  // (the file's own header, before its data: the same entry with offset 0)
      if (e.off < 0 || !readEntry(b.data(), static_cast<size_t>(e.off), static_cast<size_t>(io), fh)) { why = "entry header for " + name; return false; }
      const size_t hdr_len = fh.end - static_cast<size_t>(e.off);
      if (fh.off != 0 || fh.size != e.size || fh.usize != e.usize || fh.comp != e.comp || fh.blocks != e.blocks || std::memcmp(fh.hash, e.hash, 20) != 0 ||
          std::memcmp(b.data() + e.off + hdr_len - 5, idx + at - 5, 5) != 0) { why = "entry header for " + name; return false; }
      const uint8_t* stored = b.data() + e.off + hdr_len;
      if (static_cast<size_t>(e.off) + hdr_len + static_cast<size_t>(e.size) > static_cast<size_t>(io)) { why = "data place for " + name; return false; }
      if (!sha1(stored, static_cast<size_t>(e.size), h) || std::memcmp(h, e.hash, 20) != 0) { why = "hash of " + name; return false; }
      if (!e.comp) {
        if (e.size != e.usize || std::memcmp(stored, want->data.data(), static_cast<size_t>(e.size)) != 0) { why = "data of " + name; return false; }
        continue;
      }
      // unpack each block and compare it with what was meant to be written
      size_t out_at = 0;
      for (size_t k = 0; k < e.blocks.size(); k++) {
        const auto [s, t] = e.blocks[k];
        if (s < static_cast<int64_t>(hdr_len) || t < s || static_cast<size_t>(t) > hdr_len + static_cast<size_t>(e.size)) { why = "block of " + name; return false; }
        const size_t n = (std::min)(kBlock, want->data.size() - out_at);
        Bytes tmp(n);
        uLongf got = static_cast<uLongf>(n);
        if (uncompress(tmp.data(), &got, b.data() + e.off + s, static_cast<uLong>(t - s)) != Z_OK || got != n ||
            std::memcmp(tmp.data(), want->data.data() + out_at, n) != 0) { why = "unpacking " + name; return false; }
        out_at += n;
      }
      if (out_at != want->data.size()) { why = "size of " + name; return false; }
    }
    return true;
  }
}
