// Paint layers for the StriveLabs color editor. Standalone, std only, C++20.
//
// A Canvas holds the paint for one texture size: for each map (Lit, Shadow) one layer per blend mode.
// Each layer is RGBA8 at texture size (rgb = paint color, a = coverage), allocated on first paint.
// compose() draws a map's layers over the editor's base texture in the order Normal, Color, Multiply, Screen.
//
// Coordinates are texels; texel (x, y) covers [x, x+1) x [y, y+1), its centre is (x + 0.5, y + 0.5).
// No function throws. Bad input returns false (or an empty result). Not thread safe.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace paint {

struct Rgba { uint8_t r, g, b, a; };
struct Image { int w = 0, h = 0; std::vector<uint8_t> px; };  // RGBA8, row-major, px.size() == w*h*4

enum class Blend : uint8_t { Normal, Color, Multiply, Screen };
enum class Map : uint8_t { Lit, Shadow };

// Half-open texel rectangle [x0, x1) x [y0, y1).
struct Rect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return x0 >= x1 || y0 >= y1; }
};

class Canvas {
public:
    Canvas() = default;
    Canvas(int w, int h) { reset(w, h); }
    Canvas(const Canvas&) = delete;  // layers and history can be hundreds of MB; move instead
    Canvas& operator=(const Canvas&) = delete;
    Canvas(Canvas&&) = default;  // a moved-from canvas must be reset() before use
    Canvas& operator=(Canvas&&) = default;

    // Drop all paint and history and set the texture size (1..16384). The mask is kept if the size is
    // unchanged. False on a bad size (canvas becomes 0x0).
    bool reset(int w, int h);
    int width() const { return w_; }
    int height() const { return h_; }

    // Paint mask, w*h bytes: only texels with m[i] != 0 take paint (brush, eraser and fill).
    // The canvas copies it, so later changes to the vector have no effect until set_mask is called again.
    // nullptr = no mask. Wrong size: returns false and keeps the previous mask.
    bool set_mask(const std::vector<uint8_t>* m);

    // Brush. rgb.a is ignored. size_px = diameter. hardness = fraction of the radius at full strength.
    // opacity = ceiling for this stroke, flow = strength per stamp (both must be > 0). eraser lowers coverage
    // in all blend layers of the map (blend and rgb are ignored then). The first stroke_to() stamps at its
    // point, later ones stamp along the segment every max(1, size*0.15) texels. Starting a stroke ends any
    // open one. Brushes under 4 px snap to the texel grid so they stay crisp: odd sizes centre on the texel
    // the point is in (size 1 paints exactly that texel), even sizes on the nearest texel corner.
    bool begin_stroke(Map map, Blend blend, Rgba rgb, float size_px, float hardness, float opacity, float flow,
                      bool eraser);
    bool stroke_to(float x, float y);
    bool end_stroke();  // commits the stroke as one undo step (if it changed anything); false if none was open
    bool stroking() const { return active_; }

    // Paint bucket. Texels whose reference rgb is within tolerance (max channel difference, alpha ignored)
    // of the reference at (x, y) get the paint at the given opacity (> 0), composited over existing paint in
    // that layer. contiguous = 4-connected flood from (x, y) that stays inside the mask; else every match.
    bool fill(Map map, Blend blend, Rgba rgb, float opacity, int x, int y, const Image& reference, int tolerance,
              bool contiguous);

    // Filled shape: texels with coverage[i] != 0 (w*h bytes, the caller rasterizes) get the paint at the given
    // opacity (> 0) like fill(), crisp edges, respecting the mask. One undo step. Wrong size: false.
    bool fill_shape(Map map, Blend blend, Rgba rgb, float opacity, const std::vector<uint8_t>& coverage);

    // Make the map compose to `want` (rgb; alpha ignored) wherever it differs from `shown` (the current
    // compose): Normal paint at full coverage there, the other blends' paint taken off. One undo step.
    // The mask doesn't apply. Used to take an edited texture back in.
    bool set_texels(Map map, const Image& shown, const Image& want);

