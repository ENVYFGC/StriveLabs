#include "paint_layer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace paint {
namespace {

constexpr int kTile = 64;                   // edge length of an undo tile
constexpr size_t kUndoDepth = 50;
constexpr size_t kUndoBytes = 256u << 20;  // about 256 MB of saved bytes
constexpr int kMaxDim = 16384;
constexpr float kMaxCoord = 1e6f;           // stroke points beyond this are rejected

// round(x / 255) for x in [0, 255*255]
inline int div255(int x) { x += 128; return (x + (x >> 8)) >> 8; }
inline uint8_t to8(float v) { return v <= 0.f ? 0 : v >= 255.f ? 255 : uint8_t(v + 0.5f); }

Rect unite(Rect a, Rect b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}
Rect intersect(Rect a, Rect b) {
    Rect r{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
    return r.empty() ? Rect{} : r;
}
bool ok_map(Map m) { return uint8_t(m) < 2; }
bool ok_blend(Blend b) { return uint8_t(b) < 4; }
float clamp01(float v) { return std::clamp(v, 0.f, 1.f); }

// Brush falloff: 1 inside hardness*radius, smoothstep down to 0 at the radius. t = distance / radius.
inline float falloff(float t, float hard) {
    if (t >= 1.f) return 0.f;
    if (t <= hard) return 1.f;
    float u = (t - hard) / (1.f - hard);
    return 1.f - u * u * (3.f - 2.f * u);
}

// Paint color c (0..255) at strength s over a layer texel, src -> dst (may be the same).
// Same as compositing the old layer texel and then the new paint ("over"), so the layer stays one RGBA.
// Coverage a == 0 always stores all-zero bytes, so a blank layer is all zeros.
inline void over(const uint8_t* src, uint8_t* dst, const float* c, float s) {
    float a0 = src[3] * (1.f / 255.f);
    float a = s + a0 * (1.f - s);
    uint8_t a8 = to8(a * 255.f);
    if (a8 == 0) { std::memset(dst, 0, 4); return; }
    float k1 = s / a, k0 = 1.f - k1;
    dst[0] = to8(c[0] * k1 + src[0] * k0);
    dst[1] = to8(c[1] * k1 + src[1] * k0);
    dst[2] = to8(c[2] * k1 + src[2] * k0);
    dst[3] = a8;
}

// Lower coverage by strength s, src -> dst.
inline void erase(const uint8_t* src, uint8_t* dst, float s) {
    uint8_t a8 = to8(src[3] * (1.f - s));
    if (a8 == 0) { std::memset(dst, 0, 4); return; }
    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = a8;
}

// W3C non-separable "color": SetLum(Cs, Lum(Cb)) with ClipColor, worked in 0..255.
// b = base, s = paint, out = blended rgb. Lum(SetLum(C, l)) == l, so ClipColor uses l directly.
inline void blend_color(const uint8_t* b, const uint8_t* s, int* out) {
    float l = 0.3f * b[0] + 0.59f * b[1] + 0.11f * b[2];
    float d = l - (0.3f * s[0] + 0.59f * s[1] + 0.11f * s[2]);
    float r = s[0] + d, g = s[1] + d, bl = s[2] + d;
    float n = std::min(r, std::min(g, bl)), x = std::max(r, std::max(g, bl));
    if (n < 0.f) {
        float f = l / (l - n);
        r = l + (r - l) * f; g = l + (g - l) * f; bl = l + (bl - l) * f;
    }
    if (x > 255.f) {
        float f = (255.f - l) / (x - l);
        r = l + (r - l) * f; g = l + (g - l) * f; bl = l + (bl - l) * f;
    }
    out[0] = to8(r); out[1] = to8(g); out[2] = to8(bl);
}

// Apply one layer over out inside r: out = lerp(out, blend(out, paint), a).
template <Blend B>
void apply_layer(const uint8_t* lay, uint8_t* out, int w, Rect r) {
    for (int y = r.y0; y < r.y1; ++y) {
        const uint8_t* p = lay + (size_t(y) * w + r.x0) * 4;
        uint8_t* o = out + (size_t(y) * w + r.x0) * 4;
        for (int x = r.x0; x < r.x1; ++x, p += 4, o += 4) {
            int a = p[3];
            if (!a) continue;
            int c[3];
            if constexpr (B == Blend::Normal) {
                c[0] = p[0]; c[1] = p[1]; c[2] = p[2];
            } else if constexpr (B == Blend::Color) {
                blend_color(o, p, c);
            } else if constexpr (B == Blend::Multiply) {
                for (int i = 0; i < 3; ++i) c[i] = div255(o[i] * p[i]);
            } else {
                for (int i = 0; i < 3; ++i) c[i] = 255 - div255((255 - o[i]) * (255 - p[i]));
            }
            for (int i = 0; i < 3; ++i) o[i] = uint8_t(div255(o[i] * (255 - a) + c[i] * a));
        }
    }
}

// Save format helpers (little endian).
void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
struct Reader {
    const std::vector<uint8_t>& d;
    size_t pos = 0;
    bool u8(uint8_t& x) { if (pos >= d.size()) return false; x = d[pos++]; return true; }
    bool u32(uint32_t& x) {
        if (d.size() - pos < 4) return false;
        x = uint32_t(d[pos]) | uint32_t(d[pos + 1]) << 8 | uint32_t(d[pos + 2]) << 16 | uint32_t(d[pos + 3]) << 24;
        pos += 4;
        return true;
    }
};

// PackBits on 4-byte pixels. Header byte h < 128: h+1 literal pixels follow. h >= 128: one pixel repeated
// h-126 times. Best case is 129 pixels in 5 bytes.
void rle_encode(const uint8_t* px, size_t n, std::vector<uint8_t>& out) {
    auto pix = [&](size_t i) { uint32_t v; std::memcpy(&v, px + i * 4, 4); return v; };
    size_t i = 0;
    while (i < n) {
        size_t run = 1;
        while (i + run < n && run < 129 && pix(i + run) == pix(i)) ++run;
        if (run >= 2) {
            out.push_back(uint8_t(run + 126));
            out.insert(out.end(), px + i * 4, px + i * 4 + 4);
            i += run;
            continue;
        }
        size_t lit = 1;
        while (i + lit < n && lit < 128 && !(i + lit + 1 < n && pix(i + lit) == pix(i + lit + 1))) ++lit;
        out.push_back(uint8_t(lit - 1));
        out.insert(out.end(), px + i * 4, px + (i + lit) * 4);
        i += lit;
    }
}
// Decode exactly n pixels from in[p..len) into px, advancing p. False if the data is short or a code overruns n.
bool rle_decode(const uint8_t* in, size_t len, size_t& p, uint8_t* px, size_t n) {
    size_t i = 0;
    while (i < n) {
        if (p >= len) return false;
        uint8_t h = in[p++];
        size_t cnt = h < 128 ? h + 1 : h - 126;
        if (cnt > n - i) return false;
        if (h < 128) {
            if (len - p < cnt * 4) return false;
            std::memcpy(px + i * 4, in + p, cnt * 4);
            p += cnt * 4;
        } else {
            if (len - p < 4) return false;
            for (size_t k = 0; k < cnt; ++k) std::memcpy(px + (i + k) * 4, in + p, 4);
            p += 4;
        }
        i += cnt;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------- setup

bool Canvas::reset(int w, int h) {
    std::vector<uint8_t> mask = std::move(mask_);
    *this = Canvas();  // frees everything
    if (w < 1 || h < 1 || w > kMaxDim || h > kMaxDim) return false;
    try {
        undo_.reserve(kUndoDepth + 1);
        redo_.reserve(kUndoDepth + 1);
    } catch (...) {
        return false;
    }
    w_ = w; h_ = h;
    tiles_x_ = (w + kTile - 1) / kTile;
    tiles_y_ = (h + kTile - 1) / kTile;
    if (mask.size() == size_t(w) * h) mask_ = std::move(mask);
    return true;
}

bool Canvas::set_mask(const std::vector<uint8_t>* m) {
    if (!m) { mask_ = {}; return true; }
    if (!w_ || m->size() != size_t(w_) * h_) return false;
    try {
        mask_ = *m;
        return true;
    } catch (...) {
        return false;
    }
}

Rect Canvas::tile_rect(int t) const {
    int x = (t % tiles_x_) * kTile, y = (t / tiles_x_) * kTile;
    return {x, y, std::min(w_, x + kTile), std::min(h_, y + kTile)};
}

// Tight box of texels with a != 0 inside r.
Rect Canvas::scan(const Layer& l, Rect r) const {
    r = intersect(r, {0, 0, w_, h_});
    if (l.px.empty() || r.empty()) return {};
    Rect out;
    for (int y = r.y0; y < r.y1; ++y) {
        const uint8_t* row = &l.px[(size_t(y) * w_) * 4];
        int x0 = r.x0;
        while (x0 < r.x1 && !row[x0 * 4 + 3]) ++x0;
        if (x0 == r.x1) continue;
        int x1 = r.x1 - 1;
        while (!row[x1 * 4 + 3]) --x1;
        out = unite(out, {x0, y, x1 + 1, y + 1});
    }
    return out;
}

void Canvas::ensure_alloc(Layer& l) {
    if (l.px.empty()) l.px.assign(size_t(w_) * h_ * 4, 0);
}

const std::vector<uint8_t>& Canvas::pixels(Map m, Blend b) const {
    static const std::vector<uint8_t> none;
    if (!ok_map(m) || !ok_blend(b)) return none;
    return layers_[int(m) * 4 + int(b)].px;
}

bool Canvas::empty() const {
    for (const Layer& l : layers_)
        if (!l.bounds.empty()) return false;
    return true;
}

Rect Canvas::dirty_rect(Map m) const { return ok_map(m) ? dirty_[int(m)] : Rect{}; }

Rect Canvas::take_dirty(Map m) {
    if (!ok_map(m)) return {};
    Rect r = dirty_[int(m)];
    dirty_[int(m)] = {};
    return r;
}

// ---------------------------------------------------------------------------------------------- history

// Copy of tile t of a layer (zeros if the layer is not allocated).
Canvas::TileDelta Canvas::save_tile(int li, int t) const {
    const Layer& l = layers_[li];
    Rect r = tile_rect(t);
    size_t row = size_t(r.x1 - r.x0) * 4;
    TileDelta d{uint8_t(li), t, std::vector<uint8_t>(row * (r.y1 - r.y0), 0), {}};
    if (!l.px.empty())
        for (int y = r.y0; y < r.y1; ++y)
            std::memcpy(d.px.data() + (y - r.y0) * row, &l.px[(size_t(y) * w_ + r.x0) * 4], row);
    return d;
}

// Does the layer's tile now differ from the saved bytes?
bool Canvas::tile_differs(const TileDelta& d) const {
    const Layer& l = layers_[d.layer];
    Rect r = tile_rect(d.tile);
    size_t row = size_t(r.x1 - r.x0) * 4;
    for (int y = r.y0; y < r.y1; ++y) {
        const uint8_t* old = d.px.data() + (y - r.y0) * row;
        if (l.px.empty()) {
            if (std::any_of(old, old + row, [](uint8_t v) { return v != 0; })) return true;
        } else if (std::memcmp(old, &l.px[(size_t(y) * w_ + r.x0) * 4], row) != 0) {
            return true;
        }
    }
    return false;
}

size_t Canvas::undo_bytes() const {
    size_t n = 0;
    for (const auto* list : {&undo_, &redo_})
        for (const UndoEntry& u : *list)
            for (const TileDelta& t : u.tiles) n += t.px.size();
    return n;
}

// undo_/redo_ have capacity kUndoDepth + 1 and together never hold more than kUndoDepth, so no push allocates.
void Canvas::push_undo(UndoEntry&& e) noexcept {
    if (e.tiles.empty()) return;
    if (undo_.capacity() <= kUndoDepth || redo_.capacity() <= kUndoDepth) {  // e.g. a moved-from canvas
        try {
            undo_.reserve(kUndoDepth + 1);
            redo_.reserve(kUndoDepth + 1);
        } catch (...) {
            return;  // the change stays, it just cannot be undone
        }
    }
    redo_.clear();
    undo_.push_back(std::move(e));
    auto bytes = [](const UndoEntry& u) {
        size_t n = 0;
        for (const TileDelta& t : u.tiles) n += t.px.size();
        return n;
    };
    size_t total = 0;
    for (const UndoEntry& u : undo_) total += bytes(u);
    // The newest step is always kept, even if it alone is over the byte limit.
    size_t drop = 0;
    while (undo_.size() - drop > 1 && (undo_.size() - drop > kUndoDepth || total > kUndoBytes))
        total -= bytes(undo_[drop++]);
    undo_.erase(undo_.begin(), undo_.begin() + drop);
}

// Swap the stored bytes with the layers. Doing it twice is a no-op, so undo and redo are both this.
// Throws only before anything changed (layer allocation).
void Canvas::swap_entry(UndoEntry& e) {
    for (TileDelta& t : e.tiles)
        if (t.tile >= 0) ensure_alloc(layers_[t.layer]);
    Rect touched[8];
    for (TileDelta& t : e.tiles) {
        Layer& l = layers_[t.layer];
        if (t.tile < 0) {  // whole layer
            dirty_[t.layer / 4] = unite(dirty_[t.layer / 4], unite(l.bounds, t.bounds));
            std::swap(l.px, t.px);
            std::swap(l.bounds, t.bounds);
            continue;
        }
        Rect r = tile_rect(t.tile);
        size_t row = size_t(r.x1 - r.x0) * 4;
        for (int y = r.y0; y < r.y1; ++y) {
            uint8_t* a = &l.px[(size_t(y) * w_ + r.x0) * 4];
            std::swap_ranges(a, a + row, t.px.data() + (y - r.y0) * row);
        }
        touched[t.layer] = unite(touched[t.layer], r);
    }
    for (int i = 0; i < 8; ++i) {
        if (touched[i].empty()) continue;
        Layer& l = layers_[i];
        Rect old = l.bounds;
        l.bounds = scan(l, unite(l.bounds, touched[i]));
        if (l.bounds.empty()) l.px = {};
        dirty_[i / 4] = unite(dirty_[i / 4], intersect(unite(old, l.bounds), touched[i]));
    }
}

bool Canvas::undo() {
    end_stroke();
    if (undo_.empty()) return false;
    try {
        swap_entry(undo_.back());
    } catch (...) {
        return false;  // out of memory before anything changed, history kept
    }
    redo_.push_back(std::move(undo_.back()));
    undo_.pop_back();
    return true;
}

bool Canvas::redo() {
    end_stroke();
    if (redo_.empty()) return false;
    try {
        swap_entry(redo_.back());
    } catch (...) {
        return false;
    }
    undo_.push_back(std::move(redo_.back()));
    redo_.pop_back();
    return true;
}

bool Canvas::clear(Map m) {
    if (!ok_map(m)) return false;
    return clear_maps(m == Map::Lit, m == Map::Shadow);
}

bool Canvas::clear_all() { return clear_maps(true, true); }

// A mostly painted layer's buffer moves into the undo step (no copy); a sparse one saves just its painted
// tiles, so the step stays small and doesn't push the older steps out of the byte limit.
bool Canvas::clear_maps(bool lit, bool shadow) {
    end_stroke();
    changed_ = false;
    if (!w_) return false;
    UndoEntry e;
    bool whole[8] = {};
    try {  // nothing changes until every copy is made
        e.tiles.reserve(8);
        for (int i = 0; i < 8; ++i) {
            if (!(i < 4 ? lit : shadow) || layers_[i].bounds.empty()) continue;
            const Layer& l = layers_[i];
            const Rect& r = l.bounds;
            std::vector<int> painted;  // the box's tiles holding paint (blank ones come back as zeros on undo)
            for (int ty = r.y0 / kTile; ty <= (r.y1 - 1) / kTile; ++ty)
                for (int tx = r.x0 / kTile; tx <= (r.x1 - 1) / kTile; ++tx) {
                    const int t = ty * tiles_x_ + tx;
                    const Rect tr = tile_rect(t);
                    bool any = false;
                    for (int y = tr.y0; y < tr.y1 && !any; ++y) {
                        const uint8_t* row = &l.px[(size_t(y) * w_ + tr.x0) * 4];
                        any = std::any_of(row, row + size_t(tr.x1 - tr.x0) * 4, [](uint8_t v) { return v != 0; });
                    }
                    if (any) painted.push_back(t);
                }
            whole[i] = painted.size() * kTile * kTile * 2 > size_t(w_) * h_;
            if (whole[i]) continue;
            for (int t : painted) e.tiles.push_back(save_tile(i, t));
        }
        e.tiles.reserve(e.tiles.size() + std::count(whole, whole + 8, true));  // the pushes below can't allocate
    } catch (...) {
        return false;
    }
    for (int i = 0; i < 8; ++i) {
        if (!(i < 4 ? lit : shadow)) continue;
        Layer& l = layers_[i];
        if (l.bounds.empty()) { l.px = {}; continue; }
        dirty_[i / 4] = unite(dirty_[i / 4], l.bounds);
        if (whole[i]) e.tiles.push_back({uint8_t(i), -1, std::move(l.px), l.bounds});
        l = Layer();
    }
    changed_ = !e.tiles.empty();
    push_undo(std::move(e));
    return true;
}

// ---------------------------------------------------------------------------------------------- brush

bool Canvas::begin_stroke(Map m, Blend b, Rgba rgb, float size_px, float hardness, float opacity, float flow,
                          bool eraser) {
    end_stroke();
    if (!w_ || !ok_map(m) || !ok_blend(b)) return false;
    if (!(size_px > 0.f) || !(size_px <= 2.f * kMaxDim)) return false;
    if (!std::isfinite(hardness) || !(opacity > 0.f) || !(flow > 0.f)) return false;  // also rejects NaN
    try {
        slot_.assign(size_t(tiles_x_) * tiles_y_, -1);
    } catch (...) {
        return false;
    }
    smap_ = int(m);
    sblend_ = int(b);
    erase_ = eraser;
    color_[0] = rgb.r; color_[1] = rgb.g; color_[2] = rgb.b;
    radius_ = std::max(0.5f, size_px * 0.5f);
    hard_ = clamp01(hardness);
    opacity_ = clamp01(opacity);
    flow_ = clamp01(flow);
    spacing_ = std::max(1.f, size_px * 0.15f);
    long rounded = std::lround(size_px);
    snap_ = size_px >= 4.f ? 0 : (rounded <= 1 || rounded % 2) ? 1 : 2;
    for (int i = 0; i < 4; ++i) {
        const Layer& l = layer(smap_, i);
        targets_[i] = eraser ? !l.bounds.empty() : i == sblend_;
        pre_bounds_[i] = l.bounds;
    }
    stroke_rect_ = {};
    stiles_.clear();
    pending_ = {};
    has_last_ = false;
    active_ = true;
    return true;
}

// Stroke data for tile t; the first touch saves the tile's bytes of every target layer for undo.
Canvas::StrokeTile& Canvas::stroke_tile(int t) {
    if (slot_[t] >= 0) return stiles_[slot_[t]];
    Rect r = tile_rect(t);
    StrokeTile st;
    st.s.assign(size_t(r.x1 - r.x0) * (r.y1 - r.y0), 0.f);
    for (int i = 0; i < 4; ++i) {
        st.delta[i] = -1;
        if (!targets_[i]) continue;
        ensure_alloc(layer(smap_, i));
        pending_.tiles.push_back(save_tile(smap_ * 4 + i, t));
        st.delta[i] = int(pending_.tiles.size()) - 1;
    }
    stiles_.push_back(std::move(st));
    slot_[t] = int(stiles_.size()) - 1;
    return stiles_.back();
}

// One brush dab. Per texel the stroke keeps an accumulated strength S (flow builds it up, max 1) and
// rewrites the layer from the pre-stroke bytes with strength S * opacity, so one stroke never goes past
// its opacity but a second stroke builds on top of the first.
void Canvas::stamp(float cx, float cy) {
    if (snap_ == 1) { cx = std::floor(cx) + 0.5f; cy = std::floor(cy) + 0.5f; }
    if (snap_ == 2) { cx = std::round(cx); cy = std::round(cy); }
    Rect r = intersect({int(std::floor(cx - radius_)), int(std::floor(cy - radius_)), int(std::ceil(cx + radius_)),
                        int(std::ceil(cy + radius_))},
                       {0, 0, w_, h_});
    if (r.empty()) return;
    const float r2 = radius_ * radius_, inv_r = 1.f / radius_;
    const uint8_t* mask = mask_.empty() ? nullptr : mask_.data();
    for (int ty = r.y0 / kTile; ty <= (r.y1 - 1) / kTile; ++ty) {
        for (int tx = r.x0 / kTile; tx <= (r.x1 - 1) / kTile; ++tx) {
            int t = ty * tiles_x_ + tx;
            Rect tr = tile_rect(t);
            Rect q = intersect(r, tr);
            StrokeTile& st = stroke_tile(t);
            int tw = tr.x1 - tr.x0;
            for (int y = q.y0; y < q.y1; ++y) {
                float dy = y + 0.5f - cy;
                for (int x = q.x0; x < q.x1; ++x) {
                    float dx = x + 0.5f - cx, d2 = dx * dx + dy * dy;
                    if (d2 >= r2) continue;
                    size_t i = size_t(y) * w_ + x;
                    if (mask && !mask[i]) continue;
                    float f = falloff(std::sqrt(d2) * inv_r, hard_) * flow_;
                    if (f <= 0.f) continue;
                    size_t local = size_t(y - tr.y0) * tw + (x - tr.x0);
                    float& S = st.s[local];
                    S += (1.f - S) * f;
                    float s = S * opacity_;
                    for (int b = 0; b < 4; ++b) {
                        if (st.delta[b] < 0) continue;
                        const uint8_t* src = &pending_.tiles[st.delta[b]].px[local * 4];
                        uint8_t* dst = &layer(smap_, b).px[i * 4];
                        if (erase_) erase(src, dst, s);
                        else over(src, dst, color_, s);
                    }
                }
            }
        }
    }
    stroke_rect_ = unite(stroke_rect_, r);
    dirty_[smap_] = unite(dirty_[smap_], r);
    if (!erase_) {
        Layer& l = layer(smap_, sblend_);  // loose while drawing so compose sees the new paint, tightened at the end
        l.bounds = unite(l.bounds, r);
    }
}

bool Canvas::stroke_to(float x, float y) {
    if (!active_ || slot_.empty()) return false;  // a moved-from canvas, whose stroke went with the move
    if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(x) > kMaxCoord || std::fabs(y) > kMaxCoord) return false;
    try {
        if (!has_last_) {
            stamp(x, y);
            has_last_ = true;
            next_ = spacing_;
        } else {
            float dx = x - last_x_, dy = y - last_y_, len = std::sqrt(dx * dx + dy * dy);
            if (len <= 0.f) return true;
            float pos = next_;
            for (; pos <= len; pos += spacing_) stamp(last_x_ + dx * (pos / len), last_y_ + dy * (pos / len));
            next_ = pos - len;
        }
        last_x_ = x;
        last_y_ = y;
        return true;
    } catch (...) {
        end_stroke();  // out of memory, so keep what was painted (every changed tile is already saved)
        return false;
    }
}

bool Canvas::end_stroke() {
    if (!active_) return false;
    active_ = false;
    for (int i = 0; i < 4; ++i) {
        if (!targets_[i]) continue;
        Layer& l = layer(smap_, i);
        // Paint only adds coverage, so only the stroke area can gain texels; erasing can only remove them.
        l.bounds = erase_ ? scan(l, pre_bounds_[i]) : unite(pre_bounds_[i], scan(l, stroke_rect_));
        if (l.bounds.empty()) l.px = {};
    }
    // Keep only tiles that really changed (drops brush-square corners, masked-out tiles, no-op strokes).
    std::erase_if(pending_.tiles, [&](const TileDelta& d) { return !tile_differs(d); });
    changed_ = !pending_.tiles.empty();
    push_undo(std::move(pending_));
    pending_ = {};
    stiles_ = {};
    slot_ = {};
    return true;
}

// ---------------------------------------------------------------------------------------------- fill

bool Canvas::fill(Map m, Blend b, Rgba rgb, float opacity, int x, int y, const Image& ref, int tolerance,
                  bool contiguous) {
    end_stroke();
    changed_ = false;
    const size_t n = size_t(w_) * h_;
    if (!w_ || !ok_map(m) || !ok_blend(b) || !(opacity > 0.f)) return false;
    if (ref.w != w_ || ref.h != h_ || ref.px.size() != n * 4) return false;
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return false;
    const float s = clamp01(opacity);
    const int tol = std::clamp(tolerance, 0, 255);
    const uint8_t* mask = mask_.empty() ? nullptr : mask_.data();
    try {
        const uint8_t* seed = &ref.px[(size_t(y) * w_ + x) * 4];
        const int sr = seed[0], sg = seed[1], sb = seed[2];
        auto match = [&](size_t i) {
            const uint8_t* p = &ref.px[i * 4];
            return std::abs(p[0] - sr) <= tol && std::abs(p[1] - sg) <= tol && std::abs(p[2] - sb) <= tol &&
                   (!mask || mask[i]);
        };
        std::vector<uint8_t> sel(n, 0);
        int bx0 = w_, by0 = h_, bx1 = -1, by1 = -1;  // inclusive box of selected texels
        if (contiguous) {
            // Scanline flood: fill a whole horizontal span, then queue one texel per matching run above and below.
            std::vector<uint32_t> stack;
            if (match(size_t(y) * w_ + x)) stack.push_back(uint32_t(size_t(y) * w_ + x));
            while (!stack.empty()) {
                uint32_t i = stack.back();
                stack.pop_back();
                if (sel[i]) continue;
                int py = int(i / uint32_t(w_)), px = int(i % uint32_t(w_));
                size_t row = size_t(py) * w_;
                int xl = px, xr = px;
                while (xl > 0 && !sel[row + xl - 1] && match(row + xl - 1)) --xl;
                while (xr < w_ - 1 && !sel[row + xr + 1] && match(row + xr + 1)) ++xr;
                std::memset(&sel[row + xl], 1, size_t(xr - xl + 1));
                bx0 = std::min(bx0, xl); bx1 = std::max(bx1, xr);
                by0 = std::min(by0, py); by1 = std::max(by1, py);
                for (int ny : {py - 1, py + 1}) {
                    if (ny < 0 || ny >= h_) continue;
                    size_t nrow = size_t(ny) * w_;
                    bool in_run = false;
                    for (int xx = xl; xx <= xr; ++xx) {
                        bool ok = !sel[nrow + xx] && match(nrow + xx);
                        if (ok && !in_run) stack.push_back(uint32_t(nrow + xx));
                        in_run = ok;
                    }
                }
            }
        } else {
            for (int yy = 0; yy < h_; ++yy)
                for (int xx = 0; xx < w_; ++xx) {
                    size_t i = size_t(yy) * w_ + xx;
                    if (!match(i)) continue;
                    sel[i] = 1;
                    bx0 = std::min(bx0, xx); bx1 = std::max(bx1, xx);
                    by0 = std::min(by0, yy); by1 = std::max(by1, yy);
                }
        }
        if (bx1 < 0) return true;
        paint_sel(int(m), int(b), rgb, s, sel, {bx0, by0, bx1 + 1, by1 + 1});
        return true;
    } catch (...) {
        return false;  // only allocations before painting can fail; layers unchanged
    }
}

bool Canvas::fill_shape(Map m, Blend b, Rgba rgb, float opacity, const std::vector<uint8_t>& cov) {
    end_stroke();
    changed_ = false;
    const size_t n = size_t(w_) * h_;
    if (!w_ || !ok_map(m) || !ok_blend(b) || !(opacity > 0.f) || cov.size() != n) return false;
    const uint8_t* mask = mask_.empty() ? nullptr : mask_.data();
    try {
        std::vector<uint8_t> sel(n, 0);
        int bx0 = w_, by0 = h_, bx1 = -1, by1 = -1;  // inclusive box of selected texels
        for (int yy = 0; yy < h_; ++yy)
            for (int xx = 0; xx < w_; ++xx) {
                size_t i = size_t(yy) * w_ + xx;
                if (!cov[i] || (mask && !mask[i])) continue;
                sel[i] = 1;
                bx0 = std::min(bx0, xx); bx1 = std::max(bx1, xx);
                by0 = std::min(by0, yy); by1 = std::max(by1, yy);
            }
        if (bx1 < 0) return true;
        paint_sel(int(m), int(b), rgb, clamp01(opacity), sel, {bx0, by0, bx1 + 1, by1 + 1});
        return true;
    } catch (...) {
        return false;
    }
}

// Paint the selected texels (sel[i] == 1, all inside box) at strength s as one undo step. Throws only while
// saving undo data, before anything changed.
void Canvas::paint_sel(int m, int b, Rgba rgb, float s, const std::vector<uint8_t>& sel, Rect box) {
    const int li = m * 4 + b;
    Layer& l = layers_[li];
    ensure_alloc(l);
    UndoEntry e;
    for (int ty = box.y0 / kTile; ty <= (box.y1 - 1) / kTile; ++ty)
        for (int tx = box.x0 / kTile; tx <= (box.x1 - 1) / kTile; ++tx) {
            int t = ty * tiles_x_ + tx;
            Rect tr = intersect(tile_rect(t), box);
            bool any = false;
            for (int yy = tr.y0; yy < tr.y1 && !any; ++yy)
                any = std::memchr(&sel[size_t(yy) * w_ + tr.x0], 1, size_t(tr.x1 - tr.x0)) != nullptr;
            if (any) e.tiles.push_back(save_tile(li, t));
        }
    // All undo data is saved; now paint (nothing below allocates).
    const float c[3] = {float(rgb.r), float(rgb.g), float(rgb.b)};
    for (int yy = box.y0; yy < box.y1; ++yy)
        for (int xx = box.x0; xx < box.x1; ++xx) {
            size_t i = size_t(yy) * w_ + xx;
            if (sel[i]) over(&l.px[i * 4], &l.px[i * 4], c, s);
        }
    l.bounds = unite(l.bounds, scan(l, box));
    std::erase_if(e.tiles, [&](const TileDelta& d) { return !tile_differs(d); });
    if (l.bounds.empty()) l.px = {};
    if (!e.tiles.empty()) dirty_[m] = unite(dirty_[m], box);
    changed_ = !e.tiles.empty();
    push_undo(std::move(e));
}

bool Canvas::set_texels(Map m, const Image& shown, const Image& want) {
    end_stroke();
    changed_ = false;
    const size_t n = size_t(w_) * h_;
    if (!w_ || !ok_map(m)) return false;
    if (shown.w != w_ || shown.h != h_ || shown.px.size() != n * 4) return false;
    if (want.w != w_ || want.h != h_ || want.px.size() != n * 4) return false;
    try {
        std::vector<uint8_t> sel(n, 0);
        int bx0 = w_, by0 = h_, bx1 = -1, by1 = -1;  // inclusive box of changed texels
        for (int yy = 0; yy < h_; ++yy)
            for (int xx = 0; xx < w_; ++xx) {
                size_t i = size_t(yy) * w_ + xx;
                if (std::memcmp(&shown.px[i * 4], &want.px[i * 4], 3) == 0) continue;
                sel[i] = 1;
                bx0 = std::min(bx0, xx); bx1 = std::max(bx1, xx);
                by0 = std::min(by0, yy); by1 = std::max(by1, yy);
            }
        if (bx1 < 0) return true;
        const Rect box{bx0, by0, bx1 + 1, by1 + 1};
        const int l0 = int(m) * 4;
        ensure_alloc(layers_[l0]);  // (Normal takes the texels; the others only lose paint, so only if painted)
        UndoEntry e;
        for (int ty = box.y0 / kTile; ty <= (box.y1 - 1) / kTile; ++ty)
            for (int tx = box.x0 / kTile; tx <= (box.x1 - 1) / kTile; ++tx) {
                int t = ty * tiles_x_ + tx;
                Rect tr = intersect(tile_rect(t), box);
                bool any = false;
                for (int yy = tr.y0; yy < tr.y1 && !any; ++yy)
                    any = std::memchr(&sel[size_t(yy) * w_ + tr.x0], 1, size_t(tr.x1 - tr.x0)) != nullptr;
                if (!any) continue;
                for (int b = 0; b < 4; ++b)
                    if (b == 0 || !intersect(layers_[l0 + b].bounds, tile_rect(t)).empty()) e.tiles.push_back(save_tile(l0 + b, t));
            }
        // All undo data is saved; now change the layers (nothing below allocates).
        for (int b = 0; b < 4; ++b) {
            Layer& l = layers_[l0 + b];
            if (l.px.empty()) continue;
            for (int yy = box.y0; yy < box.y1; ++yy)
                for (int xx = box.x0; xx < box.x1; ++xx) {
                    size_t i = size_t(yy) * w_ + xx;
                    if (!sel[i]) continue;
                    uint8_t* d = &l.px[i * 4];
                    if (b == 0) { std::memcpy(d, &want.px[i * 4], 3); d[3] = 255; }
                    else std::memset(d, 0, 4);
                }
            l.bounds = b == 0 ? unite(l.bounds, scan(l, box)) : scan(l, l.bounds);
            if (l.bounds.empty()) l.px = {};
        }
        std::erase_if(e.tiles, [&](const TileDelta& d) { return !tile_differs(d); });
        if (!e.tiles.empty()) dirty_[int(m)] = unite(dirty_[int(m)], box);
        changed_ = !e.tiles.empty();
        push_undo(std::move(e));
        return true;
    } catch (...) {
        return false;  // only allocations before the change can fail; layers unchanged
    }
}

// ---------------------------------------------------------------------------------------------- compose

bool Canvas::compose(Map m, const Image& base, Image& out) const {
    if (!w_ || !ok_map(m) || &base == &out) return false;
    if (base.w != w_ || base.h != h_ || base.px.size() != size_t(w_) * h_ * 4) return false;
    try {
        out.px = base.px;
    } catch (...) {
        return false;
    }
    out.w = w_;
    out.h = h_;
    apply_layers(int(m), out, {0, 0, w_, h_});
    return true;
}

bool Canvas::compose_rect(Map m, const Image& base, Image& out, int x0, int y0, int x1, int y1) const {
    if (!w_ || !ok_map(m) || &base == &out) return false;
    if (base.w != w_ || base.h != h_ || base.px.size() != size_t(w_) * h_ * 4) return false;
    if (out.w != w_ || out.h != h_ || out.px.size() != base.px.size()) return compose(m, base, out);
    Rect r = intersect({x0, y0, x1, y1}, {0, 0, w_, h_});
    for (int y = r.y0; y < r.y1; ++y) {
        size_t o = (size_t(y) * w_ + r.x0) * 4;
        std::memcpy(&out.px[o], &base.px[o], size_t(r.x1 - r.x0) * 4);
    }
    apply_layers(int(m), out, r);
    return true;
}

// Layers of map m over out inside r, skipping blank layers and texels outside each layer's paint.
void Canvas::apply_layers(int m, Image& out, Rect r) const {
    for (int b = 0; b < 4; ++b) {
        const Layer& l = layers_[m * 4 + b];
        Rect q = intersect(r, l.bounds);
        if (q.empty() || l.px.empty()) continue;
        switch (Blend(b)) {
            case Blend::Normal: apply_layer<Blend::Normal>(l.px.data(), out.px.data(), w_, q); break;
            case Blend::Color: apply_layer<Blend::Color>(l.px.data(), out.px.data(), w_, q); break;
            case Blend::Multiply: apply_layer<Blend::Multiply>(l.px.data(), out.px.data(), w_, q); break;
            case Blend::Screen: apply_layer<Blend::Screen>(l.px.data(), out.px.data(), w_, q); break;
        }
    }
}

// ---------------------------------------------------------------------------------------------- save / load

// Version 2 layout (little endian): "SLPN", u32 version = 2, u32 w, u32 h, u32 layer count, then per layer:
// u8 map, u8 blend, u32 x0, y0, x1, y1 (the painted box; texels outside are zero), u32 payload bytes,
// payload = for each row of the box, PackBits RLE of that row's pixels (see rle_encode).
// Version 1 (read only): no box; the payload is one RLE stream of all w*h pixels.
std::vector<uint8_t> Canvas::save() const {
    std::vector<uint8_t> v;
    try {
        v = {'S', 'L', 'P', 'N'};
        put_u32(v, 2);
        put_u32(v, uint32_t(w_));
        put_u32(v, uint32_t(h_));
        uint32_t count = 0;
        for (const Layer& l : layers_) count += !l.bounds.empty();
        put_u32(v, count);
        for (int i = 0; i < 8; ++i) {
            const Layer& l = layers_[i];
            const Rect r = l.bounds;
            if (r.empty()) continue;
            v.push_back(uint8_t(i / 4));
            v.push_back(uint8_t(i % 4));
            for (int c : {r.x0, r.y0, r.x1, r.y1}) put_u32(v, uint32_t(c));
            size_t at = v.size();
            put_u32(v, 0);
            for (int y = r.y0; y < r.y1; ++y) rle_encode(&l.px[(size_t(y) * w_ + r.x0) * 4], size_t(r.x1 - r.x0), v);
            uint32_t len = uint32_t(v.size() - at - 4);
            for (int k = 0; k < 4; ++k) v[at + k] = uint8_t(len >> (8 * k));
        }
    } catch (...) {
        v.clear();
    }
    return v;
}

bool Canvas::load(const std::vector<uint8_t>& data, int w, int h) {
    try {
        Reader rd{data};
        uint8_t magic[4];
        for (uint8_t& c : magic)
            if (!rd.u8(c)) return false;
        if (std::memcmp(magic, "SLPN", 4) != 0) return false;
        uint32_t ver, fw, fh, count;
        if (!rd.u32(ver) || !rd.u32(fw) || !rd.u32(fh) || !rd.u32(count)) return false;
        if ((ver != 1 && ver != 2) || fw != uint32_t(w) || fh != uint32_t(h) || count > 8) return false;
        if (w < 1 || h < 1 || w > kMaxDim || h > kMaxDim) return false;
        const size_t n = size_t(w) * h;
        Layer fresh[8];
        for (uint32_t k = 0; k < count; ++k) {
            uint8_t m, b;
            if (!rd.u8(m) || !rd.u8(b) || m > 1 || b > 3) return false;
            Rect r{0, 0, w, h};
            if (ver == 2) {
                uint32_t c[4];
                for (uint32_t& v : c)
                    if (!rd.u32(v)) return false;
                if (c[0] >= c[2] || c[1] >= c[3] || c[2] > uint32_t(w) || c[3] > uint32_t(h)) return false;
                r = {int(c[0]), int(c[1]), int(c[2]), int(c[3])};
            }
            uint32_t len;
            if (!rd.u32(len) || data.size() - rd.pos < len) return false;
            const size_t rw = size_t(r.x1 - r.x0), area = rw * (r.y1 - r.y0);
            if (uint64_t(len) * 129 < uint64_t(area) * 5) return false;  // too short to fill the box
            Layer& l = fresh[m * 4 + b];
            if (!l.px.empty()) return false;  // same layer twice
            l.px.assign(n * 4, 0);
            const uint8_t* in = data.data() + rd.pos;
            size_t p = 0;
            if (ver == 1) {
                if (!rle_decode(in, len, p, l.px.data(), n)) return false;
            } else {
                for (int y = r.y0; y < r.y1; ++y)
                    if (!rle_decode(in, len, p, &l.px[(size_t(y) * w + r.x0) * 4], rw)) return false;
            }
            if (p != len) return false;
            rd.pos += len;
            // Keep the "a == 0 means all-zero bytes" rule even for hand-made files.
            for (int y = r.y0; y < r.y1; ++y)
                for (int x = r.x0; x < r.x1; ++x) {
                    uint8_t* px = &l.px[(size_t(y) * w + x) * 4];
                    if (!px[3]) std::memset(px, 0, 4);
                }
            l.bounds = r;
        }
        if (rd.pos != data.size()) return false;

        if (!reset(w, h)) return false;
        for (int i = 0; i < 8; ++i) {
            Layer& l = fresh[i];
            if (l.px.empty()) continue;
            l.bounds = scan(l, l.bounds);
            if (!l.bounds.empty()) layers_[i] = std::move(l);
        }
        dirty_[0] = dirty_[1] = {0, 0, w, h};
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace paint
