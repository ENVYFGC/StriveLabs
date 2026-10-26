// StriveLabs menu, direction A: side menu on the left, the selected page on the
// right, StriveLabs colors, square corners. Included by imgui_overlay.cpp.
// Draws only from ModMenu::view(); mouse input goes back through ModMenu::click().
// Every window (main menu, popped-out pages, meters overlay) can be dragged,
// resized from its edges or bottom-right corner, minimized, and closed.

namespace menu_ui {
  // StriveLabs colors (sRGB of the HUD menu's linear colors)
  constexpr ImU32 RGBA(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
  // Theme colors: set every frame by apply_theme() from the Appearance settings.
  ImU32 PANEL     = RGBA(4, 6, 14, 240);
  ImU32 NAV_BG    = RGBA(9, 11, 20, 250);
  const ImU32 BORDER    = RGBA(33, 39, 60);
  ImU32 SECTION   = RGBA(18, 26, 44, 235);
  ImU32 RED       = RGBA(255, 0, 0);        // accent
  ImU32 RED_TEXT  = RGBA(255, 60, 60);      // accent, lighter, for titles
  ImU32 RED_LINE  = RGBA(255, 60, 60, 153);
  ImU32 CURSOR    = RGBA(255, 0, 0, 38);
  ImU32 FOCUS     = RGBA(255, 0, 0, 72);
  ImU32 ON_ACCENT = RGBA(232, 234, 240);    // text and knobs drawn on the accent color
  ImU32 OVERLAY_BG = RGBA(4, 6, 14, 215);   // meters and frame bar background
  ImU32 BAR_BG     = RGBA(4, 6, 14, 200);
  float opacity = 0.94f;
  const ImU32 HOVER     = RGBA(255, 255, 255, 12);
  const ImU32 DIVIDER   = RGBA(255, 255, 255, 16);
  const ImU32 CHIP      = RGBA(24, 29, 44);
  const ImU32 TEXT      = RGBA(232, 234, 240);
  const ImU32 LABEL     = RGBA(205, 208, 216);
  const ImU32 MUTED     = RGBA(140, 146, 160);
  const ImU32 DIM       = RGBA(92, 98, 112);

  ImFont* f_body = nullptr;
  ImFont* f_bold = nullptr;
  ImFont* f_title = nullptr;
  ImFont* f_small = nullptr;
  ImFont* f_framebar = nullptr;
  ImFont* f_framebar_stats = nullptr;
  ImFont* f_meters = nullptr;
  ImFont* f_meters_bold = nullptr;
  float S = 1.0f;  // UI scale, 1.0 at 1080p

  // Sizes at 1080p, multiplied by S
  constexpr float ROW_H = 36, PAD = 14, HEAD = 32, NAV_W = 190, NAV_ROW = 34, CHIP_H = 22;

  ImTextureID logo = 0;      // StriveLabs_logo.png, set by imgui_overlay.cpp
  float logo_aspect = 1.0f;  // height / width

  std::string loaded_font;    // overlay font the fonts were built with ("" = Barlow)

  // Each window has its own size setting. S and the shared fonts follow the
  // window being drawn: call use() before drawing it.
  enum Elem { E_MENU, E_METERS, E_BAR, E_TIMELINE, E_COUNT };
  float SE[E_COUNT] = {1, 1, 1, 1};             // screen scale * that window's size
  float loaded_scales[E_COUNT] = {1, 1, 1, 1};  // sizes the fonts were built at
  struct FontSet { ImFont *body = nullptr, *bold = nullptr, *title = nullptr, *small_ = nullptr; };
  FontSet fs_menu, fs_timeline;
  ImFont* f_bar_title = nullptr;
  ImFont* f_input = nullptr;  // input bar letters
  ImFont* f_status = nullptr; // status line, only the screen scale (it has its own size setting)
  ImFont* f_meters_title = nullptr;

  void use(Elem e) {
    S = SE[e];
    const FontSet& fs = e == E_TIMELINE ? fs_timeline : fs_menu;
    f_body = fs.body; f_bold = fs.bold; f_title = fs.title; f_small = fs.small_;
  }

  void load_fonts(float screen_h) {
    const float base = screen_h > 0 ? screen_h / 1080.0f : 1.0f;
    for (int e = 0; e < E_COUNT; e++) {
      loaded_scales[e] = ModMenu::uiScale(e);
      SE[e] = base * loaded_scales[e];
    }
    ImGuiIO& io = ImGui::GetIO();
    const auto dir = ModMenu::dataDir() / "StriveLabs_fonts";
    auto add = [&](const char* file, float px, float s) -> ImFont* {
      const auto path = (dir / file).string();
      if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
      ImFontConfig cfg; cfg.OversampleH = 2;
      return io.Fonts->AddFontFromFileTTF(path.c_str(), px * s, &cfg);
    };
    auto fallback = [&](float px, float s) -> ImFont* {  // fonts missing: ImGui's own font
      ImFontConfig cfg; cfg.SizePixels = px * s;
      return io.Fonts->AddFontDefault(&cfg);
    };
    // Meters and frame bar: the font the player picked, if it is there and looks
    // like a .ttf/.otf. Otherwise Barlow. ImGui opens UTF-8 paths.
    loaded_font = ModMenu::overlayFont();
    std::string ov_path;
    if (!loaded_font.empty()) {
      const int n = MultiByteToWideChar(CP_UTF8, 0, loaded_font.c_str(), -1, nullptr, 0);
      std::wstring wname(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
      if (n > 0) MultiByteToWideChar(CP_UTF8, 0, loaded_font.c_str(), -1, wname.data(), n);
      const std::wstring wpath = (dir / wname).wstring();
      char magic[4] = {};
      std::ifstream in(wpath, std::ios::binary);
      if (in.read(magic, 4) && (!memcmp(magic, "OTTO", 4) || !memcmp(magic, "\0\1\0\0", 4) || !memcmp(magic, "true", 4))) {
        const int m = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        ov_path.assign(static_cast<size_t>(m > 0 ? m - 1 : 0), '\0');
        if (m > 0) WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, ov_path.data(), m, nullptr, nullptr);
      } else {
        COMBO_LOG("imgui: overlay font missing or not a .ttf/.otf, using Barlow");
      }
    }
    auto add_ov = [&](const char* file, float px, float s) -> ImFont* {
      ImFont* f = nullptr;
      if (!ov_path.empty()) {
        ImFontConfig cfg; cfg.OversampleH = 2;
        f = io.Fonts->AddFontFromFileTTF(ov_path.c_str(), px * s, &cfg);
      }
      if (!f) f = add(file, px, s);
      return f ? f : fallback(px, s);
    };
    auto menu_set = [&](float s) {
      FontSet fs;
      fs.body = add("Barlow-Medium.ttf", 17.0f, s);
      fs.bold = add("Barlow-SemiBold.ttf", 17.0f, s);
      fs.title = add("BarlowCondensed-Bold.ttf", 24.0f, s);
      fs.small_ = add("Barlow-Medium.ttf", 14.0f, s);
      if (!fs.body || !fs.bold || !fs.title || !fs.small_) {
        fs.body = fs.bold = fs.title = fs.small_ = fallback(14.0f, s);
        COMBO_LOG("imgui: StriveLabs_fonts not found, using the default font");
      }
      return fs;
    };
    fs_menu = menu_set(SE[E_MENU]);
    fs_timeline = std::fabs(SE[E_TIMELINE] - SE[E_MENU]) < 0.001f ? fs_menu : menu_set(SE[E_TIMELINE]);
    f_framebar = add_ov("Barlow-Medium.ttf", 20.0f, SE[E_BAR]);
    f_framebar_stats = add_ov("Barlow-Medium.ttf", 24.0f, SE[E_BAR]);
    f_bar_title = add_ov("Barlow-SemiBold.ttf", 20.0f, SE[E_BAR]);
    f_input = add_ov("Barlow-SemiBold.ttf", 32.0f, SE[E_BAR]);
    f_status = add_ov("Barlow-SemiBold.ttf", 48.0f, base);
    f_meters = add_ov("Barlow-Medium.ttf", 32.0f, SE[E_METERS]);
    f_meters_bold = add_ov("Barlow-SemiBold.ttf", 32.0f, SE[E_METERS]);
    f_meters_title = add_ov("Barlow-SemiBold.ttf", 20.0f, SE[E_METERS]);
    use(E_MENU);
  }

  void style() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = st.ChildRounding = st.FrameRounding = st.PopupRounding = 0.0f;
    st.ScrollbarRounding = st.GrabRounding = st.TabRounding = 0.0f;
    st.WindowBorderSize = 0.0f;
    st.WindowPadding = ImVec2(0, 0);
    st.ItemSpacing = ImVec2(0, 0);
    st.WindowMinSize = ImVec2(40, 20);
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(TEXT);
    // We draw our own square grip; hide ImGui's triangle
    st.Colors[ImGuiCol_ResizeGrip] = st.Colors[ImGuiCol_ResizeGripHovered] = st.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0, 0, 0, 0);
    ImGui::GetIO().ConfigWindowsResizeFromEdges = true;
  }

  // Small solid triangles stand in for arrow glyphs, so no font coverage is needed.
  void arrow(ImDrawList* dl, ImVec2 c, float r, bool right, ImU32 col) {
    if (right) dl->AddTriangleFilled(ImVec2(c.x - r * 0.6f, c.y - r), ImVec2(c.x - r * 0.6f, c.y + r), ImVec2(c.x + r * 0.7f, c.y), col);
    else       dl->AddTriangleFilled(ImVec2(c.x + r * 0.6f, c.y - r), ImVec2(c.x + r * 0.6f, c.y + r), ImVec2(c.x - r * 0.7f, c.y), col);
  }

  bool clicked_rect(const char* id, ImVec2 a, ImVec2 b) {
    ImGui::SetCursorScreenPos(a);
    return ImGui::InvisibleButton(id, ImVec2(std::max(b.x - a.x, 1.0f), std::max(b.y - a.y, 1.0f)));
  }

  // size 0 = the font's own size
  void text(ImDrawList* dl, ImFont* f, ImVec2 p, ImU32 col, const std::string& s, float size = 0.0f) {
    dl->AddText(f, size > 0.0f ? size : f->FontSize, p, col, s.c_str());
  }
  void text_outlined(ImDrawList* dl, ImFont* f, ImVec2 p, ImU32 col, const std::string& s, float size = 0.0f) {
    const float width = std::max(S, 1.0f);
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++)
        if (dx || dy) {
          // Equal radius on axes and diagonals, rather than a square stencil.
          const float step = (dx && dy) ? width * 0.70710678f : width;
          const int first_vertex = dl->VtxBuffer.Size;
          text(dl, f, p, RGBA(0, 0, 0), s, size);
          // AddText snaps positions to pixels. Offset the emitted vertices so
          // fractional shifts stay symmetric and are filtered by the renderer.
          for (int i = first_vertex; i < dl->VtxBuffer.Size; i++) {
            dl->VtxBuffer[i].pos.x += dx * step;
            dl->VtxBuffer[i].pos.y += dy * step;
          }
        }
    text(dl, f, p, col, s, size);
  }
  float text_w(ImFont* f, const std::string& s) { return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, s.c_str()).x; }
  std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
  }

  bool is_toggle(const MenuRowView& r) {
    if (r.choices.size() != 2) return false;
    const auto& a = r.choices[0]; const auto& b = r.choices[1];
    return (a == "Disabled" && b == "Enabled") || (a == "Off" && b == "On");
  }

  // Click helper: the menu's row 0 is Back, which the ImGui menu does not show,
  // so shift by one. On the root page, open the previewed page first.
  void send(const MenuView& v, MenuClick c) {
    if (v.nav_focus) ModMenu::instance().click(MenuClick{MenuClick::Type::OpenNav, v.nav, 0});
    c.a += 1;
    ModMenu::instance().click(c);
  }

  struct ScrollState { float scroll = 0; int last_cursor = -1; std::string key; };

  void draw_scrollbar(ImDrawList* dl, float x1, float top, float bottom, float content_h, float scroll) {
    const float view_h = std::max(bottom - top, 0.0f);
    const float max_scroll = content_h - view_h;
    if (max_scroll <= 0 || view_h <= 0) return;
    const float tx = x1 - 5 * S;
    const float thumb_h = std::min(view_h, std::max(view_h * view_h / content_h, 20 * S));
    const float thumb_y = top + (view_h - thumb_h) * (scroll / max_scroll);
    dl->AddRectFilled(ImVec2(tx, top), ImVec2(tx + 3 * S, bottom), CHIP);
    dl->AddRectFilled(ImVec2(tx, thumb_y), ImVec2(tx + 3 * S, thumb_y + thumb_h), RED);
  }

  // Rows of one page between top and bottom, scrolled with the wheel and kept on
  // the selected row. cursor = -1 when nothing is selected.
  void draw_rows(ImDrawList* dl, const std::vector<MenuRowView>& rows, int cursor, const std::string& key,
                 float px, float x1, float top, float bottom, ScrollState& st,
                 const std::function<void(MenuClick)>& sendf) {
    ImGuiIO& io = ImGui::GetIO();
    const float row_h = ROW_H * S, pad = PAD * S, ch = CHIP_H * S * 0.5f;
    const float section_h = 28 * S;
    std::vector<float> offsets;
    offsets.reserve(rows.size());
    float rows_h = 0;
    for (const auto& r : rows) {
      offsets.push_back(rows_h);
      rows_h += row_h + (r.section.empty() ? 0 : section_h);
    }
    const float view_h = std::max(bottom - top, 0.0f);
    if (key != st.key) { st.scroll = 0; st.last_cursor = -1; st.key = key; }
    float& scroll = st.scroll;
    if (ImGui::IsMouseHoveringRect(ImVec2(px, top), ImVec2(x1, bottom), false)) scroll -= io.MouseWheel * row_h;
    if (cursor != st.last_cursor && cursor >= 0 && cursor < static_cast<int>(rows.size())) {  // keep the selected control and heading in view
      const float rt = offsets[cursor];
      const float rb = rt + row_h + (rows[cursor].section.empty() ? 0 : section_h);
      if (rt < scroll) scroll = rt;
      if (rb > scroll + view_h) scroll = rb - view_h;
    }
    st.last_cursor = cursor;
    const float max_scroll = std::max(0.0f, rows_h - view_h);
    scroll = std::clamp(scroll, 0.0f, max_scroll);
    ImGui::PushClipRect(ImVec2(px, top), ImVec2(x1, bottom), true);

    float y = top - scroll;
    const float lx = px + pad, rx = x1 - pad - (max_scroll > 0 ? 6 * S : 0);
    for (int i = 0; i < static_cast<int>(rows.size()); i++) {
      const MenuRowView& r = rows[i];
      if (!r.section.empty()) {
        dl->AddRectFilled(ImVec2(px, y), ImVec2(x1, y + section_h), SECTION);
        dl->AddRectFilled(ImVec2(px, y), ImVec2(px + 3 * S, y + section_h), RED_LINE);
        dl->PushClipRect(ImVec2(lx, y), ImVec2(std::max(lx, rx), y + section_h), true);
        text(dl, f_small, ImVec2(lx, y + (section_h - f_small->FontSize) * 0.5f), MUTED, upper(r.section));
        dl->PopClipRect();
        y += section_h;
      }
      const ImVec2 a(px, y), b(x1, y + row_h);
      if (b.y <= top || a.y >= bottom) { y += row_h; continue; }  // off screen: no drawing, no clicks
      const bool sel = i == cursor;
      const float cy = y + row_h * 0.5f;
      ImGui::PushID(i);
      if (sel) {
        dl->AddRectFilled(a, b, CURSOR);
        dl->AddRectFilled(a, ImVec2(b.x, a.y + 2 * S), RED_LINE);
        dl->AddRectFilled(a, ImVec2(a.x + 3 * S, b.y), RED);
      }
      dl->AddLine(ImVec2(a.x + pad, b.y - 1), ImVec2(b.x - pad, b.y - 1), DIVIDER);
      float ctrl_left = rx;  // leftmost x of this row's controls; the label is cut there

      if (r.kind == MenuRowView::Kind::Choice && is_toggle(r)) {
        const float tw = 36 * S, th = 18 * S;
        const ImVec2 ta(rx - tw, cy - th * 0.5f), tb(rx, cy + th * 0.5f);
        ctrl_left = ta.x;
        const bool on = r.choice == 1;
        dl->AddRectFilled(ta, tb, on ? RED : CHIP);
        const float k = th - 6 * S;
        const float kx = on ? tb.x - 3 * S - k : ta.x + 3 * S;
        dl->AddRectFilled(ImVec2(kx, ta.y + 3 * S), ImVec2(kx + k, ta.y + 3 * S + k), on ? ON_ACCENT : MUTED);
        if (clicked_rect("tgl", ta, tb)) sendf(MenuClick{MenuClick::Type::SetChoice, i, on ? 0 : 1});
      } else if (r.kind == MenuRowView::Kind::Choice && r.choices.size() <= 6 && !r.choices.empty()) {
        float total = 0;
        for (const auto& c : r.choices) total += text_w(f_small, c) + 16 * S;
        float cx = rx - total - 3 * S * (r.choices.size() - 1);
        ctrl_left = cx;
        if (!r.value.empty()) {
          ctrl_left = cx - text_w(f_small, r.value) - 10 * S;
          text(dl, f_small, ImVec2(ctrl_left, cy - f_small->FontSize * 0.5f), DIM, r.value);
        }
        for (int c = 0; c < static_cast<int>(r.choices.size()); c++) {
          const float cw = text_w(f_small, r.choices[c]) + 16 * S;
          const ImVec2 ca(cx, cy - ch), cb(cx + cw, cy + ch);
          const bool act = c == r.choice;
          dl->AddRectFilled(ca, cb, act ? RED : CHIP);
          text(dl, f_small, ImVec2(ca.x + 8 * S, cy - f_small->FontSize * 0.5f), act ? ON_ACCENT : MUTED, r.choices[c]);
          ImGui::PushID(c);
          if (clicked_rect("chip", ca, cb)) sendf(MenuClick{MenuClick::Type::SetChoice, i, c});
          ImGui::PopID();
          cx += cw + 3 * S;
        }
      } else if (r.kind == MenuRowView::Kind::Choice || r.kind == MenuRowView::Kind::Number) {
        const std::string val = r.kind == MenuRowView::Kind::Choice && r.choice >= 0 && r.choice < static_cast<int>(r.choices.size())
                                  ? r.choices[r.choice] : r.value;
        const float aw = CHIP_H * S;
        const float vw = std::max(text_w(f_small, val), 96 * S);
        const ImVec2 la(rx - vw - 2 * aw - 8 * S, cy - ch), lb(la.x + aw, cy + ch);
        const ImVec2 ra(rx - aw, cy - ch), rb(rx, cy + ch);
        ctrl_left = la.x;
        dl->AddRectFilled(la, lb, CHIP); dl->AddRectFilled(ra, rb, CHIP);
        arrow(dl, ImVec2((la.x + lb.x) * 0.5f, cy), 4 * S, false, sel ? TEXT : MUTED);
        arrow(dl, ImVec2((ra.x + rb.x) * 0.5f, cy), 4 * S, true, sel ? TEXT : MUTED);
        text(dl, f_small, ImVec2(lb.x + (ra.x - lb.x - text_w(f_small, val)) * 0.5f, cy - f_small->FontSize * 0.5f), TEXT, val);
        if (clicked_rect("l", la, lb)) sendf(MenuClick{MenuClick::Type::Row, i, 0});
        if (clicked_rect("r", ra, rb)) sendf(MenuClick{MenuClick::Type::Row, i, 1});
      } else if (r.kind == MenuRowView::Kind::Action || r.kind == MenuRowView::Kind::Open) {
        const float bw = text_w(f_small, r.value) + 22 * S;
        const ImVec2 ba(rx - bw, cy - ch), bb(rx, cy + ch);
        ctrl_left = ba.x;
        if (clicked_rect("act", ba, bb)) sendf(MenuClick{MenuClick::Type::Row, i, 1});
        const bool hov = ImGui::IsItemHovered();
        dl->AddRectFilled(ba, bb, hov || sel ? RED : CHIP);
        dl->AddRect(ba, bb, RED);
        text(dl, f_small, ImVec2(ba.x + 11 * S, cy - f_small->FontSize * 0.5f), hov || sel ? ON_ACCENT : TEXT, r.value);
      } else if (r.kind == MenuRowView::Kind::Bind) {
        if (r.capturing) {
          const std::string msg = "Press a key or button   Esc cancels   Backspace clears";
          ctrl_left = rx - text_w(f_small, msg);
          text(dl, f_small, ImVec2(ctrl_left, cy - f_small->FontSize * 0.5f), RED_TEXT, msg);
        } else {
          const std::string pad_name = r.choices.empty() ? "none" : r.choices[0];
          float cx = rx;
          for (const std::string* s : {&pad_name, &r.value}) {
            const float cw = text_w(f_small, *s) + 16 * S;
            cx -= cw;
            dl->AddRectFilled(ImVec2(cx, cy - ch), ImVec2(cx + cw, cy + ch), CHIP);
            text(dl, f_small, ImVec2(cx + 8 * S, cy - f_small->FontSize * 0.5f), *s == "none" ? DIM : TEXT, *s);
            cx -= 4 * S;
          }
          ctrl_left = cx;
          if (clicked_rect("bind", ImVec2(cx, a.y), b)) sendf(MenuClick{MenuClick::Type::Row, i, 1});
        }
      }
      dl->PushClipRect(ImVec2(lx, a.y), ImVec2(std::max(lx, ctrl_left - 8 * S), b.y), true);
      text(dl, sel ? f_bold : f_body, ImVec2(lx, cy - f_body->FontSize * 0.5f), sel ? TEXT : LABEL, r.label);
      dl->PopClipRect();
      ImGui::PopID();
      y += row_h;
    }
    ImGui::PopClipRect();

    // Square scrollbar, only when the page is longer than the pane
    draw_scrollbar(dl, x1, top, bottom, rows_h, scroll);
  }

  // ---- Window chrome: title strip, minimize, close, resize --------------------
  struct WinState { bool collapsed = false; float open_h = 0; int gen = 0; };
  std::atomic<int> g_layout_gen{0};  // bumped by Layout > Reset all window positions
  std::map<std::string, WinState> g_win;

  struct Frame {
    ImDrawList* dl = nullptr;
    ImVec2 o;           // top-left
    float w = 0, h = 0; // current size
    float head = 0;     // title strip height
    bool collapsed = false;
    bool closed = false;
  };

  // Starts a window with our square chrome. Always pair with end_window().
  // pos_anchor: (0,0) top-left, (0.5,0.5) centered on pos.
  Frame begin_window(const std::string& id, const std::string& title, ImVec2 def_pos, ImVec2 def_size,
                     ImVec2 min_size, bool interactive, ImU32 bg = PANEL, bool hide_decorations = false,
                     ImFont* title_font = nullptr, int title_mode = -1, int panel_mode = -1) {
    WinState& ws = g_win[id];
    // While the menu is closed: title 0 shown, 1 only its accent line, 2 hidden;
    // panel shown or not. -1 = follow "Hide decorations" like the other windows.
    // With the menu open everything is shown, so windows can still be moved.
    const int title_state = interactive ? 0 : title_mode >= 0 ? title_mode : (hide_decorations ? 2 : 0);
    const bool panel = interactive || (panel_mode >= 0 ? panel_mode == 0 : !hide_decorations);
    Frame f;
    f.head = HEAD * S;
    const int gen = g_layout_gen;
    const bool reset = ws.gen != gen;
    if (reset) { ws.gen = gen; ws.collapsed = false; ws.open_h = 0; }
    const ImGuiWindow* existing = ImGui::FindWindowByName(id.c_str());
    if (ws.collapsed) {
      const float w = existing ? existing->Size.x : def_size.x;
      ImGui::SetNextWindowSize(ImVec2(w, f.head), ImGuiCond_Always);
    } else {
      if (ws.open_h > 0 && existing) {  // just restored: back to the height it had
        ImGui::SetNextWindowSize(ImVec2(existing->Size.x, ws.open_h), ImGuiCond_Always);
        ws.open_h = 0;
      } else {
        ImGui::SetNextWindowSize(def_size, reset ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
      }
      ImGui::SetNextWindowSizeConstraints(min_size, ImVec2(FLT_MAX, FLT_MAX));
    }
    if (!(ImGui::GetCurrentContext()->NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasPos))
      ImGui::SetNextWindowPos(def_pos, reset ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ws.collapsed) flags |= ImGuiWindowFlags_NoResize;
    if (!interactive) flags |= ImGuiWindowFlags_NoInputs;
    if (title_state != 0 || !panel) flags |= ImGuiWindowFlags_NoBackground;
    ImGui::Begin(id.c_str(), nullptr, flags);
    f.dl = ImGui::GetWindowDrawList();
    f.o = ImGui::GetWindowPos();
    f.w = ImGui::GetWindowSize().x;
    f.h = ImGui::GetWindowSize().y;
    f.collapsed = ws.collapsed;
    ImDrawList* dl = f.dl;
    const ImVec2 o = f.o;

    // Title as a line or hidden: the title space stays empty and the content keeps
    // its place, so nothing moves when the menu opens again.
    if (title_state != 0) {
      const float top = o.y + f.head - (title_state == 1 ? 3 * S : 0.0f);
      if (panel) {
        dl->AddRectFilled(ImVec2(o.x, top), ImVec2(o.x + f.w, o.y + f.h), bg);
        dl->AddRect(ImVec2(o.x, top), ImVec2(o.x + f.w, o.y + f.h), BORDER);
      }
      if (title_state == 1) dl->AddRectFilled(ImVec2(o.x, top), ImVec2(o.x + f.w, top + 3 * S), RED);
      return f;
    }

    if (panel) dl->AddRectFilled(o, ImVec2(o.x + f.w, o.y + f.h), bg);
    dl->AddRectFilled(o, ImVec2(o.x + f.w, o.y + f.head), SECTION);
    if (panel) dl->AddRect(o, ImVec2(o.x + f.w, o.y + f.h), BORDER);
    dl->AddRectFilled(o, ImVec2(o.x + f.w, o.y + 3 * S), RED);
    // Buttons, right to left: close, minimize
    const float bs = f.head - 12 * S;
    if (!title.empty()) {
      if (!title_font) title_font = f_bold;
      const float title_end = interactive ? o.x + f.w - 8 * S - 2 * (bs + 4 * S) - 6 * S
                                          : o.x + f.w - PAD * S;
      dl->PushClipRect(ImVec2(o.x, o.y), ImVec2(std::max(o.x, title_end), o.y + f.head), true);
      text(dl, title_font, ImVec2(o.x + PAD * S, o.y + (f.head - title_font->FontSize) * 0.5f + 1 * S), RED_TEXT, upper(title));
      dl->PopClipRect();
    }
    float bx = o.x + f.w - 8 * S - bs;
    const float by = o.y + 6 * S + 1 * S;
    auto square_button = [&](const char* bid, bool is_close) {
      const ImVec2 ba(bx, by), bb(bx + bs, by + bs);
      const bool hit = clicked_rect(bid, ba, bb);
      const bool hov = ImGui::IsItemHovered();
      if (hov) dl->AddRectFilled(ba, bb, is_close ? RED : CHIP);
      const ImU32 col = hov ? TEXT : MUTED;
      const float m = bs * 0.3f;
      if (is_close) {
        dl->AddLine(ImVec2(ba.x + m, ba.y + m), ImVec2(bb.x - m, bb.y - m), col, 1.6f * S);
        dl->AddLine(ImVec2(bb.x - m, ba.y + m), ImVec2(ba.x + m, bb.y - m), col, 1.6f * S);
      } else if (ws.collapsed) {  // restore: empty square
        dl->AddRect(ImVec2(ba.x + m, ba.y + m), ImVec2(bb.x - m, bb.y - m), col, 0.0f, 0, 1.6f * S);
      } else {                    // minimize: bar
        dl->AddRectFilled(ImVec2(ba.x + m, bb.y - m - 2 * S), ImVec2(bb.x - m, bb.y - m), col);
      }
      bx -= bs + 4 * S;
      return hit;
    };
    // Passive overlays keep their title and content, but have no window controls.
    if (interactive) {
      if (square_button("close", true)) f.closed = true;
      if (square_button("min", false)) {
        if (!ws.collapsed) { ws.collapsed = true; ws.open_h = f.h; }
        else { ws.collapsed = false; }
      }
    }

    // Our own square resize grip, bottom-right
    if (interactive && !ws.collapsed) {
      const float g = 10 * S;
      const ImVec2 c(o.x + f.w - 3 * S, o.y + f.h - 3 * S);
      dl->AddRectFilled(ImVec2(c.x - 3 * S, c.y - g), c, RED_LINE);
      dl->AddRectFilled(ImVec2(c.x - g, c.y - 3 * S), c, RED_LINE);
    }
    return f;
  }

  void end_window() { ImGui::End(); }

  // ---- Popped-out pages and the meters overlay window -------------------------
  unsigned g_popped = 0;  // bit per MenuPage shown in its own window
  bool popped_has(int page) { return (g_popped >> page) & 1u; }
  void toggle_popped(int page) {
    g_popped ^= 1u << page;
    ModMenu::instance().setPopped(g_popped);
  }

  // Meter lines from the game thread (imgui_overlay::setTensionLines)
  std::mutex t_mutex;
  std::vector<std::string> t_lines;
  unsigned long long t_time = 0;

  void draw_panel(const MenuPanelView& pv) {
    const std::string id = pv.title + "##sl_panel" + std::to_string(pv.page);
    const float status_h = pv.status.empty() ? 0.0f : 26 * S;
    float def_h = HEAD * S + status_h + 6 * S;
    for (size_t i = 0; i < std::min(pv.rows.size(), size_t{8}); i++)
      def_h += (ROW_H + (pv.rows[i].section.empty() ? 0 : 28)) * S;
    Frame f = begin_window(id, pv.title, ImVec2(60 * S + 36 * S * pv.page, 100 * S + 36 * S * pv.page),
                           ImVec2(470 * S, def_h), ImVec2(300 * S, HEAD * S + ROW_H * S * 2), true);
    if (f.closed) toggle_popped(pv.page);
    if (!f.collapsed) {
      static std::map<int, ScrollState> scrolls;
      const int page = pv.page;
      const float bottom = f.o.y + f.h - status_h - 6 * S;
      draw_rows(f.dl, pv.rows, -1, pv.title, f.o.x, f.o.x + f.w, f.o.y + f.head, bottom, scrolls[page],
                [page](MenuClick c) { c.a += 1; c.page = page; ModMenu::instance().click(c); });
      if (!pv.status.empty()) text(f.dl, f_small, ImVec2(f.o.x + PAD * S, bottom + 6 * S), MUTED, pv.status);
    }
    end_window();
  }

  // ---- Frame bar window ------------------------------------------------------
  std::mutex b_mutex;
  imgui_overlay::FrameBarView b_view;
  std::string b_info[2];
  unsigned long long b_time = 0;
  ImVec4 bar_rect(0, 0, 0, 0);   // x, y, w, h of the frame bar window this frame
  int bar_frame = -1;            // ImGui frame the bar was drawn on

  void draw_framebar(bool interactive, bool hide_decorations, int title_mode, int panel_mode) {
    use(E_BAR);
    imgui_overlay::FrameBarView fb;
    std::string info[2];
    {
      std::lock_guard<std::mutex> lock(b_mutex);
      if (GetTickCount64() - b_time > 300) return;  // bar off, or a loading screen
      fb = b_view;
      info[0] = b_info[0]; info[1] = b_info[1];
    }
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 def_size(1660 * S, 150 * S);
    const float lh = f_framebar_stats->FontSize + 4 * S;
    Frame f = begin_window("Frame bar##sl_bar", "Frame bar",
                           ImVec2(io.DisplaySize.x * 0.5f - def_size.x * 0.5f, io.DisplaySize.y * 0.8f - def_size.y * 0.5f),
                           def_size, ImVec2(360 * S, HEAD * S + 2 * lh + (fb.inputs ? 220 : 60) * S), interactive, BAR_BG, hide_decorations, f_bar_title, title_mode, panel_mode);
    if (f.closed) ModMenu::instance().click(MenuClick{MenuClick::Type::BarWindow, 0, 0});
    bar_rect = ImVec4(f.o.x, f.o.y, f.w, f.h);
    bar_frame = ImGui::GetFrameCount();
    if (!f.collapsed) {
      ImDrawList* dl = f.dl;
      const float pad = 10 * S, gap = 4 * S;
      const float x0 = f.o.x + pad, x1 = f.o.x + f.w - pad;
      float y = f.o.y + f.head + 3 * S;
      // Each shown side: its stats line, its bar and, if on, an input row with three
      // lanes (direction, buttons, macro) sharing the height with the bars.
      const int nbars = int(fb.show[0]) + int(fb.show[1]);
      const int nin = int(fb.show_inputs[0]) + int(fb.show_inputs[1]);
      const float avail = std::max(0.0f, f.o.y + f.h - 4 * S - y - nbars * lh - (nbars > 1 ? gap : 0.0f));
      const float in_h = nin ? std::min((x1 - x0) / 120.0f * 8.0f, avail * (nin > 1 ? 0.35f : 0.5f)) : 0.0f;
      const float bar_h = std::max((avail - nin * (in_h + gap)) / static_cast<float>(std::max(nbars, 1)), 6 * S);
      // Like the HUD bar, fit 120 cells plus a trailing gap into the background.
      // Keep enough space for the delimiter and crossup markers at either edge.
      const float desired_sp = std::max((x1 - x0) * (4.0f / (120 * 24.0f + 4.0f)), std::max(1.5f * S, 1.0f));
      const float sp = std::min(desired_sp, (x1 - x0) / (120 * 2.0f + 1.0f));
      const float cell = (x1 - x0 - sp) / 120.0f;
      const float seg_w = cell - sp;
      const float vsp = bar_h * (4.0f / 44.0f);
      auto stats = [&](const std::string& s, float ty) {
        const float outline = std::max(S, 1.0f);
        dl->PushClipRect(ImVec2(x0 - outline, ty - outline), ImVec2(x1 + outline, ty + lh + outline), true);
        text_outlined(dl, f_framebar_stats, ImVec2(x0, ty), TEXT, s);
        dl->PopClipRect();
      };
      auto bar = [&](int row, float top) {
        dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, top + bar_h), fb.bg);
        const float sy = top + vsp, sh = bar_h - 2 * vsp, bt = std::min(sp, std::max(1.5f * S, 1.0f));
        // One block: a stretch has its cells joined, all but its last few (as many as
        // were set when it began). A stretch ends at a segment end or an empty cell; one
        // that runs past the right end goes on at the left, so that part is all block.
        bool join[120] = {};
        if (fb.blocks) {
          for (int s = 0; s < 120;) {
            if (!fb.rows[row][s].on) { s++; continue; }
            int e = s;
            while (!(fb.rows[row][e].mods & 4) && e < 119 && fb.rows[row][e + 1].on) e++;
            const bool wraps = e == 119 && !(fb.rows[row][e].mods & 4) && fb.rows[row][0].on;
            for (int j = s; j < (wraps ? e : e - fb.rows[row][e].keep); j++) join[j] = true;
            s = e + 1;
          }
        }
        for (int i = 0; i < 120; i++) {
          const auto& c = fb.rows[row][i];
          if (!c.on) continue;
          const float left = x0 + cell * i + sp;
          if (c.mods & 2) dl->AddRectFilled(ImVec2(left - bt, sy - bt), ImVec2(left + seg_w + bt, sy + sh + bt), fb.bg);
          dl->AddRectFilled(ImVec2(left, sy), ImVec2(left + seg_w, sy + sh), c.color);
          if (join[i]) dl->AddRectFilled(ImVec2(left + seg_w, sy), ImVec2(left + cell, sy + sh), c.color);
          if (fb.perfect && (c.mods & 0x20)) {
            // White outline, and a white cap on top with a black star.
            dl->AddRect(ImVec2(left - bt * 0.5f, sy - bt * 0.5f), ImVec2(left + seg_w + bt * 0.5f, sy + sh + bt * 0.5f), RGBA(255, 255, 255), 0, 0, bt);
            const float ch = std::min(seg_w, sh * 0.45f), r = ch * 0.42f;
            dl->AddRectFilled(ImVec2(left, sy), ImVec2(left + seg_w, sy + ch), RGBA(255, 255, 255));
            ImVec2 p[10];
            for (int k = 0; k < 10; k++) {
              const float a = -IM_PI / 2 + k * IM_PI / 5, rr = (k & 1) ? r * 0.4f : r;
              p[k] = ImVec2(left + seg_w * 0.5f + std::cos(a) * rr, sy + ch * 0.5f + std::sin(a) * rr);
            }
            dl->AddConcavePolyFilled(p, 10, RGBA(0, 0, 0));
          }
          if (fb.delim && (c.mods & 4)) dl->AddRectFilled(ImVec2(left + seg_w, sy - bt), ImVec2(left + seg_w + bt, sy + sh + bt), RGBA(255, 255, 255));
          if (fb.cancels && (c.mods & 1)) dl->AddRectFilled(ImVec2(left, sy + sh * 2 / 3), ImVec2(left + seg_w, sy + sh), RGBA(255, 255, 255));
          if (fb.crossups && (c.mods & 8)) dl->AddRectFilled(ImVec2(left + seg_w - bt, sy - bt), ImVec2(left + seg_w + bt, sy + sh + bt), RED);
        }
        for (int i = 0; i < 120; i++) {  // numbers on top of the cells
          const auto& c = fb.rows[row][i];
          if (!c.on || c.trunc <= 0) continue;
          const std::string n = std::to_string(c.trunc);
          const float tx = x0 + cell * (i + 1) - text_w(f_framebar, n);
          const float ty = sy + (sh - f_framebar->FontSize) * 0.5f;
          text_outlined(dl, f_framebar, ImVec2(tx, ty), TEXT, n);
        }
      };
      // Input row: arrow for the direction, P K S H D letters (bright = new
      // press, faint = held), and a box for a macro (dash, RC, FD, Respect).
      struct Btn { unsigned short bit; const char* s; ImU32 col; };
      static const Btn BTNS[] = {{0x10, "P", RGBA(232, 90, 168)}, {0x20, "K", RGBA(61, 139, 255)},
                                 {0x40, "S", RGBA(63, 195, 90)},  {0x80, "H", RGBA(232, 70, 60)},
                                 {0x100, "D", RGBA(243, 154, 30)}};
      static const Btn MACROS[] = {{0x400, "\xC2\xBB", RGBA(90, 99, 114)}, {0x800, "R", RGBA(142, 68, 214)},
                                   {0x8000, "F", RGBA(31, 163, 163)}, {0x200, "T", RGBA(184, 134, 43)}};
      static const char* MACRO_NAMES[] = {"Dash", "Roman Cancel", "Faultless Defense", "Respect"};
      auto with_alpha = [](ImU32 c, int a) { return (c & 0x00FFFFFF) | (static_cast<ImU32>(a) << 24); };
      auto inputs = [&](int row, float top) {
        dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, top + in_h), fb.bg);
        // Three lanes of the same size in every row: direction, buttons, macro.
        // Symbols only on the frame an input starts; while it is held, a thin line
        // carries on. Buttons active on the same frame share the button lane, so only
        // that frame gets smaller letters. The next cells are usually empty, so a
        // symbol may spread into them.
        const float u = in_h / 3.0f;
        const float sym = std::min(u * 0.9f, cell * 2.6f);
        const float fs = std::min(f_input->FontSize, sym * 1.1f);
        const float thin = std::max(1.0f, u * 0.08f);
        auto hold_line = [&](float left, float ly, ImU32 col) {
          dl->AddRectFilled(ImVec2(left - sp, ly - thin), ImVec2(left + seg_w, ly + thin), col);
        };
        int prev_dir = -1;
        unsigned short prev_macro = 0;
        int hovered = -1;
        for (int i = 0; i < 120; i++) {
          const auto& c = fb.rows[row][i];
          if (!c.on || c.dir == 0) {
            prev_dir = -1; prev_macro = 0;
            continue;
          }
          const float left = x0 + cell * i + sp, cx = left + seg_w * 0.5f;
          if (interactive && ImGui::IsMouseHoveringRect(ImVec2(left, top), ImVec2(left + seg_w, top + in_h), false)) hovered = i;
          // Direction
          const float ay = top + u * 0.5f;
          if (c.dir != 5 && c.dir == prev_dir) {
            hold_line(left, ay, with_alpha(TEXT, 110));
          } else if (c.dir != 5) {
            const int col = (c.dir - 1) % 3 - 1, rw = (c.dir - 1) / 3 - 1;
            float vx = static_cast<float>(col), vy = static_cast<float>(-rw);
            const float len = std::sqrt(vx * vx + vy * vy); vx /= len; vy /= len;
            const float r = sym * 0.42f;
            const ImVec2 tip(cx + vx * r, ay + vy * r), tail(cx - vx * r, ay - vy * r);
            dl->AddLine(tail, tip, TEXT, std::max(1.0f, sym * 0.12f));
            const float hw = r * 0.6f;
            dl->AddTriangleFilled(tip, ImVec2(tip.x - vx * hw - vy * hw * 0.8f, tip.y - vy * hw + vx * hw * 0.8f),
                                  ImVec2(tip.x - vx * hw + vy * hw * 0.8f, tip.y - vy * hw - vx * hw * 0.8f), TEXT);
          }
          prev_dir = c.dir;
          // Buttons: every active one gets an equal share of the button lane, in P K S H D order
          int n = 0;
          for (const auto& b : BTNS) if ((c.held | c.pressed) & b.bit) n++;
          const float share = u / static_cast<float>(std::max(n, 1));
          const float bfs = std::min(fs, share * 1.1f);
          int slot = 0;
          for (const auto& b : BTNS) {
            if (!((c.held | c.pressed) & b.bit)) continue;
            const float ly = top + u + share * slot++;
            if (c.pressed & b.bit) {
              const float tw = f_input->CalcTextSizeA(bfs, FLT_MAX, 0.0f, b.s).x;
              dl->AddText(f_input, bfs, ImVec2(cx - tw * 0.5f, ly + (share - bfs) * 0.5f), b.col, b.s);
            } else {
              hold_line(left, ly + share * 0.5f, with_alpha(b.col, 150));
            }
          }
          // Macro: a box on the frame it starts, a line while held
          unsigned short macro = 0;
          for (const auto& m : MACROS) {
            if (!((c.held | c.pressed) & m.bit)) continue;
            macro = m.bit;
            const float by = top + in_h - u;
            if ((c.pressed & m.bit) || prev_macro != m.bit) {
              const float bw = std::min(sym, cell * 2.6f - sp);
              dl->AddRectFilled(ImVec2(cx - bw * 0.5f, by + (u - sym) * 0.5f), ImVec2(cx + bw * 0.5f, by + (u + sym) * 0.5f), m.col);
              const float tw = f_input->CalcTextSizeA(fs, FLT_MAX, 0.0f, m.s).x;
              dl->AddText(f_input, fs, ImVec2(cx - tw * 0.5f, by + (u - fs) * 0.5f), RGBA(255, 255, 255), m.s);
            } else {
              hold_line(left, by + u * 0.5f, with_alpha(m.col, 170));
            }
            break;
          }
          prev_macro = macro;
        }
        if (hovered >= 0) {  // details for the cell under the mouse (menu open)
          const auto& c = fb.rows[row][hovered];
          std::string pressed, held;
          auto add = [](std::string& s, const char* t) { if (!s.empty()) s += " + "; s += t; };
          for (const auto& b : BTNS) { if (c.pressed & b.bit) add(pressed, b.s); else if (c.held & b.bit) add(held, b.s); }
          for (int k = 0; k < 4; k++) { if (c.pressed & MACROS[k].bit) add(pressed, MACRO_NAMES[k]); else if (c.held & MACROS[k].bit) add(held, MACRO_NAMES[k]); }
          std::string t = std::string(row == 0 ? "P1" : "P2") + ", direction " + std::to_string(c.dir);
          if (!pressed.empty()) t += "\nPressed: " + pressed;
          if (!held.empty()) t += "\nHeld: " + held;
          if (c.trunc > 0) t += "\nCovers " + std::to_string(c.trunc) + " frames";
          ImGui::SetTooltip("%s", t.c_str());
        }
      };
      if (fb.show[0]) {
        stats(info[0], y);          y += lh;
        bar(0, y);                  y += bar_h;
        if (fb.show_inputs[0]) { y += gap; inputs(0, y); y += in_h; }
      }
      if (fb.show[1]) {
        if (fb.show[0]) y += gap;
        bar(1, y);                  y += bar_h;
        if (fb.show_inputs[1]) { y += gap; inputs(1, y); y += in_h; }
        stats(info[1], y + 1 * S);
      }
    }
    end_window();
  }

  // The meters overlay as its own window. Always on screen while enabled; it can
  // be moved, resized or minimized only while the F5 menu is open.
  void draw_meters(bool interactive, bool right_side, bool snap, bool hide_decorations) {
    use(E_METERS);
    std::vector<std::string> lines;
    {
      std::lock_guard<std::mutex> lock(t_mutex);
      if (GetTickCount64() - t_time > 300) return;  // overlay hidden (loading screen, disabled)
      lines = t_lines;
    }
    if (lines.empty()) return;
    ImGuiIO& io = ImGui::GetIO();
    // Line pitch 1.4x the font size, like the game's own training text (Damage, Combo Damage)
    const float lh = std::max(f_meters->FontSize, f_meters_bold->FontSize) * 1.4f, pad = 12 * S;
    const float def_h = HEAD * S + lh * static_cast<float>(lines.size()) + pad;
    // Snapped: sit right on top of the frame bar, on the Overlay Side edge. While the
    // mouse is held on the window (resizing or dragging it), the snap waits, so it
    // doesn't fight the resize; on release it snaps back at the new size.
    static bool held = false;
    {
      ImGuiContext& g = *ImGui::GetCurrentContext();
      const ImGuiWindow* mw = ImGui::FindWindowByName("Meters##sl_tension");
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mw && g.HoveredWindow == mw) held = true;
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) held = false;
    }
    if (snap && !held && bar_frame == ImGui::GetFrameCount()) {
      if (const ImGuiWindow* win = ImGui::FindWindowByName("Meters##sl_tension")) {
        const float x = right_side ? bar_rect.x + bar_rect.z - win->Size.x : bar_rect.x;
        ImGui::SetNextWindowPos(ImVec2(x, bar_rect.y - win->Size.y - 4 * S), ImGuiCond_Always);
      }
    }
    // Overlay Side: when it changes, jump to that side of the screen at the same height.
    static int last_side = -1;
    const int side = right_side ? 1 : 0;
    if (!snap && last_side != -1 && side != last_side) {
      if (const ImGuiWindow* win = ImGui::FindWindowByName("Meters##sl_tension")) {
        const float x = right_side ? io.DisplaySize.x - win->Size.x - 40 * S : 40 * S;
        ImGui::SetNextWindowPos(ImVec2(x, win->Pos.y), ImGuiCond_Always);
      }
    }
    last_side = side;
    Frame f = begin_window("Meters##sl_tension", "Meters", ImVec2(40 * S, io.DisplaySize.y * 0.40f),
                           ImVec2(300 * S, def_h), ImVec2(200 * S, HEAD * S + lh * 2), interactive, OVERLAY_BG, hide_decorations, f_meters_title);
    if (f.closed) ModMenu::instance().click(MenuClick{MenuClick::Type::TensionWindow, 0, 0});
    if (!f.collapsed) {
      const float top = f.o.y + f.head + 4 * S, bottom = f.o.y + f.h - 4 * S;
      static float scroll = 0;
      const float content = lh * static_cast<float>(lines.size());
      const float view_h = std::max(bottom - top, 0.0f);
      const float max_scroll = std::max(0.0f, content - view_h);
      if (interactive && ImGui::IsMouseHoveringRect(ImVec2(f.o.x, top), ImVec2(f.o.x + f.w, bottom), false)) scroll -= io.MouseWheel * lh;
      scroll = std::clamp(scroll, 0.0f, max_scroll);
      // Reserve the same space even when the menu is closed, so text stays put.
      const float rx = f.o.x + f.w - pad - (max_scroll > 0 ? 6 * S : 0);
      ImGui::PushClipRect(ImVec2(f.o.x, top), ImVec2(f.o.x + f.w, bottom), true);
      float y = top - scroll;
      for (const auto& line : lines) {
        const auto cut = line.find(": ");
        const std::string label = cut == std::string::npos ? line : line.substr(0, cut);
        const std::string value = cut == std::string::npos ? std::string() : line.substr(cut + 2);
        const bool hot = value.rfind("STICK", 0) == 0;
        const float vx = rx - text_w(f_meters_bold, value);
        text_outlined(f.dl, f_meters_bold, ImVec2(vx, y + (lh - f_meters_bold->FontSize) * 0.5f), hot ? RED_TEXT : TEXT, value);
        f.dl->PushClipRect(ImVec2(f.o.x, y), ImVec2(std::max(f.o.x, vx - 8 * S), y + lh), true);
        text_outlined(f.dl, f_meters, ImVec2(f.o.x + pad, y + (lh - f_meters->FontSize) * 0.5f), hot ? RED_TEXT : TEXT, label);
        f.dl->PopClipRect();
        y += lh;
      }
      ImGui::PopClipRect();
      if (interactive) draw_scrollbar(f.dl, f.o.x + f.w, top, bottom, content, scroll);
    }
    end_window();
  }


  // ---- Timeline window: rewind, set point, play / pause, step, record ---------
  std::atomic<bool> g_timeline_visible{false};  // mouse goes to ImGui while it's up

  bool icon_button(ImDrawList* dl, const char* id, ImVec2 a, ImVec2 b, bool active, bool enabled,
                   const char* tip, const std::function<void(ImVec2, ImU32)>& icon) {
    const bool hit = clicked_rect(id, a, b) && enabled;
    const bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(a, b, active ? RED : (hov && enabled ? RGBA(40, 47, 68) : CHIP));
    icon(ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), !enabled ? DIM : (active ? ON_ACCENT : hov ? TEXT : LABEL));
    if (hov) ImGui::SetTooltip("%s", tip);
    return hit;
  }

  void draw_timeline(const MenuView& v) {
    use(E_TIMELINE);
    if (!v.timeline_on || v.mode == 0) { g_timeline_visible = false; return; }
    g_timeline_visible = true;
    ImGuiIO& io = ImGui::GetIO();
    Frame f = begin_window("Timeline##sl_timeline", "Timeline",
                           ImVec2(io.DisplaySize.x * 0.5f - 270 * S, io.DisplaySize.y - 460 * S),
                           ImVec2(540 * S, 420 * S), ImVec2(380 * S, (HEAD + 92) * S), true);
    if (f.closed) ModMenu::instance().click(MenuClick{MenuClick::Type::TlShow, 0, 0});
    if (!f.collapsed) {
      ImDrawList* dl = f.dl;
      const float pad = PAD * S, bw = 38 * S, bh = 30 * S, gap = 4 * S;
      float x = f.o.x + pad;
      const float y = f.o.y + f.head + 10 * S;
      const float r = 7 * S;
      auto next = [&](float w) { const ImVec2 a(x, y); x += w + gap; return std::make_pair(a, ImVec2(a.x + w, a.y + bh)); };

      auto [ra, rb] = next(bw);   // rewind: bar + two triangles
      if (icon_button(dl, "rw", ra, rb, false, true, "Rewind to your saved point (F9)", [&](ImVec2 c, ImU32 col) {
            dl->AddRectFilled(ImVec2(c.x - r - 2 * S, c.y - r), ImVec2(c.x - r, c.y + r), col);
            arrow(dl, ImVec2(c.x - 1 * S, c.y), r * 0.8f, false, col);
            arrow(dl, ImVec2(c.x + r - 1 * S, c.y), r * 0.8f, false, col); }))
        ModMenu::instance().click(MenuClick{MenuClick::Type::TlRewind});
      auto [sa, sb] = next(bw);   // set point: flag
      if (icon_button(dl, "sp", sa, sb, false, true, "Save the current moment as your rewind point (F6)", [&](ImVec2 c, ImU32 col) {
            dl->AddRectFilled(ImVec2(c.x - r * 0.7f, c.y - r), ImVec2(c.x - r * 0.7f + 2 * S, c.y + r), col);
            dl->AddRectFilled(ImVec2(c.x - r * 0.7f, c.y - r), ImVec2(c.x + r * 0.9f, c.y), col); }))
        ModMenu::instance().click(MenuClick{MenuClick::Type::TlSetPoint});
      auto [pa, pb] = next(bw);   // play / pause
      if (icon_button(dl, "pp", pa, pb, v.paused, true, v.paused ? "Play" : "Pause", [&](ImVec2 c, ImU32 col) {
            if (v.paused) arrow(dl, ImVec2(c.x + 1 * S, c.y), r, true, col);
            else {
              dl->AddRectFilled(ImVec2(c.x - r * 0.7f, c.y - r), ImVec2(c.x - r * 0.2f, c.y + r), col);
              dl->AddRectFilled(ImVec2(c.x + r * 0.2f, c.y - r), ImVec2(c.x + r * 0.7f, c.y + r), col);
            } }))
        ModMenu::instance().click(MenuClick{MenuClick::Type::TlPlay});
      auto [ta, tb] = next(bw);   // step one frame
      if (icon_button(dl, "st", ta, tb, false, v.paused, v.paused ? "Step one frame" : "Pause first to step", [&](ImVec2 c, ImU32 col) {
            arrow(dl, ImVec2(c.x - 2 * S, c.y), r * 0.9f, true, col);
            dl->AddRectFilled(ImVec2(c.x + r * 0.6f, c.y - r), ImVec2(c.x + r * 0.6f + 2 * S, c.y + r), col); }))
        ModMenu::instance().click(MenuClick{MenuClick::Type::TlStep});
      x += 10 * S;
      auto [ca, cb] = next(bw);   // record
      const bool can_rec = v.mode == 2;
      if (icon_button(dl, "rec", ca, cb, v.recording, can_rec,
                      !can_rec ? "Recording works in replays" : v.recording ? "Stop and save" : "Record up to 40 s of the capture side",
                      [&](ImVec2 c, ImU32 col) {
            if (v.recording) dl->AddRectFilled(ImVec2(c.x - r * 0.75f, c.y - r * 0.75f), ImVec2(c.x + r * 0.75f, c.y + r * 0.75f), col);
            else dl->AddCircleFilled(c, r * 0.85f, can_rec ? RED_TEXT : col); }))
        ModMenu::instance().click(MenuClick{MenuClick::Type::TlRecord});

      // State text, right of the buttons
      std::string state;
      ImU32 state_col = MUTED;
      if (v.recording) {
        char buf[48];
        snprintf(buf, sizeof(buf), "REC  %.1f s / 40 s", v.rec_frames / 60.0);
        state = buf; state_col = RED_TEXT;
      } else state = v.paused ? "PAUSED" : (v.mode == 2 ? "REPLAY" : "TRAINING");
      text(dl, f_bold, ImVec2(f.o.x + f.w - pad - text_w(f_bold, state), y + (bh - f_bold->FontSize) * 0.5f), state_col, state);

      // 40 second track: fills while recording, ticks every 10 s
      const float tx0 = f.o.x + pad, tx1 = f.o.x + f.w - pad;
      const float ty = y + bh + 12 * S, th = 8 * S;
      dl->AddRectFilled(ImVec2(tx0, ty), ImVec2(tx1, ty + th), CHIP);
      const float frac = std::clamp(v.rec_frames / 2400.0f, 0.0f, 1.0f);
      if (v.recording) dl->AddRectFilled(ImVec2(tx0, ty), ImVec2(tx0 + (tx1 - tx0) * frac, ty + th), RED);
      for (int s = 0; s <= 40; s += 10) {
        const float tx = tx0 + (tx1 - tx0) * (s / 40.0f);
        dl->AddLine(ImVec2(tx, ty + th), ImVec2(tx, ty + th + 4 * S), DIM);
        const std::string lab = std::to_string(s) + (s == 40 ? " s" : "");
        const float lx = std::clamp(tx - text_w(f_small, lab) * 0.5f, tx0, tx1 - text_w(f_small, lab));
        text(dl, f_small, ImVec2(lx, ty + th + 5 * S), DIM, lab);
      }
      // Last message (saved file, errors)
      const float sy = ty + th + 5 * S + f_small->FontSize + 4 * S;
      if (!v.rec_status.empty() && sy + f_small->FontSize < f.o.y + f.h) {
        dl->PushClipRect(ImVec2(tx0, sy), ImVec2(tx1, sy + f_small->FontSize + 2 * S), true);
        text(dl, f_small, ImVec2(tx0, sy), MUTED, v.rec_status);
        dl->PopClipRect();
      }

      // Recordings controls: slot, save slot to file, file + browse, player side,
      // load file into slot, capture side. Same rows as the Recordings page.
      static constexpr int kShown[] = {0, 1, 2, 3, 4, 5, 7};  // Recordings row order, see buildRows
      std::vector<MenuRowView> rows;
      std::vector<int> index;
      for (int k : kShown)
        if (k < static_cast<int>(v.rec_rows.size())) { rows.push_back(v.rec_rows[k]); index.push_back(k); }
      const float rows_top = sy + f_small->FontSize + 8 * S;
      if (!rows.empty() && rows_top < f.o.y + f.h - 8 * S) {
        dl->AddLine(ImVec2(f.o.x + pad, rows_top - 3 * S), ImVec2(f.o.x + f.w - pad, rows_top - 3 * S), DIVIDER);
        static ScrollState tl_scroll;
        const int rec_page = 9;  // MenuPage::Recordings
        draw_rows(dl, rows, -1, "timeline", f.o.x, f.o.x + f.w, rows_top, f.o.y + f.h - 6 * S, tl_scroll,
                  [&index, rec_page](MenuClick c) {
                    if (c.a < 0 || c.a >= static_cast<int>(index.size())) return;
                    c.a = index[c.a] + 1;  // + Back row
                    c.page = rec_page;
                    ModMenu::instance().click(c);
                  });
      }
    }
    end_window();
  }

  // ---- Appearance -------------------------------------------------------------
  unsigned preview_rgb = 0;  // custom color being dragged in the picker (0 = none)

  void apply_theme(const MenuView& v) {
    const unsigned rgb = preview_rgb ? preview_rgb : v.accent_rgb;
    const int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    auto mix = [](int c, float t) { return static_cast<int>(c + (255 - c) * t); };
    RED = RGBA(r, g, b);
    RED_TEXT = RGBA(mix(r, 0.25f), mix(g, 0.25f), mix(b, 0.25f));
    RED_LINE = RGBA(mix(r, 0.25f), mix(g, 0.25f), mix(b, 0.25f), 153);
    CURSOR = RGBA(r, g, b, 38);
    FOCUS = RGBA(r, g, b, 72);
    const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    ON_ACCENT = lum > 160 ? RGBA(12, 14, 20) : RGBA(232, 234, 240);
    opacity = std::clamp(v.opacity, 0.3f, 1.0f);
    auto a = [](float base) { return static_cast<int>(std::clamp(base * opacity / 0.94f, 0.0f, 255.0f)); };
    PANEL = RGBA(4, 6, 14, a(240));
    NAV_BG = RGBA(9, 11, 20, a(250));
    SECTION = RGBA(18, 26, 44, a(235));
    OVERLAY_BG = RGBA(4, 6, 14, a(215));
    BAR_BG = RGBA(4, 6, 14, a(200));
  }

  // Custom accent color picker, opened from Appearance > Custom color.
  void draw_picker(const MenuView& v) {
    static int seen_seq = -1;
    static bool open = false;
    static float col[3] = {1, 0, 0};
    if (seen_seq < 0) seen_seq = v.picker_seq;
    if (v.picker_seq != seen_seq) {
      seen_seq = v.picker_seq;
      open = true;
      col[0] = ((v.custom_rgb >> 16) & 0xFF) / 255.0f;
      col[1] = ((v.custom_rgb >> 8) & 0xFF) / 255.0f;
      col[2] = (v.custom_rgb & 0xFF) / 255.0f;
    }
    if (!open) { preview_rgb = 0; return; }
    ImGuiIO& io = ImGui::GetIO();
    Frame f = begin_window("Accent color##sl_picker", "Accent color",
                           ImVec2(io.DisplaySize.x * 0.5f - 160 * S, io.DisplaySize.y * 0.3f),
                           ImVec2(320 * S, (HEAD + 300) * S), ImVec2(220 * S, (HEAD + 180) * S), true);
    if (f.closed) open = false;
    if (!f.collapsed) {
      ImGui::SetCursorScreenPos(ImVec2(f.o.x + 12 * S, f.o.y + f.head + 10 * S));
      ImGui::PushItemWidth(f.w - 24 * S);
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * S, 6 * S));
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6 * S, 4 * S));
      ImGui::PushFont(f_small);
      ImGui::ColorPicker3("##accent", col, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview |
                                            ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_PickerHueBar);
      const unsigned rgb = (static_cast<unsigned>(col[0] * 255 + 0.5f) << 16) |
                           (static_cast<unsigned>(col[1] * 255 + 0.5f) << 8) | static_cast<unsigned>(col[2] * 255 + 0.5f);
      preview_rgb = rgb ? rgb : 1;  // live preview while the window is open
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && rgb != v.custom_rgb)  // save once the drag ends
        ModMenu::instance().click(MenuClick{MenuClick::Type::SetColor, static_cast<int>(rgb), 0});
      ImGui::PopFont();
      ImGui::PopStyleVar(2);
      ImGui::PopItemWidth();
    }
    end_window();
    if (!open) preview_rgb = 0;
  }

  // bar >= 0 adds a draining bar under the text (1 = full).
  void draw_status_message(const char* label, int row, float bar = -1.0f) {
    ImFont* font = f_title ? f_title : ImGui::GetFont();
    const float size = 30.0f * S;
    const ImVec2 text = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label);
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const ImVec2 pos(screen.x * 0.5f - text.x * 0.5f, screen.y * 0.25f + row * 72.0f * S);
    const float pad = 14.0f * S;
    auto* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(pos.x - pad, pos.y - pad),
                      ImVec2(pos.x + text.x + pad, pos.y + text.y + pad), OVERLAY_BG);
    dl->AddText(font, size, pos, TEXT, label);
    if (bar >= 0.0f) {
      const float y = pos.y + text.y + pad - 6.0f * S;
      const float x0 = pos.x - pad, x1 = pos.x + text.x + pad;
      dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + 6.0f * S), BAR_BG);
      dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + (x1 - x0) * bar, y + 6.0f * S), RED);
    }
  }

  // Takeover state in one line, in its own window: dragged while the menu is open,
  // and the place is kept like the other windows. Starts above P1's health bar.
  void draw_status_line(const ReplayManager::Status& s, bool interactive) {
    std::string t = s.replay ? "Replay" : "Training";
    auto add = [&t](const std::string& part) { t += "  \xC2\xB7  " + part; };
    if (s.rewindPoint) add("Rewind point set");
    char buf[64];
    if (s.assist == 1) { snprintf(buf, sizeof(buf), "Assist: Standby %.1f s", std::max(0.0f, s.assistLeftSeconds)); add(buf); }
    else if (s.assist == 2) add("Assist: Active");
    else if (s.assistSeconds > 0.0) { snprintf(buf, sizeof(buf), "Assist %.1f s", s.assistSeconds); add(buf); }
    if (s.takeover) { snprintf(buf, sizeof(buf), "Takeover: P%d", s.takeover); add(buf); }
    else if (s.replay) add("Playback");

    // Its own font at twice the normal size, so it stays sharp when made bigger.
    ImFont* font = f_status ? f_status : ImGui::GetFont();
    const float size = font->FontSize * 0.5f * std::clamp(ModMenu::instance().statusLineSize(), 0.5f, 2.0f);
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    static int placed_gen = -1;  // "Reset all window positions" puts it back too
    ImGui::SetNextWindowPos(ImVec2(screen.x * (340.0f / 1920.0f), screen.y * (66.0f / 1080.0f)),
                            placed_gen != g_layout_gen && placed_gen != -1 ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
    placed_gen = g_layout_gen;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing;
    if (!interactive) flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;
    // Black at the chosen opacity. While the menu is open it never goes fully clear,
    // so there is something to grab.
    float bg = std::clamp(ModMenu::instance().statusLineOpacity(), 0.0f, 1.0f);
    if (interactive) bg = std::max(bg, 0.3f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(size * 0.25f, size * 0.12f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, bg));
    if (ImGui::Begin("Status##sl_status", nullptr, flags)) {
      const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, t.c_str());
      text_outlined(ImGui::GetWindowDrawList(), font, ImGui::GetCursorScreenPos(), TEXT, t, size);
      ImGui::Dummy(extent);
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
  }

  void draw_replay_status(const MenuView& v) {
    int row = 0;
    const auto& rs = v.replay;
    const bool status_line = ModMenu::instance().statusLineEnabled();
    if (status_line) draw_status_line(rs, v.showing);
    if (rs.prompt) {
      char q[96], keys[128];
      if (!rs.promptStop) snprintf(q, sizeof(q), "Take over P%d?", rs.prompt);
      else if (rs.promptRewinds) snprintf(q, sizeof(q), "Stop P%d and go back to the rewind point?", rs.prompt);
      else snprintf(q, sizeof(q), "Stop taking over P%d?", rs.prompt);
      const int a = rs.prompt == 1 ? binds::TAKEOVER_P1 : binds::TAKEOVER_P2;
      const std::wstring key = binds::key[a] ? binds::keyName(binds::key[a]) : binds::padName(binds::pad[a]);
      char key8[48]{};
      WideCharToMultiByte(CP_UTF8, 0, key.c_str(), -1, key8, sizeof(key8) - 1, nullptr, nullptr);
      snprintf(keys, sizeof(keys), "%s yes     Esc no     (pad: Right / Left)", key8);
      draw_status_message(q, row++);
      draw_status_message(keys, row++);
    }
    if (v.takeover_player && !status_line) {
      char label[32];
      snprintf(label, sizeof(label), "Takeover: P%d", *v.takeover_player);
      draw_status_message(label, row++);
    }
    if (rs.assist == 1) draw_status_message("Replay Assist: Standby", row++, rs.assistFraction);
    else if (rs.assist == 2) draw_status_message("Replay Assist: Active", row++);
    if (rs.rewindLeftSeconds > 0.0f) {
      char label[64];
      snprintf(label, sizeof(label), "Playback in %.1f s", rs.rewindLeftSeconds);
      draw_status_message(label, row++, rs.rewindFraction);
    }
    if (ModMenu::instance().isRewindPointNotificationVisible())
      draw_status_message("Rewind point created", row++);
  }

  void draw() {
    // Outside a training or replay fight (menus, loading, character select): show nothing
    if (!modScreenActive()) { g_timeline_visible = false; return; }
    const MenuView v = ModMenu::instance().view();
    apply_theme(v);
    draw_framebar(v.showing, v.hide_window_decorations, v.bar_title - 1, v.bar_panel - 1);
    draw_meters(v.showing, v.meters_right, v.meters_snap, v.hide_window_decorations);
    draw_timeline(v);
    use(E_MENU);
    draw_replay_status(v);
    if (!v.showing) return;
    ImGuiIO& io = ImGui::GetIO();
    const float pad = PAD * S;

    // The main menu keeps its saved center (Layout page, or the last drag).
    static float applied_x = -1, applied_y = -1;
    static bool pending = false;
    static ImVec2 last_size(0, 0);
    if (pending && std::fabs(v.x_ratio - applied_x) < 0.0005f && std::fabs(v.y_ratio - applied_y) < 0.0005f) {
      // Saving rounds the ratios. Acknowledge that result without moving the
      // window again: ImGui's pixel rounding would otherwise feed back into saves.
      applied_x = v.x_ratio; applied_y = v.y_ratio;
      pending = false;
    }
    if (!pending && (v.x_ratio != applied_x || v.y_ratio != applied_y) && last_size.x > 0) {
      ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * v.x_ratio - last_size.x * 0.5f,
                                     io.DisplaySize.y * v.y_ratio - last_size.y * 0.5f), ImGuiCond_Always);
      applied_x = v.x_ratio; applied_y = v.y_ratio;
    }
    const ImVec2 def_size((NAV_W + 540) * S + v.extra_width * 0.5f * S, 500 * S);
    Frame f = begin_window("StriveLabs##sl_menu", "",
                           ImVec2(io.DisplaySize.x * v.x_ratio - def_size.x * 0.5f, io.DisplaySize.y * v.y_ratio - def_size.y * 0.5f),
                           def_size, ImVec2((NAV_W + 320) * S, (HEAD + v.nav_items.size() * NAV_ROW + 40) * S), true);
    if (f.closed) ModMenu::instance().click(MenuClick{MenuClick::Type::CloseMenu, 0, 0});
    ImDrawList* dl = f.dl;
    const ImVec2 o = f.o;
    const float w = f.w, h = f.h;
    last_size = ImVec2(w, f.collapsed ? h : h);
    {  // remember where it was dragged, as a screen-center ratio
      const float cx = (o.x + w * 0.5f) / std::max(io.DisplaySize.x, 1.0f);
      const float cy = (o.y + h * 0.5f) / std::max(io.DisplaySize.y, 1.0f);
      if (applied_x < 0) { applied_x = v.x_ratio; applied_y = v.y_ratio; }
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !pending &&
          (std::fabs(cx - applied_x) > 0.0005f || std::fabs(cy - applied_y) > 0.0005f)) {
        applied_x = std::clamp(cx, 0.0f, 1.0f); applied_y = std::clamp(cy, 0.0f, 1.0f);
        pending = true;
        ModMenu::instance().click(MenuClick{MenuClick::Type::Move, static_cast<int>(applied_x * 10000), static_cast<int>(applied_y * 10000)});
      }
    }

    // Logo text in the title strip
    text(dl, f_title, ImVec2(o.x + pad, o.y + (f.head - f_title->FontSize) * 0.5f + 1 * S), TEXT, "STRIVE");
    text(dl, f_title, ImVec2(o.x + pad + text_w(f_title, "STRIVE") + 1 * S, o.y + (f.head - f_title->FontSize) * 0.5f + 1 * S), RED, "LABS");

    if (!f.collapsed) {
      const float nav_w = NAV_W * S, nav_row = NAV_ROW * S;
      const float body_top = o.y + f.head;
      dl->AddRectFilled(ImVec2(o.x + 1, body_top), ImVec2(o.x + nav_w, o.y + h - 1), NAV_BG);
      dl->AddLine(ImVec2(o.x + nav_w, body_top), ImVec2(o.x + nav_w, o.y + h), BORDER);

      // Side menu
      float y = body_top + 8 * S;
      for (int i = 0; i < static_cast<int>(v.nav_items.size()); i++) {
        const ImVec2 a(o.x, y), b(o.x + nav_w, y + nav_row);
        const bool sel = i == v.nav;
        ImGui::PushID(1000 + i);
        if (clicked_rect("nav", a, b)) ModMenu::instance().click(MenuClick{MenuClick::Type::OpenNav, i, 0});
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (sel) {
          dl->AddRectFilled(a, b, (v.nav_focus || v.back_focus) ? FOCUS : CURSOR);
          dl->AddRectFilled(a, ImVec2(a.x + 3 * S, b.y), RED);
        } else if (hov) dl->AddRectFilled(a, b, HOVER);
        text(dl, sel ? f_bold : f_body, ImVec2(a.x + pad + 3 * S, a.y + (nav_row - f_body->FontSize) * 0.5f), sel ? TEXT : MUTED, v.nav_items[i]);
        y += nav_row;
      }

      // Logo, bottom left, under the side menu; credit line under it
      const float credit_y = o.y + h - 22 * S;
      if (logo) {
        const float room_h = credit_y - 6 * S - (y + 8 * S);
        const float lw = std::min(nav_w - 2 * pad, room_h / std::max(logo_aspect, 0.01f));
        if (lw > 24 * S) {
          const float lh = lw * logo_aspect;
          const ImVec2 la(o.x + (nav_w - lw) * 0.5f, credit_y - 6 * S - lh);
          dl->AddImage(logo, la, ImVec2(la.x + lw, la.y + lh), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 230));
        }
      }
      text(dl, f_small, ImVec2(o.x + pad, credit_y), DIM, "Built on WistfulHopes' work");

      // Page header
      const float px = o.x + nav_w;
      {
        const ImVec2 a(px, body_top + 6 * S), b(o.x + w - 1, a.y + 30 * S);
        dl->AddRectFilled(a, b, CHIP);
        dl->AddRectFilled(a, ImVec2(a.x + 3 * S, b.y), RED);
        float title_end = b.x;
        // Header button: meters page shows/hides the overlay window, other pages pop out
        if (v.tension_page || v.poppable) {
          const std::string label = v.tension_page ? (v.tension_on ? "Hide overlay" : "Show overlay")
                                                   : (popped_has(v.page) ? "Close window" : "Pop out");
          const float bw = text_w(f_small, label) + 20 * S;
          const ImVec2 ba(b.x - 6 * S - bw, a.y + 4 * S), bb(b.x - 6 * S, b.y - 4 * S);
          ImGui::PushID("hdr");
          if (clicked_rect("btn", ba, bb)) {
            if (v.tension_page) ModMenu::instance().click(MenuClick{MenuClick::Type::TensionWindow, 0, v.tension_on ? 0 : 1});
            else toggle_popped(v.page);
          }
          const bool hov = ImGui::IsItemHovered();
          ImGui::PopID();
          dl->AddRectFilled(ba, bb, hov ? RED : PANEL);
          dl->AddRect(ba, bb, RED);
          text(dl, f_small, ImVec2(ba.x + 10 * S, (ba.y + bb.y) * 0.5f - f_small->FontSize * 0.5f), TEXT, label);
          title_end = ba.x - 8 * S;
        }
        dl->PushClipRect(a, ImVec2(std::max(a.x, title_end), b.y), true);
        text(dl, f_bold, ImVec2(a.x + pad, (a.y + b.y) * 0.5f - f_bold->FontSize * 0.5f), RED_TEXT, upper(v.title));
        dl->PopClipRect();
      }

      // Rows and footer
      static ScrollState main_scroll;
      const float footer_h = 44 * S;
      const float top = body_top + 42 * S, bottom = o.y + h - footer_h;
      draw_rows(dl, v.rows, v.nav_focus ? -1 : v.cursor, v.title, px, o.x + w - 1, top, bottom, main_scroll,
                [&v](MenuClick c) { send(v, c); });
      const float lx = px + pad;
      dl->AddLine(ImVec2(lx, bottom + 2 * S), ImVec2(o.x + w - pad, bottom + 2 * S), DIVIDER);
      if (!v.status.empty()) text(dl, f_small, ImVec2(lx, bottom + 6 * S), MUTED, v.status);
      dl->PushClipRect(ImVec2(lx, o.y + h - 22 * S), ImVec2(o.x + w - pad, o.y + h), true);
      text(dl, f_small, ImVec2(lx, o.y + h - 20 * S), DIM, "Up/Down move    Left/Right change    F5 close");
      dl->PopClipRect();
    }
    end_window();

    for (const auto& pv : v.panels) draw_panel(pv);
    draw_picker(v);
  }
}