    // Did the last end_stroke / fill / set_texels / clear / clear_all change any texel (and so add an undo step)?
    bool last_changed() const { return changed_; }

    // out = base with the map's layers applied. base must be width x height; out must not be base.
    bool compose(Map map, const Image& base, Image& out) const;
    // Recompose only [x0,x1) x [y0,y1). out must already hold a full compose of the SAME map and base
    // (keep one out image per map); if out has the wrong size this does a full compose instead.
    bool compose_rect(Map map, const Image& base, Image& out, int x0, int y0, int x1, int y1) const;

    // History: each changing stroke, fill and clear is one step. Depth 50 or ~256 MB, oldest dropped first
    // (the newest step is always kept). undo/redo return false if there is nothing to do or memory ran out.
    bool undo();
    bool redo();
    size_t undo_count() const { return undo_.size(); }
    size_t redo_count() const { return redo_.size(); }
    size_t undo_bytes() const;  // memory held by undo + redo steps
    bool clear(Map map);  // undoable
    bool clear_all();     // undoable, one step

    bool empty() const;              // nothing painted anywhere
    Rect dirty_rect(Map map) const;  // area changed since the last take_dirty(map)
    Rect take_dirty(Map map);        // returns dirty_rect(map) and resets it

    // Binary save of all layers (not the history). load() requires the saved size to equal (w, h),
    // replaces everything on success (history cleared, canvas becomes w x h) and leaves the canvas
    // untouched on failure. Reads versions 1 and 2, writes 2.
    std::vector<uint8_t> save() const;
    bool load(const std::vector<uint8_t>& data, int w, int h);

    // Raw layer bytes (w*h*4), empty if the layer was never painted or is fully clear. For debugging/tests.
    const std::vector<uint8_t>& pixels(Map map, Blend blend) const;

private:
    struct Layer { std::vector<uint8_t> px; Rect bounds; };  // bounds = tight box of texels with a != 0
    // Saved bytes for undo: one 64x64 tile, or with tile == -1 the whole layer (px + bounds, from clear).
    struct TileDelta { uint8_t layer; int tile; std::vector<uint8_t> px; Rect bounds; };
    struct UndoEntry { std::vector<TileDelta> tiles; };
    struct StrokeTile { std::vector<float> s; int delta[4]; };  // per-stroke accumulated strength

    Layer& layer(int m, int b) { return layers_[m * 4 + b]; }
    Rect tile_rect(int t) const;
    Rect scan(const Layer& l, Rect r) const;
    void ensure_alloc(Layer& l);
    TileDelta save_tile(int layer_index, int t) const;
    bool tile_differs(const TileDelta& d) const;
    void swap_entry(UndoEntry& e);
    void push_undo(UndoEntry&& e) noexcept;
    StrokeTile& stroke_tile(int t);
    void stamp(float cx, float cy);
    void paint_sel(int m, int b, Rgba rgb, float s, const std::vector<uint8_t>& sel, Rect box);
    bool clear_maps(bool lit, bool shadow);
    void apply_layers(int m, Image& out, Rect r) const;

    int w_ = 0, h_ = 0, tiles_x_ = 0, tiles_y_ = 0;
    Layer layers_[8];
    Rect dirty_[2];
    std::vector<uint8_t> mask_;  // empty or exactly w*h
    std::vector<UndoEntry> undo_, redo_;  // capacity reserved so pushes never allocate
    bool changed_ = false;

    // Open stroke.
    bool active_ = false, erase_ = false, has_last_ = false;
    int smap_ = 0, sblend_ = 0, snap_ = 0;  // snap_: 0 none, 1 texel centre, 2 texel corner
    float color_[3] = {}, radius_ = 0, hard_ = 0, opacity_ = 0, flow_ = 0, spacing_ = 1;
    float last_x_ = 0, last_y_ = 0, next_ = 0;
    bool targets_[4] = {};
    Rect pre_bounds_[4], stroke_rect_;
    std::vector<int> slot_;  // tile -> index in stiles_, -1 if untouched this stroke
    std::vector<StrokeTile> stiles_;
    UndoEntry pending_;
};

}  // namespace paint
