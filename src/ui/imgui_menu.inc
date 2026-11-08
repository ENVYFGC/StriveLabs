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
  // Makes a texture from RGBA pixels and lets the old one go. Set by imgui_overlay.cpp.
  ImTextureID (*upload_rgba)(const uint8_t* px, int w, int h, ImTextureID old) = nullptr;
  int device_gen = 0;  // Bumped by imgui_overlay.cpp when it (re)starts. Textures made before belong to an old device
  ImDrawCallback point_sampling = nullptr;  // Set by imgui_overlay.cpp. Draws texels sharp until ImDrawCallback_ResetRenderState
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
    // Japanese for the menu text (player names and comments from Combo Maker): merged
    // into the menu fonts from a Japanese font Windows ships, if there is one. Only the
    // menu's: every font would make the atlas far too big.
    std::string jp_path;
    for (const char* f : {"YuGothM.ttc", "meiryo.ttc", "msgothic.ttc"}) {
      char win[MAX_PATH] = {};
      if (!GetWindowsDirectoryA(win, MAX_PATH)) break;
      const std::string p = std::string(win) + "\\Fonts\\" + f;
      if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES) { jp_path = p; break; }
    }
    bool jp = true;  // off for the timeline's set, which never shows Combo Maker text
    auto merge_jp = [&](ImFont* into, float px, float s) {
      // ponytail: size cap instead of measuring the atlas; ~3000 glyphs per font at 48 px
      // keeps it far under the 16384 rows a texture can have.
      if (!jp || !into || jp_path.empty() || px * s > 48.0f) return;
      ImFontConfig cfg;
      cfg.MergeMode = true;
      cfg.OversampleH = 1;
      cfg.PixelSnapH = true;
      io.Fonts->AddFontFromFileTTF(jp_path.c_str(), px * s, &cfg, io.Fonts->GetGlyphRangesJapanese());
    };
    auto menu_set = [&](float s) {
      FontSet fs;
      fs.body = add("Barlow-Medium.ttf", 17.0f, s);
      merge_jp(fs.body, 17.0f, s);
      fs.bold = add("Barlow-SemiBold.ttf", 17.0f, s);
      merge_jp(fs.bold, 17.0f, s);
      fs.title = add("BarlowCondensed-Bold.ttf", 24.0f, s);
      fs.small_ = add("Barlow-Medium.ttf", 14.0f, s);
      merge_jp(fs.small_, 14.0f, s);
      if (!fs.body || !fs.bold || !fs.title || !fs.small_) {
        fs.body = fs.bold = fs.title = fs.small_ = fallback(14.0f, s);
        COMBO_LOG("imgui: StriveLabs_fonts not found, using the default font");
      }
      return fs;
    };
    fs_menu = menu_set(SE[E_MENU]);
    jp = false;
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
  // Returns where the rows end on screen.
  float draw_rows(ImDrawList* dl, const std::vector<MenuRowView>& rows, int cursor, const std::string& key,
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
          const std::string msg = "Press a key \xC2\xB7 Esc cancel \xC2\xB7 Backspace clear";
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
      if (!r.tip.empty() && ImGui::IsMouseHoveringRect(a, b)) ImGui::SetTooltip("%s", r.tip.c_str());
      ImGui::PopID();
      y += row_h;
    }
    ImGui::PopClipRect();

    // Square scrollbar, only when the page is longer than the pane
    draw_scrollbar(dl, x1, top, bottom, rows_h, scroll);
    return top - scroll + rows_h;
  }

  // Frame Bar page: what the bar's colors and marks mean, in the room under the settings.
  void draw_bar_legend(ImDrawList* dl, float x0, float x1, float y, float bottom, const MenuView& v) {
    const float fs = f_small->FontSize, sw = fs * 0.9f, gx = 18 * S, line = fs + 9 * S;
    if (y + line * 3 > bottom) return;  // no room: the settings fill the page
    text(dl, f_small, ImVec2(x0, y), MUTED, "LEGEND");
    y += line;
    float x = x0;
    // Room for an item of width w on this line, or the next one; where it goes.
    auto place = [&](float w) {
      if (x > x0 && x + w > x1) { x = x0; y += line; }
      const float at = x;
      x += w + gx;
      return at;
    };
    auto label = [&](float at, const char* s) { text(dl, f_small, ImVec2(at, y), LABEL, s); };
    struct Swatch { int state; const char* name; };
    static const Swatch SW[] = {{0, "Neutral"}, {3, "Startup"}, {4, "Active"}, {5, "Projectile active"},
                                {6, "Recovery"}, {1, "Blockstun"}, {2, "Hitstun"}};
    for (const auto& s : SW) {
      const float at = place(sw + 6 * S + text_w(f_small, s.name));
      if (y + fs > bottom) return;
      dl->AddRectFilled(ImVec2(at, y + (fs - sw) * 0.5f), ImVec2(at + sw, y + (fs + sw) * 0.5f), v.bar_colors[s.state]);
      label(at + sw + 6 * S, s.name);
    }
    x = x0; y += line;
    // A glyph, then its meaning.
    auto mark = [&](float gw, const char* s, const std::function<void(float, float)>& glyph) {
      const float at = place(gw + 6 * S + text_w(f_small, s));
      if (y + fs > bottom) return;
      glyph(at, y);
      label(at + gw + 6 * S, s);
    };
    auto sample = [&](const char* n, ImU32 col, const char* s) {
      mark(text_w(f_small, n), s, [&](float at, float ty) { text_outlined(dl, f_small, ImVec2(at, ty), col, n); });
    };
    mark(sw, "Hit", [&](float at, float ty) {
      dl->AddTriangleFilled(ImVec2(at, ty + fs * 0.2f), ImVec2(at + sw, ty + fs * 0.2f), ImVec2(at + sw * 0.5f, ty + fs * 0.8f), TEXT);
    });
    mark(sw, "Frame perfect move", [&](float at, float ty) {
      ImVec2 p[10];
      const float cx = at + sw * 0.5f, cy = ty + fs * 0.5f, r = sw * 0.55f;
      for (int k = 0; k < 10; k++) {
        const float a = -IM_PI / 2 + k * IM_PI / 5, rr = (k & 1) ? r * 0.4f : r;
        p[k] = ImVec2(cx + std::cos(a) * rr, cy + std::sin(a) * rr);
      }
      dl->AddConcavePolyFilled(p, 10, TEXT);
    });
    sample("12", TEXT, "Frame count");
    sample("3", RGBA(255, 214, 0), "Gap");
    if (v.bar_compact) {
      sample("6", RGBA(124, 255, 107), "Punish window");
      mark(sw * 1.6f, "Stretch length", [&](float at, float ty) {
        const float ly = ty + fs * 0.75f, w = sw * 1.6f, t = std::max(1.0f, S * 1.2f);
        dl->AddRectFilled(ImVec2(at, ly - t), ImVec2(at + w, ly), TEXT);
        dl->AddRectFilled(ImVec2(at, ly - t * 4), ImVec2(at + t, ly), TEXT);
        dl->AddRectFilled(ImVec2(at + w - t, ly - t * 4), ImVec2(at + w, ly), TEXT);
      });
    }
    mark(sw * 0.4f, "Stun/recovery end", [&](float at, float ty) {
      dl->AddRectFilled(ImVec2(at, ty), ImVec2(at + sw * 0.4f, ty + fs), RGBA(120, 200, 255));
    });
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
      } else if (!(ImGui::GetCurrentContext()->NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasSize)) {
        ImGui::SetNextWindowSize(def_size, reset ? ImGuiCond_Always : ImGuiCond_FirstUseEver);  // unless the caller set it
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
  // The game's training damage panel, per side, from the game thread.
  std::mutex d_mutex;
  std::vector<imgui_overlay::DamageRow> d_rows[2];
  // The game's own pause menu is open: the overlays step out of its way.
  std::atomic<bool> g_game_menu_open{false};
  // The online version window, opened by its own key outside fights.
  std::atomic<bool> g_online_picker{false};
  std::atomic<int> g_online_pick_step{0};
  // The battle version's name, from the game thread.
  std::mutex bv_mutex;
  std::string bv_text;
  unsigned long long bv_time = 0;
  unsigned long long d_time = 0;

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
      if (GetTickCount64() - b_time > 1000) return;  // bar off, or a loading screen
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
      const float br_h = fb.compact ? f_framebar_stats->FontSize + 8 * S : 0.0f;  // compact: a row's brackets
      const float avail = std::max(0.0f, f.o.y + f.h - 4 * S - y - nbars * (lh + br_h) - (nbars > 1 ? gap : 0.0f));
      const float in_h = nin ? std::min((x1 - x0) / 120.0f * 8.0f, avail * (nin > 1 ? 0.35f : 0.5f)) : 0.0f;
      const float bar_h = std::max((avail - nin * (in_h + gap)) / static_cast<float>(std::max(nbars, 1)), 6 * S);
      // Like the HUD bar, fit 120 cells plus a trailing gap into the background.
      // Keep enough space for the delimiter and crossup markers at either edge.
      const float desired_sp = std::max((x1 - x0) * (4.0f / (120 * 24.0f + 4.0f)), std::max(1.5f * S, 1.0f));
      const float sp = std::min(desired_sp, (x1 - x0) / (120 * 2.0f + 1.0f));
      const float cell = (x1 - x0 - sp) / 120.0f;
      const float seg_w = cell - sp;
      // Folded cells are two frames wide, so their count fits. Folds are in the same
      // columns on both rows, so the rows stay lined up. If that runs past the bar,
      // every column narrows to fit.
      float xs[121];
      {
        float units[120], total = 0;
        for (int i = 0; i < 120; i++) total += units[i] = (fb.rows[0][i].frames > 1 || fb.rows[1][i].frames > 1) ? 2.0f : 1.0f;
        const float k = cell * std::min(1.0f, 120.0f / total);
        xs[0] = x0;
        for (int i = 0; i < 120; i++) xs[i + 1] = xs[i] + units[i] * k;
      }
      auto segw = [&](int i) { return xs[i + 1] - xs[i] - sp; };
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
          // Where the other side gets free while this one is still stuck, the block keeps
          // a border: the frames from there on are the other side's window to act (a
          // punish, or the advantage), with their own count.
          const auto& other = fb.rows[1 - row];
          for (int j = 0; j < 120; j++) {
            const int n = (j + 1) % 120;
            if (join[j] && !fb.rows[row][j].free && (other[j].mods & 4) && other[n].on && other[n].free) join[j] = false;
          }
        }
        for (int i = 0; i < 120; i++) {
          const auto& c = fb.rows[row][i];
          if (!c.on) continue;
          const float left = xs[i] + sp, seg_w = segw(i);
          if (c.mods & 2) dl->AddRectFilled(ImVec2(left - bt, sy - bt), ImVec2(left + seg_w + bt, sy + sh + bt), fb.bg);
          dl->AddRectFilled(ImVec2(left, sy), ImVec2(left + seg_w, sy + sh), c.color);
          if (join[i]) dl->AddRectFilled(ImVec2(left + seg_w, sy), ImVec2(xs[i + 1] + sp, sy + sh), c.color);
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
          if (fb.hits && (c.mods & 0x40)) {
            // Where the move connected: a white notch at the top of that active cell.
            const float hw = seg_w * 0.45f, th = std::min(hw * 1.4f, sh * 0.4f);
            dl->AddTriangleFilled(ImVec2(left + seg_w * 0.5f - hw, sy), ImVec2(left + seg_w * 0.5f + hw, sy),
                                  ImVec2(left + seg_w * 0.5f, sy + th), RGBA(255, 255, 255));
          }
          if (fb.delim && (c.mods & 4)) dl->AddRectFilled(ImVec2(left + seg_w, sy - bt), ImVec2(left + seg_w + bt, sy + sh + bt), RGBA(255, 255, 255));
          if (fb.cancels && (c.mods & 1)) dl->AddRectFilled(ImVec2(left, sy + sh * 2 / 3), ImVec2(left + seg_w, sy + sh), RGBA(255, 255, 255));
          if (fb.crossups && (c.mods & 8)) dl->AddRectFilled(ImVec2(left + seg_w - bt, sy - bt), ImVec2(left + seg_w + bt, sy + sh + bt), RED);
        }
        if (fb.compact) {
          // Folded compact mode: one number per cell at most. A folded cell shows how many
          // frames it holds (not in stun: only where stun ends matters, and its bracket
          // says that); a stretch in one cell shows its length; a stretch over several
          // cells gets a bracket beside the bar with its length, P1's above, P2's below.
          // In green, a punish window: how long this side was free while the other was
          // still recovering. Gaps keep their yellow count.
          const auto& me = fb.rows[row];
          const auto& other = fb.rows[1 - row];
          auto free_vs_recovery = [&](int i) { return me[i].on && other[i].on && me[i].free && other[i].recovering; };
          int window[120] = {};  // on a punish window's last cell: its length
          for (int i = 0, n = 0; i < 120; i++) {
            n = free_vs_recovery(i) ? n + me[i].frames : 0;
            if (n && (i == 119 || !free_vs_recovery(i + 1))) window[i] = n;
          }
          // Numbers go where they don't touch another one: the punish windows and gaps
          // first, then single-cell stretches, then folded counts. One with no room is left
          // out (narrow cells side by side would pile them up).
          struct Num { int i, n, prio; ImU32 col; };
          std::vector<Num> nums;
          auto num = [&](int i, int n, ImU32 col, int prio) { nums.push_back({i, n, prio, col}); };
          struct Label { float x, y; std::string t; };
          std::vector<Label> labels;  // bracket totals, left to right
          auto fits = [&](std::vector<std::pair<float, float>>& taken, float a, float b) {
            for (const auto& [l, r] : taken)
              if (a < r + 3 * S && b + 3 * S > l) return false;
            taken.emplace_back(a, b);
            return true;
          };
          const float bfs = f_framebar_stats->FontSize, line = std::max(1.0f, S * 1.2f), tick = 5 * S;
          for (int s = 0; s < 120;) {
            if (!me[s].on) { s++; continue; }
            int e = s, total = me[s].frames;
            while (!(me[e].mods & 4) && e < 119 && me[e + 1].on) total += me[++e].frames;
            for (int i = s; i <= e; i++) {
              const auto& c = me[i];
              if (fb.gaps && c.gap > 0) num(i, c.gap, RGBA(255, 214, 0), 2);
              else if (window[i]) num(i, window[i], RGBA(124, 255, 107), 2);
              else if (s == e) { if (total > 2) num(i, total, TEXT, 1); }
              else if (c.frames > 1 && !c.stun) num(i, c.frames, TEXT, 0);
            }
            if (e > s && total > 2) {
              const float bx0 = xs[s] + sp + 1 * S, bx1 = xs[e + 1] - 1 * S;
              const std::string t = std::to_string(total);
              const float tx = (bx0 + bx1 - text_w(f_framebar_stats, t)) * 0.5f;
              if (row == 0) {  // above the bar, ticks down
                const float ly = top - 3 * S;
                dl->AddRectFilled(ImVec2(bx0, ly - line), ImVec2(bx1, ly), TEXT);
                dl->AddRectFilled(ImVec2(bx0, ly - line), ImVec2(bx0 + line, ly - line + tick), TEXT);
                dl->AddRectFilled(ImVec2(bx1 - line, ly - line), ImVec2(bx1, ly - line + tick), TEXT);
                labels.push_back({tx, ly - line - bfs - 1 * S, t});
              } else {  // below the bar, ticks up
                const float ly = top + bar_h + 3 * S;
                dl->AddRectFilled(ImVec2(bx0, ly), ImVec2(bx1, ly + line), TEXT);
                dl->AddRectFilled(ImVec2(bx0, ly + line - tick), ImVec2(bx0 + line, ly + line), TEXT);
                dl->AddRectFilled(ImVec2(bx1 - line, ly + line - tick), ImVec2(bx1, ly + line), TEXT);
                labels.push_back({tx, ly + line + 1 * S, t});
              }
            }
            s = e + 1;
          }
          std::stable_sort(nums.begin(), nums.end(), [](const Num& a, const Num& b) { return a.prio > b.prio; });
          std::vector<std::pair<float, float>> taken;
          for (const Num& n : nums) {
            // Smaller (down to 65%) where it's wider than its own cell.
            const std::string t = std::to_string(n.n);
            float fs = f_framebar->FontSize, w = text_w(f_framebar, t);
            if (const float room = xs[n.i + 1] - xs[n.i]; w > room) {
              fs = std::max(fs * 0.65f, fs * room / w);
              w = f_framebar->CalcTextSizeA(fs, FLT_MAX, 0.0f, t.c_str()).x;
            }
            const float x = xs[n.i + 1] - w;
            if (fits(taken, x, x + w)) text_outlined(dl, f_framebar, ImVec2(x, sy + (sh - fs) * 0.5f), n.col, t, fs);
          }
          taken.clear();
          for (const Label& l : labels)
            if (fits(taken, l.x, l.x + text_w(f_framebar_stats, l.t))) text_outlined(dl, f_framebar_stats, ImVec2(l.x, l.y), TEXT, l.t);
        }
        for (int i = 0; i < 120 && !fb.compact; i++) {  // numbers on top of the cells
          const auto& c = fb.rows[row][i];
          if (!c.on) continue;
          // A folded cell's own length, small at its top, so no folded cell is a guess.
          // Not when it holds its whole stretch: the total below says the same.
          const bool own = c.frames > 1 && c.frames != c.trunc;
          if (own) {
            const std::string n = std::to_string(c.frames);
            const float fs = std::min(f_framebar->FontSize * 0.62f, sh * 0.42f);
            const float tw = f_framebar->CalcTextSizeA(fs, FLT_MAX, 0.0f, n.c_str()).x;
            text_outlined(dl, f_framebar, ImVec2(xs[i + 1] - tw - sp, sy), RGBA(200, 200, 200), n, fs);
          }
          // Gaps: the free frames between two stuns, in yellow, instead of a count.
          // Otherwise the stretch's total length, at its end; under a cell's own length
          // when it has one.
          const bool gap = fb.gaps && c.gap > 0;
          if (!gap && c.trunc <= 0) continue;
          const std::string n = std::to_string(gap ? c.gap : c.trunc);
          const float fs = own ? std::min(f_framebar->FontSize, sh * 0.58f) : f_framebar->FontSize;
          const float tx = xs[i + 1] - f_framebar->CalcTextSizeA(fs, FLT_MAX, 0.0f, n.c_str()).x;
          const float ty = own ? sy + sh - fs : sy + (sh - fs) * 0.5f;
          text_outlined(dl, f_framebar, ImVec2(tx, ty), gap ? RGBA(255, 214, 0) : TEXT, n, fs);
        }
        if (fb.left[row] > 0) {
          // The rest of the stun or recovery, drawn ahead as outlined cells in its color,
          // with a blue line where it ends. The line stays put as the real cells fill in.
          const int n = std::min(fb.left[row], 119);
          const ImU32 col = (fb.rows[row][fb.newest].color & 0x00FFFFFF) | (170u << 24);
          const float th = std::max(1.0f, bt * 0.7f);
          for (int k = 1; k <= n; k++) {
            const int ci = (fb.newest + k) % 120;
            const float l = xs[ci] + sp, w = segw(ci);
            dl->AddRect(ImVec2(l + th * 0.5f, sy + th * 0.5f), ImVec2(l + w - th * 0.5f, sy + sh - th * 0.5f), col, 0, 0, th);
          }
          const int e = (fb.newest + n) % 120;
          const float lx = xs[e + 1] + sp * 0.5f, lw = std::max(2.0f, bt * 1.5f);
          dl->AddRectFilled(ImVec2(lx - lw * 0.5f, sy - bt * 2), ImVec2(lx + lw * 0.5f, sy + sh + bt * 2), RGBA(120, 200, 255));
          if (fb.both) {
            const std::string a = (fb.adv[row] > 0 ? "+" : "") + std::to_string(fb.adv[row]);
            float tx = lx + lw + sp;
            if (tx + text_w(f_framebar, a) > x1) tx = lx - lw - sp - text_w(f_framebar, a);
            text_outlined(dl, f_framebar, ImVec2(tx, sy + (sh - f_framebar->FontSize) * 0.5f), RGBA(120, 200, 255), a);
          }
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
        float cur_w = seg_w;  // the width of the cell being drawn
        auto hold_line = [&](float left, float ly, ImU32 col) {
          dl->AddRectFilled(ImVec2(left - sp, ly - thin), ImVec2(left + cur_w, ly + thin), col);
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
          const float seg_w = cur_w = segw(i), left = xs[i] + sp, cx = left + seg_w * 0.5f;
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
          if (c.frames > 1) t += "\nCovers " + std::to_string(c.frames) + " frames";
          ImGui::SetTooltip("%s", t.c_str());
        }
      };
      if (fb.show[0]) {
        stats(info[0], y);          y += lh + br_h;
        bar(0, y);                  y += bar_h;
        if (fb.show_inputs[0]) { y += gap; inputs(0, y); y += in_h; }
      }
      if (fb.show[1]) {
        if (fb.show[0]) y += gap;
        bar(1, y);                  y += bar_h + br_h;
        if (fb.show_inputs[1]) { y += gap; inputs(1, y); y += in_h; }
        stats(info[1], y + 1 * S);
      }
    }
    end_window();
  }

  // The meters overlay as its own window. Always on screen while enabled; it can
  // be moved, resized or minimized only while the F5 menu is open.
  // One meters window. Its own scroll, snap and side state, kept by id.
  // Where each of our windows ended up this frame, so another can be pinned to it.
  struct PlacedRect { ImVec4 r; int frame = -1; };
  std::map<std::string, PlacedRect> g_placed;
  const ImVec4* placedThisFrame(const char* id) {
    auto it = g_placed.find(id);
    return it != g_placed.end() && it->second.frame == ImGui::GetFrameCount() ? &it->second.r : nullptr;
  }

  // under: sit right below that window (pinned). fill: take exactly that spot and size.
  void draw_meters_window(const char* id, const char* title, const std::vector<std::string>& lines, ImVec2 def_pos,
                          bool interactive, bool right_side, bool snap, bool hide_decorations, int panel_mode,
                          const ImVec4* under = nullptr, const ImVec4* fill = nullptr, int title_mode = -1) {
    ImGuiIO& io = ImGui::GetIO();
    // Line pitch 1.4x the font size, like the game's own training text (Damage, Combo Damage)
    const float lh = std::max(f_meters->FontSize, f_meters_bold->FontSize) * 1.4f, pad = 12 * S;
    const float def_h = HEAD * S + lh * static_cast<float>(lines.size()) + pad;
    struct State { bool held = false; int last_side = -1; float scroll = 0; };
    static std::map<std::string, State> states;
    State& st = states[id];
    // Snapped: sit right on top of the frame bar, on this window's edge. While the
    // mouse is held on the window (resizing or dragging it), the snap waits, so it
    // doesn't fight the resize; on release it snaps back at the new size.
    {
      ImGuiContext& g = *ImGui::GetCurrentContext();
      const ImGuiWindow* mw = ImGui::FindWindowByName(id);
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mw && g.HoveredWindow == mw) st.held = true;
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) st.held = false;
    }
    if (fill) {
      ImGui::SetNextWindowPos(ImVec2(fill->x, fill->y), ImGuiCond_Always);
      ImGui::SetNextWindowSize(ImVec2(fill->z, fill->w), ImGuiCond_Always);
    } else if (under && !st.held) {
      ImGui::SetNextWindowPos(ImVec2(under->x, under->y + under->w + 4 * S), ImGuiCond_Always);
      if (const ImGuiWindow* win = ImGui::FindWindowByName(id))
        ImGui::SetNextWindowSize(ImVec2(under->z, win->Size.y), ImGuiCond_Always);  // same width, own height
    } else if (snap && !st.held && bar_frame == ImGui::GetFrameCount()) {
      if (const ImGuiWindow* win = ImGui::FindWindowByName(id)) {
        const float x = right_side ? bar_rect.x + bar_rect.z - win->Size.x : bar_rect.x;
        ImGui::SetNextWindowPos(ImVec2(x, bar_rect.y - win->Size.y - 4 * S), ImGuiCond_Always);
      }
    }
    // Overlay Side: when it changes, jump to that side of the screen at the same height.
    const int side = right_side ? 1 : 0;
    if (!snap && !under && !fill && st.last_side != -1 && side != st.last_side) {
      if (const ImGuiWindow* win = ImGui::FindWindowByName(id)) {
        const float x = right_side ? io.DisplaySize.x - win->Size.x - 40 * S : 40 * S;
        ImGui::SetNextWindowPos(ImVec2(x, win->Pos.y), ImGuiCond_Always);
      }
    }
    st.last_side = side;
    Frame f = begin_window(id, title, def_pos, ImVec2(300 * S, def_h), ImVec2(200 * S, HEAD * S + lh * 2), interactive,
                           OVERLAY_BG, hide_decorations, f_meters_title, title_mode, panel_mode);
    if (f.closed) ModMenu::instance().click(MenuClick{MenuClick::Type::TensionWindow, 0, 0});
    g_placed[id] = PlacedRect{ImVec4(f.o.x, f.o.y, f.w, f.h), ImGui::GetFrameCount()};
    if (!f.collapsed) {
      const float top = f.o.y + f.head + 4 * S, bottom = f.o.y + f.h - 4 * S;
      const float content = lh * static_cast<float>(lines.size());
      const float view_h = std::max(bottom - top, 0.0f);
      const float max_scroll = std::max(0.0f, content - view_h);
      if (interactive && ImGui::IsMouseHoveringRect(ImVec2(f.o.x, top), ImVec2(f.o.x + f.w, bottom), false)) st.scroll -= io.MouseWheel * lh;
      st.scroll = std::clamp(st.scroll, 0.0f, max_scroll);
      // Reserve the same space even when the menu is closed, so text stays put.
      const float rx = f.o.x + f.w - pad - (max_scroll > 0 ? 6 * S : 0);
      ImGui::PushClipRect(ImVec2(f.o.x, top), ImVec2(f.o.x + f.w, bottom), true);
      float y = top - st.scroll;
      for (const auto& line : lines) {
        if (line.empty()) { y += lh * 0.35f; continue; }  // a small gap between groups
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
      if (interactive) draw_scrollbar(f.dl, f.o.x + f.w, top, bottom, content, st.scroll);
    }
    end_window();
  }

  void draw_meters(bool interactive, bool right_side, bool snap, bool hide_decorations, int panel_mode, bool split, bool under_damage,
                   int title_mode) {
    use(E_METERS);
    std::vector<std::string> lines;
    {
      std::lock_guard<std::mutex> lock(t_mutex);
      if (GetTickCount64() - t_time > 1000) return;  // overlay hidden (loading screen, disabled)
      lines = t_lines;
    }
    if (lines.empty()) return;
    ImGuiIO& io = ImGui::GetIO();
    if (!split) {
      draw_meters_window("Meters##sl_tension", "Meters", lines, ImVec2(40 * S, io.DisplaySize.y * 0.40f),
                         interactive, right_side, snap, hide_decorations, panel_mode,
                         under_damage ? placedThisFrame("P1 Damage##sl_damage_p1") : nullptr, nullptr, title_mode);
      return;
    }
    // A window per side, like the game's own 1P and 2P training panels. "P1 "/"P2 "
    // come off each line, since the window title says it; lines for both sides (tension
    // advantage, seen from P1) stay with P1.
    std::vector<std::string> p1, p2;
    for (const auto& l : lines) {
      if (l.rfind("P2 ", 0) == 0) p2.push_back(l.substr(3));
      else p1.push_back(l.rfind("P1 ", 0) == 0 ? l.substr(3) : l);
    }
    if (!p1.empty())
      draw_meters_window("P1 Meters##sl_tension_p1", "P1 Meters", p1, ImVec2(40 * S, io.DisplaySize.y * 0.40f),
                         interactive, false, snap, hide_decorations, panel_mode,
                         under_damage ? placedThisFrame("P1 Damage##sl_damage_p1") : nullptr, nullptr, title_mode);
    if (!p2.empty())
      draw_meters_window("P2 Meters##sl_tension_p2", "P2 Meters", p2, ImVec2(io.DisplaySize.x - 340 * S, io.DisplaySize.y * 0.40f),
                         interactive, true, snap, hide_decorations, panel_mode,
                         under_damage ? placedThisFrame("P2 Damage##sl_damage_p2") : nullptr, nullptr, title_mode);
  }


  // The game's training damage panel in our own windows, one per side.
  // The game's own panel is used as is: just note where it is on screen, so meters
  // pinned under the damage panel sit under it.
  bool game_bounds(const std::vector<imgui_overlay::DamageRow>& rows, ImVec2& lo, ImVec2& hi);
  bool note_game_panel(const char* id, const std::vector<imgui_overlay::DamageRow>& rows) {
    ImVec2 lo, hi;
    if (!game_bounds(rows, lo, hi)) return false;
    const float pad = 10 * S;
    g_placed[id] = PlacedRect{ImVec4(lo.x - pad, lo.y - pad, hi.x - lo.x + 2 * pad, hi.y - lo.y + 2 * pad), ImGui::GetFrameCount()};
    return true;
  }

  // The game's rows' screen rects, all of them, or none when the game hasn't laid them out.
  bool game_bounds(const std::vector<imgui_overlay::DamageRow>& rows, ImVec2& lo, ImVec2& hi) {
    lo = ImVec2(FLT_MAX, FLT_MAX); hi = ImVec2(-FLT_MAX, -FLT_MAX);
    for (const auto& r : rows) {
      if (r.label.empty()) continue;
      if (!r.placed) return false;
      for (const float* q : {r.label_rect, r.value_rect}) {
        lo = ImVec2(std::min(lo.x, q[0]), std::min(lo.y, q[1]));
        hi = ImVec2(std::max(hi.x, q[0] + q[2]), std::max(hi.y, q[1] + q[3]));
      }
    }
    return lo.x < hi.x && lo.y < hi.y;
  }

  // StriveLabs Online: the online features, from anywhere outside a fight (main menu, Park, private lobby)
  // without going to training. That covers the online version (arrows or the menu's Left / Right) and
  // recolor sharing (its switch and what it's doing).
  void draw_online_picker() {
    use(E_MENU);
    ImGuiIO& io = ImGui::GetIO();
    const float pad = PAD * S, lh = f_body->FontSize + 8 * S, bh = f_bold->FontSize + 10 * S;
    const std::string rx_info = recolor_share::enabled() ? recolor_test::shareInfo() : std::string();  // The opponent's recolor, if one came
    static bool confirm = false;
    const std::string saved = recolor_test::shareSaveStatus();
    const bool result_line = !rx_info.empty() && (confirm || !saved.empty());
    const int ver = combo_trials::onlineVersion();
    const bool share_on = recolor_share::enabled();
    const bool share_shown = recolor_share::available();  // (sharing is off in this build: no switch)
    const float h = HEAD * S + pad + bh + 6 * S + (ver ? lh : 0.0f) + (share_shown ? pad + bh + 6 * S + (share_on ? lh : 0.0f) : 0.0f) +
                    (rx_info.empty() ? 0.0f : 6 * S + bh) + (result_line ? 6 * S + lh : 0.0f) + pad;
    const ImVec2 size(440 * S, h);
    Frame f = begin_window("Online version##sl_online", "StriveLabs Online", ImVec2((io.DisplaySize.x - size.x) * 0.5f, io.DisplaySize.y * 0.3f),
                           size, ImVec2(360 * S, size.y), true);
    if (f.closed) g_online_picker = false;
    if (!f.collapsed) {
      auto chip = [&](const char* id, ImVec2 a, ImVec2 b, const std::string& label, ImU32 col) {  // A button with its rect and its text
        ImGui::PushID(id);
        const bool hit = clicked_rect(id, a, b);
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        f.dl->AddRectFilled(a, b, CHIP);
        if (hov) f.dl->AddRectFilled(a, b, HOVER);
        text(f.dl, f_bold, ImVec2((a.x + b.x - text_w(f_bold, label)) * 0.5f, a.y + 5 * S), col, label);
        return hit;
      };
      // The online version can only be picked between fights (both players must be on the same one).
      const std::string name = ver ? battleVersionName(ver) : std::string("Off");
      const float bw = 34 * S;
      float y = f.o.y + f.head + pad;
      const bool fighting = recolor_share::fightRunning();
      if (chip("ver_prev", ImVec2(f.o.x + pad, y), ImVec2(f.o.x + pad + bw, y + bh), "<", TEXT) && !fighting) g_online_pick_step = -1;
      if (chip("ver_next", ImVec2(f.o.x + f.w - pad - bw, y), ImVec2(f.o.x + f.w - pad, y + bh), ">", TEXT) && !fighting) g_online_pick_step = 1;
      text(f.dl, f_bold, ImVec2(f.o.x + (f.w - text_w(f_bold, name)) * 0.5f, y + 5 * S), ver ? RED_TEXT : TEXT, name);
      y += bh + 6 * S;
      if (ver) {
        text(f.dl, f_body, ImVec2(f.o.x + pad, y), MUTED, "Park and rooms only. Opponent must match.");
        y += lh;
      }
      // Recolor sharing, with its switch, what it's doing, and the opponent's recolor to keep.
      if (share_shown) {
      y += pad;
      f.dl->AddLine(ImVec2(f.o.x + pad, y - pad * 0.5f), ImVec2(f.o.x + f.w - pad, y - pad * 0.5f), DIVIDER);
      const bool on = share_on;
      const std::string label = std::string("Recolor sharing: ") + (on ? "On" : "Off");
      if (chip("share", ImVec2(f.o.x + pad, y), ImVec2(f.o.x + pad + text_w(f_bold, label) + 20 * S, y + bh), label, on ? RED_TEXT : TEXT))
        recolor_share::setEnabled(!on);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Both players need it on. Shares your Steam ID with them.");
      y += bh + 6 * S;
      if (on) {  // When off, the switch already says so
        text(f.dl, f_body, ImVec2(f.o.x + pad, y), TEXT, recolor_share::status());
        y += lh;
      }
      if (!rx_info.empty()) {
        static std::string for_info;
        static int slot = 1;
        const std::string chr = rx_info.substr(0, rx_info.find(' '));
        if (for_info != rx_info) {
          for_info = rx_info;
          confirm = false;
          const size_t c = rx_info.find("Color");
          slot = c != std::string::npos ? std::clamp(std::atoi(rx_info.c_str() + c + 5), 1, 99) : 1;
        }
        char slot_name[16];
        snprintf(slot_name, sizeof(slot_name), "Color%02d", slot);
        y += 6 * S;
        const std::string slot_shown = recolor_test::slotLabel(slot_name);  // Shown as "Color 5", while slot_name stays "Color05"
        const float sw = 30 * S, nw = text_w(f_bold, "Color 00") + 20 * S;
        const ImVec2 l0(f.o.x + pad, y), r0(l0.x + sw + nw, y);
        if (chip("slot_prev", l0, ImVec2(l0.x + sw, y + bh), "<", TEXT)) { slot = slot > 1 ? slot - 1 : 99; confirm = false; }
        text(f.dl, f_bold, ImVec2(l0.x + sw + (nw - text_w(f_bold, slot_shown)) * 0.5f, y + 5 * S), TEXT, slot_shown);
        if (chip("slot_next", r0, ImVec2(r0.x + sw, y + bh), ">", TEXT)) { slot = slot < 99 ? slot + 1 : 1; confirm = false; }
        const std::string save_label = confirm ? "Replace it" : "Save";
        const ImVec2 s0(r0.x + sw + 10 * S, y);
        if (chip("save", s0, ImVec2(s0.x + text_w(f_bold, save_label) + 20 * S, y + bh), save_label, confirm ? RED_TEXT : TEXT)) {
          if (!confirm && recolor_test::slotHasRecolor(chr, slot_name)) confirm = true;  // Ask first, since yours gets replaced
          else { recolor_test::shareSave(slot_name); confirm = false; }
        }
        y += bh + 6 * S;
        if (confirm) text(f.dl, f_body, ImVec2(f.o.x + pad, y), RED_TEXT, "Replaces your " + slot_shown + ".");
        else if (!saved.empty()) text(f.dl, f_body, ImVec2(f.o.x + pad, y), MUTED, saved);
      }
      }  // (share_shown)
    }
    end_window();
  }

  // A small window with the battle version the fight runs on: its own spot, moved like
  // the other windows while the menu is open.
  void draw_version(bool interactive, bool hide_decorations, int panel_mode, int title_mode, float bg_alpha) {
    use(E_METERS);
    std::string t;
    {
      std::lock_guard<std::mutex> lock(bv_mutex);
      if (GetTickCount64() - bv_time > 1000) return;  // no battle running
      t = bv_text;
    }
    ImGuiIO& io = ImGui::GetIO();
    const float pad = 10 * S, tw = text_w(f_meters_bold, t), th = f_meters_bold->FontSize;
    const ImVec2 size(tw + 2 * pad, HEAD * S + th + 2 * pad);
    Frame f = begin_window("Battle version##sl_version", "Battle version", ImVec2((io.DisplaySize.x - size.x) * 0.5f, 4 * S),
                           size, ImVec2(60 * S, HEAD * S + th), interactive,
                           (OVERLAY_BG & 0x00FFFFFF) | (static_cast<ImU32>(std::clamp(bg_alpha, 0.0f, 1.0f) * 255.0f) << 24),
                           hide_decorations, f_meters_title, title_mode, panel_mode);
    if (f.closed) ModMenu::instance().click(MenuClick{MenuClick::Type::VersionWindow, 0, 0});
    if (!f.collapsed)
      text_outlined(f.dl, f_meters_bold, ImVec2(f.o.x + (f.w - tw) * 0.5f, f.o.y + f.head + (f.h - f.head - th) * 0.5f), TEXT, t);
    end_window();
  }

  void draw_damage(bool interactive, bool hide_decorations, int panel_mode, bool at_game, int title_mode) {
    use(E_METERS);
    std::vector<imgui_overlay::DamageRow> rows[2];
    {
      std::lock_guard<std::mutex> lock(d_mutex);
      if (GetTickCount64() - d_time > 1000) return;  // not in training, or turned off
      rows[0] = d_rows[0];
      rows[1] = d_rows[1];
    }
    ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    const char* ids[2] = {"P1 Damage##sl_damage_p1", "P2 Damage##sl_damage_p2"};
    const char* titles[2] = {"P1 Damage", "P2 Damage"};
    for (int side = 0; side < 2; side++) {
      if (rows[side].empty()) continue;
      if (at_game) { note_game_panel(ids[side], rows[side]); continue; }
      const ImVec2 def_pos(side ? W - 340 * S : 40 * S, H * 0.22f);
      std::vector<std::string> lines;
      for (const auto& r : rows[side]) lines.push_back(r.label.empty() ? std::string() : r.label + ": " + r.value);
      draw_meters_window(ids[side], titles[side], lines, def_pos,
                         interactive, side == 1, false, hide_decorations, panel_mode, nullptr, nullptr, title_mode);
    }
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

      auto keyed = [](const char* what, int a) {  // Labels like "Rewind (F9)", with the key bound now
        const std::wstring k = binds::key[a] ? binds::keyName(binds::key[a]) : binds::pad[a] ? binds::padName(binds::pad[a]) : std::wstring();
        if (k.empty()) return std::string(what);
        char k8[48]{};
        WideCharToMultiByte(CP_UTF8, 0, k.c_str(), -1, k8, sizeof(k8) - 1, nullptr, nullptr);
        return std::string(what) + " (" + k8 + ")";
      };
      const std::string rw_tip = keyed("Rewind", binds::LOAD), sp_tip = keyed("Set rewind point", binds::SAVE);
      auto [ra, rb] = next(bw);   // rewind: bar + two triangles
      if (icon_button(dl, "rw", ra, rb, false, true, rw_tip.c_str(), [&](ImVec2 c, ImU32 col) {
            dl->AddRectFilled(ImVec2(c.x - r - 2 * S, c.y - r), ImVec2(c.x - r, c.y + r), col);
            arrow(dl, ImVec2(c.x - 1 * S, c.y), r * 0.8f, false, col);
            arrow(dl, ImVec2(c.x + r - 1 * S, c.y), r * 0.8f, false, col); }))
        ModMenu::instance().click(MenuClick{MenuClick::Type::TlRewind});
      auto [sa, sb] = next(bw);   // set point: flag
      if (icon_button(dl, "sp", sa, sb, false, true, sp_tip.c_str(), [&](ImVec2 c, ImU32 col) {
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
                      !can_rec ? "Replays only" : v.recording ? "Stop and save" : "Record (40 s max)",
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
      } else if (v.paused) state = "PAUSED";
      text(dl, f_bold, ImVec2(f.o.x + f.w - pad - text_w(f_bold, state), y + (bh - f_bold->FontSize) * 0.5f), state_col, state);

      // 40 second track: fills while recording, ticks every 10 s
      const float tx0 = f.o.x + pad, tx1 = f.o.x + f.w - pad;
      const float ty = y + bh + 12 * S, th = 8 * S;
      if (v.recording) {  // Only while recording
        dl->AddRectFilled(ImVec2(tx0, ty), ImVec2(tx1, ty + th), CHIP);
        const float frac = std::clamp(v.rec_frames / 2400.0f, 0.0f, 1.0f);
        dl->AddRectFilled(ImVec2(tx0, ty), ImVec2(tx0 + (tx1 - tx0) * frac, ty + th), RED);
        for (int s = 0; s <= 40; s += 10) {
          const float tx = tx0 + (tx1 - tx0) * (s / 40.0f);
          dl->AddLine(ImVec2(tx, ty + th), ImVec2(tx, ty + th + 4 * S), DIM);
          const std::string lab = std::to_string(s) + (s == 40 ? " s" : "");
          const float lx = std::clamp(tx - text_w(f_small, lab) * 0.5f, tx0, tx1 - text_w(f_small, lab));
          text(dl, f_small, ImVec2(lx, ty + th + 5 * S), DIM, lab);
        }
      }
      // Last message (saved file, errors)
      const float sy = v.recording ? ty + th + 5 * S + f_small->FontSize + 4 * S : ty;
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
    // The meters' and the frame bar's backgrounds, each with its own opacity setting.
    OVERLAY_BG = RGBA(4, 6, 14, static_cast<int>(std::clamp(v.overlay_bg, 0.0f, 1.0f) * 255.0f));
    BAR_BG = RGBA(4, 6, 14, static_cast<int>(std::clamp(v.bar_bg, 0.0f, 1.0f) * 255.0f));
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
    std::string t;
    auto add = [&t](const std::string& part) { t += (t.empty() ? "" : "  \xC2\xB7  ") + part; };
    if (s.rewindPoint) add("Rewind point set");
    char buf[64];
    if (s.assist == 1) { snprintf(buf, sizeof(buf), "Assist %.1f s", std::max(0.0f, s.assistLeftSeconds)); add(buf); }
    else if (s.assist == 2) add("Assist: Active");
    else if (s.assistSeconds > 0.0) { snprintf(buf, sizeof(buf), "Assist %.1f s", s.assistSeconds); add(buf); }
    if (s.takeover) { snprintf(buf, sizeof(buf), "Takeover: P%d", s.takeover); add(buf); }
    if (t.empty()) t = s.replay ? "Replay" : "Training";  // Nothing else to say, but keep a line to see and drag

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
      else if (rs.promptRewinds) snprintf(q, sizeof(q), "Stop P%d and rewind?", rs.prompt);
      else snprintf(q, sizeof(q), "Stop taking over P%d?", rs.prompt);
      const int a = rs.prompt == 1 ? binds::TAKEOVER_P1 : binds::TAKEOVER_P2;
      const std::wstring key = binds::key[a] ? binds::keyName(binds::key[a]) : binds::padName(binds::pad[a]);
      char key8[48]{};
      WideCharToMultiByte(CP_UTF8, 0, key.c_str(), -1, key8, sizeof(key8) - 1, nullptr, nullptr);
      snprintf(keys, sizeof(keys), "%s Yes \xC2\xB7 Esc No \xC2\xB7 Pad Right / Left", key8);
      draw_status_message(q, row++);
      draw_status_message(keys, row++);
    }
    if (v.takeover_player && !status_line) {
      char label[32];
      snprintf(label, sizeof(label), "Takeover: P%d", *v.takeover_player);
      draw_status_message(label, row++);
    }
    if (rs.assist == 1 && !status_line) draw_status_message("Replay Assist: Standby", row++, rs.assistFraction);  // Otherwise it's on the status line
    else if (rs.assist == 2 && !status_line) draw_status_message("Replay Assist: Active", row++);
    if (rs.rewindLeftSeconds > 0.0f) {
      char label[64];
      snprintf(label, sizeof(label), "Playback in %.1f s", rs.rewindLeftSeconds);
      draw_status_message(label, row++, rs.rewindFraction);
    }
    if (ModMenu::instance().isRewindPointNotificationVisible() && !status_line)  // Otherwise "Rewind point set" shows on the status line
      draw_status_message("Rewind point created", row++);
  }

  // The live color editor (recolor_test), in a window of its own so the fighter stays in view. Clicking a
  // row makes that part blink on the fighter (Ctrl+click adds, Shift+click selects a range). Clicking a
  // swatch opens a picker with a hex field, and the fighter changes when it's let go. Right-clicking a row
  // gives copy, paste and reset. Gradient spreads colors over the selection, in the order it was picked.
  int g_pick_region = -1, g_pick_which = 0;  // The open picker's region, and 0 for lit or 1 for shadow
  float g_pick_col[3] = {};
  std::vector<int> g_sel;                    // selected regions, in the order picked
  int g_sel_anchor = -1;                     // where a Shift range starts
  bool g_pick_armed = false;                 // With "Pick on fighter" on, the next click on the game picks
  bool g_texview = false;                    // The "Texture" window, showing the flat texture with the selection lit
  ImVec2 g_pick_click;                       // Where the pick click was
  bool g_clip_set = false;                   // Set once a color pair has been copied
  uint32_t g_clip_lit = 0, g_clip_shadow = 0;
  bool g_grad_open = false;
  int g_tab = 0;  // The editor's tab (0 Regions, 1 Auto, 2 Effects, 3 Save & share, 4 Bake), kept while closed
  float g_grad_from[3] = {0.55f, 0.05f, 0.08f}, g_grad_to[3] = {1.0f, 0.75f, 0.2f}, g_grad_mid[3] = {0.95f, 0.30f, 0.10f};
  bool g_grad_use_mid = false;  // The custom gradient also has a middle color
  bool g_grad_each = true;      // Every region gets its own step, a row's look-alikes too, instead of one step per row
  bool g_grad_dark = true;      // Dark to light by the worn color, otherwise in the order picked
  ImU32 rgb_u32(uint32_t c) { return RGBA(static_cast<int>(c >> 16 & 0xFF), static_cast<int>(c >> 8 & 0xFF), static_cast<int>(c & 0xFF)); }
  uint32_t pack_rgb(const float c[3]) {
    uint32_t v = 0;
    for (int k = 0; k < 3; k++) v = (v << 8) | static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(c[k] * 255.0f)), 0, 255));
    return v;
  }
  void unpack_rgb(uint32_t v, float c[3]) { for (int k = 0; k < 3; k++) c[k] = static_cast<float>(v >> (16 - 8 * k) & 0xFF) / 255.0f; }
  std::string region_label(const recolor_test::View& v, int r) {
    if (r >= 0 && r < static_cast<int>(v.regions.size()) && !v.regions[static_cast<size_t>(r)].name.empty()) return v.regions[static_cast<size_t>(r)].name;
    return "Region " + std::to_string(r + 1);
  }
  int g_rename_region = -1;  // the row whose name is being typed
  bool g_rename_focus = false;
  char g_rename_buf[64] = {};
  std::string hex_of(uint32_t c) {
    char b[8];
    snprintf(b, sizeof(b), "#%06X", static_cast<unsigned>(c & 0xFFFFFF));
    return b;
  }
  bool small_button(ImDrawList* dl, const char* id, const char* label, ImVec2 a, bool enabled = true, bool on = false) {
    const float bw = text_w(f_small, label) + 22 * S, bh = CHIP_H * S;
    const ImVec2 b(a.x + bw, a.y + bh);
    const bool hit = clicked_rect(id, a, b) && enabled;
    const bool hov = enabled && ImGui::IsItemHovered();
    dl->AddRectFilled(a, b, on || hov ? RED : CHIP);
    dl->AddRect(a, b, enabled ? RED : BORDER);
    text(dl, f_small, ImVec2(a.x + 11 * S, a.y + (bh - f_small->FontSize) * 0.5f), !enabled ? DIM : on || hov ? ON_ACCENT : TEXT, label);
    return hit;
  }
  float small_button_w(const char* label) { return text_w(f_small, label) + 22 * S; }
  // ImGui's own widgets (picker, popups) in the menu's colors.
  void push_widget_style() {
    const auto c = [](ImU32 v) { return ImGui::ColorConvertU32ToFloat4(v); };
    ImGui::PushStyleColor(ImGuiCol_FrameBg, c(CHIP));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, c(RGBA(36, 42, 60)));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, c(RGBA(46, 52, 72)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, c(RGBA(9, 11, 20, 250)));
    ImGui::PushStyleColor(ImGuiCol_Border, c(BORDER));
    ImGui::PushStyleColor(ImGuiCol_Text, c(TEXT));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, c(DIM));
    ImGui::PushStyleColor(ImGuiCol_Header, c(CURSOR));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, c(RGBA(255, 0, 95, 90)));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, c(RGBA(255, 0, 95, 140)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6 * S, 4 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * S, 8 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * S, 5 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
  }
  void pop_widget_style() { ImGui::PopStyleVar(4); ImGui::PopStyleColor(10); }

  // Palettes: 50 picked from Color Hunt, Coolors, Lospec and SchemeColor, each ordered darkest to lightest.
  // Auto can fill its slots from one (Palettes), and each also works as a gradient with its colors as stops.
  struct Palette { const char* name; const char* mood; std::vector<uint32_t> colors; };
  const std::vector<Palette>& palettes() {
    static const std::vector<Palette> p = {
        {"Nightshade", "gothic", {0x2E073F, 0x7A1CAC, 0xAD49E1, 0xEBD3F8}},
        {"Ink Pink", "gothic", {0x260D34, 0x452459, 0x87286A, 0xD03791, 0xFE6C90, 0xFFFFFF}},
        {"Vampire", "vampire", {0x000000, 0x8E1616, 0xE8C999, 0xF8EEDF}},
        {"Evil Rose", "dark", {0x000A05, 0x6B0D0D, 0x805232, 0xC80027, 0xE49470, 0xF3CFBF}},
        {"Demon Eyes", "demonic", {0x221622, 0x4C3A4B, 0x725D6F, 0x968496, 0xFFC200}},
        {"Inferno", "fiery", {0x03071E, 0x6A040F, 0xDC2F02, 0xF48C06, 0xFFBA08}},
        {"Ember", "fiery", {0x3B060A, 0x8A0000, 0xC83F12, 0xFFF287}},
        {"Rust Flame", "warm", {0x521C0D, 0xD5451B, 0xFF9B45, 0xF4E7E1}},
        {"Glacier", "icy", {0x0F2854, 0x1C4D8D, 0x4988C4, 0xBDE8F5}},
        {"Deep Steel", "cold", {0x0D1321, 0x1D2D44, 0x3E5C76, 0x748CAB, 0xF0EBD8}},
        {"Polar", "icy", {0x070810, 0x18284A, 0x52A5DE, 0xACD6F6, 0xEBF9FF}},
        {"Sea Slate", "calm", {0x2C3333, 0x395B64, 0xA5C9CA, 0xE7F6F2}},
        {"Arcade", "neon", {0x272727, 0x900DFF, 0xFF0181, 0x32DBF0, 0xFAE100}},
        {"Synth Gold", "neon", {0x090040, 0x471396, 0xB13BFF, 0xFFCC00}},
        {"Vaporwave", "neon", {0x000000, 0x9929EA, 0xFF5FCF, 0xFAEB92}},
        {"Toxic", "toxic", {0x000000, 0x4100C2, 0xEE1111, 0x00E6BF, 0xC0FF38}},
        {"Cyber Gum", "cyber", {0x3A2B3B, 0x2D4A54, 0x0C7475, 0xBC4A9B, 0xEB8D9C, 0xFFD8BA}},
        {"Dragon Volt", "neon", {0x010506, 0xE82B2B, 0x018AE6, 0x46FDFF, 0xF7F21D}},
        {"Oil Pastel", "pastel", {0x272744, 0x494D7E, 0x8B6D9C, 0xC69FA5, 0xF2D3AB, 0xFBF5EF}},
        {"Twilight", "pastel", {0x292831, 0x333F58, 0x4A7A96, 0xEE8695, 0xFBBBAD}},
        {"Sweet Bloom", "pastel", {0x253657, 0x6F4367, 0xE76D8A, 0xF5A1A1, 0xF8E9E5}},
        {"Candy Navy", "pastel", {0x15173D, 0x982598, 0xE491C9, 0xF1E9E9}},
        {"Field Camo", "military", {0x322822, 0x454E35, 0x6C5744, 0x957A5A, 0xDBB57E}},
        {"Olive Drab", "earthy", {0x283618, 0x606C38, 0xBC6C25, 0xDDA15E, 0xFEFAE0}},
        {"Fern Sand", "earthy", {0x1B211A, 0x628141, 0x8BAE66, 0xEBD5AB}},
        {"Earth Tone", "earthy", {0x0A0908, 0x22333B, 0x5E503F, 0xC6AC8F, 0xEAE0D5}},
        {"Navy Khaki", "earthy", {0x0A2947, 0x8B5E3C, 0xD3D4C0, 0xF3E4C9}},
        {"Black Gold", "royal", {0x000000, 0x14213D, 0xFCA311, 0xE5E5E5, 0xFFFFFF}},
        {"Purple Knight", "royal", {0x301C51, 0x6A5A85, 0x9D8BB3, 0xF5DB9B}},
        {"Majesty", "royal", {0x000D80, 0xBD1000, 0xBD8A00, 0xF9AD15}},
        {"Amethyst", "royal", {0x210F37, 0x4F1C51, 0xA55B4B, 0xDCA06D}},
        {"Violet Night", "royal", {0x10002B, 0x3C096C, 0x7B2CBF, 0xC77DFF, 0xE0AAFF}},
        {"Gunmetal Gold", "sleek", {0x222831, 0x393E46, 0xFFD369, 0xEEEEEE}},
        {"Carbon Teal", "sleek", {0x212121, 0x323232, 0x0D7377, 0x14FFEC}},
        {"Rustic Charm", "sleek", {0x252422, 0x403D39, 0xEB5E28, 0xCCC5B9, 0xFFFCF2}},
        {"Summer Hero", "heroic", {0x023047, 0x219EBC, 0xFB8500, 0x8ECAE6, 0xFFB703}},
        {"Flag Bearer", "heroic", {0x003049, 0xD62828, 0xF77F00, 0xFCBF49, 0xEAE2B7}},
        {"Star Captain", "heroic", {0x1D3557, 0x457B9D, 0xE63946, 0xA8DADC, 0xF1FAEE}},
        {"Pico Hero", "heroic", {0x1D2B53, 0x7E2553, 0xFF004D, 0xFAEF5D}},
        {"Angel Devil", "heroic", {0x1C1C1C, 0xE60100, 0xEEB720, 0xC5EFF6, 0xFDFBFA}},
        {"Dusk Ramp", "vintage", {0x0D2B45, 0x544E68, 0x8D697A, 0xD08159, 0xFFAA5E, 0xFFECD6}},
        {"Old Banner", "vintage", {0x540B0E, 0x335C67, 0x9E2A2B, 0xE09F3E, 0xFFF3B0}},
        {"Faded Coral", "vintage", {0x0F3040, 0x464858, 0xA56F63, 0xD99B7F}},
        {"Oxblood", "vintage", {0x2D0000, 0x6D0808, 0x757D6F, 0xEEEAD7}},
        {"Samurai", "samurai", {0x04373A, 0xB42D1C, 0x378673, 0xC09554, 0x91BF9F, 0xE5DFA3}},
        {"Pirate", "pirate", {0x010307, 0x395461, 0x835211, 0x449FAF, 0xF4D499, 0xB1F4FC}},
        {"Red Dragon", "draconic", {0x180606, 0x610008, 0xA32015, 0xC57200, 0xCDB023}},
        {"Jester", "chaotic", {0x000200, 0x270E42, 0x084605, 0x710A03, 0xCCD1D4, 0xFCFCFC}},
        {"Saiyan Kid", "anime", {0x252525, 0x8C2D0B, 0x076700, 0xC60209, 0xE4BF55, 0xE4CDBC}},
        {"Monk Orange", "anime", {0x101010, 0x4A5779, 0xC58945, 0xEF8028, 0xF5B877, 0xF9F8F2}},
    };
    return p;
  }
  // Gradient presets as color stops, dark to light. Rainbow and Mono are made on the fly. The palettes come
  // after them.
  struct GradPreset { const char* name; std::vector<uint32_t> stops; };
  const std::vector<GradPreset>& grad_presets() {
    static const std::vector<GradPreset> p = {
        {"Rainbow", {}},  // The first seven stay in their old places, since a slot's gradient is kept by its number
        {"Fire", {0x2A0000, 0x7A0000, 0xD42A00, 0xFF7A00, 0xFFD54A}},
        {"Ice", {0x060B2E, 0x0B1D51, 0x2F6FD6, 0x6FC3FF, 0xDDF9FF}},
        {"Toxic", {0x061A0A, 0x0F3D0F, 0x2E9E1F, 0x8BFF3A, 0xEFFFB0}},
        {"Sunset", {0x1B0A3C, 0x4B1673, 0xB52E6B, 0xF26B4F, 0xFFC46B}},
        {"Royal", {0x12002A, 0x3D0A6B, 0x7A2DB8, 0xB98A2E, 0xF2D58A}},
        {"Mono", {}},
        {"Blood", {0x120004, 0x3D0010, 0x7A0A1E, 0xB8122B, 0xE85A5A}},
        {"Ocean", {0x021821, 0x05374D, 0x0B6E7F, 0x2DB3A6, 0xB6F2DC}},
        {"Gold", {0x1F1203, 0x5C3A0A, 0xA6741C, 0xE0B341, 0xFFF0B3}},
        {"Neon", {0x14002E, 0x5B00B5, 0xFF00A8, 0xFF7AD9, 0x7AF6FF}},
        {"Forest", {0x0B1408, 0x22331A, 0x4A5E2A, 0x8C8A4B, 0xD9CFA0}},
        {"Steel", {0x0E1116, 0x2B323C, 0x56606E, 0x9AA5B1, 0xE6ECF2}},
        {"Rose", {0x2A0A16, 0x6B1F3C, 0xB44B73, 0xE89AB4, 0xFFE3EC}},
        {"Ember", {0x140A08, 0x3A1B12, 0x7A3418, 0xC7561E, 0xF2A65A}},
        {"Void", {0x05030A, 0x150B2B, 0x2E1B5C, 0x5E3FA8, 0xB9A6F2}},
    };
    return p;
  }
  uint32_t lerp_rgb(uint32_t a, uint32_t b, float t) {
    uint32_t v = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
      const float x = static_cast<float>(a >> sh & 0xFF) + (static_cast<float>(b >> sh & 0xFF) - static_cast<float>(a >> sh & 0xFF)) * t;
      v |= static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(x)), 0, 255)) << sh;
    }
    return v;
  }
  uint32_t hsv_rgb(float h, float s, float v) {
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(h - std::floor(h), std::clamp(s, 0.0f, 1.0f), std::clamp(v, 0.0f, 1.0f), r, g, b);
    const float c[3] = {r, g, b};
    return pack_rgb(c);
  }
  // OKLab (Bjorn Ottosson's) gives lightness, chroma and hue as the eye sees them. Gradients blend in it
  // (no muddy middle), and New colors builds its palettes in it (h in turns, 0..1).
  float to_lin(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
  float to_srgb(float c) { return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }
  void rgb_oklab(uint32_t v, float lab[3]) {
    float c[3];
    unpack_rgb(v, c);
    const float r = to_lin(c[0]), g = to_lin(c[1]), b = to_lin(c[2]);
    const float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float q = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    lab[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * q;
    lab[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * q;
    lab[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * q;
  }
  bool oklab_rgb(const float lab[3], float out[3]) {  // Returns false when outside sRGB (the result is clamped)
    const float l = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
    const float m = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
    const float q = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];
    const float L3 = l * l * l, M3 = m * m * m, Q3 = q * q * q;
    const float lin[3] = {4.0767416621f * L3 - 3.3077115913f * M3 + 0.2309699292f * Q3,
                          -1.2684380046f * L3 + 2.6097574011f * M3 - 0.3413193965f * Q3,
                          -0.0041960863f * L3 - 0.7034186147f * M3 + 1.7076147010f * Q3};
    bool in = true;
    for (int k = 0; k < 3; k++) {
      in = in && lin[k] >= -0.001f && lin[k] <= 1.001f;
      out[k] = std::clamp(to_srgb(std::clamp(lin[k], 0.0f, 1.0f)), 0.0f, 1.0f);
    }
    return in;
  }
  uint32_t oklch_rgb(float L, float C, float h) {  // Lowers the chroma until it fits, keeping the hue and lightness
    float c[3];
    for (int i = 0; i < 40; i++, C *= 0.92f) {
      const float lab[3] = {L, C * std::cos(h * 6.2831853f), C * std::sin(h * 6.2831853f)};
      if (oklab_rgb(lab, c) || C < 0.002f) break;
    }
    return pack_rgb(c);
  }
  uint32_t lerp_oklab(uint32_t a, uint32_t b, float t) {
    float la[3], lb[3], m[3], c[3];
    rgb_oklab(a, la);
    rgb_oklab(b, lb);
    for (int k = 0; k < 3; k++) m[k] = la[k] + (lb[k] - la[k]) * t;
    oklab_rgb(m, c);
    return pack_rgb(c);
  }
  // A preset's color at t (0..1). A preset of -1 is the custom one. base is the first selected region's color.
  uint32_t grad_at(int preset, float t, uint32_t base) {
    if (preset < 0) {
      if (!g_grad_use_mid) return lerp_oklab(pack_rgb(g_grad_from), pack_rgb(g_grad_to), t);
      return t < 0.5f ? lerp_oklab(pack_rgb(g_grad_from), pack_rgb(g_grad_mid), t * 2.0f) : lerp_oklab(pack_rgb(g_grad_mid), pack_rgb(g_grad_to), t * 2.0f - 1.0f);
    }
    const size_t np = grad_presets().size();
    if (static_cast<size_t>(preset) >= np) {  // A palette, with its colors as the stops
      const auto& st = palettes()[(std::min)(static_cast<size_t>(preset) - np, palettes().size() - 1)].colors;
      const float x = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(st.size() - 1);
      const size_t i = std::min(static_cast<size_t>(x), st.size() - 2);
      return lerp_oklab(st[i], st[i + 1], x - static_cast<float>(i));
    }
    const GradPreset& p = grad_presets()[static_cast<size_t>(preset)];
    if (!std::strcmp(p.name, "Rainbow")) {  // Yellow lighter, as the eye sees it, so it isn't mustard
      const float h = 0.08f + t * 0.85f;
      return oklch_rgb(0.70f + 0.17f * (std::max)(0.0f, 1.0f - std::fabs(h - 0.30f) / 0.12f), 0.16f, h);
    }
    if (!std::strcmp(p.name, "Mono")) {  // The base color's hue, dark to light
      float lab[3];
      rgb_oklab(base, lab);
      const float C = std::sqrt(lab[1] * lab[1] + lab[2] * lab[2]), h = std::atan2(lab[2], lab[1]) / 6.2831853f;
      return oklch_rgb(0.18f + 0.74f * t, C * (0.6f + 0.4f * std::sin(t * 3.14159f)), h);
    }
    const auto& st = p.stops;
    if (st.size() == 1) return st[0];
    const float x = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(st.size() - 1);
    const size_t i = std::min(static_cast<size_t>(x), st.size() - 2);
    return lerp_oklab(st[i], st[i + 1], x - static_cast<float>(i));
  }
  int g_regions_n = 0;  // the regions listed now
  std::string part_name(const std::string& k) { return recolor_test::partName(k); }
  std::vector<int> g_leads;  // Per region, the row it's listed under. Regions that look the same share one
  int lead_of(int r) { return r >= 0 && r < static_cast<int>(g_leads.size()) ? g_leads[static_cast<size_t>(r)] : r; }
  void set_selection(const std::vector<int>& s) {
    g_sel.clear();
    for (int r : s) {
      if (r < 0 || r >= g_regions_n) continue;  // another target's numbers don't carry over
      const int l = lead_of(r);                 // For a twin, its row
      if (std::find(g_sel.begin(), g_sel.end(), l) == g_sel.end()) g_sel.push_back(l);
    }
    recolor_test::selectMany(g_sel);
  }

  // Auto colors spread two to ten slots (each a color or a gradient) over the outfit's parts. Lines and (by
  // default) skin stay. The biggest parts take the slots in turn, and smaller parts go with the big part
  // their color was closest to. Neutrals (metal, white cloth) get a faint tint. Every part keeps its
  // brightness relative to the others on its slot, so darker parts stay darker.
  // Swap moves the slots around under the same rules, and New colors gives the slots new colors.
  struct Slot { bool gradient = false; int preset = 0; float col[3] = {}; };
  constexpr int kMaxSlots = 10;
  Slot g_slots[kMaxSlots] = {{false, 0, {0.72f, 0.08f, 0.14f}}, {false, 0, {0.10f, 0.12f, 0.18f}},
                             {false, 0, {0.90f, 0.84f, 0.72f}}, {false, 0, {0.86f, 0.64f, 0.14f}}};
  const char* const kSlotNames[kMaxSlots] = {"Main", "Second", "Third", "Accent", "Extra 1", "Extra 2", "Extra 3", "Extra 4", "Extra 5", "Extra 6"};
  int g_nslots = 4;  // 2 to kMaxSlots
  bool g_keep_skin = true;
  int g_seed = 0;
  struct AutoLook { int seed = 0, n = 0; Slot slots[kMaxSlots]; };
  std::vector<AutoLook> g_looks;  // Earlier looks (from Swap, New colors), newest last. Previous goes back through them
  std::vector<int> g_slot_regions[kMaxSlots];  // Where each slot went last time, for showing them
  // The skin before Auto changed it, per fighter and part ("SOL Color 8#0"). Maps region -> lit, shadow, and
  // whether you had edited it then. Turning Keep skin back on puts it back: your own edits exactly, and
  // untouched regions reset (not turned into edits).
  struct SkinSnap { uint32_t lit = 0, shadow = 0; bool edited = false; };
  struct SkinSnaps { uint64_t map_sig = 0; std::map<int, SkinSnap> regions; };  // map_sig is the regions as they were. If they were remapped since, the snapshot is dropped
  std::map<std::string, SkinSnaps> g_skin_snap;
  uint64_t map_sig(const recolor_test::View& v) {  // The part's region map as numbered now (count and the game's colors)
    uint64_t h = 1469598103934665603ull ^ v.regions.size();
    for (const auto& rg : v.regions) h = (h ^ rg.orig_lit) * 1099511628211ull;
    return h;
  }
  // Called when it's another fighter, the body / weapon switches, or the editor closes, since what was picked
  // belongs to the old one.
  void forget_selection() {
    g_sel.clear();
    g_sel_anchor = -1;
    g_pick_region = -1;
    for (auto& sr : g_slot_regions) sr.clear();
  }
  float luma(uint32_t c) { return (0.2126f * (c >> 16 & 0xFF) + 0.7152f * (c >> 8 & 0xFF) + 0.0722f * (c & 0xFF)) / 255.0f; }
  uint32_t slot_color(int s, float t, uint32_t base) {
    const Slot& sl = g_slots[s];
    return sl.gradient ? grad_at(sl.preset, t, base) : pack_rgb(sl.col);
  }
  float hue_gap(uint32_t a, uint32_t b) {
    float ca[3], cb[3], ha, sa, va, hb, sb, vb;
    unpack_rgb(a, ca); unpack_rgb(b, cb);
    ImGui::ColorConvertRGBtoHSV(ca[0], ca[1], ca[2], ha, sa, va);
    ImGui::ColorConvertRGBtoHSV(cb[0], cb[1], cb[2], hb, sb, vb);
    const float dh = std::min(std::fabs(ha - hb), 1.0f - std::fabs(ha - hb));
    return dh * 2.0f * std::min(sa, sb) + std::fabs(va - vb) + std::fabs(sa - sb) * 0.5f;
  }
  std::mt19937& auto_rng() { static std::mt19937 r(std::random_device{}()); return r; }
  float auto_rand(float a, float b) { return a + (b - a) * std::uniform_real_distribution<float>(0.0f, 1.0f)(auto_rng()); }
  // The skin is about to change. Save its colors now, the first time only, so Keep skin can put them back.
  void snap_skin(const recolor_test::View& v, const std::vector<int>& regions) {
    auto& snaps = g_skin_snap[v.title + "#" + std::to_string(v.part)];
    const uint64_t sig = map_sig(v);
    if (snaps.map_sig != sig) snaps = SkinSnaps{sig, {}};
    auto& snap = snaps.regions;
    for (int r : regions)
      if (r >= 0 && r < static_cast<int>(v.regions.size())) {
        const auto& rg = v.regions[static_cast<size_t>(r)];
        snap.emplace(r, SkinSnap{rg.lit, rg.shadow, rg.edited});
      }
  }
  // Keep skin turned back on restores the skin as it was (your edits exactly, untouched regions back to the
  // game's own), and the snapshot is done with.
  std::vector<recolor_test::Pick> skin_restore(const recolor_test::View& v) {
    std::vector<recolor_test::Pick> picks;
    auto it = g_skin_snap.find(v.title + "#" + std::to_string(v.part));
    if (it == g_skin_snap.end()) return picks;
    if (it->second.map_sig == map_sig(v))  // If remapped since, the numbers mean other regions now, so nothing is put back
      for (const auto& [r, c] : it->second.regions) {
        if (r >= static_cast<int>(v.regions.size())) continue;
        picks.push_back(c.edited ? recolor_test::Pick{r, c.lit, 1, c.shadow} : recolor_test::Pick{r, 0, 3, 0});  // Your edit exactly, otherwise the game's color
      }
    g_skin_snap.erase(it);
    return picks;
  }
  // picks holds more picks for the same update (Keep skin's restore), applied first.
  void auto_apply(const recolor_test::View& v, std::vector<recolor_test::Pick> picks = {}) {
    const int n = g_nslots;
    std::vector<const recolor_test::GroupView*> colored, neutral;
    for (const auto& g : v.groups) {
      if (g.line || (g.skin && g_keep_skin) || g.regions.empty()) continue;
      if (g.skin) snap_skin(v, g.regions);  // With Keep skin off the skin changes, so save it to put back later
      (g.neutral ? neutral : colored).push_back(&g);
    }
    auto worn = [&](const recolor_test::GroupView* g) { return v.regions[static_cast<size_t>(g->regions[0])].orig_lit; };
    std::mt19937 rng(static_cast<uint32_t>(g_seed) * 7919u + 13u);
    std::vector<int> perm(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) perm[static_cast<size_t>(i)] = i;
    if (g_seed > 0) std::shuffle(perm.begin(), perm.end(), rng);
    std::vector<int> slot(colored.size(), 0);
    const size_t big = (std::min)(static_cast<size_t>(n), colored.size());
    for (size_t i = 0; i < big; i++) slot[i] = perm[i];
    for (size_t i = big; i < colored.size(); i++) {  // Each goes with the big part it looked most like
      size_t best = 0;
      float gap = 1e9f;
      for (size_t b = 0; b < big; b++) {
        const float d = hue_gap(worn(colored[i]), worn(colored[b]));
        if (d < gap) { gap = d; best = b; }
      }
      slot[i] = slot[best];
      if (g_seed > 0 && rng() % 5 == 0) slot[i] = perm.back();  // Now and then use the last slot (the accent)
    }
    for (auto& sr : g_slot_regions) sr.clear();
    for (int s = 0; s < n; s++) {
      std::vector<size_t> mine;
      for (size_t i = 0; i < colored.size(); i++) if (slot[i] == s) mine.push_back(i);
      if (mine.empty()) continue;
      if (g_slots[s].gradient) {  // Every region of its parts gets its own step, dark to light as worn, so the whole gradient shows
        std::vector<int> rs;
        for (size_t k : mine) rs.insert(rs.end(), colored[k]->regions.begin(), colored[k]->regions.end());
        std::stable_sort(rs.begin(), rs.end(), [&](int a, int b) {
          return luma(v.regions[static_cast<size_t>(a)].orig_lit) < luma(v.regions[static_cast<size_t>(b)].orig_lit);
        });
        for (size_t i = 0; i < rs.size(); i++) {
          const float t = rs.size() == 1 ? 0.5f : static_cast<float>(i) / static_cast<float>(rs.size() - 1);
          picks.push_back({rs[i], grad_at(g_slots[s].preset, t, pack_rgb(g_slots[s].col)), 2, 0});
          g_slot_regions[s].push_back(rs[i]);
        }
        continue;
      }
      const float anchor = std::max(luma(worn(colored[mine[0]])), 0.04f);
      for (size_t k = 0; k < mine.size(); k++) {
        const auto* g = colored[mine[k]];
        const float t = mine.size() == 1 ? 0.0f : static_cast<float>(k) / static_cast<float>(mine.size() - 1);
        float c[3], h, sat, val;
        unpack_rgb(slot_color(s, t, worn(g)), c);
        ImGui::ColorConvertRGBtoHSV(c[0], c[1], c[2], h, sat, val);
        const float ratio = luma(worn(g)) / anchor;
        val = std::clamp(val * std::pow(ratio, 0.85f), 0.04f, 1.0f);
        const uint32_t out = hsv_rgb(h, sat, val);
        for (int r : g->regions) { picks.push_back({r, out, 2, 0}); g_slot_regions[s].push_back(r); }
      }
    }
    const int tint = perm[static_cast<size_t>(1 % n)];  // Neutrals get a faint tint of one slot and keep their own brightness
    for (const auto* g : neutral) {
      float c[3], h, sat, val, wc[3], wh, ws, wv;
      unpack_rgb(slot_color(tint, 0.5f, worn(g)), c);
      ImGui::ColorConvertRGBtoHSV(c[0], c[1], c[2], h, sat, val);
      unpack_rgb(worn(g), wc);
      ImGui::ColorConvertRGBtoHSV(wc[0], wc[1], wc[2], wh, ws, wv);
      const uint32_t out = hsv_rgb(h, sat * 0.18f, wv);
      for (int r : g->regions) { picks.push_back({r, out, 2, 0}); g_slot_regions[tint].push_back(r); }
    }
    if (!picks.empty()) recolor_test::setPairs(picks, 0);  // Each region gets its own part's color, since twins may be different parts
  }
  bool has_eye(const std::string& s) {  // Any case, so "Eye" and "eyelash" both match
    std::string l = s;
    for (auto& ch : l) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return l.find("eye") != std::string::npos;
  }
  // Skin & eyes: a new skin tone (natural to stylised) on the skin groups and a new eye color on the rows
  // named with "eye", nothing else. Each keeps its brightness next to the others, as auto_apply does.
  // It was asked for, so Keep skin is set aside.
  void skin_and_eyes(const recolor_test::View& v) {
    const int R = static_cast<int>(v.regions.size());
    std::vector<char> eye(static_cast<size_t>(R), 0);
    for (int r = 0; r < R; r++) eye[static_cast<size_t>(r)] = has_eye(region_label(v, r)) || has_eye(region_label(v, lead_of(r)));  // For a twin, its row's name
    float sh, ss, sv;  // the skin
    if (auto_rand(0.0f, 1.0f) < 0.6f) { sh = auto_rand(0.02f, 0.10f); ss = auto_rand(0.18f, 0.6f); sv = auto_rand(0.3f, 0.95f); }  // Natural, pale to dark
    else { sh = auto_rand(0.0f, 1.0f); ss = auto_rand(0.1f, 0.55f); sv = auto_rand(0.4f, 0.95f); }  // Stylised, like blue, green or grey
    const float eh = auto_rand(0.0f, 1.0f), es = auto_rand(0.45f, 0.9f), ev = auto_rand(0.5f, 0.95f);  // the eyes
    auto keep = [](uint32_t worn, float anchor, float h, float s, float val) {  // Keeps its brightness next to the first one's
      return hsv_rgb(h, s, std::clamp(val * std::pow(luma(worn) / anchor, 0.85f), 0.04f, 1.0f));
    };
    std::vector<recolor_test::Pick> picks;
    float anchor = -1.0f;
    for (const auto& g : v.groups) {
      if (!g.skin || g.line || g.regions.empty()) continue;
      std::vector<int> mine;
      for (int r : g.regions) if (r >= 0 && r < R && !eye[static_cast<size_t>(r)]) mine.push_back(r);
      if (mine.empty()) continue;
      const uint32_t w = v.regions[static_cast<size_t>(g.regions[0])].orig_lit;
      if (anchor < 0) anchor = (std::max)(luma(w), 0.04f);
      snap_skin(v, mine);  // So turning Keep skin back on can put it back
      const uint32_t out = keep(w, anchor, sh, ss, sv);
      for (int r : mine) picks.push_back({r, out, 2, 0});
    }
    anchor = -1.0f;
    for (int r = 0; r < R; r++) {
      if (!eye[static_cast<size_t>(r)]) continue;
      const uint32_t w = v.regions[static_cast<size_t>(r)].orig_lit;
      if (anchor < 0) anchor = (std::max)(luma(w), 0.04f);
      picks.push_back({r, keep(w, anchor, eh, es, ev), 2, 0});
    }
    if (!picks.empty()) recolor_test::setPairs(picks, 0);
  }
  bool palette_row(size_t i, bool on = false) {  // A popup row with the name, then the colors
    const Palette& p = palettes()[i];
    ImGui::PushID(static_cast<int>(i));
    const bool hit = ImGui::Selectable("##pal", on, 0, ImVec2(250 * S, ImGui::GetTextLineHeight()));
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddText(ImVec2(a.x + 2 * S, a.y), ImGui::GetColorU32(ImGuiCol_Text), p.name);
    const float sw = 18 * S, x = b.x - sw * static_cast<float>(p.colors.size());  // Right-aligned
    for (size_t k = 0; k < p.colors.size(); k++)
      d->AddRectFilled(ImVec2(x + sw * k, a.y + 1 * S), ImVec2(x + sw * (k + 1), b.y - 1 * S), rgb_u32(p.colors[k]));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.mood);
    ImGui::PopID();
    return hit;
  }
  void remember_look();
  // A palette onto the slots, one role each. Main gets its most colorful middle color, Second the darkest,
  // Third the lightest, Accent the most colorful one left, then the rest. Uses as many slots as it has colors.
  void fill_from_palette(size_t i) {
    remember_look();
    std::vector<uint32_t> left = palettes()[i].colors;
    auto lc = [](uint32_t c, float& L, float& C) { float lab[3]; rgb_oklab(c, lab); L = lab[0]; C = std::sqrt(lab[1] * lab[1] + lab[2] * lab[2]); };
    auto take = [&](auto score) {
      size_t best = 0;
      float bs = -1e9f;
      for (size_t k = 0; k < left.size(); k++) {
        float L, C;
        lc(left[k], L, C);
        const float sc = score(L, C);
        if (sc > bs) { bs = sc; best = k; }
      }
      const uint32_t c = left[best];
      left.erase(left.begin() + static_cast<std::ptrdiff_t>(best));
      return c;
    };
    std::vector<uint32_t> out;
    out.push_back(take([](float L, float C) { return C * (1.0f - std::fabs(L - 0.55f)); }));
    if (!left.empty()) out.push_back(take([](float L, float) { return -L; }));
    if (!left.empty()) out.push_back(take([](float L, float) { return L; }));
    if (!left.empty()) out.push_back(take([](float, float C) { return C; }));
    out.insert(out.end(), left.begin(), left.end());
    g_nslots = std::clamp(static_cast<int>(out.size()), 2, kMaxSlots);
    for (int k = 0; k < g_nslots; k++) {
      unpack_rgb(out[static_cast<size_t>(k) % out.size()], g_slots[k].col);
      g_slots[k].gradient = false;
    }
  }
  void remember_look() {  // For Previous. Keeps the last 32
    AutoLook l;
    l.seed = g_seed;
    l.n = g_nslots;
    std::copy(g_slots, g_slots + kMaxSlots, l.slots);
    g_looks.push_back(l);
    if (g_looks.size() > 32) g_looks.erase(g_looks.begin());
  }
  // New colors: a palette that holds together. It picks a color scheme (related hues: close, opposite,
  // split, three apart, one hue, or neutrals with one accent) from a base hue, and gives each slot a role by
  // lightness. Main is the base color, Second a dark, Third a light, Accent the scheme's other hue, and extras
  // are shades of the scheme's hues. It never uses more than the scheme's two or three hues, so the slots
  // never repeat a strong green three times.
  void new_colors() {
    remember_look();
    float h0 = auto_rand(0.0f, 1.0f);
    if (h0 > 0.30f && h0 < 0.40f && auto_rand(0.0f, 1.0f) < 0.7f) h0 = auto_rand(0.0f, 1.0f);  // Yellow-greens are the muddy ones, so pick them less often
    static const std::vector<std::vector<float>> schemes = {
        {0.0f, 1.0f / 12.0f, -1.0f / 12.0f},  // close hues
        {0.0f, 0.5f},                         // opposites
        {0.0f, 0.42f, 0.58f},                 // split opposites
        {0.0f, 1.0f / 3.0f, 2.0f / 3.0f},     // three apart
        {0.0f},                               // one hue
        {0.0f, 0.5f},                         // neutrals and one accent (Main and Second barely colored)
    };
    const uint32_t weights[6] = {3, 3, 2, 1, 2, 3};
    uint32_t pick = auto_rng()() % (g_nslots < 4 ? 11u : 14u), k = 0;  // Under four slots, skip neutrals-and-accent since it would be all grey
    while (pick >= weights[k]) pick -= weights[k++];
    const auto& H = schemes[k];
    const bool neutral = k == 5;
    auto hue = [&](size_t i) { return h0 + H[i % H.size()] + auto_rand(-0.015f, 0.015f); };
    for (int s = 0; s < g_nslots; s++) {
      float L, C, h;
      switch (g_nslots < 4 && s == g_nslots - 1 ? 3 : s) {  // Under four slots, the last one is the accent
        case 0: L = auto_rand(0.42f, 0.68f); C = neutral ? auto_rand(0.01f, 0.04f) : auto_rand(0.09f, 0.17f); h = hue(0); break;  // Main
        case 1: L = auto_rand(0.14f, 0.28f); C = auto_rand(0.01f, 0.06f); h = hue(auto_rand(0.0f, 1.0f) < 0.5f ? 0 : 1); break;     // a dark
        case 2: L = auto_rand(0.80f, 0.93f); C = auto_rand(0.01f, 0.05f); h = hue(0); break;                                         // a light
        case 3: L = auto_rand(0.55f, 0.80f); C = auto_rand(0.12f, 0.20f); h = hue(H.size() - 1); break;                              // Accent
        default: {  // shades of the scheme's hues, dark, mid or light
          const float steps[3] = {0.32f, 0.52f, 0.72f};
          L = steps[auto_rng()() % 3] + auto_rand(-0.05f, 0.05f);
          C = auto_rand(0.03f, 0.13f);
          h = hue(static_cast<size_t>(s - 3));
        }
      }
      if (H.size() == 1 && (s == 3 || (g_nslots < 4 && s == g_nslots - 1))) h = h0 + auto_rand(-0.04f, 0.04f);  // With one hue, the accent is a brighter version of it
      unpack_rgb(oklch_rgb(L, C, h), g_slots[s].col);
      g_slots[s].gradient = false;
    }
  }
  void add_slot() {  // Uses a hue turned away from Main's, so it stands apart
    float h, s, val;
    ImGui::ColorConvertRGBtoHSV(g_slots[0].col[0], g_slots[0].col[1], g_slots[0].col[2], h, s, val);
    const int i = g_nslots++;
    g_slots[i] = Slot{};
    unpack_rgb(hsv_rgb(h + 0.618034f * static_cast<float>(i), (std::max)(s, 0.45f), (std::max)(val, 0.5f)), g_slots[i].col);
  }
  void remove_slot(int s) {
    for (int k = s; k + 1 < g_nslots; k++) g_slots[k] = g_slots[k + 1];
    g_nslots--;
  }
  // The Auto tab shows the slots, then the looks. Clicking a slot shows its color or gradient and where it
  // went, right-click removes it, and "+" adds one more.
  void draw_auto(ImDrawList* dl, float x0, float x1, float& y, const recolor_test::View& v) {
    if (v.groups.empty()) {
      text(dl, f_small, ImVec2(x0, y), MUTED, "Loading...");
      y += f_small->FontSize + 10 * S;
      return;
    }
    const float tw = (x1 - x0 - 4 * S * 3) / 4.0f, th = 40 * S;  // Four to a row
    const int tiles = (std::min)(g_nslots + 1, kMaxSlots);    // The slots, plus "+" while under ten
    for (int s = 0; s < tiles; s++) {
      const ImVec2 a(x0 + static_cast<float>(s % 4) * (tw + 4 * S), y + static_cast<float>(s / 4) * (th + 4 * S)), b(a.x + tw, a.y + th);
      ImGui::PushID(s);
      if (s == g_nslots) {  // The "+" tile adds one more
        const bool hit = clicked_rect("slot_add", a, b);
        const bool hov = ImGui::IsItemHovered();
        dl->AddRect(a, b, hov ? RED : BORDER);
        text(dl, f_bold, ImVec2((a.x + b.x - text_w(f_bold, "+")) * 0.5f, (a.y + b.y - f_bold->FontSize) * 0.5f), hov ? TEXT : MUTED, "+");
        if (hov) ImGui::SetTooltip("Add a slot");
        if (hit) { add_slot(); auto_apply(v); }
        ImGui::PopID();
        continue;
      }
      const float sh = th - f_small->FontSize - 6 * S;
      if (g_slots[s].gradient)
        for (int k = 0; k < 16; k++)
          dl->AddRectFilled(ImVec2(a.x + tw * k / 16.0f, a.y), ImVec2(a.x + tw * (k + 1) / 16.0f, a.y + sh),
                            rgb_u32(grad_at(g_slots[s].preset, k / 15.0f, pack_rgb(g_slots[s].col))));
      else
        dl->AddRectFilled(a, ImVec2(b.x, a.y + sh), rgb_u32(pack_rgb(g_slots[s].col)));
      const bool hit = clicked_rect("slot", a, b);
      const bool hov = ImGui::IsItemHovered();
      dl->AddRect(a, b, hov ? RED : BORDER);
      text(dl, f_small, ImVec2(a.x + 4 * S, b.y - f_small->FontSize - 2 * S), hov ? TEXT : LABEL, kSlotNames[s]);
      bool removed = false;
      push_widget_style();
      if (ImGui::BeginPopupContextItem("slot_ctx")) {
        if (ImGui::MenuItem("Remove", nullptr, false, g_nslots > 2)) { remove_slot(s); removed = true; }
        ImGui::EndPopup();
      }
      pop_widget_style();
      if (removed) { auto_apply(v); ImGui::PopID(); break; }  // The rest moved down, so they're drawn next frame
      if (hit) {
        ImGui::OpenPopup("slotpop");
        if (!g_slot_regions[s].empty()) set_selection(g_slot_regions[s]);
      }
      push_widget_style();
      if (ImGui::BeginPopup("slotpop")) {
        ImGui::TextUnformatted(kSlotNames[s]);
        if (!g_slots[s].gradient) {
          ImGui::SetNextItemWidth(240 * S);
          ImGui::ColorPicker3("##slotcol", g_slots[s].col, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview |
                                                          ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_DisplayHex);
          if (ImGui::IsItemDeactivatedAfterEdit()) auto_apply(v);
        }
        ImGui::Separator();
        ImGui::TextDisabled("Or a gradient:");
        const auto& ps = grad_presets();
        for (size_t i = 0; i < ps.size(); i++) {
          if (i % 4) ImGui::SameLine();
          if (ImGui::Selectable(ps[i].name, g_slots[s].gradient && g_slots[s].preset == static_cast<int>(i), 0, ImVec2(56 * S, 0))) {
            g_slots[s].gradient = true;
            g_slots[s].preset = static_cast<int>(i);
            auto_apply(v);
          }
        }
        if (ImGui::BeginMenu("A palette's colors")) {
          for (size_t i = 0; i < palettes().size(); i++) {
            const int id = static_cast<int>(ps.size() + i);
            if (palette_row(i, g_slots[s].gradient && g_slots[s].preset == id)) { g_slots[s].gradient = true; g_slots[s].preset = id; auto_apply(v); }
          }
          ImGui::EndMenu();
        }
        if (g_slots[s].gradient && ImGui::Selectable("Single color")) { g_slots[s].gradient = false; auto_apply(v); }
        ImGui::EndPopup();
      }
      pop_widget_style();
      ImGui::PopID();
    }
    y += static_cast<float>((tiles + 3) / 4) * (th + 4 * S) + 4 * S;
    float bx = x0;
    auto at = [&](const char* label) {  // The next button's place, on another row when it doesn't fit
      const float w = small_button_w(label);
      if (bx > x0 && bx + w > x1) { bx = x0; y += CHIP_H * S + 4 * S; }
      const ImVec2 p(bx, y);
      bx += w + 4 * S;
      return p;
    };
    if (small_button(dl, "auto_apply", "Apply", at("Apply"))) auto_apply(v);
    if (small_button(dl, "auto_swap", "Swap", at("Swap"))) { remember_look(); g_seed++; auto_apply(v); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Your colors trade places");
    if (small_button(dl, "auto_new", "New colors", at("New colors"))) { new_colors(); auto_apply(v); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("New colors for the slots");
    if (small_button(dl, "auto_pal", "Palettes", at("Palettes"))) ImGui::OpenPopup("auto_palettes");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("One of 50 palettes onto the slots");
    push_widget_style();
    if (ImGui::BeginPopup("auto_palettes")) {
      if (ImGui::Selectable("Random palette")) { fill_from_palette(auto_rng()() % palettes().size()); auto_apply(v); }
      ImGui::Separator();
      ImGui::BeginChild("pals", ImVec2(272 * S, 320 * S));
      for (size_t i = 0; i < palettes().size(); i++)
        if (palette_row(i)) { fill_from_palette(i); auto_apply(v); ImGui::CloseCurrentPopup(); }
      ImGui::EndChild();
      ImGui::EndPopup();
    }
    pop_widget_style();
    if (small_button(dl, "auto_skin", "Skin & eyes", at("Skin & eyes"))) skin_and_eyes(v);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Only skin and eyes (rows named with \"eye\")");
    if (small_button(dl, "auto_back", "Previous", at("Previous"), !g_looks.empty())) {
      const AutoLook l = g_looks.back();
      g_looks.pop_back();
      g_seed = l.seed;
      g_nslots = l.n;
      std::copy(l.slots, l.slots + kMaxSlots, g_slots);
      auto_apply(v);
    }
    y += CHIP_H * S + 8 * S;
    {  // Keep skin (turned on again, the skin Auto changed goes back as it was), and which look this is
      const float tw2 = 36 * S, th2 = 18 * S;
      const ImVec2 ta(x1 - tw2, y), tb(x1, y + th2);
      const char* ks = "Keep skin";
      const float kx0 = ta.x - 8 * S - text_w(f_small, ks);
      text(dl, f_small, ImVec2(kx0, y + (th2 - f_small->FontSize) * 0.5f), LABEL, ks);
      if (clicked_rect("keepskin", ImVec2(kx0, y), tb)) {
        g_keep_skin = !g_keep_skin;
        auto_apply(v, g_keep_skin ? skin_restore(v) : std::vector<recolor_test::Pick>{});
      }
      dl->AddRectFilled(ta, tb, g_keep_skin ? RED : CHIP);
      const float k = th2 - 6 * S, kx = g_keep_skin ? tb.x - 3 * S - k : ta.x + 3 * S;
      dl->AddRectFilled(ImVec2(kx, ta.y + 3 * S), ImVec2(kx + k, ta.y + 3 * S + k), g_keep_skin ? ON_ACCENT : MUTED);
      text(dl, f_small, ImVec2(x0, y + (th2 - f_small->FontSize) * 0.5f), DIM, "Look " + std::to_string(g_seed + 1));
      y += th2 + 10 * S;
    }
  }
  void apply_gradient(int preset, const recolor_test::View& v) {
    if (g_sel.empty()) return;
    const int R = static_cast<int>(v.regions.size());
    const uint32_t base = g_sel[0] < R ? v.regions[static_cast<size_t>(g_sel[0])].lit : 0;
    std::vector<int> rs;  // the steps, in order
    for (int row : g_sel) {
      if (row < 0 || row >= R) continue;
      if (!g_grad_each) { rs.push_back(row); continue; }
      for (int r = 0; r < R; r++) if (r == row || lead_of(r) == row) rs.push_back(r);  // The row and its look-alikes, each its own step
    }
    if (g_grad_dark)
      std::stable_sort(rs.begin(), rs.end(), [&](int a, int b) { return luma(v.regions[static_cast<size_t>(a)].orig_lit) < luma(v.regions[static_cast<size_t>(b)].orig_lit); });
    std::vector<recolor_test::Pick> picks;
    const size_t n = rs.size();
    for (size_t k = 0; k < n; k++)
      picks.push_back({rs[k], grad_at(preset, n == 1 ? 0.5f : static_cast<float>(k) / static_cast<float>(n - 1), base), 2, 0});
    recolor_test::setPairs(picks, g_grad_each ? 0 : 1);  // Each region its own step, otherwise a row's look-alikes move with it
  }
  void paste_to(const std::vector<int>& regions) {
    if (!g_clip_set) return;
    std::vector<recolor_test::Pick> picks;
    for (int r : regions) picks.push_back({r, g_clip_lit, 1, g_clip_shadow});
    recolor_test::setPairs(picks);
  }
  int g_esc_used = -1;  // the frame Esc was used for something else (closing the pick's list)
  // Esc with the mouse on (or focus in) the current window clears the selection.
  void deselect_on_escape() {
    if (g_esc_used == ImGui::GetFrameCount()) return;
    if (g_sel.empty() || !ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
    if (ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive()) return;  // Esc there cancels the typing, not the selection
    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) return;
    set_selection({});
    g_sel_anchor = -1;
  }
  // The flat texture of the shown part (edits on), with the selection lit and boxed in red. Shown whole, or
  // zoomed to the selection.
  void draw_texture_view() {
    static int seen = 0, tw = 0, th = 0;
    static ImTextureID tex = 0;
    static float box[4] = {};
    static float zl = 1.0f, zc[2] = {0.5f, 0.5f};  // zoom level (1: the whole texture) and the middle of what's seen (0..1)
    static int map = 0;  // 0 lit, 1 shadow, 2 decal lines
    // Painting state: the tool, its brush, and what's under the mouse (Photoshop-like)
    enum Tool { kPick, kBrush, kEraser, kFill, kDropper, kLine, kRect, kEllipse };
    static int tool = kPick, tool_before_dropper = kBrush;
    static recolor_test::PaintBrush brush;
    static bool shadow_too = true, contiguous = true;
    static int tolerance = 24;
    static bool shape_filled = false, shaping = false;  // Shapes are filled or outline (a line is always outline), and whether one is being dragged
    static float shape_u = 0, shape_v = 0;              // Where the shape was pressed
    static int shape_frame = 0;                         // The last frame it was seen. A gap drops it
    static bool stroking = false, have_last = false;
    static float last_u = 0, last_v = 0, prev_u = -1, prev_v = -1;
    static std::vector<uint8_t> view_px;  // the picture as shown (for the eyedropper)
    static int stroke_part = -1, stroke_map = -1, stroke_tool = -1;
    static int view_w = 0, view_h = 0;
    const recolor_test::View v = recolor_test::view();
    if (map == 2 && !v.decal_lines) map = 0;
    recolor_test::textureViewMap(map);
    recolor_test::textureViewPlain(tool != kPick);  // With a paint tool out, show the picture undimmed
    recolor_test::textureViewOpen(true);
    {
      std::vector<uint8_t> px;
      int w = 0, h = 0;
      static int gen = -1;
      if (gen != device_gen) { gen = device_gen; tex = 0; seen = 0; }  // From an old device, so it's dropped, not released
      if (recolor_test::textureView(seen, px, w, h, box) && upload_rgba) {
        if (!px.empty() && w > 0 && h > 0) { tex = upload_rgba(px.data(), w, h, tex); tw = w; th = h; view_px = px; view_w = w; view_h = h; }
        else { tex = upload_rgba(nullptr, 0, 0, tex); tw = th = 0; }  // Nothing to show, so the old picture goes
      }
    }
    ImGuiIO& io = ImGui::GetIO();
    Frame f = begin_window("Texture##sl_texview", "Texture", ImVec2(io.DisplaySize.x * 0.02f, io.DisplaySize.y * 0.30f), ImVec2(380 * S, 430 * S),
                           ImVec2(220 * S, 240 * S), true);
    if (f.closed) g_texview = false;
    deselect_on_escape();
    if (!f.collapsed) {
      ImDrawList* dl = f.dl;
      const float pad = PAD * S;
      float y = f.o.y + f.head + pad * 0.6f;
      {  // Which part (shared with the editor, so switching it here switches the editor) and which texture
        auto partName = [](const std::string& k) { return part_name(k); };
        const char* maps[3] = {"Lit", "Shadow", "Lines"};
        const float cw = (f.w - 2 * pad - 6 * S) * 0.5f;
        push_widget_style();
        ImGui::SetCursorScreenPos(ImVec2(f.o.x + pad, y));
        ImGui::SetNextItemWidth(cw);
        const std::string cur = v.part >= 0 && v.part < static_cast<int>(v.parts.size()) ? partName(v.parts[static_cast<size_t>(v.part)]) : std::string("-");
        if (ImGui::BeginCombo("##tv_part", cur.c_str())) {
          for (int i = 0; i < static_cast<int>(v.parts.size()); i++) {
            const std::string n = partName(v.parts[static_cast<size_t>(i)]) + (v.part_hidden.size() > static_cast<size_t>(i) && v.part_hidden[static_cast<size_t>(i)] ? "  (move only)" : "");
            if (ImGui::Selectable(n.c_str(), i == v.part) && i != v.part && i < static_cast<int>(v.part_ids.size()))
              recolor_test::showPart(v.part_ids[static_cast<size_t>(i)]);
          }
          ImGui::EndCombo();
        }
        ImGui::SetCursorScreenPos(ImVec2(f.o.x + pad + cw + 6 * S, y));
        ImGui::SetNextItemWidth(cw);
        if (ImGui::BeginCombo("##tv_map", maps[map])) {
          for (int i = 0; i < 3; i++) {
            if (i == 2 && !v.decal_lines) continue;  // This part has no decal lines
            if (ImGui::Selectable(maps[i], i == map)) map = i;
          }
          ImGui::EndCombo();
        }
        pop_widget_style();
        y += ImGui::GetFrameHeight() + pad * 0.5f;
      }
      const bool can_paint = map != 2 && v.tex_w > 0;  // The decal's lines aren't painted here
      if (!can_paint && tool != kPick) tool = kPick;
      const bool shape = tool == kLine || tool == kRect || tool == kEllipse;
      const bool outline = tool == kLine || (shape && !shape_filled);  // Drawn with the brush
      {  // the tools
        const char* names[8] = {"Pick", "Brush", "Eraser", "Fill", "Eyedropper", "Line", "Rectangle", "Ellipse"};
        const char* tips[8] = {"Select a region (Shift adds)", "Paint \xC2\xB7 Shift: line \xC2\xB7 Alt: pick color",
                               "Erase paint", "Fill similar colors \xC2\xB7 Alt: pick color", "Click to take a color for the brush",
                               "Drag a line \xC2\xB7 Shift: 0/45/90\xC2\xB0 \xC2\xB7 Alt: pick color", "Drag a rectangle \xC2\xB7 Shift: square \xC2\xB7 Alt: pick color",
                               "Drag an ellipse \xC2\xB7 Shift: circle \xC2\xB7 Alt: pick color"};
        float bx = f.o.x + pad;
        for (int i = 0; i < 8; i++) {
          if (i > 0 && bx + small_button_w(names[i]) > f.o.x + f.w - pad) { y += CHIP_H * S + pad * 0.3f; bx = f.o.x + pad; }  // Too narrow, so go to the next row
          ImGui::PushID(9100 + i);
          if (small_button(dl, "tool", names[i], ImVec2(bx, y), i == kPick || can_paint, tool == i)) {
            if (i == kDropper && tool != kDropper) tool_before_dropper = tool == kPick ? kBrush : tool;
            tool = i;
          }
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tips[i]);
          ImGui::PopID();
          bx += small_button_w(names[i]) + 4 * S;
        }
        if (v.painted && small_button(dl, "tv_clear", "Clear paint", ImVec2(bx + 8 * S, y), can_paint)) recolor_test::paintClear(map == 1 ? 1 : shadow_too ? 2 : 0);
        if (v.painted) bx += 8 * S + small_button_w("Clear paint") + 4 * S;
        {  // Undo and redo are the editor's own (paint and colors, in the order they were made)
          float rx = f.o.x + f.w - pad - small_button_w("Redo") - 4 * S - small_button_w("Undo");
          if (rx < bx + 8 * S) { y += CHIP_H * S + pad * 0.3f; rx = f.o.x + pad; }  // Too narrow, so they get their own row
          if (small_button(dl, "tv_undo", "Undo", ImVec2(rx, y), v.can_undo)) recolor_test::undo();
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ctrl+Z");
          if (small_button(dl, "tv_redo", "Redo", ImVec2(rx + small_button_w("Undo") + 4 * S, y), v.can_redo)) recolor_test::redo();
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ctrl+Y or Ctrl+Shift+Z");
        }
        y += CHIP_H * S + pad * 0.5f;
      }
      if (tool != kPick && can_paint) {  // the brush's options
        push_widget_style();
        const float cw = f.w - 2 * pad;
        ImGui::SetCursorScreenPos(ImVec2(f.o.x + pad, y));
        ImGui::ColorEdit3("##tv_col", brush.rgb, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_PickerHueWheel);
        ImGui::SameLine();
        const char* blends[4] = {"Normal", "Color (keeps shading)", "Multiply (darker)", "Screen (lighter)"};
        ImGui::SetNextItemWidth(cw * 0.42f);
        if (ImGui::BeginCombo("##tv_blend", blends[brush.blend])) {
          for (int i = 0; i < 4; i++) if (ImGui::Selectable(blends[i], i == brush.blend)) brush.blend = i;
          ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (map == 0) ImGui::Checkbox("Shadow too", &shadow_too);
        ImGui::SetCursorScreenPos(ImVec2(f.o.x + pad, ImGui::GetCursorScreenPos().y));
        ImGui::SetNextItemWidth(cw * 0.48f);
        if (tool == kBrush || tool == kEraser || outline) {
          ImGui::SliderFloat("##tv_size", &brush.size, 1.0f, 256.0f, "Size %.0f", ImGuiSliderFlags_Logarithmic);
          ImGui::SameLine();
          ImGui::SetNextItemWidth(cw * 0.48f);
          ImGui::SliderFloat("##tv_hard", &brush.hardness, 0.0f, 1.0f, "Hardness %.2f");
          ImGui::SetCursorScreenPos(ImVec2(f.o.x + pad, ImGui::GetCursorScreenPos().y));
          ImGui::SetNextItemWidth(cw * 0.48f);
          ImGui::SliderFloat("##tv_opac", &brush.opacity, 0.01f, 1.0f, "Opacity %.2f");
          ImGui::SameLine();
          ImGui::SetNextItemWidth(cw * 0.48f);
          ImGui::SliderFloat("##tv_flow", &brush.flow, 0.01f, 1.0f, "Flow %.2f");
        } else if (tool == kFill) {
          ImGui::SliderInt("##tv_tol", &tolerance, 0, 128, "Tolerance %d");
          ImGui::SameLine();
          ImGui::SetNextItemWidth(cw * 0.48f);
          ImGui::SliderFloat("##tv_fopac", &brush.opacity, 0.01f, 1.0f, "Opacity %.2f");
          ImGui::SetCursorScreenPos(ImVec2(f.o.x + pad, ImGui::GetCursorScreenPos().y));
          ImGui::Checkbox("Contiguous", &contiguous);
          ImGui::SameLine();
        } else if (shape) {  // A filled shape is crisp, so it only has the opacity
          ImGui::SliderFloat("##tv_sopac", &brush.opacity, 0.01f, 1.0f, "Opacity %.2f");
        }
        if (tool == kRect || tool == kEllipse) {
          ImGui::SetCursorScreenPos(ImVec2(f.o.x + pad, ImGui::GetCursorScreenPos().y));
          if (ImGui::RadioButton("Outline", !shape_filled)) shape_filled = false;
          ImGui::SameLine();
          if (ImGui::RadioButton("Filled", shape_filled)) shape_filled = true;
          ImGui::SameLine();
        }
        if (tool == kBrush || tool == kEraser || tool == kFill || shape) {
          if (tool != kFill && tool != kRect && tool != kEllipse) ImGui::SetCursorScreenPos(ImVec2(f.o.x + pad, ImGui::GetCursorScreenPos().y));
          ImGui::Checkbox("Stay in region", &brush.keep_region);
        }
        y = ImGui::GetCursorScreenPos().y + pad * 0.3f;
        pop_widget_style();
      }
      const bool has_box = box[2] > box[0] && box[3] > box[1];
      if (small_button(dl, "tv_whole", "Whole", ImVec2(f.o.x + pad, y), true, zl <= 1.0f)) { zl = 1.0f; zc[0] = zc[1] = 0.5f; }
      const float zx = f.o.x + pad + small_button_w("Whole") + 4 * S;
      if (small_button(dl, "tv_zoom", "Zoom", ImVec2(zx, y), has_box, false)) {  // Zoom onto the selection, to a tenth of the texture at least
        zc[0] = (box[0] + box[2]) * 0.5f;
        zc[1] = (box[1] + box[3]) * 0.5f;
        zl = std::clamp(1.0f / ((std::max)(box[2] - box[0], box[3] - box[1]) * 2.5f), 1.0f, 64.0f);
      }
      {
        char zt[64];
        snprintf(zt, sizeof(zt), "%.0fx", zl);
        const ImVec2 zp(zx + small_button_w("Zoom") + 10 * S, y + (CHIP_H * S - f_small->FontSize) * 0.5f);
        text(dl, f_small, zp, MUTED, zt);
        if (ImGui::IsMouseHoveringRect(zp, ImVec2(zp.x + text_w(f_small, zt), zp.y + f_small->FontSize)))
          ImGui::SetTooltip(tool == kPick ? "Wheel: zoom, right-drag: move, click: pick" : "Wheel: zoom, right-drag: move, [ ]: size");
      }
      y += CHIP_H * S + pad * 0.6f;
      const float sz = 1.0f / zl;  // the part of the texture seen
      zc[0] = std::clamp(zc[0], sz * 0.5f, 1.0f - sz * 0.5f);
      zc[1] = std::clamp(zc[1], sz * 0.5f, 1.0f - sz * 0.5f);
      ImVec2 uv0(zc[0] - sz * 0.5f, zc[1] - sz * 0.5f), uv1(zc[0] + sz * 0.5f, zc[1] + sz * 0.5f);
      const float aw = f.w - 2 * pad, ah = f.o.y + f.h - pad - y;
      if (tex && tw > 0 && th > 0 && aw > 10 && ah > 10) {
        const float iw = tw * (uv1.x - uv0.x), ih = th * (uv1.y - uv0.y), k = (std::min)(aw / iw, ah / ih);
        const ImVec2 a(f.o.x + pad + (aw - iw * k) * 0.5f, y), b(a.x + iw * k, a.y + ih * k);
        dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 255));
        if (point_sampling && zl > 1.5f) dl->AddCallback(point_sampling, nullptr);  // When zoomed in, draw texels sharp so thin lines stay lines
        dl->AddImage(tex, a, b, uv0, uv1);
        if (point_sampling && zl > 1.5f) dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        {  // Mouse on the picture. Wheel zooms around it, right-drag moves, a click picks the region (Shift adds/removes)
          ImGui::SetCursorScreenPos(a);
          ImGui::InvisibleButton("tv_img", ImVec2(b.x - a.x, b.y - a.y), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
          ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);  // The wheel zooms the picture and nothing else
          const bool hov = ImGui::IsItemHovered();
          const ImVec2 mp = io.MousePos;
          const float fu = (mp.x - a.x) / (b.x - a.x), fv = (mp.y - a.y) / (b.y - a.y);  // 0..1 across the picture
          const float u = uv0.x + fu * (uv1.x - uv0.x), vv = uv0.y + fv * (uv1.y - uv0.y);
          if (hov && io.MouseWheel != 0.0f) {
            const float nz = std::clamp(zl * std::pow(1.25f, io.MouseWheel), 1.0f, 64.0f), ns = 1.0f / nz;
            zc[0] = u - (fu - 0.5f) * ns;  // Keeps the point under the mouse under it
            zc[1] = vv - (fv - 0.5f) * ns;
            zl = nz;
          }
          if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)) {
            zc[0] -= io.MouseDelta.x / (b.x - a.x) * (uv1.x - uv0.x);
            zc[1] -= io.MouseDelta.y / (b.y - a.y) * (uv1.y - uv0.y);
          }
          if (tool != kPick && can_paint) {  // painting
            brush.map = map == 1 ? 1 : shadow_too ? 2 : 0;
            brush.eraser = tool == kEraser;
            const bool alt = io.KeyAlt;
            auto dropper = [&]() {  // the color under the mouse, as the picture shows it
              if (view_px.empty() || view_w <= 0 || view_h <= 0) return;
              const int px = std::clamp(static_cast<int>(u * view_w), 0, view_w - 1), py = std::clamp(static_cast<int>(vv * view_h), 0, view_h - 1);
              const uint8_t* c = &view_px[(static_cast<size_t>(py) * view_w + px) * 4];
              for (int k = 0; k < 3; k++) brush.rgb[k] = c[k] / 255.0f;
            };
            if (hov && ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) brush.size = (std::max)(1.0f, brush.size / 1.25f);
            if (hov && ImGui::IsKeyPressed(ImGuiKey_RightBracket)) brush.size = (std::min)(256.0f, brush.size * 1.25f);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
              if (tool == kDropper || alt) {
                dropper();
                if (tool == kDropper) tool = tool_before_dropper;
              } else if (tool == kFill) {
                recolor_test::paintFill(u, vv, brush, tolerance, contiguous);
              } else if (shape) {  // A shape goes from here to where the button goes up
                shaping = true;
                shape_u = u; shape_v = vv;
                shape_frame = ImGui::GetFrameCount();
                stroke_part = v.part; stroke_map = map; stroke_tool = tool;
              } else if (io.KeyShift && have_last) {  // a straight line from the last point
                recolor_test::paintBegin(last_u, last_v, brush);
                recolor_test::paintTo(u, vv);
                recolor_test::paintEnd();
                last_u = u; last_v = vv;
              } else {
                recolor_test::paintBegin(u, vv, brush);
                stroking = true;
                stroke_part = v.part; stroke_map = map; stroke_tool = tool;
                prev_u = u; prev_v = vv;
              }
            }
            if (stroking && (v.part != stroke_part || map != stroke_map || tool != stroke_tool)) {  // Switched mid-stroke, so the stroke ends here
              recolor_test::paintEnd();
              stroking = false;
            }
            if (stroking) {
              if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (u != prev_u || vv != prev_v) { recolor_test::paintTo(u, vv); prev_u = u; prev_v = vv; }
              } else {
                recolor_test::paintEnd();
                stroking = false;
                have_last = true;
                last_u = prev_u; last_v = prev_v;
              }
            }
            if (shaping && (v.part != stroke_part || map != stroke_map || tool != stroke_tool || ImGui::GetFrameCount() - shape_frame > 1)) shaping = false;  // Switched or hidden mid-shape, so it's dropped
            shape_frame = ImGui::GetFrameCount();
            if (shaping && v.tex_w > 0 && v.tex_h > 0) {
              float eu = u, ev = vv;  // The end point. Shift makes a square or circle, or a line at 0/45/90 degrees (in texels)
              if (io.KeyShift) {
                const float dx = (u - shape_u) * v.tex_w, dy = (vv - shape_v) * v.tex_h;
                if (tool == kLine) {
                  const float len = std::sqrt(dx * dx + dy * dy), ang = std::round(std::atan2(dy, dx) / 0.78539816f) * 0.78539816f;
                  eu = shape_u + len * std::cos(ang) / v.tex_w;
                  ev = shape_v + len * std::sin(ang) / v.tex_h;
                } else {
                  const float d = (std::max)(std::fabs(dx), std::fabs(dy));
                  eu = shape_u + std::copysign(d, dx) / v.tex_w;
                  ev = shape_v + std::copysign(d, dy) / v.tex_h;
                }
              }
              const bool filled = !outline;
              if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {  // The preview. The canvas takes it on release
                auto scr = [&](float pu, float pv) { return ImVec2(a.x + (pu - uv0.x) / (uv1.x - uv0.x) * (b.x - a.x), a.y + (pv - uv0.y) / (uv1.y - uv0.y) * (b.y - a.y)); };
                const float per = (b.x - a.x) / ((uv1.x - uv0.x) * v.tex_w);  // screen pixels per texel
                const ImU32 col = IM_COL32(static_cast<int>(brush.rgb[0] * 255), static_cast<int>(brush.rgb[1] * 255), static_cast<int>(brush.rgb[2] * 255),
                                           static_cast<int>(60 + 195 * brush.opacity));
                const float th = (std::max)(1.5f * S, brush.size * per);
                dl->PushClipRect(a, b, true);
                if (filled) {  // the texels it covers, as painted
                  const float tx0 = std::floor((std::min)(shape_u, eu) * v.tex_w), tx1 = std::floor((std::max)(shape_u, eu) * v.tex_w) + 1;
                  const float ty0 = std::floor((std::min)(shape_v, ev) * v.tex_h), ty1 = std::floor((std::max)(shape_v, ev) * v.tex_h) + 1;
                  const ImVec2 q0 = scr(tx0 / v.tex_w, ty0 / v.tex_h), q1 = scr(tx1 / v.tex_w, ty1 / v.tex_h);
                  if (tool == kRect) dl->AddRectFilled(q0, q1, col);
                  else dl->AddEllipseFilled(ImVec2((q0.x + q1.x) * 0.5f, (q0.y + q1.y) * 0.5f), ImVec2((q1.x - q0.x) * 0.5f, (q1.y - q0.y) * 0.5f), col, 0.0f, 96);
                } else {
                  const ImVec2 p0 = scr(shape_u, shape_v), p1 = scr(eu, ev);
                  const ImVec2 lo((std::min)(p0.x, p1.x), (std::min)(p0.y, p1.y)), hi((std::max)(p0.x, p1.x), (std::max)(p0.y, p1.y));
                  if (tool == kLine) dl->AddLine(p0, p1, col, th);
                  else if (tool == kRect) dl->AddRect(lo, hi, col, 0.0f, 0, th);
                  else dl->AddEllipse(ImVec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f), ImVec2((hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f), col, 0.0f, 96, th);
                }
                dl->PopClipRect();
              } else {
                recolor_test::paintShape(shape_u, shape_v, eu, ev, brush, tool - kLine, filled);
                shaping = false;
              }
            }
            if (hov && (tool == kBrush || tool == kEraser || outline) && v.tex_w > 0) {  // the brush's outline
              const float per = (b.x - a.x) / ((uv1.x - uv0.x) * v.tex_w);  // screen pixels per texel
              dl->PushClipRect(a, b, true);
              dl->AddCircle(mp, (std::max)(2.0f, brush.size * 0.5f * per), IM_COL32(255, 255, 255, 200), 0, 1.5f * S);
              dl->AddCircle(mp, (std::max)(2.0f, brush.size * 0.5f * per) + 1.5f * S, IM_COL32(0, 0, 0, 160), 0, 1.0f * S);
              dl->PopClipRect();
            }
          }
          const int at = hov && map != 2 && tool == kPick ? lead_of(recolor_test::textureRegionAt(u, vv)) : -1;  // Its row
          if (at >= 0 && at < static_cast<int>(v.regions.size())) {
            const auto& rg = v.regions[static_cast<size_t>(at)];
            ImGui::SetTooltip("%s%s", region_label(v, at).c_str(), rg.edited ? "  (edited)" : "");
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
              std::vector<int> s;
              if (io.KeyShift) {
                s = g_sel;
                auto it = std::find(s.begin(), s.end(), at);
                if (it != s.end()) s.erase(it); else s.push_back(at);
              } else {
                s = {at};
              }
              set_selection(s);
              g_sel_anchor = at;
            }
          }
        }
        if (has_box) {  // Where the selection is (red), a little wider so a thin line's box shows
          auto at = [&](float u, float v) { return ImVec2(a.x + (u - uv0.x) / (uv1.x - uv0.x) * (b.x - a.x), a.y + (v - uv0.y) / (uv1.y - uv0.y) * (b.y - a.y)); };
          ImVec2 p0 = at(box[0], box[1]), p1 = at(box[2], box[3]);
          p0.x -= 3 * S; p0.y -= 3 * S; p1.x += 3 * S; p1.y += 3 * S;
          dl->PushClipRect(a, b, true);
          dl->AddRect(p0, p1, RED, 0.0f, 0, 2 * S);
          dl->PopClipRect();
        }
      } else {
        text(dl, f_small, ImVec2(f.o.x + pad, y), DIM, "Loading texture...");
      }
    }
    end_window();
  }
  // ---- Import's own file window: folders and drives to go through, image files to tick ----
  struct TexEntry { std::wstring name; bool folder = false; std::string label; };
  struct TexBrowser {
    bool open = false, want_open = false;
    std::filesystem::path dir;  // empty: the drives
    std::vector<TexEntry> entries;
    std::set<std::wstring> ticked;  // full paths
    std::string error;
  } g_tex_browser;
  std::string tb_narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
  }
  void tex_browser_list(const std::filesystem::path& dir) {
    TexBrowser& b = g_tex_browser;
    b.dir = dir;
    b.entries.clear();
    b.error.clear();
    if (dir.empty()) {  // The drives
      const DWORD drives = GetLogicalDrives();
      for (int i = 0; i < 26; i++)
        if (drives & (1u << i)) { const std::wstring d = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\"; b.entries.push_back({d, true, tb_narrow(d)}); }
      return;
    }
    std::error_code ec;
    std::vector<TexEntry> folders, files;
    for (std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
      const std::wstring name = it->path().filename().wstring();
      if (name.empty() || name[0] == L'.') continue;  // (the export's own .exported copies)
      std::error_code e2;
      if (it->is_directory(e2)) { folders.push_back({name, true}); continue; }
      std::wstring ext = it->path().extension().wstring();
      for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
      std::wstring low = name;
      for (auto& c : low) c = static_cast<wchar_t>(towlower(c));
      const bool mat = ext == L".txt" && ((low.size() > 20 && low.compare(low.size() - 20, 20, L"_material_colors.txt") == 0) || low == L"effects.txt");  // (an export's material and effect colors)
      if (ext == L".png" || ext == L".bmp" || ext == L".tif" || ext == L".tiff" || mat) files.push_back({name, false});
    }
    if (ec) b.error = "Couldn't read this folder.";
    std::sort(folders.begin(), folders.end(), [](const TexEntry& x, const TexEntry& y) {  // Dated exports newest first, the rest A to Z
      const bool dx = !x.name.empty() && iswdigit(x.name[0]), dy = !y.name.empty() && iswdigit(y.name[0]);
      if (dx && dy) return x.name > y.name;
      if (dx != dy) return dx;
      return _wcsicmp(x.name.c_str(), y.name.c_str()) < 0;
    });
    std::sort(files.begin(), files.end(), [](const TexEntry& x, const TexEntry& y) { return _wcsicmp(x.name.c_str(), y.name.c_str()) < 0; });
    b.entries = std::move(folders);
    b.entries.insert(b.entries.end(), files.begin(), files.end());
    for (TexEntry& e : b.entries) e.label = tb_narrow(e.name);
  }
  void tex_browser_open() {
    TexBrowser& b = g_tex_browser;
    b.ticked.clear();
    std::error_code ec;
    std::filesystem::path d = recolor_test::textureDir();
    if (!std::filesystem::is_directory(d, ec)) d = recolor_test::exportsDir();
    if (!std::filesystem::is_directory(d, ec)) d.clear();
    tex_browser_list(d);
    b.want_open = true;
  }
  void tex_browser_draw() {
    TexBrowser& b = g_tex_browser;
    if (b.want_open) { ImGui::OpenPopup("tex_browser"); b.want_open = false; b.open = true; }
    if (!b.open) return;
    push_widget_style();
    const ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSize(ImVec2((std::min)(vp.x * 0.6f, 900.0f * S), (std::min)(vp.y * 0.7f, 640.0f * S)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("tex_browser", &b.open, ImGuiWindowFlags_NoTitleBar)) {
      ImGui::TextUnformatted("Import textures");
      ImGui::TextDisabled("%s", b.dir.empty() ? "Drives" : tb_narrow(b.dir.wstring()).c_str());
      if (ImGui::SmallButton("Up")) tex_browser_list(b.dir.empty() || b.dir == b.dir.root_path() ? std::filesystem::path() : b.dir.parent_path());
      ImGui::SameLine();
      if (ImGui::SmallButton("Exports")) tex_browser_list(recolor_test::exportsDir());
      ImGui::SameLine();
      if (ImGui::SmallButton("Drives")) tex_browser_list({});
      int files = 0;
      for (const TexEntry& e : b.entries) files += e.folder ? 0 : 1;
      if (files) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Tick all")) for (const TexEntry& e : b.entries) if (!e.folder) b.ticked.insert((b.dir / e.name).wstring());
      }
      if (!b.ticked.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Untick all")) b.ticked.clear();
      }
      if (!b.error.empty()) ImGui::TextUnformatted(b.error.c_str());
      ImGui::Separator();
      const float foot = ImGui::GetFrameHeightWithSpacing() * 2.0f;
      std::filesystem::path go;
      if (ImGui::BeginChild("tex_list", ImVec2(0, -foot), false)) {
        const bool second_click = ImGui::GetIO().MouseClickedLastCount[0] >= 2;  // (a double click's second half lands on the new list: ignored)
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(b.entries.size()), ImGui::GetFrameHeightWithSpacing());  // (every row one frame tall, folders and files alike)
        while (clip.Step())
          for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const TexEntry& e = b.entries[static_cast<size_t>(i)];
            ImGui::PushID(i);
            if (e.folder) {
              if (ImGui::Selectable(("[" + e.label + "]").c_str(), false, 0, ImVec2(0, ImGui::GetFrameHeight())) && !second_click) go = b.dir.empty() ? std::filesystem::path(e.name) : b.dir / e.name;
            } else {
              const std::wstring full = (b.dir / e.name).wstring();
              bool on = b.ticked.count(full) > 0;
              if (ImGui::Checkbox(e.label.c_str(), &on) && !second_click) { if (on) b.ticked.insert(full); else b.ticked.erase(full); }
            }
            ImGui::PopID();
          }
        if (b.entries.empty() && b.error.empty()) ImGui::TextDisabled("No folders or images here.");
      }
      ImGui::EndChild();
      if (!go.empty()) tex_browser_list(go);
      ImGui::Separator();
      const std::string imp = "Import " + std::to_string(b.ticked.size()) + (b.ticked.size() == 1 ? " texture" : " textures");
      ImGui::BeginDisabled(b.ticked.empty());
      if (ImGui::Button(imp.c_str())) {
        recolor_test::importTextureFiles(std::vector<std::wstring>(b.ticked.begin(), b.ticked.end()));
        b.open = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) { b.open = false; ImGui::CloseCurrentPopup(); }
      ImGui::EndPopup();
    } else {
      b.open = false;
    }
    pop_widget_style();
  }
  // ---- Script & boxes. Each fighter's state, sprite, boxes (editable) and the state's script ----
  void draw_box_editor(const MenuView& mv) {
    use(E_MENU);
    const box_editor::View v = box_editor::view();
    ImGuiIO& io = ImGui::GetIO();
    const float pad = PAD * S;
    const ImVec2 size(540 * S, 780 * S);
    Frame f = begin_window("Script & boxes##sl_boxes", "Script & boxes", ImVec2(24 * S, io.DisplaySize.y * 0.08f), size, ImVec2(380 * S, 320 * S), true);
    if (f.closed) box_editor::toggle();
    static int side = 0, picked = -1;  // the side shown, and the box picked (in the table or on the fighter)
    static std::string picked_of;
    if (!f.collapsed) {
      ImGui::SetCursorScreenPos(ImVec2(f.o.x + pad, f.o.y + f.head + 6 * S));
      push_widget_style();
      ImGui::BeginChild("be_body", ImVec2(f.w - 2 * pad, f.h - f.head - 12 * S), false);
      // Fixed places: every item on a line at a fixed x, numbers right-aligned in a fixed slot, so nothing moves as values
      // change (a line starting with a slot begins with an empty item)
      auto at = [&](float x) { ImGui::SameLine((std::max)(1.0f, x * S)); };
      auto right = [&](float x_end, const std::string& t, bool dim) {
        ImGui::SameLine((std::max)(1.0f, x_end * S - ImGui::CalcTextSize(t.c_str()).x));
        if (dim) ImGui::TextDisabled("%s", t.c_str());
        else ImGui::TextUnformatted(t.c_str());
      };
      auto line_start = [&]() { ImGui::Dummy(ImVec2(1, ImGui::GetTextLineHeight())); };
      auto wid = [&](const char* t) { return ImGui::CalcTextSize(t).x / S; };  // (in the units at() takes)
      // Fields on one line: each a label, then a value slot as wide as the widest value given; returns where each label
      // starts and where its value ends. (Measured with the font in use, so nothing overlaps.)
      struct Slot { float label = 0, value_from = 0, value_end = 0; };
      auto slots = [&](std::initializer_list<std::pair<const char*, const char*>> fields, float x0) {
        std::vector<Slot> out;
        float x = x0;
        for (const auto& [lab, widest] : fields) {
          Slot sl;
          sl.label = x;
          sl.value_from = x + (lab[0] ? wid(lab) + 6 : 0);
          sl.value_end = sl.value_from + wid(widest);
          out.push_back(sl);
          x = sl.value_end + 14;
        }
        return out;
      };
      auto after_item = [&](float gap) { return (ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x) / S + gap; };  // (x just past the last item)
      auto field = [&](const Slot& sl, const char* lab, const std::string& val, bool num) {  // its label, its value
        if (lab[0]) { at(sl.label); ImGui::TextDisabled("%s", lab); }
        if (num) right(sl.value_end, val, false);
        else { at(sl.value_from); ImGui::TextUnformatted(val.c_str()); }
      };
      if (ImGui::RadioButton("P1", side == 0)) side = 0;
      ImGui::SameLine();
      if (ImGui::RadioButton("P2", side == 1)) side = 1;
      const box_editor::SideView& s = v.side[side];
      if (s.present && !s.objects.empty()) {  // the fighter, or one of its objects (a projectile...)
        at(110);
        auto label = [](const box_editor::ObjView& o) { return (o.state.empty() ? std::string("object") : o.state) + " (" + o.sprite + ")"; };
        const std::string now = s.object >= 0 ? label(s.objects[static_cast<size_t>(s.object)]) : std::string("Fighter");
        ImGui::SetNextItemWidth(220 * S);
        if (ImGui::BeginCombo("##be_obj", now.c_str())) {
          if (ImGui::Selectable("Fighter", s.object < 0)) box_editor::showObject(side, nullptr);
          for (size_t k = 0; k < s.objects.size(); k++) {
            ImGui::PushID(static_cast<int>(k));
            if (ImGui::Selectable(label(s.objects[k]).c_str(), static_cast<int>(k) == s.object)) box_editor::showObject(side, s.objects[k].id);
            ImGui::PopID();
          }
          ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show one of this fighter's objects (a projectile...) instead.\nPause first: most are only out for a moment.");
      } else {
        at(110);
        ImGui::Dummy(ImVec2(220 * S, ImGui::GetFrameHeight()));  // (its place, kept when there are no objects)
      }
      if (!s.present) {
        ImGui::TextDisabled("No fighter on this side yet.");
      } else {
        at(340);
        ImGui::TextUnformatted(s.chr.c_str());
        {  // the move playing, on a row of its own (names run to 32 letters)
          const auto r = slots({{"Move", ""}}, 1);
          line_start();
          field(r[0], "Move", s.state.empty() ? std::string("?") : s.state, false);
        }
        bool sprite_edited = false;
        for (const box_editor::BoxView& b : s.boxes) sprite_edited |= b.edited;
        {  // pause and step first (one fixed width each), then the sprite and its frame in fixed slots
          auto bound = [](int a) {  // what it's bound to in Controls
            const std::wstring k = binds::key[a] ? binds::keyName(binds::key[a]) : binds::pad[a] ? binds::padName(binds::pad[a]) : std::wstring();
            return k.empty() ? std::string("(bind it in Controls: ") + tb_narrow(binds::LABELS[a]) + ")" : tb_narrow(k);
          };
          const float bw = (std::max)({ImGui::CalcTextSize("Pause").x, ImGui::CalcTextSize("Play").x, ImGui::CalcTextSize("Step").x}) + 2 * ImGui::GetStyle().FramePadding.x + 4 * S;
          const ImVec2 bsz(bw, 0);
          ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0));
          if (ImGui::Button(mv.paused ? "Play##be_pp" : "Pause##be_pp", bsz)) ModMenu::instance().click(MenuClick{MenuClick::Type::TlPlay});
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", bound(binds::BOX_PAUSE).c_str());
          at(after_item(6));  // (Pause is one fixed width, so Step's place is fixed)
          ImGui::BeginDisabled(!mv.paused);
          if (ImGui::Button("Step##be_st", bsz)) ModMenu::instance().click(MenuClick{MenuClick::Type::TlStep});
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", bound(binds::BOX_STEP).c_str());
          ImGui::EndDisabled();
          ImGui::PopStyleVar();
          const auto r = slots({{"Sprite", "sol000_00_ex_00_abcdefgh *"}, {"frame", "9999"}, {"of", "9999"}}, after_item(14));
          field(r[0], "Sprite", s.sprite + (sprite_edited ? " *" : ""), false);
          field(r[1], "frame", std::to_string(s.sprite_frame), true);
          field(r[2], "of", std::to_string(s.sprite_frames), true);
        }
        {  // each in its own fixed slot, blank when off; the row is always there (the game keeps these in game units,
           // 1000 to a box unit)
          char t[96];
          const auto r1 = slots({{"", "strike invulnerable"}, {"", "throw invulnerable"}}, 1);
          line_start();
          if (s.strike_invuln) { at(r1[0].label); ImGui::TextDisabled("strike invulnerable"); }
          if (s.throw_invuln) { at(r1[1].label); ImGui::TextDisabled("throw invulnerable"); }
          const auto r2 = slots({{"throw range", "9999.9"}, {"collision", "9999.9 x 9999.9"}}, 1);
          line_start();
          if (s.throw_range >= 0) { snprintf(t, sizeof(t), "%g", s.throw_range / 1000.0); field(r2[0], "throw range", t, true); }
          if (s.push_on) {
            snprintf(t, sizeof(t), "%g x %g", std::abs(s.push[2] - s.push[0]) / 1000.0, std::abs(s.push[1] - s.push[3]) / 1000.0);
            field(r2[1], "collision", t, true);
          }
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Boxes");
        {
          const auto r = slots({{"", "999"}, {"", "(?)"}}, after_item(6));
          right(r[0].value_end, std::to_string(s.boxes.size()), true);
          at(r[1].label);
          ImGui::TextDisabled("(?)");
        }
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("X, Y is a corner of the box; W, H reach from it, in the game's box units.\n+Y is down, the feet are at 0.\nW grows the box forward and H grows it up (X and Y move so the back and bottom stay).\nDrag a value, or Ctrl+click to type. Shift drags faster, Alt slower.\nWith Hitboxes on: hover a hurtbox or hitbox on the fighter to light it, click to pick it, drag an edge or corner to resize, the middle to move.\nHover a row to light its box; click its name to keep it lit.");
        if (picked_of != s.sprite + "|" + std::to_string(side)) { picked = -1; picked_of = s.sprite + "|" + std::to_string(side); }
        int hovered = -1;
        // Where an edit to a box goes: 0 this frame only, 1 also the rest of the move, 2 the whole move (the move open in
        // the Move panel, else the one playing; its frames are its sprite lines)
        static int scope = 0;
        {  // (back to this sprite only when another move is opened: a choice never carries over unseen)
          static std::string scope_move;
          const std::string m = std::to_string(side) + "|" + std::to_string(s.object) + "|" + s.selected;  // (the move opened, not the one playing: that flips with every action)
          if (m != scope_move) { scope = 0; scope_move = m; }
        }
        const std::vector<box_editor::InstrView>& scope_sc = s.selected.empty() ? s.script : s.sel_script;
        std::vector<std::string> scope_to;
        bool scope_has = false;  // the move shows this sprite at all
        {
          bool after = false;
          for (const box_editor::InstrView& in : scope_sc) {
            if (in.op != 2) continue;  // sprite: s32'name', frames
            const size_t a = in.args.find('\''), b = a == std::string::npos ? a : in.args.find('\'', a + 1);
            if (b == std::string::npos) continue;
            const std::string name = in.args.substr(a + 1, b - a - 1);
            if (name == s.sprite) { after = scope_has = true; continue; }
            if ((scope == 2 || (scope == 1 && after)) && std::find(scope_to.begin(), scope_to.end(), name) == scope_to.end()) scope_to.push_back(name);
          }
        }
        auto spread = [&](int index, int fields) {  // the edit just made, to the other frames chosen (in its undo step)
          if (scope > 0 && !scope_to.empty() && fields) box_editor::copyBox(side, s.sprite, index, fields, scope_to, true);
        };
        static ImGuiID drag_id = 0;  // the value being dragged: one undo step for the whole drag
        if (!ImGui::IsAnyItemActive()) drag_id = 0;
        const float cell_y = 2 * ImGui::GetStyle().CellPadding.y;  // the header and 8 rows, always (more scroll)
        const float box_rows_h = ImGui::GetTextLineHeight() + cell_y + 8 * (ImGui::GetFrameHeight() + cell_y) + 2 * S;  // (+2: rounding never adds a scrollbar)
        if (ImGui::BeginTable("be_boxes", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY, ImVec2(0, box_rows_h))) {
          ImGui::TableSetupScrollFreeze(0, 1);
          {  // fixed, measured widths: a scrollbar coming or going never moves them
            const float name_w = ImGui::CalcTextSize("Combo hit 9 (invulnerable)").x + 8 * S, num_w = ImGui::CalcTextSize("-99999").x + 12 * S;
            ImGui::TableSetupColumn("Box", ImGuiTableColumnFlags_WidthFixed, name_w);
            for (const char* c : {"X", "Y", "W", "H"}) ImGui::TableSetupColumn(c, ImGuiTableColumnFlags_WidthFixed, num_w);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 22 * S);
          }
          ImGui::TableHeadersRow();
          ImGui::PushID(s.sprite.c_str());  // (another sprite, other IDs: a drag can't carry over into it)
          int of_kind[19] = {};
          for (int i = 0; i < static_cast<int>(s.boxes.size()); i++) {
            const box_editor::BoxView& b = s.boxes[static_cast<size_t>(i)];
            const int nth = b.kind >= 0 && b.kind < 19 ? ++of_kind[b.kind] : 0;
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const ImVec4 col = b.kind == 0 ? ImVec4(0.21f, 0.84f, 0.42f, 1) : b.kind == 1 || b.kind == 18 ? ImVec4(0.95f, 0.30f, 0.30f, 1) : b.kind == 5 ? ImVec4(1.0f, 0.9f, 0.3f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1);
            const std::string name = std::string(b.kind == 0 ? "Hurt" : b.kind == 1 ? "Hit" : b.kind == 5 ? "Push" : b.kind == 18 ? "Combo hit" : box_editor::kindName(b.kind)) + " " + std::to_string(nth) +
                                     (b.kind == 0 && s.strike_invuln ? " (invulnerable)" : "");
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            if (ImGui::Selectable(name.c_str(), picked == i)) picked = picked == i ? -1 : i;
            ImGui::PopStyleColor();
            const bool can = box_editor::editable(b.kind);
            const float now[4] = {b.x, b.y, b.w, b.h}, was[4] = {b.ox, b.oy, b.ow, b.oh};
            static const char* const ids[4] = {"##x", "##y", "##w", "##h"};
            for (int c = 0; c < 4; c++) {
              ImGui::TableNextColumn();
              if (!can) { ImGui::TextDisabled("%g", now[c]); continue; }
              const bool changed_here = b.edited && now[c] != was[c];
              if (changed_here) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.55f, 0.32f, 0.08f, 1));
              float val = now[c];
              ImGui::SetNextItemWidth(-FLT_MIN);
              if (ImGui::DragFloat(ids[c], &val, 1.0f, 0.0f, 0.0f, "%g", ImGuiSliderFlags_NoRoundToFormat) && std::isfinite(val)) {
                box_editor::BoxView nb = b;
                // W grows the front and H the top: the back and bottom edges stay, X and Y move with them (on screen
                // the box reaches toward -x and -y from X, Y, when W, H and the fighter's scale have the same sign)
                if (c == 0) nb.x = val;
                else if (c == 1) nb.y = val;
                else if (c == 2) { if (b.w == 0 ? s.sx > 0 : b.w * s.sx > 0) nb.x -= val - b.w; nb.w = val; }  // (from 0: as a box of that facing would)
                else { if (b.h == 0 ? s.sy > 0 : b.h * s.sy > 0) nb.y -= val - b.h; nb.h = val; }
                const ImGuiID id = ImGui::GetItemID();
                box_editor::editBox(side, s.sprite, i, nb, id != drag_id);
                drag_id = id;
              }
              if (ImGui::IsItemDeactivatedAfterEdit()) spread(i, c == 0 ? 1 : c == 1 ? 2 : c == 2 ? 4 | 1 : 8 | 2);
              if (changed_here) {
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) ImGui::SetTooltip("was %g", was[c]);
              }
            }
            ImGui::TableNextColumn();
            if (b.edited) {
              if (ImGui::SmallButton("x")) box_editor::resetBox(side, s.sprite, i);
              if (ImGui::IsItemHovered()) ImGui::SetTooltip("Put this box back as the game has it");
            }
            if (ImGui::TableGetHoveredRow() == ImGui::TableGetRowIndex()) hovered = i;
            ImGui::PopID();
          }
          ImGui::PopID();
          ImGui::EndTable();
        }
        {  // On the fighter: hover a box to light it, click to pick it, drag an edge or corner to resize, the middle to move
          struct Drag { bool on = false, first = false; int index = -1, edges = 0, side = 0; ImVec2 from; box_editor::BoxView start; std::string sprite; };
          static Drag drag;
          if (drag.on && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {  // let go: the change goes to the other frames chosen
            drag.on = false;
            if (drag.side == side && drag.sprite == s.sprite && drag.index < static_cast<int>(s.boxes.size())) {
              const int e = drag.edges;  // what the drag moves: the whole box, or its sides
              spread(drag.index, (e & 16 ? 1 | 2 : 0) | (e & 1 ? 1 | 4 : 0) | (e & 2 ? 4 : 0) | (e & 4 ? 2 | 8 : 0) | (e & 8 ? 8 : 0));
            }
          }
          box_editor::ScreenMap m;
          if (box_editor::screenMap(side, m)) {
            const ImVec2 D = io.DisplaySize, mp = io.MousePos;
            auto at = [&](float x, float y) { return ImVec2((m.ox + m.ax * x + m.bx * y) * D.x, (m.oy + m.ay * x + m.by * y) * D.y); };
            auto near_seg = [&](ImVec2 a, ImVec2 b, float tol) {
              const float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
              const float t = l2 > 0 ? (std::max)(0.0f, (std::min)(1.0f, ((mp.x - a.x) * dx + (mp.y - a.y) * dy) / l2)) : 0.0f;
              const float ex = a.x + t * dx - mp.x, ey = a.y + t * dy - mp.y;
              return ex * ex + ey * ey < tol * tol;
            };
            // edges: 1 the X side, 2 the X+W side, 4 the Y side, 8 the Y+H side, 16 the whole box
            auto cursor = [&](const box_editor::BoxView& b, int edges) {
              if (edges & 16) { ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll); return; }
              const bool xs = (edges & 3) != 0, ys = (edges & 12) != 0;
              if (xs && ys) {
                const ImVec2 c = at((edges & 1) ? b.x : b.x + b.w, (edges & 4) ? b.y : b.y + b.h), mid = at(b.x + b.w / 2, b.y + b.h / 2);
                ImGui::SetMouseCursor((c.x - mid.x) * (c.y - mid.y) > 0 ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
              } else {
                const ImVec2 d = xs ? ImVec2(m.ax * D.x, m.ay * D.y) : ImVec2(m.bx * D.x, m.by * D.y);  // the way that edge moves on screen
                ImGui::SetMouseCursor(std::fabs(d.x) >= std::fabs(d.y) ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
              }
            };
            const int n = static_cast<int>(s.boxes.size());
            if (drag.on && (drag.side != side || drag.sprite != s.sprite || drag.index >= n)) drag.on = false;  // (the sprite changed under it)
            if (drag.on) {
              const ImVec2 d(mp.x - drag.from.x, mp.y - drag.from.y);
              const float ax = m.ax * D.x, ay = m.ay * D.y, bx = m.bx * D.x, by = m.by * D.y, det = ax * by - bx * ay;
              if (std::fabs(det) > 1e-9f) {
                const float du = std::round((d.x * by - bx * d.y) / det), dv = std::round((ax * d.y - d.x * ay) / det);  // the mouse's move, in box units
                box_editor::BoxView nb = drag.start;
                if (drag.edges & 16) { nb.x += du; nb.y += dv; }
                if (drag.edges & 1) { nb.x += du; nb.w -= du; }
                if (drag.edges & 2) nb.w += du;
                if (drag.edges & 4) { nb.y += dv; nb.h -= dv; }
                if (drag.edges & 8) nb.h += dv;
                const box_editor::BoxView& cur = s.boxes[static_cast<size_t>(drag.index)];
                if (nb.x != cur.x || nb.y != cur.y || nb.w != cur.w || nb.h != cur.h) { box_editor::editBox(side, s.sprite, drag.index, nb, drag.first); drag.first = false; }
              }
              cursor(drag.start, drag.edges);
              hovered = drag.index;
            } else if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) && !ImGui::IsAnyItemActive()) {
              const float tol = 7 * S;
              int hit = -1, hit_edges = 0;
              float hit_area = FLT_MAX;
              for (int i = 0; i < n; i++) {
                const box_editor::BoxView& b = s.boxes[static_cast<size_t>(i)];
                // (on the fighter: the boxes the overlay draws - hurt, hit, combo hit - and the one picked in the table; the
                // other kinds aren't drawn and many are tiny, so they'd snatch drags meant for a hurtbox)
                if (!box_editor::editable(b.kind) || !(b.kind == 0 || b.kind == 1 || b.kind == 18 || i == picked)) continue;
                const ImVec2 c[4] = {at(b.x, b.y), at(b.x + b.w, b.y), at(b.x + b.w, b.y + b.h), at(b.x, b.y + b.h)};
                float x0 = c[0].x, x1 = c[0].x, y0 = c[0].y, y1 = c[0].y;
                for (const ImVec2& q : c) { x0 = (std::min)(x0, q.x); x1 = (std::max)(x1, q.x); y0 = (std::min)(y0, q.y); y1 = (std::max)(y1, q.y); }
                if (mp.x < x0 - tol || mp.x > x1 + tol || mp.y < y0 - tol || mp.y > y1 + tol) continue;
                int edges = (near_seg(c[0], c[3], tol) ? 1 : 0) | (near_seg(c[1], c[2], tol) ? 2 : 0) | (near_seg(c[0], c[1], tol) ? 4 : 0) | (near_seg(c[3], c[2], tol) ? 8 : 0);
                if ((edges & 3) == 3) edges &= ~2;  // (a box too thin to tell its sides apart)
                if ((edges & 12) == 12) edges &= ~8;
                const bool inside = mp.x >= x0 && mp.x <= x1 && mp.y >= y0 && mp.y <= y1;
                if (!edges && !inside) continue;
                const float area = i == picked ? -1.0f : (x1 - x0) * (y1 - y0);  // the picked box first, then the smallest
                const bool point = b.w == 0 && b.h == 0;  // (a point box: dragging it moves it, there's no side to pull)
                if (area < hit_area) { hit = i; hit_edges = edges && !point ? edges : 16; hit_area = area; }
              }
              if (hit >= 0) {
                hovered = hit;
                cursor(s.boxes[static_cast<size_t>(hit)], hit_edges);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                  picked = hit;
                  drag.on = true;
                  drag.first = true;
                  drag.index = hit;
                  drag.edges = hit_edges;
                  drag.side = side;
                  drag.from = mp;
                  drag.start = s.boxes[static_cast<size_t>(hit)];
                  drag.sprite = s.sprite;
                }
              }
            }
            const int shown = hovered >= 0 ? hovered : picked >= 0 && picked < n ? picked : -1;
            if (shown >= 0 && box_editor::editable(s.boxes[static_cast<size_t>(shown)].kind)) {  // handles: the corners and the sides' middles
              const box_editor::BoxView& b = s.boxes[static_cast<size_t>(shown)];
              ImDrawList* bg = ImGui::GetBackgroundDrawList();
              const float r = 4 * S;
              for (float fx : {0.0f, 0.5f, 1.0f})
                for (float fy : {0.0f, 0.5f, 1.0f}) {
                  if (fx == 0.5f && fy == 0.5f) continue;
                  const ImVec2 p = at(b.x + b.w * fx, b.y + b.h * fy);
                  bg->AddRectFilled(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), IM_COL32(255, 255, 255, 255));
                  bg->AddRect(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), IM_COL32(0, 0, 0, 255));
                }
            }
          }
        }
        const int lit = hovered >= 0 ? hovered : picked < static_cast<int>(s.boxes.size()) ? picked : -1;
        box_editor::setHighlight(lit >= 0 ? side : -1, lit);
        if (lit >= 0) {  // where the box reaches, from the fighter's feet: + is forward and up
          const box_editor::BoxView& b = s.boxes[static_cast<size_t>(lit)];
          const float x0 = -b.x * s.sx, x1 = -(b.x + b.w) * s.sx, y0 = -b.y * s.sy, y1 = -(b.y + b.h) * s.sy;
          char t[32];
          const auto r = slots({{"front", "-9999.9"}, {"back", "-9999.9"}, {"top", "-9999.9"}, {"bottom", "-9999.9"}}, 1);
          ImGui::BeginGroup();
          line_start();
          snprintf(t, sizeof(t), "%g", (std::max)(x0, x1)); field(r[0], "front", t, true);
          snprintf(t, sizeof(t), "%g", (std::min)(x0, x1)); field(r[1], "back", t, true);
          snprintf(t, sizeof(t), "%g", (std::max)(y0, y1)); field(r[2], "top", t, true);
          snprintf(t, sizeof(t), "%g", (std::min)(y0, y1)); field(r[3], "bottom", t, true);
          ImGui::EndGroup();
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("Where the box reaches, from the fighter's feet: + is forward and up.");
        } else {
          ImGui::TextDisabled("Hover a box to see where it reaches%s.", ModMenu::instance().hitboxEnabled() ? " and light it on the fighter" : " (turn on Hitboxes to see it on the fighter)");
        }
        {  // which frames an edit goes to
          const std::string& move = s.selected.empty() ? s.state : s.selected;
          ImGui::TextUnformatted("Edits apply to:");
          ImGui::SameLine();
          ImGui::RadioButton("this sprite", &scope, 0);
          ImGui::SameLine();
          ImGui::RadioButton("the rest of the move", &scope, 1);
          ImGui::SameLine();
          ImGui::RadioButton("the whole move", &scope, 2);
          if (scope > 0)
            ImGui::TextDisabled("%s", !v.db ? "The move's frames need the instruction database (StriveLabs_scripts/db)."
                                      : !scope_has ? (move + " doesn't use " + s.sprite + ". Open a move that does, or close the Move panel to use the one playing.").c_str()
                                      : scope_to.empty() ? ("No other sprites in " + move + ".").c_str()
                                                       : ("Also " + std::to_string(scope_to.size()) + (scope_to.size() == 1 ? " other sprite" : " other sprites") + " of " + move +
                                                          ", when you let go of a drag or finish typing.").c_str());
          if (scope > 0) {  // or now, with the picked box as it is (the row is there whenever a range is chosen)
            const bool ok = !scope_to.empty() && picked >= 0 && picked < static_cast<int>(s.boxes.size()) && box_editor::editable(s.boxes[static_cast<size_t>(picked)].kind);
            ImGui::BeginDisabled(!ok);
            if (ImGui::SmallButton("Apply to those sprites now")) {
              const box_editor::BoxView& pb = s.boxes[static_cast<size_t>(picked)];
              const int fields = pb.edited ? (pb.x != pb.ox ? 1 : 0) | (pb.y != pb.oy ? 2 : 0) | (pb.w != pb.ow ? 4 : 0) | (pb.h != pb.oh ? 8 : 0) : 15;
              box_editor::copyBox(side, s.sprite, picked, fields ? fields : 15, scope_to);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
              ImGui::SetTooltip("%s", ok ? "Copies the picked box's changed values (all four if none changed) to those sprites now"
                                    : scope_to.empty() ? "No other sprites to apply to" : "Click a box first (its name, or on the fighter)");
          }
        }
        if (sprite_edited || !v.edits.empty())
          ImGui::TextWrapped("Box edits belong to the sprite: every move that uses it changes, on both sides in a mirror match.");
        {  // Save / load everything edited (boxes, script lines, versions), by name
          static char save_name[64] = "";
          if (ImGui::SmallButton("Save edits...")) ImGui::OpenPopup("be_save");
          ImGui::SameLine();
          if (ImGui::SmallButton("Load edits...")) ImGui::OpenPopup("be_load");
          if (ImGui::BeginPopup("be_save")) {
            ImGui::TextUnformatted("Save every edit (boxes, script lines, versions) as:");
            ImGui::SetNextItemWidth(220 * S);
            const bool enter = ImGui::InputTextWithHint("##be_save_name", "name", save_name, sizeof(save_name), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            std::string nm = save_name;
            for (char& ch : nm) if (std::strchr("\\/:*?\"<>|", ch)) ch = '_';
            ImGui::BeginDisabled(nm.empty());
            if ((ImGui::SmallButton("Save") || enter) && !nm.empty()) { box_editor::saveEdits(nm); ImGui::CloseCurrentPopup(); }
            ImGui::EndDisabled();
            ImGui::EndPopup();
          }
          if (ImGui::BeginPopup("be_load")) {
            const std::vector<std::string> saved = box_editor::savedEdits();
            if (saved.empty()) ImGui::TextDisabled("Nothing saved yet (StriveLabs_scripts/edits/).");
            for (const std::string& nm : saved)
              if (ImGui::Selectable(nm.c_str())) { box_editor::loadEdits(nm); ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
          }
        }
        if (ImGui::SmallButton("Export boxes (COL JSON)")) box_editor::exportBoxes(side);
        ImGui::SameLine();
        if (ImGui::SmallButton("Export move (.bbs)")) box_editor::exportState(side);
        ImGui::BeginDisabled(!v.can_undo);
        ImGui::SameLine();
        if (ImGui::SmallButton("Undo")) box_editor::undo();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Ctrl+Z");
        ImGui::EndDisabled();
        if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false) &&
            (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || !ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow)))  // (also after a drag on the fighter)
          box_editor::undo();
        {
          bool keep = v.keep;
          if (ImGui::Checkbox("Keep edits for new fights", &keep)) box_editor::setKeep(keep);
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("Puts your edits back on when a new training fight loads.\nEdits stay when this window closes. Reset all puts the game's boxes back.");
        }
        if (!v.edits.empty()) {
          int boxes = 0;
          for (const box_editor::EditedView& ev : v.edits) boxes += 1 + static_cast<int>(std::count(ev.boxes.begin(), ev.boxes.end(), ','));
          const std::string head = "Edited (" + std::to_string(v.edits.size()) + (v.edits.size() == 1 ? " sprite, " : " sprites, ") + std::to_string(boxes) + (boxes == 1 ? " box)" : " boxes)") + "###be_edited";
          if (ImGui::TreeNode(head.c_str())) {
            for (size_t k = 0; k < v.edits.size(); k++) {
              const box_editor::EditedView& ev = v.edits[k];
              ImGui::PushID(static_cast<int>(k));
              if (ImGui::SmallButton("Reset")) box_editor::resetSprite(ev.chr, ev.sprite);
              ImGui::SameLine();
              ImGui::Text("%s  %s", ev.sprite.c_str(), ev.boxes.c_str());
              ImGui::PopID();
            }
            if (ImGui::SmallButton("Reset all")) box_editor::resetAll();
            ImGui::TreePop();
          }
        }
        ImGui::Separator();
        static char filter[64] = "";
        ImGui::TextUnformatted("Moves");
        right(80, std::to_string(s.states.size()), true);
        at(95);
        ImGui::SetNextItemWidth(160 * S);
        ImGui::InputTextWithHint("##be_filter", "Search", filter, sizeof(filter));
        if (!s.note.empty()) ImGui::TextDisabled("%s", s.note.c_str());
        const float foot = ImGui::GetTextLineHeightWithSpacing() * 2.2f;
        if (ImGui::BeginChild("be_moves", ImVec2(0, -foot), true)) {
          std::string low_f = filter;
          for (auto& c : low_f) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          for (int i = 0; i < static_cast<int>(s.states.size()); i++) {
            const std::string& n = s.states[static_cast<size_t>(i)];
            if (!low_f.empty()) {
              std::string low = n;
              for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
              if (low.find(low_f) == std::string::npos) continue;
            }
            const bool running = n == s.state, open_one = n == s.selected;
            if (running) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1));  // (the move playing, lit)
            ImGui::PushID(i);
            if (ImGui::Selectable(n.c_str(), open_one)) box_editor::selectState(side, open_one ? std::string() : n);
            ImGui::PopID();
            if (running) ImGui::PopStyleColor();
          }
          if (!s.routines.empty()) {  // the routines (RoundInit: the move definitions, inputs and follow-ups)
            ImGui::Spacing();
            ImGui::TextDisabled("Routines (%d)", static_cast<int>(s.routines.size()));
            for (int i = 0; i < static_cast<int>(s.routines.size()); i++) {
              const std::string& n = s.routines[static_cast<size_t>(i)];
              if (!low_f.empty()) {
                std::string low = n;
                for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (low.find(low_f) == std::string::npos) continue;
              }
              ImGui::PushID(100000 + i);
              if (ImGui::Selectable(n.c_str(), n == s.selected)) box_editor::selectState(side, n == s.selected ? std::string() : n);
              if (n == "RoundInit" && ImGui::IsItemHovered()) ImGui::SetTooltip("The move definitions: each move's input, stance and whether it's a follow-up.");
              ImGui::PopID();
            }
          }
        }
        ImGui::EndChild();
      }
      ImGui::TextDisabled("%s", v.status.empty() ? v.db_note.c_str() : v.status.c_str());
      ImGui::EndChild();
      pop_widget_style();
      // The opened move, in a small panel of its own: its script, the running line lit while it plays
      if (s.present && !s.selected.empty()) {
        const ImVec2 msize(470 * S, 640 * S);  // (the frame data, the line editor and the script)
        Frame m = begin_window("Move##sl_move", s.selected, ImVec2(f.o.x + f.w + 12 * S, f.o.y), msize, ImVec2(280 * S, 200 * S), true);
        if (m.closed) box_editor::selectState(side, std::string());
        if (!m.collapsed) {
          ImGui::SetCursorScreenPos(ImVec2(m.o.x + pad, m.o.y + m.head + 6 * S));
          push_widget_style();
          ImGui::BeginChild("be_move", ImVec2(m.w - 2 * pad, m.h - m.head - 12 * S), false);
          ImGui::TextDisabled("%s", s.sel_current >= 0 ? "Playing now" : "Not playing");
          {  // (experimental) the move as it was in another battle version
            const auto& vs = s.sel_versions;
            if (vs.size() > 1) {
              ImGui::SameLine(150 * S);
              ImGui::TextDisabled("Version");
              ImGui::SameLine(210 * S);
              ImGui::SetNextItemWidth(150 * S);
              std::string shown = s.sel_loaded.empty() ? "today's" : s.sel_loaded;
              for (const auto& vv : vs) if (s.sel_loaded.empty() && vv.now) shown = vv.label + " (today's)";
              if (ImGui::BeginCombo("##c2_ver", shown.c_str())) {
                for (const auto& vv : vs) {
                  const std::string item = vv.label + (vv.now ? " (today's)" : "");
                  if (ImGui::Selectable(item.c_str(), vv.label == s.sel_loaded)) box_editor::loadVersion(side, s.selected, vv.now ? std::string() : vv.folder);
                }
                ImGui::EndCombo();
              }
              if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The move as it was in an older battle version (the game keeps every version's scripts).\nExperimental. Reset move puts today's back.");
            }
            if (!s.selected.empty()) {  // the move from a .bbs text file
              ImGui::SameLine(370 * S);
              static std::vector<std::pair<std::string, std::string>> files;
              if (ImGui::SmallButton("From file...")) { files = box_editor::moveFiles(s.chr); ImGui::OpenPopup("be_from_file"); }
              if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Load this move from a .bbs text file in StriveLabs_scripts/%s/ (Export move writes one to start from).\n"
                                  "You can add and remove lines in it. Reset move puts today's back.", s.chr.c_str());
              if (ImGui::BeginPopup("be_from_file")) {
                int shown_files = 0;
                for (const auto& [file, st] : files) {
                  if (st != s.selected) continue;
                  shown_files++;
                  if (ImGui::Selectable((file + ".bbs").c_str())) { box_editor::loadMoveFile(side, s.selected, file); ImGui::CloseCurrentPopup(); }
                }
                if (!shown_files) ImGui::TextDisabled("No .bbs file for %s in StriveLabs_scripts/%s/ (Export move writes one).", s.selected.c_str(), s.chr.c_str());
                ImGui::EndPopup();
              }
            }
          }
          static int pick_frame = -1;  // the frame clicked on the strip (its sprite line is lit below)
          static std::string pick_of;
          static int scrolled_to = -1;  // (the script below scrolls to the clicked frame's line once per click)
          if (pick_of != s.selected) { pick_frame = -1; pick_of = s.selected; scrolled_to = -1; }
          int pick_line = -1;
          {  // The move's frames: a mini frame bar, colored as the frame bar is
            const box_editor::FramesView& fr = s.sel_frames;
            if (!fr.cells.empty()) {
              const float q = 9 * S, gap = 1 * S, step = q + gap;
              const float hgt = q + 2 * ImGui::GetStyle().WindowPadding.y + ImGui::GetStyle().ScrollbarSize + 2 * S;  // (fits: no vertical bar)
              if (ImGui::BeginChild("be_strip", ImVec2(0, hgt), false, ImGuiWindowFlags_HorizontalScrollbar)) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 o = ImGui::GetCursorScreenPos();
                static const int bar_state[] = {3, 4, 4, 6, 0};  // startup, active, on without hitbox, recovery, can act
                int hover = -1;
                const ImVec2 mp = ImGui::GetIO().MousePos;
                const bool in_strip = ImGui::IsWindowHovered() && mp.y >= o.y && mp.y < o.y + q;
                const float clip0 = ImGui::GetWindowPos().x - step, clip1 = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x;
                for (const box_editor::CellView& c : fr.cells)
                  for (int k = 0; k < c.dur; k++) {
                    const int f = c.first + k;
                    const ImVec2 a(o.x + f * step, o.y), b(a.x + q, a.y + q);
                    if (f == pick_frame) pick_line = c.line;
                    if (a.x < clip0 || a.x > clip1) continue;  // (off screen: not drawn)
                    ImU32 col = mv.bar_colors[bar_state[c.phase]];
                    if (c.phase == 2) col = (col & 0x00FFFFFF) | 0x70000000;  // (the attack is on, but this sprite has no hitbox)
                    dl->AddRectFilled(a, b, col);
                    if (k == 0) dl->AddLine(ImVec2(a.x - gap, a.y - 1 * S), ImVec2(a.x - gap, b.y + 1 * S), IM_COL32(0, 0, 0, 255), 1.5f * S);  // (a new sprite)
                    if (f == fr.now) dl->AddRect(ImVec2(a.x - 1 * S, a.y - 1 * S), ImVec2(b.x + 1 * S, b.y + 1 * S), IM_COL32(255, 255, 255, 255), 0, 0, 1.5f * S);
                    if (f == pick_frame) dl->AddRect(a, b, IM_COL32(255, 220, 80, 255), 0, 0, 1.5f * S);
                    if (in_strip && mp.x >= a.x && mp.x < a.x + step) hover = f;
                  }
                ImGui::Dummy(ImVec2(fr.total * step, q));
                if (hover >= 0) {
                  for (const box_editor::CellView& c : fr.cells)
                    if (hover >= c.first && hover < c.first + c.dur) {
                      static const char* const ph[] = {"startup", "active", "attack on, but this sprite has no hitbox", "recovery", "can act again"};
                      ImGui::SetTooltip("Frame %d: %s (frame %d of %d)\n%s%s\nboxes: %s", hover + 1, c.sprite.c_str(), hover - c.first + 1, c.dur, ph[c.phase], c.held ? " (held until an event)" : "",
                                        c.hits < 0 ? "the box file wasn't found" : (std::to_string(c.hurts) + " hurt, " + std::to_string(c.hits) + " hit").c_str());
                    }
                  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { pick_frame = pick_frame == hover ? -1 : hover; scrolled_to = -1; }
                }
              }
              ImGui::EndChild();
              {  // the frame data it adds up to
                int first_active = -1, last_active = -1, can_act = -1;
                for (const box_editor::CellView& c : fr.cells) {
                  if (c.phase == 1) { if (first_active < 0) first_active = c.first; last_active = c.first + c.dur - 1; }
                  if (c.phase == 4 && can_act < 0) can_act = c.first;
                }
                const auto r = slots({{"", "9999"}, {"frames   startup", "999"}, {"active", "999-999"}, {"can act", "999"}}, 1);
                ImGui::BeginGroup();  // (each in a measured slot)
                line_start();
                right(r[0].value_end, std::to_string(fr.total), false);
                at(r[1].label); ImGui::TextDisabled("frames");
                if (first_active >= 0) {
                  at(r[1].label + wid("frames   ")); ImGui::TextDisabled("startup");
                  right(r[1].value_end, std::to_string(first_active + 1), false);
                  field(r[2], "active", std::to_string(first_active + 1) + "-" + std::to_string(last_active + 1), true);
                }
                if (can_act >= 0) field(r[3], "can act", std::to_string(can_act + 1), true);
                ImGui::EndGroup();
                if (!fr.linear) ImGui::TextDisabled("then %s", fr.note.c_str());  // (its own line, there for every move)
                else ImGui::TextDisabled(" ");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Worked out from the script (sprite frames, hit, recoveryState) and which sprites have hitboxes.\nCheck it against the frame bar. Click a frame to light its sprite line below.");
              }
              {  // The frame data to edit, the attack level, and what it gives on block / on hit
                const float x0 = ImGui::GetCursorPosX();
                const float lw = ImGui::CalcTextSize("Level (other)").x + 8 * S, iw = 52 * S, colw = lw + iw + 16 * S;
                auto num = [&](int col, const char* label, const char* id, int value, int lo, int hi, bool can, auto&& set) {
                  ImGui::SetCursorPosX(x0 + col * colw);
                  ImGui::AlignTextToFramePadding();
                  ImGui::TextDisabled("%s", label);
                  ImGui::SameLine(x0 + col * colw + lw);
                  ImGui::SetNextItemWidth(iw);
                  int val = value;
                  ImGui::BeginDisabled(!can);
                  ImGui::InputInt(id, &val, 0, 0);
                  if (ImGui::IsItemDeactivatedAfterEdit() && can && val != value) set(std::clamp(val, lo, hi));
                  ImGui::EndDisabled();
                };
                auto cellSet = [&](int cell, int delta) {  // a part's length: longer on its last sprite; shorter from its last sprite back
                  const int phase = fr.cells[static_cast<size_t>(cell)].phase;
                  std::vector<int> used;  // (a sprite line shown twice is changed once)
                  for (int i = cell; i >= 0 && delta != 0; i--) {
                    const box_editor::CellView& c = fr.cells[static_cast<size_t>(i)];
                    if (c.phase != phase && !(phase == 1 && c.phase == 2)) break;  // (active: its gaps without a hitbox too)
                    if (c.held || c.line < 0 || std::find(used.begin(), used.end(), c.line) != used.end()) continue;
                    used.push_back(c.line);
                    const int to = (std::max)(1, c.dur + delta);
                    if (to != c.dur) box_editor::setFrames(side, s.selected, s.sel_script[static_cast<size_t>(c.line)].offset, to);
                    delta -= to - c.dur;
                  }
                };
                auto editable = [&](int cell) { return cell >= 0 && !fr.cells[static_cast<size_t>(cell)].held; };
                if (fr.startup > 0) {
                  num(0, "Startup", "##c1_su", fr.startup, 1, 999, editable(fr.last_startup), [&](int v) { cellSet(fr.last_startup, v - fr.startup); });
                  ImGui::SameLine();
                  num(1, "Active", "##c1_ac", fr.active, 1, 999, editable(fr.last_active), [&](int v) { cellSet(fr.last_active, v - fr.active); });
                  ImGui::SameLine();
                  num(2, "Recovery", "##c1_rc", fr.recovery, 1, 999, editable(fr.last_recovery), [&](int v) { cellSet(fr.last_recovery, v - fr.recovery); });
                  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Longer: the part's last sprite gets the frames. Shorter: they come off its last sprite, then the ones before it (each keeps 1). Click a frame on the strip to change one sprite.");
                  // the attack level of each hit that sets one, then on block / on hit
                  for (size_t k = 0; k < fr.levels.size() && k < 3; k++) {
                    const auto& lv = fr.levels[k];
                    int own = 0, own_k = 0;  // (the move's own levels: numbered only if there are several, each a hit)
                    for (size_t q = 0; q < fr.levels.size(); q++) if (fr.levels[q].event.empty()) { own++; if (q < k) own_k++; }
                    const std::string label = !lv.event.empty() ? (lv.event == "HIT" ? std::string("Level (hit)") : std::string("Level (other)"))
                                              : own > 1 ? "Level " + std::to_string(own_k + 1) : std::string("Level");
                    num(static_cast<int>(k), label.c_str(), ("##c1_lv" + std::to_string(k)).c_str(), lv.level, 0, 4, true,
                        [&](int v) { box_editor::setLevel(side, s.selected, s.sel_script[static_cast<size_t>(lv.line)].offset, v); });
                    if (ImGui::IsItemHovered())
                      ImGui::SetTooltip(lv.event.empty() ? "Attack level (0-4): sets hitstun, blockstun and hitstop."
                                                         : "Attack level when %s happens (Sidewinder's clean hit, for one): (0-4), sets hitstun, blockstun and hitstop.",
                                        lv.event.c_str());
                    if (k + 1 < fr.levels.size() && k < 2) ImGui::SameLine();
                  }
                  if (fr.levels.empty()) ImGui::TextDisabled("Level: not set in this move's script");
                  else if (fr.levels.size() > 3) { ImGui::SameLine(); ImGui::TextDisabled("(+%d more in the script)", static_cast<int>(fr.levels.size()) - 3); }
                  auto adv = [](int v) { return (v > 0 ? "+" : "") + std::to_string(v); };
                  ImGui::SetCursorPosX(x0);
                  ImGui::TextDisabled("On block");
                  ImGui::SameLine(x0 + lw);
                  ImGui::TextUnformatted(fr.has_adv ? adv(fr.on_block).c_str() : fr.multi ? "multi-hit" : "varies");
                  ImGui::SameLine(x0 + colw);
                  ImGui::TextDisabled("On hit");
                  ImGui::SameLine(x0 + colw + lw);
                  ImGui::TextUnformatted(!fr.has_adv ? (fr.multi ? "multi-hit" : "varies") : fr.launches ? "launches" : adv(fr.on_hit).c_str());
                  if (ImGui::IsItemHovered() && fr.multi) ImGui::SetTooltip("Several hits (or levels): not worked out here. The frame bar has it.");
                  else if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", fr.launches ? "It has an attack angle, so the opponent floats: on hit depends on the float (see the frame bar)."
                                                       : "Against a standing opponent, from the first level's hitstun. Crouching: +1.");
                  ImGui::SameLine(x0 + 2 * colw);
                  if (ImGui::SmallButton("Reset move")) box_editor::resetMove(side, s.selected);
                  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Puts this move's script back as the game has it (edits, a loaded version or file)");
                  // the picked frame's sprite: its length
                  int pc = -1;
                  for (size_t k = 0; k < fr.cells.size(); k++)
                    if (pick_frame >= fr.cells[k].first && pick_frame < fr.cells[k].first + fr.cells[k].dur) pc = static_cast<int>(k);
                  if (pc >= 0) {
                    const box_editor::CellView& c = fr.cells[static_cast<size_t>(pc)];
                    num(0, "Sprite", "##c1_sp", c.dur, 1, 999, !c.held, [&](int v) {
                      box_editor::setFrames(side, s.selected, s.sel_script[static_cast<size_t>(c.line)].offset, v);
                    });
                    ImGui::SameLine(x0 + colw);
                    ImGui::TextDisabled("frames of %s%s", c.sprite.c_str(), s.sel_script[static_cast<size_t>(c.line)].edited ? " (edited)" : "");
                  } else {
                    ImGui::SetCursorPosX(x0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("Click a frame on the strip to change its sprite's length.");
                  }
                }
              }
            }
          }
          static int edit_line = -1;  // the script line picked to edit its numbers
          static std::string edit_of;
          if (edit_of != s.selected) { edit_line = -1; edit_of = s.selected; }
          {  // The picked line's numbers (a fixed-height box: the list below never moves)
            const float rowh = ImGui::GetFrameHeightWithSpacing();
            if (ImGui::BeginChild("be_line_edit", ImVec2(0, rowh * 5 + 8 * S), true)) {
              const box_editor::InstrView* in = edit_line >= 0 && edit_line < static_cast<int>(s.sel_script.size()) ? &s.sel_script[static_cast<size_t>(edit_line)] : nullptr;
              if (!in) ImGui::TextDisabled("Click a line of the script below to change its numbers.");
              else {
                ImGui::TextUnformatted(in->name.c_str());
                if (ImGui::IsItemHovered() && !in->desc.empty()) ImGui::SetTooltip("%s", in->desc.c_str());
                const float lw = 320 * S, iw = 90 * S;
                ImGui::SameLine(lw);
                const std::string state_text = !in->replaced_from.empty() ? "replaced (was " + in->replaced_from + ")" : in->edited ? std::string("edited (Reset move puts it back)") : in->call;
                ImGui::TextDisabled("%s", state_text.c_str());
                {  // the line as another instruction of its size (the button at a fixed place on the right)
                  const float bw = ImGui::CalcTextSize("Replace with...").x + ImGui::GetStyle().FramePadding.x * 2;
                  ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - bw);
                  static std::vector<std::pair<int, std::string>> repl;
                  static char repl_find[48] = "";
                  if (ImGui::SmallButton("Replace with...")) {
                    const int base = in->replaced_op >= 0 ? in->replaced_op : in->op;  // (the instruction the line was)
                    repl = box_editor::replacements(base);
                    repl.erase(std::remove_if(repl.begin(), repl.end(), [&](const auto& x) { return x.first == in->op; }), repl.end());
                    if (base != in->op) repl.insert(repl.begin(), {base, in->replaced_from + "  (put back)"});
                    repl_find[0] = 0;
                    ImGui::OpenPopup("be_replace");
                  }
                  if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Turns this line into another instruction of the same size, so nothing after it moves. Its numbers start at 0: set them here.\nBlock lines (if, upon, labels...), sprites, gotos and calls can't be replaced, and nothing that takes a name can go in.");
                  if (ImGui::BeginPopup("be_replace")) {
                    if (repl.empty()) ImGui::TextDisabled("This line can't be replaced (a block line, sprite, goto or call, or nothing else is its size).");
                    else {
                      ImGui::SetNextItemWidth(260 * S);
                      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                      ImGui::InputTextWithHint("##be_repl_find", "Search instructions", repl_find, sizeof(repl_find));
                      std::string q = repl_find;
                      for (auto& c : q) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                      if (ImGui::BeginChild("be_repl_list", ImVec2(320 * S, 260 * S), true)) {
                        for (const auto& [o, n] : repl) {
                          std::string low = n;
                          for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                          if (!q.empty() && low.find(q) == std::string::npos) continue;
                          ImGui::PushID(o);
                          if (ImGui::Selectable(n.c_str())) { box_editor::replaceLine(side, s.selected, in->offset, o); ImGui::CloseCurrentPopup(); }
                          ImGui::PopID();
                        }
                      }
                      ImGui::EndChild();
                    }
                    ImGui::EndPopup();
                  }
                }
                auto fit = [&](const std::string& t) {  // (a label cut to its column, "..." if longer: the tooltip has it whole)
                  if (ImGui::CalcTextSize(t.c_str()).x <= lw - 10 * S) return t;
                  std::string c = t;
                  while (!c.empty() && ImGui::CalcTextSize((c + "...").c_str()).x > lw - 10 * S) c.pop_back();
                  return c + "...";
                };
                for (size_t k = 0; k < in->argv.size(); k++) {
                  const auto& a = in->argv[k];
                  ImGui::PushID(static_cast<int>(k));
                  ImGui::AlignTextToFramePadding();
                  ImGui::TextUnformatted(fit(a.label).c_str());
                  if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\n%s", a.label.c_str(), !a.tip.empty() ? a.tip.c_str() : !in->desc.empty() ? in->desc.c_str() : "No description in the database.");
                  ImGui::SameLine(lw);
                  ImGui::SetNextItemWidth(iw);
                  int val = a.value;
                  ImGui::BeginDisabled(!a.editable);
                  ImGui::InputInt("##v", &val, 0, 0);
                  if (ImGui::IsItemDeactivatedAfterEdit() && a.editable && val != a.value) box_editor::setArg(side, s.selected, in->offset, a.field, val);
                  ImGui::EndDisabled();
                  if (!a.editable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("A variable (Mem), not a number: can't be edited.");
                  ImGui::SameLine(lw + iw + 8 * S);
                  ImGui::TextDisabled("%s", a.value_name.c_str());
                  ImGui::PopID();
                }
                if (in->argv.empty()) ImGui::TextDisabled("No numbers on this line.");
              }
            }
            ImGui::EndChild();
          }
          // Search the script: lines whose text (or description) holds it are lit; the current one is scrolled to
          static char find[64] = "";
          static int find_at = 0, find_scroll = -1;  // (which match is current; the line to scroll to once)
          static std::string find_was, find_of;
          std::vector<int> found;
          {
            auto low = [](std::string t) { for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return t; };
            const std::string q = low(find);
            if (!q.empty())
              for (int i = 0; i < static_cast<int>(s.sel_script.size()); i++) {
                const box_editor::InstrView& in = s.sel_script[static_cast<size_t>(i)];
                if (low(in.name + ": " + in.args).find(q) != std::string::npos || low(in.desc).find(q) != std::string::npos) found.push_back(i);
              }
            if (find_was != q || find_of != s.selected) { find_was = q; find_of = s.selected; find_at = 0; find_scroll = found.empty() ? -1 : found[0]; }
            const float x0 = ImGui::GetCursorPosX();
            ImGui::SetNextItemWidth(200 * S);
            const bool enter = ImGui::InputTextWithHint("##be_find", "Search the script", find, sizeof(find), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine(x0 + 208 * S);
            const std::string count = q.empty() ? std::string(" ") : found.empty() ? std::string("none") : std::to_string(find_at + 1) + " of " + std::to_string(found.size());
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", count.c_str());
            ImGui::SameLine(x0 + 208 * S + ImGui::CalcTextSize("999 of 999").x + 10 * S);
            ImGui::BeginDisabled(found.empty());
            const bool prev = ImGui::SmallButton("<");
            ImGui::SameLine();
            const bool next = ImGui::SmallButton(">") || (enter && !found.empty());
            ImGui::EndDisabled();
            if (enter) ImGui::SetKeyboardFocusHere(-1);  // (Enter keeps the box: Enter again for the next)
            if (!found.empty() && (prev || next)) {
              const int n = static_cast<int>(found.size());
              find_at = ((find_at + (next ? 1 : -1)) % n + n) % n;
              find_scroll = found[static_cast<size_t>(find_at)];
            }
          }
          if (ImGui::BeginChild("be_move_lines", ImVec2(0, 0), true)) {
            for (int i = 0; i < static_cast<int>(s.sel_script.size()); i++) {
              const box_editor::InstrView& in = s.sel_script[static_cast<size_t>(i)];
              const std::string line = std::string(static_cast<size_t>(in.depth) * 2, ' ') + in.name + ": " + in.args;
              const bool hit = std::binary_search(found.begin(), found.end(), i);
              const bool hit_now = hit && !found.empty() && found[static_cast<size_t>(find_at)] == i;
              if (hit) {  // (a match: a band behind it, brighter for the current one)
                const ImVec2 a = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(a, ImVec2(a.x + ImGui::GetContentRegionAvail().x, a.y + ImGui::GetTextLineHeight()),
                                                         hit_now ? IM_COL32(90, 140, 40, 200) : IM_COL32(60, 90, 30, 140));
              }
              if (i == find_scroll) { ImGui::SetScrollHereY(0.3f); find_scroll = -1; }
              const bool cur = i == s.sel_current;
              const bool frame = in.op == 2;  // a sprite: a frame of the move
              const bool picked = i == pick_line;
              if (picked) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.86f, 0.31f, 1));
              else if (cur) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1));
              else if (in.edited) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.55f, 1));  // (a script edit)
              else if (frame) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.85f, 1.0f, 1));
              ImGui::PushID(i);
              if (ImGui::Selectable(line.c_str(), i == edit_line)) edit_line = i == edit_line ? -1 : i;  // (picked: its numbers above)
              ImGui::PopID();
              if (picked && scrolled_to != pick_frame) { ImGui::SetScrollHereY(0.3f); scrolled_to = pick_frame; }
              if (cur || frame || picked || in.edited) ImGui::PopStyleColor();
              if (ImGui::IsItemHovered() && !in.desc.empty()) ImGui::SetTooltip("%s (%d)\n%s", in.name.c_str(), in.op, in.desc.c_str());
            }
          }
          ImGui::EndChild();
          ImGui::EndChild();
          pop_widget_style();
        }
        end_window();
      }
    }
    end_window();
  }
  void draw_colors() {
    use(E_MENU);
    const recolor_test::View v = recolor_test::view();
    const int R = static_cast<int>(v.regions.size());
    g_regions_n = R;
    g_leads.resize(static_cast<size_t>(R));
    for (int i = 0; i < R; i++) g_leads[static_cast<size_t>(i)] = v.regions[static_cast<size_t>(i)].lead < 0 ? i : v.regions[static_cast<size_t>(i)].lead;
    static bool rows_were = true;
    if (v.group_same != rows_were) {  // "Same colors as one row" was toggled, so the selection moves to its rows
      rows_were = v.group_same;
      std::vector<int> rows;
      for (int r : g_sel) { const int l = lead_of(r); if (std::find(rows.begin(), rows.end(), l) == rows.end()) rows.push_back(l); }
      if (rows != g_sel) set_selection(rows);
    }
    if (R == 0) forget_selection();  // A new fighter or color is getting ready, so the old numbers mean nothing
    g_sel.erase(std::remove_if(g_sel.begin(), g_sel.end(), [&](int r) { return r >= R; }), g_sel.end());
    if (g_pick_region >= R) g_pick_region = -1;
    ImGuiIO& io = ImGui::GetIO();
    const float pad = PAD * S, row = 32 * S, sw = 22 * S;
    const ImVec2 size(440 * S, 680 * S);
    Frame f = begin_window("Colors##sl_colors", "Colors", ImVec2(io.DisplaySize.x - size.x - 24 * S, io.DisplaySize.y * 0.08f),
                           size, ImVec2(360 * S, 300 * S), true);
    if (f.closed) recolor_test::toggle();
    {  // The selection's tag on the fighter, naming the part and pointing at it
      float tx = 0, ty = 0;
      if (!g_sel.empty() && g_sel[0] < R && recolor_test::tagAt(tx, ty)) {
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        const ImVec2 p(tx * io.DisplaySize.x, ty * io.DisplaySize.y), e(p.x + 34 * S, p.y - 34 * S);
        const std::string n = g_sel.size() == 1 ? region_label(v, g_sel[0]) : std::to_string(g_sel.size()) + " parts";
        bg->AddLine(p, e, RED, 2 * S);
        bg->AddCircleFilled(p, 4 * S, RED);
        const float th = f_body->FontSize;
        const ImVec2 a(e.x, e.y - th * 0.5f - 5 * S), b(e.x + text_w(f_body, n) + 16 * S, e.y + th * 0.5f + 5 * S);
        bg->AddRectFilled(a, b, PANEL);
        bg->AddRect(a, b, RED);
        text(bg, f_body, ImVec2(a.x + 8 * S, a.y + 5 * S), TEXT, n);
      }
    }
    if (!f.collapsed) {
      ImDrawList* dl = f.dl;
      const float x0 = f.o.x + pad, x1 = f.o.x + f.w - pad;
      float y = f.o.y + f.head + 10 * S;
      text(dl, f_bold, ImVec2(x0, y), TEXT, v.title.empty() ? std::string("No fighter yet") : v.title);
      {  // the other fighter, and this one's body or weapon
        float bx = x1;
        if (v.can_switch) {
          bx -= small_button_w("Switch");
          if (small_button(dl, "switch", "Switch", ImVec2(bx, y - 2 * S))) { recolor_test::nextTarget(); forget_selection(); }
          bx -= 10 * S;
        }
        float parts_h = 0;  // Height of the parts on lines of their own, when they don't fit beside the title
        if (v.parts.size() > 1) {
          const int n = static_cast<int>(v.parts.size());
          float row_w = 0;
          for (int i = 0; i < n; i++) row_w += small_button_w(part_name(v.parts[static_cast<size_t>(i)]).c_str()) + (i ? 4 * S : 0);
          const float title_end = x0 + text_w(f_bold, v.title.empty() ? std::string("No fighter yet") : v.title) + 12 * S;
          const bool beside = bx - row_w >= title_end;  // Otherwise below the title, left to right, wrapping
          float cx = beside ? bx - row_w : x0, cy = beside ? y - 2 * S : y + f_bold->FontSize + 8 * S;
          if (!beside) parts_h = CHIP_H * S + 6 * S;
          for (int i = 0; i < n; i++) {  // the body first
            const std::string name = part_name(v.parts[static_cast<size_t>(i)]);
            const float w = small_button_w(name.c_str());
            if (!beside && cx > x0 && cx + w > x1) { cx = x0; cy += CHIP_H * S + 4 * S; parts_h += CHIP_H * S + 4 * S; }
            ImGui::PushID(i);
            if (small_button(dl, "part", name.c_str(), ImVec2(cx, cy), true, i == v.part) && i != v.part) {
              recolor_test::showPart(v.part_ids[static_cast<size_t>(i)]);
              forget_selection();
            }
            if (ImGui::IsItemHovered() && v.part_hidden[static_cast<size_t>(i)]) ImGui::SetTooltip("Only seen during a move");
            ImGui::PopID();
            cx += w + 4 * S;
          }
        }
        y += parts_h;
      }
      y += f_bold->FontSize + 10 * S;
      if (v.mapping >= 0) {  // the parts of the model, mapped once per character
        text(dl, f_small, ImVec2(x0, y), MUTED, "Mapping regions...");
        y += f_small->FontSize + 6 * S;
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + 4 * S), CHIP);
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + (x1 - x0) * v.mapping, y + 4 * S), RED);
        y += 14 * S;
      }
      if (update_check::gamePatchedSince()) {  // The game was patched since this build was tested. Say so, but don't block
        text(dl, f_small, ImVec2(x0, y), RED_TEXT, "Strive was updated after this StriveLabs. Colors may not apply right: check for a StriveLabs update.");
        y += f_small->FontSize + 8 * S;
      }
      if (!v.status.empty()) { text(dl, f_small, ImVec2(x0, y), MUTED, v.status); y += f_small->FontSize + 8 * S; }
      if (!v.drawn_from.empty() && v.part >= 0 && v.part < static_cast<int>(v.parts.size())) {  // A hidden model partly colored from another part's texture
        const std::string n = part_name(v.parts[static_cast<size_t>(v.part)]);
        text(dl, f_small, ImVec2(x0, y), MUTED, "Some of it is colored from " + v.drawn_from + ": edit those rows there (marked \"" + n + " only\").");
        y += f_small->FontSize + 8 * S;
      }
      if (v.progress >= 0) {
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + 4 * S), CHIP);
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + (x1 - x0) * v.progress, y + 4 * S), RED);
        y += 14 * S;
      }
      {  // Undo, redo and save (for every tab) on the right, unsaved on the left
        float rx = x1 - small_button_w(v.unsaved ? "Save*" : "Save");  // (baking is on its own tab)
        if (small_button(dl, "save", v.unsaved ? "Save*" : "Save", ImVec2(rx, y), v.unsaved, v.unsaved)) recolor_test::saveRecolor();
        rx -= 4 * S + small_button_w("Redo");
        if (small_button(dl, "redo", "Redo", ImVec2(rx, y), v.can_redo)) recolor_test::redo();
        rx -= 4 * S + small_button_w("Undo");
        if (small_button(dl, "undo", "Undo", ImVec2(rx, y), v.can_undo)) recolor_test::undo();
        if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false) && (!box_editor::isOpen() || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))) {  // (Script & boxes has its own)
          if (io.KeyShift) recolor_test::redo(); else recolor_test::undo();
        }
        if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) recolor_test::redo();
        if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) recolor_test::saveRecolor();
        const char* hint = v.unsaved ? "Unsaved (Ctrl+S)" : "";
        if (x0 + text_w(f_small, hint) < rx - 8 * S) text(dl, f_small, ImVec2(x0, y + (CHIP_H * S - f_small->FontSize) * 0.5f), DIM, hint);
        y += CHIP_H * S + 8 * S;
      }
      {  // The tabs. The last one stays when the editor is closed and opened again
        const char* const tabs[5] = {"Regions", "Auto", "Effects", "Save & share", "Bake"};
        float tx = x0;
        for (int i = 0; i < 5; i++) {
          ImGui::PushID(i);
          if (small_button(dl, "tab", tabs[i], ImVec2(tx, y), true, g_tab == i) && g_tab != i) { g_tab = i; g_pick_armed = false; }  // An armed pick is dropped, since its button is gone
          ImGui::PopID();
          tx += small_button_w(tabs[i]) + 4 * S;
        }
        y += CHIP_H * S + 8 * S;
        dl->AddLine(ImVec2(f.o.x + 1, y - 1), ImVec2(f.o.x + f.w - 1, y - 1), BORDER);
        y += 8 * S;
      }
      // Drags and the wheel outside our windows turn and zoom the camera (any tab).
      if (v.free_cam && !io.WantCaptureMouse) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !g_pick_armed && !v.picking) recolor_test::cameraInput(io.MouseDelta.x, io.MouseDelta.y, 0.0f);
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) recolor_test::cameraPan(io.MouseDelta.x, io.MouseDelta.y);
        if (io.MouseWheel != 0.0f) recolor_test::cameraInput(0.0f, 0.0f, io.MouseWheel);
      }
      if (g_tab == 0) {  // Regions: the view, the selection's tools, gradients
        {  // shadow follows the lit color (a toggle like the menu's)
          const float tw = 36 * S, th = 18 * S;
          const ImVec2 ta(x1 - tw, y), tb(x1, y + th);
          text(dl, f_small, ImVec2(x0, y + (th - f_small->FontSize) * 0.5f), LABEL, "Link shadow color");
          if (clicked_rect("follow", ImVec2(x0, y), tb)) recolor_test::setShadowFollows(!v.shadow_follows);
          dl->AddRectFilled(ta, tb, v.shadow_follows ? RED : CHIP);
          const float k = th - 6 * S, kx = v.shadow_follows ? tb.x - 3 * S - k : ta.x + 3 * S;
          dl->AddRectFilled(ImVec2(kx, ta.y + 3 * S), ImVec2(kx + k, ta.y + 3 * S + k), v.shadow_follows ? ON_ACCENT : MUTED);
          y += th + 8 * S;
          const ImVec2 ba(x1 - tw, y), bb(x1, y + th);
          text(dl, f_small, ImVec2(x0, y + (th - f_small->FontSize) * 0.5f), LABEL, "Highlight selection");
          if (clicked_rect("blackout", ImVec2(x0, y), bb)) recolor_test::setBlackout(!v.blackout);
          dl->AddRectFilled(ba, bb, v.blackout ? RED : CHIP);
          const float bkx = v.blackout ? bb.x - 3 * S - k : ba.x + 3 * S;
          dl->AddRectFilled(ImVec2(bkx, ba.y + 3 * S), ImVec2(bkx + k, ba.y + 3 * S + k), v.blackout ? ON_ACCENT : MUTED);
          y += th + 8 * S;
          {  // free camera
            const ImVec2 ca(x1 - tw, y), cb(x1, y + th);
            text(dl, f_small, ImVec2(x0, y + (th - f_small->FontSize) * 0.5f), LABEL, "Free camera");
            if (ImGui::IsMouseHoveringRect(ImVec2(x0, y), ImVec2(x0 + text_w(f_small, "Free camera"), y + th))) ImGui::SetTooltip("%s", recolor_test::kCamHelp);
            if (clicked_rect("freecam", ImVec2(x0, y), cb)) recolor_test::setFreeCamera(!v.free_cam);
            dl->AddRectFilled(ca, cb, v.free_cam ? RED : CHIP);
            const float ckx = v.free_cam ? cb.x - 3 * S - k : ca.x + 3 * S;
            dl->AddRectFilled(ImVec2(ckx, ca.y + 3 * S), ImVec2(ckx + k, ca.y + 3 * S + k), v.free_cam ? ON_ACCENT : MUTED);
            y += th + 8 * S;
            {  // follow the selection, a row of its own like the others
              const ImVec2 fa(x1 - tw, y), fb(x1, y + th);
              text(dl, f_small, ImVec2(x0, y + (th - f_small->FontSize) * 0.5f), LABEL, "Follow selection");
              if (clicked_rect("follow_sel", ImVec2(x0, y), fb)) recolor_test::setFollow(!v.follow);
              dl->AddRectFilled(fa, fb, v.follow ? RED : CHIP);
              const float fkx = v.follow ? fb.x - 3 * S - k : fa.x + 3 * S;
              dl->AddRectFilled(ImVec2(fkx, fa.y + 3 * S), ImVec2(fkx + k, fa.y + 3 * S + k), v.follow ? ON_ACCENT : MUTED);
            }
            y += th + 4 * S;
            if (!v.cam_info.empty() && v.cam_info != recolor_test::kCamHelp) { text(dl, f_small, ImVec2(x0, y), MUTED, v.cam_info); y += f_small->FontSize + 4 * S; }
            y += 6 * S;
          }
        }
        {  // the selection's tools
          const bool any = !g_sel.empty();
          float bx = x0;
          if (small_button(dl, "copy", "Copy", ImVec2(bx, y), any)) {
            g_clip_set = true;
            g_clip_lit = v.regions[static_cast<size_t>(g_sel[0])].lit;
            g_clip_shadow = v.regions[static_cast<size_t>(g_sel[0])].shadow;
          }
          bx += small_button_w("Copy") + 4 * S;
          if (small_button(dl, "paste", "Paste", ImVec2(bx, y), any && g_clip_set)) paste_to(g_sel);
          bx += small_button_w("Paste") + 4 * S;
          if (small_button(dl, "grad", "Gradient", ImVec2(bx, y), true, g_grad_open)) g_grad_open = !g_grad_open;
          bx += small_button_w("Gradient") + 4 * S;
          if (small_button(dl, "reset_sel", "Reset", ImVec2(bx, y), any))
            for (int r : g_sel) recolor_test::reset(r);
          bx += small_button_w("Reset") + 10 * S;
          {  // Chroma key colors paint the selection flat (lit and shadow the same pure color) to key out later
            const struct { const char* name; uint32_t rgb; } keys[3] = {{"G", 0x00FF00}, {"B", 0x0000FF}, {"R", 0xFF0000}};
            for (const auto& k : keys) {
              ImGui::PushID(k.name);
              if (small_button(dl, "key", k.name, ImVec2(bx, y), any)) {
                std::vector<recolor_test::Pick> picks;
                for (int r : g_sel) picks.push_back({r, k.rgb, 1, k.rgb});  // Shadow exactly the same, so no shading breaks the key
                recolor_test::setPairs(picks, 2);  // Exactly the key color, on a gradient row too
              }
              if (ImGui::IsItemHovered()) ImGui::SetTooltip("Flat chroma %s, for keying", k.rgb == 0x00FF00 ? "green" : k.rgb == 0x0000FF ? "blue" : "red");
              ImGui::PopID();
              bx += small_button_w(k.name) + 2 * S;
            }
            bx += 8 * S;
          }
          if (g_clip_set) {  // what's on the clipboard
            const ImVec2 ca(bx, y + 3 * S), cb(bx + 16 * S, y + CHIP_H * S - 3 * S);
            dl->AddRectFilled(ca, ImVec2(ca.x + 8 * S, cb.y), rgb_u32(g_clip_lit));
            dl->AddRectFilled(ImVec2(ca.x + 8 * S, ca.y), cb, rgb_u32(g_clip_shadow));
            dl->AddRect(ca, cb, BORDER);
            bx += 22 * S;
          }
          y += CHIP_H * S + 8 * S;
        }
        if (g_grad_open) {  // options, presets, then a custom one
          {
            push_widget_style();
            ImGui::SetCursorScreenPos(ImVec2(x0, y));
            ImGui::Checkbox("Every region", &g_grad_each);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Each region its own step, the look-alikes under a row too (more steps). Off: one step per row.");
            ImGui::SameLine();
            ImGui::Checkbox("Dark to light", &g_grad_dark);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Steps go from the darkest region to the lightest, as worn. Off: the order you picked them.");
            pop_widget_style();
            y += CHIP_H * S + 6 * S;
          }
          const auto& ps = grad_presets();
          const float gw = (x1 - x0 - 4 * S * 3) / 4.0f, gh = 34 * S;
          for (size_t i = 0; i < ps.size(); i++) {
            const float gx = x0 + static_cast<float>(i % 4) * (gw + 4 * S), gy = y + static_cast<float>(i / 4) * (gh + 4 * S);
            const ImVec2 a(gx, gy), b(gx + gw, gy + gh);
            const uint32_t base = !g_sel.empty() ? v.regions[static_cast<size_t>(g_sel[0])].lit : 0xC0303A;
            for (int s = 0; s < 16; s++) {  // the gradient as 16 slices
              const float sx0 = a.x + (b.x - a.x) * s / 16.0f, sx1 = a.x + (b.x - a.x) * (s + 1) / 16.0f;
              dl->AddRectFilled(ImVec2(sx0, a.y), ImVec2(sx1, a.y + gh - f_small->FontSize - 6 * S),
                                rgb_u32(grad_at(static_cast<int>(i), s / 15.0f, base)));
            }
            ImGui::PushID(static_cast<int>(i));
            const bool hit = clicked_rect("preset", a, b);
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            dl->AddRect(a, b, hov ? RED : BORDER);
            text(dl, f_small, ImVec2(a.x + 4 * S, b.y - f_small->FontSize - 2 * S), hov ? TEXT : LABEL, ps[i].name);
            if (hit) apply_gradient(static_cast<int>(i), v);
          }
          y += (gh + 4 * S) * static_cast<float>((ps.size() + 3) / 4) + 2 * S;
          {  // a palette's colors as the gradient
            const char* pl = "A palette's colors...";
            if (small_button(dl, "gpal", pl, ImVec2(x0, y), !g_sel.empty())) ImGui::OpenPopup("grad_palettes");
            push_widget_style();
            if (ImGui::BeginPopup("grad_palettes")) {
              ImGui::BeginChild("gpals", ImVec2(272 * S, 320 * S));
              for (size_t i = 0; i < palettes().size(); i++)
                if (palette_row(i)) { apply_gradient(static_cast<int>(ps.size() + i), v); ImGui::CloseCurrentPopup(); }
              ImGui::EndChild();
              ImGui::EndPopup();
            }
            pop_widget_style();
            y += CHIP_H * S + 6 * S;
          }
          // custom: from -> to, then apply
          push_widget_style();
          ImGui::SetCursorScreenPos(ImVec2(x0, y));
          ImGui::TextUnformatted("Custom");
          ImGui::SameLine();
          ImGui::ColorEdit3("##gfrom", g_grad_from, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_PickerHueBar);
          ImGui::SameLine();
          ImGui::TextUnformatted(g_grad_use_mid ? "via" : "to");
          ImGui::SameLine();
          if (g_grad_use_mid) {
            ImGui::ColorEdit3("##gmid", g_grad_mid, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_PickerHueBar);
            ImGui::SameLine();
            ImGui::TextUnformatted("to");
            ImGui::SameLine();
          }
          ImGui::ColorEdit3("##gto", g_grad_to, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_PickerHueBar);
          ImGui::SameLine();
          ImGui::Checkbox("Middle", &g_grad_use_mid);
          pop_widget_style();
          const bool enough = g_sel.size() > 1 || (g_grad_each && !g_sel.empty());
          const char* apply = enough ? "Apply" : "Select 2+ rows first";
          if (small_button(dl, "gapply", apply, ImVec2(x1 - small_button_w(apply), y - 2 * S), enough)) apply_gradient(-1, v);
          y += CHIP_H * S + 10 * S;
        }
      }
      {  // A pick: the next click on the fighter selects what's under it. Works on any tab, so a pick under way lands
        static int pick_seen = 0;
        bool& pick_armed = g_pick_armed;
        static std::vector<int> choices;  // When several regions are there, choose from a list at the click
        static int choices_part = -1;
        if (v.pick_serial != pick_seen) {
          pick_seen = v.pick_serial;
          if (v.picked >= 0 && v.picked < R) { set_selection({v.picked}); g_sel_anchor = v.picked; }
          choices.clear();
          for (int c : v.pick_choices) if (c >= 0 && c < R) choices.push_back(c);
          if (choices.size() > 1) { ImGui::OpenPopup("pick_choices"); choices_part = v.part; }
        }
        ImGui::SetNextWindowPos(g_pick_click, ImGuiCond_Appearing);
        if (ImGui::BeginPopup("pick_choices")) {
          if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { ImGui::CloseCurrentPopup(); g_esc_used = ImGui::GetFrameCount(); }  // Esc closes the list and the selection stays
          if (v.part != choices_part || v.pick_choices.empty()) ImGui::CloseCurrentPopup();  // Another part or fighter now, or another selection
          for (int c : choices) {
            if (c >= R || v.part != choices_part) continue;
            const auto& rg = v.regions[static_cast<size_t>(c)];
            ImGui::PushID(c);
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const float hh = 24 * S;
            const std::string lab = region_label(v, c) + (rg.edited ? "  (edited)" : "");
            const bool hit = ImGui::InvisibleButton("c", ImVec2(260 * S, hh));
            ImDrawList* pdl = ImGui::GetWindowDrawList();
            if (ImGui::IsItemHovered()) pdl->AddRectFilled(a, ImVec2(a.x + 260 * S, a.y + hh), CURSOR);
            auto rgb = [](uint32_t x) { return IM_COL32(x >> 16 & 0xFF, x >> 8 & 0xFF, x & 0xFF, 255); };
            pdl->AddRectFilled(ImVec2(a.x + 4 * S, a.y + 4 * S), ImVec2(a.x + 4 * S + hh - 8 * S, a.y + hh - 4 * S), rgb(rg.lit));
            text(pdl, f_body, ImVec2(a.x + hh + 6 * S, a.y + (hh - f_body->FontSize) * 0.5f), c == g_sel_anchor ? TEXT : LABEL, lab);
            if (hit) { set_selection({c}); g_sel_anchor = c; ImGui::CloseCurrentPopup(); }
            ImGui::PopID();
          }
          ImGui::EndPopup();
        }
        const char* pl = v.picking ? "Picking..." : pick_armed ? "Click the fighter" : "Pick on fighter";
        if (g_tab == 0 && small_button(dl, "pick_btn", pl, ImVec2(x0, y), R > 0 && !v.picking, pick_armed || v.picking)) pick_armed = !pick_armed;
        else if (pick_armed && ImGui::IsMouseClicked(0) && !io.WantCaptureMouse) {  // A click on the game, not on our windows
          const ImVec2 mp = ImGui::GetIO().MousePos;
          recolor_test::pickAt(mp.x / std::max(1.0f, io.DisplaySize.x), mp.y / std::max(1.0f, io.DisplaySize.y));
          g_pick_click = mp;
          pick_armed = false;
        }
        if (pick_armed && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) pick_armed = false;
        else deselect_on_escape();
        if (g_tab == 0 && small_button(dl, "texview_btn", "Texture", ImVec2(x0 + small_button_w(pl) + 6 * S, y), R > 0, g_texview)) g_texview = !g_texview;
        if (g_tab == 0) y += CHIP_H * S + 8 * S;
      }
      if (g_tab == 0) {
        {  // select all (a tick box), and how many are selected
          const float bs = 16 * S;
          int listed = 0, picked = 0;  // Every region but the small ones off the model, and how many of those are selected
          for (const auto& r : v.regions) listed += r.listed;
          for (int i : g_sel) picked += i >= 0 && i < R && v.regions[static_cast<size_t>(i)].listed;
          const bool all = R > 0 && picked == listed;
          const char* al = "Select all";
          const ImVec2 ba(x0, y), bb(x0 + bs, y + bs);
          dl->AddRectFilled(ba, bb, all ? RED : CHIP);
          dl->AddRect(ba, bb, BORDER);
          if (all) ImGui::RenderCheckMark(dl, ImVec2(ba.x + 3 * S, ba.y + 3 * S), ON_ACCENT, bs - 6 * S);
          text(dl, f_small, ImVec2(bb.x + 8 * S, y + (bs - f_small->FontSize) * 0.5f), R > 0 ? LABEL : DIM, al);
          if (R > 0 && clicked_rect("sel_all", ba, ImVec2(bb.x + 8 * S + text_w(f_small, al), bb.y))) {
            std::vector<int> s;
            if (!all) for (int i = 0; i < R; i++) if (v.regions[static_cast<size_t>(i)].listed) s.push_back(i);  // Only the rows, so no twins and none off the model
            set_selection(s);
            g_sel_anchor = -1;
          }
          {  // Regions in the selection's worn color, where the same paint went (Jam's stocking stripes are her skin)
            const char* sl = "+ Same color";
            const float sx0 = bb.x + 8 * S + text_w(f_small, al) + 18 * S;
            const bool can = !g_sel.empty();
            text(dl, f_small, ImVec2(sx0, y + (bs - f_small->FontSize) * 0.5f), can ? LABEL : DIM, sl);
            if (can && clicked_rect("same_col", ImVec2(sx0, y), ImVec2(sx0 + text_w(f_small, sl), y + bs))) {
              std::vector<uint32_t> cols;
              for (int r : g_sel) if (r >= 0 && r < R) cols.push_back(v.regions[static_cast<size_t>(r)].orig_lit);
              std::vector<int> s = g_sel;
              for (int i = 0; i < R; i++)
                if (std::find(cols.begin(), cols.end(), v.regions[static_cast<size_t>(i)].orig_lit) != cols.end() && std::find(s.begin(), s.end(), i) == s.end())
                  s.push_back(i);
              set_selection(s);
            }
            if (can && ImGui::IsItemHovered()) ImGui::SetTooltip("Add all regions with this color");
          }
          std::string n = g_sel.empty() ? std::string()
                                : g_sel.size() == 1 ? region_label(v, g_sel[0])
                                : std::to_string(g_sel.size()) + " selected";
          const float aw = 18 * S;
          {  // Shortened to fit between the buttons on the left and the arrows
            const float left = bb.x + 8 * S + text_w(f_small, al) + 18 * S + text_w(f_small, "+ Same color") + 12 * S;
            const float room = x1 - aw - 16 * S - aw - left;
            while (n.size() > 3 && text_w(f_small, n) > room) n = n.substr(0, n.size() - 4) + "...";
          }
          const float nw = text_w(f_small, n);
          const ImVec2 na(x1 - aw, y), nb(x1, y + bs), pa(na.x - 16 * S - nw - aw, y), pb(pa.x + aw, y + bs);
          text(dl, f_small, ImVec2(pb.x + 8 * S, y + (bs - f_small->FontSize) * 0.5f), g_sel.empty() ? DIM : TEXT, n);
          for (int d : {-1, 1}) {
            const ImVec2 a = d < 0 ? pa : na, b = d < 0 ? pb : nb;
            const bool hit = R > 0 && clicked_rect(d < 0 ? "sel_prev" : "sel_next", a, b);
            const bool hov = R > 0 && ImGui::IsItemHovered();
            arrow(dl, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), 5 * S, d > 0, hov ? RED : R > 0 ? MUTED : DIM);
            if (hit) {
              const int from = g_sel.empty() ? (d > 0 ? -1 : R) : g_sel[0];
              int to = ((from + d) % R + R) % R;
              auto under = [&](int k) {  // A small one is listed under its region, and the arrows go by the rows
                const auto& rk = v.regions[static_cast<size_t>(k)];
                const int pr = rk.parent;
                return rk.detail && pr >= 0 && pr < R && v.regions[static_cast<size_t>(pr)].listed && !v.regions[static_cast<size_t>(pr)].detail;
              };
              for (int n = 0; n < R && (!v.regions[static_cast<size_t>(to)].listed || under(to)); n++) to = ((to + d) % R + R) % R;
              set_selection({to});
              g_sel_anchor = to;
            }
          }
          y += bs + 10 * S;
        }
        dl->AddLine(ImVec2(f.o.x + 1, y - 1), ImVec2(f.o.x + f.w - 1, y - 1), BORDER);
      }
      // The tab's scrolling page: the regions (biggest first), Auto, the effects, or saving and sharing.
      const float list_h = (std::max)(f.o.y + f.h - pad - y, 40 * S);
      ImGui::SetCursorScreenPos(ImVec2(f.o.x + 1, y));
      ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
      ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
      const char* const pages[5] = {"##sl_color_rows", "##sl_color_auto", "##sl_color_effects", "##sl_color_share", "##sl_color_bake"};  // Each has its own scroll
      ImGui::BeginChild(pages[(std::min)((std::max)(g_tab, 0), 4)], ImVec2(f.w - 2, list_h), false);
      ImDrawList* cl = ImGui::GetWindowDrawList();
      const float cw = ImGui::GetContentRegionAvail().x;
      if (g_tab == 0) {
        static bool details_open = false;  // The small regions' group. While it's closed, the list stays the outfit's parts
        // Small details sit under the big region they're on, and its row's "+3" opens them. The rest (none near)
        // go in the group.
        static std::vector<char> kids_open;
        static int kids_part = -2, kids_r = -1;
        if (kids_part != v.part || kids_r != R) { kids_open.assign(static_cast<size_t>(R), 0); kids_part = v.part; kids_r = R; }
        auto is_row = [&](int k) { return k >= 0 && k < R && v.regions[static_cast<size_t>(k)].listed && !v.regions[static_cast<size_t>(k)].detail; };
        std::vector<std::vector<int>> kids(static_cast<size_t>(R));
        std::vector<int> loose;  // small ones with no row to sit under
        for (int i = 0; i < R; i++) {
          const auto& r = v.regions[static_cast<size_t>(i)];
          if (!r.detail) continue;
          if (r.listed && is_row(r.parent)) kids[static_cast<size_t>(r.parent)].push_back(i);
          else loose.push_back(i);
        }
        static std::vector<int> sel_seen;
        if (sel_seen != g_sel) {  // A newly selected one shows by opening its row once. Closing it again keeps it closed
          sel_seen = g_sel;
          for (int s : g_sel)
            if (s >= 0 && s < R && v.regions[static_cast<size_t>(s)].detail && is_row(v.regions[static_cast<size_t>(s)].parent))
              kids_open[static_cast<size_t>(v.regions[static_cast<size_t>(s)].parent)] = 1;
        }
        std::vector<std::pair<int, int>> order;  // (region, depth): the rows in list order
        for (int i = 0; i < R; i++) {
          if (v.regions[static_cast<size_t>(i)].detail) continue;
          order.push_back({i, 0});
          for (int k : kids[static_cast<size_t>(i)])  // Open, or the selected one, which always shows
            if (kids_open[static_cast<size_t>(i)] || std::find(g_sel.begin(), g_sel.end(), k) != g_sel.end()) order.push_back({k, 1});
        }
        for (int k : loose) order.push_back({k, 0});
        int details = 0;
        bool header_done = false;
        for (int k : loose) details += v.regions[static_cast<size_t>(k)].listed;
        for (const auto& [i, depth] : order) {
          const auto& r = v.regions[static_cast<size_t>(i)];
          const bool chosen = std::find(g_sel.begin(), g_sel.end(), i) != g_sel.end();
          if (!r.listed && !chosen) continue;  // A twin is listed under its row, and one off the model has nothing to show
          if (r.detail && depth == 0 && !header_done && details > 0) {  // the group's header, before the first loose small region
            header_done = true;
            ImGui::PushID("details");
            const ImVec2 ha = ImGui::GetCursorScreenPos(), hb(ha.x + cw, ha.y + row);
            const bool hit = ImGui::InvisibleButton("hdr", ImVec2(cw, row));
            if (hit) details_open = !details_open;
            const std::string h = std::string(details_open ? "[-]  " : "[+]  ") + "Small details (" + std::to_string(details) + ")";
            text(cl, f_body, ImVec2(ha.x + pad, (ha.y + hb.y) * 0.5f - f_body->FontSize * 0.5f), ImGui::IsItemHovered() ? TEXT : LABEL, h);
            ImGui::PopID();
          }
          if (r.detail && depth == 0 && !details_open && !chosen) continue;
          ImGui::PushID(i);
          const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + cw, a.y + row);
          const auto it = std::find(g_sel.begin(), g_sel.end(), i);
          const bool sel = it != g_sel.end(), picking = g_pick_region == i;
          if (sel) {
            cl->AddRectFilled(a, b, CURSOR);
            cl->AddRectFilled(a, ImVec2(a.x + 3 * S, b.y), RED);
          }
          const float cy = a.y + row * 0.5f, sx = a.x + pad + depth * 18 * S;  // Indented when it's under its region
          if (depth > 0) cl->AddLine(ImVec2(a.x + pad + 6 * S, a.y), ImVec2(a.x + pad + 6 * S, cy), BORDER);
          const ImVec2 la(sx, cy - sw * 0.5f), lb(sx + sw, cy + sw * 0.5f), sa(lb.x + 4 * S, la.y), sb(sa.x + sw, lb.y);
          const uint32_t lit = picking && g_pick_which == 0 ? pack_rgb(g_pick_col) : r.lit;
          const uint32_t shadow = picking && g_pick_which == 1 ? pack_rgb(g_pick_col) : r.shadow;
          cl->AddRectFilled(la, lb, rgb_u32(lit));
          cl->AddRect(la, lb, picking && g_pick_which == 0 ? RED : BORDER);
          cl->AddRectFilled(sa, sb, rgb_u32(shadow));
          cl->AddRect(sa, sb, picking && g_pick_which == 1 ? RED : BORDER);
          float lx = sb.x + 10 * S;
          if (sel && g_sel.size() > 1) {  // the order a gradient goes in
            const std::string ord = std::to_string(it - g_sel.begin() + 1);
            text(cl, f_small, ImVec2(lx, cy - f_small->FontSize * 0.5f), RED_TEXT, ord);
            lx += text_w(f_small, "00") + 4 * S;
          }
          const std::string label = region_label(v, i);
          const bool renaming = g_rename_region == i;
          if (!renaming) text(cl, f_body, ImVec2(lx, cy - f_body->FontSize * 0.5f), sel ? TEXT : r.unseen ? DIM : LABEL, label);  // Unseen means the camera can't show it
          float name_w = renaming ? 200 * S : std::max(text_w(f_body, label.c_str()), text_w(f_body, "Region 000"));
          if (!renaming && r.twins > 1) {  // Regions that look the same in this color share one row and are edited together
            const std::string tw = "x" + std::to_string(r.twins);
            const float tx = lx + name_w + 6 * S;
            text(cl, f_small, ImVec2(tx, cy - f_small->FontSize * 0.5f), DIM, tw);
            if (ImGui::IsMouseHoveringRect(ImVec2(tx, a.y), ImVec2(tx + text_w(f_small, tw.c_str()), b.y)))
              ImGui::SetTooltip("%d regions share this color", r.twins);
            name_w += 6 * S + text_w(f_small, tw.c_str());
          }
          if (!renaming && !r.only_on.empty()) {  // Only a hidden model draws it, like Ky's Dragon Install glow
            const std::string tg = r.only_on + " only";
            const float tx = lx + name_w + 6 * S;
            text(cl, f_small, ImVec2(tx, cy - f_small->FontSize * 0.5f), DIM, tg);
            if (ImGui::IsMouseHoveringRect(ImVec2(tx, a.y), ImVec2(tx + text_w(f_small, tg.c_str()), b.y)))
              ImGui::SetTooltip("Only the %s model draws this (seen during its move)", r.only_on.c_str());
            name_w += 6 * S + text_w(f_small, tg.c_str());
          }
          text(cl, f_small, ImVec2(lx + name_w + 8 * S, cy - f_small->FontSize * 0.5f), DIM, hex_of(lit));
          char pct[16];
          snprintf(pct, sizeof(pct), "%.1f%%", r.share * 100.0f);
          float rx = a.x + cw - pad;
          if (r.edited) {
            const float bw = small_button_w("Reset");
            if (small_button(cl, "reset", "Reset", ImVec2(rx - bw, cy - CHIP_H * S * 0.5f))) recolor_test::reset(i);
            rx -= bw + 8 * S;
          }
          if (depth == 0 && !r.detail && !kids[static_cast<size_t>(i)].empty()) {  // Open or close its small details here
            const int nk = static_cast<int>(kids[static_cast<size_t>(i)].size());
            const std::string kl = (kids_open[static_cast<size_t>(i)] ? "- " : "+ ") + std::to_string(nk);
            const float kw = small_button_w(kl.c_str());
            if (small_button(cl, "kids", kl.c_str(), ImVec2(rx - kw, cy - CHIP_H * S * 0.5f), true, kids_open[static_cast<size_t>(i)] != 0))
              kids_open[static_cast<size_t>(i)] = !kids_open[static_cast<size_t>(i)];
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s %d detail%s", kids_open[static_cast<size_t>(i)] ? "Hide" : "Show", nk, nk == 1 ? "" : "s");
            rx -= kw + 8 * S;
          }
          // A swatch opens (or closes) its picker; the rest of the row selects the region.
          for (int which = 0; which < 2; which++) {
            if (!clicked_rect(which ? "sw_s" : "sw_l", which ? sa : la, which ? sb : lb)) continue;
            if (picking && g_pick_which == which) { g_pick_region = -1; continue; }
            g_pick_region = i;
            g_pick_which = which;
            unpack_rgb(which ? r.shadow : r.lit, g_pick_col);
          }
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(ImVec2(lx, a.y), ImVec2(lx + name_w, b.y)) && !renaming) {
            g_rename_region = i;
            g_rename_focus = true;
            snprintf(g_rename_buf, sizeof(g_rename_buf), "%s", r.name.c_str());
          }
          const bool row_hit = clicked_rect("row", ImVec2(sb.x + 4 * S, a.y), ImVec2(rx - text_w(f_small, pct) - 6 * S, b.y));
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s of the texture", pct);
          if (row_hit) {
            if (io.KeyShift && g_sel_anchor >= 0) {
              std::vector<int> range;  // In the list's order, where a region's details sit under it
              int from = -1, to = -1;
              for (int k = 0; k < static_cast<int>(order.size()); k++) {
                if (order[static_cast<size_t>(k)].first == g_sel_anchor) from = k;
                if (order[static_cast<size_t>(k)].first == i) to = k;
              }
              if (from < 0) from = to;
              for (int k = std::min(from, to); k >= 0 && k <= std::max(from, to); k++) {
                const int rg = order[static_cast<size_t>(k)].first;
                if (v.regions[static_cast<size_t>(rg)].listed) range.push_back(rg);
              }
              if (to < from) std::reverse(range.begin(), range.end());
              set_selection(range);
            } else if (io.KeyCtrl) {
              std::vector<int> s = g_sel;
              if (sel) s.erase(std::find(s.begin(), s.end(), i)); else s.push_back(i);
              set_selection(s);
              g_sel_anchor = i;
            } else {
              set_selection(sel && g_sel.size() == 1 ? std::vector<int>{} : std::vector<int>{i});
              g_sel_anchor = i;
            }
          }
          push_widget_style();
          if (ImGui::BeginPopupContextItem("ctx")) {
            const std::vector<int> targets = sel ? g_sel : std::vector<int>{i};
            if (ImGui::MenuItem("Copy colors")) { g_clip_set = true; g_clip_lit = r.lit; g_clip_shadow = r.shadow; }
            if (ImGui::MenuItem("Group same colors", nullptr, v.group_same)) recolor_test::groupSame(!v.group_same);
            if (ImGui::MenuItem(targets.size() > 1 ? "Paste colors to the selection" : "Paste colors", nullptr, false, g_clip_set)) paste_to(targets);
            if (ImGui::MenuItem(targets.size() > 1 ? "Reset the selection" : "Reset", nullptr, false, true))
              for (int t : targets) recolor_test::reset(t);
            if (ImGui::MenuItem("Rename")) {
              g_rename_region = i;
              g_rename_focus = true;
              snprintf(g_rename_buf, sizeof(g_rename_buf), "%s", r.name.c_str());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remap regions", nullptr, false, !v.exporting)) recolor_test::remap();
            if (ImGui::MenuItem("Reload colors", nullptr, false, !v.exporting)) recolor_test::readColorsAgain();
            ImGui::EndPopup();
          }
          if (renaming) {  // Typed in place of the label. Enter keeps it, Escape leaves it as it was, empty clears it
            ImGui::SetCursorScreenPos(ImVec2(lx, cy - ImGui::GetFrameHeight() * 0.5f));
            ImGui::SetNextItemWidth(200 * S);
            if (g_rename_focus) { ImGui::SetKeyboardFocusHere(); g_rename_focus = false; }
            const bool done = ImGui::InputTextWithHint("##rename", ("Region " + std::to_string(i + 1)).c_str(), g_rename_buf, sizeof(g_rename_buf),
                                                       ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) g_rename_region = -1;
            else if (done || ImGui::IsItemDeactivated()) { recolor_test::renameRegion(i, g_rename_buf); g_rename_region = -1; }
          }
          pop_widget_style();
          cl->AddLine(ImVec2(a.x + pad, b.y - 1), ImVec2(b.x - pad, b.y - 1), DIVIDER);
          ImGui::SetCursorScreenPos(a);
          ImGui::Dummy(ImVec2(cw, row));
          if (picking) {
            push_widget_style();
            ImGui::SetCursorScreenPos(ImVec2(sx, ImGui::GetCursorScreenPos().y + 4 * S));
            ImGui::SetNextItemWidth(std::min(cw - 2 * pad, 280 * S));
            ImGui::ColorPicker3("##pick", g_pick_col, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview |
                                                      ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_DisplayHex);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
              if (g_pick_which == 0) recolor_test::setLit(i, pack_rgb(g_pick_col));
              else recolor_test::setShadow(i, pack_rgb(g_pick_col));
            }
            pop_widget_style();
            ImGui::Dummy(ImVec2(cw, 8 * S));
          }
          ImGui::PopID();
        }
        if (!v.material_colors.empty()) {  // colors the materials draw with besides the texture (glow, edges)
          ImGui::Dummy(ImVec2(cw, 8 * S));
          { const ImVec2 at = ImGui::GetCursorScreenPos(); text(cl, f_small, ImVec2(at.x + pad, at.y), MUTED, "Material colors"); }
          ImGui::Dummy(ImVec2(cw, f_small->FontSize + 6 * S));
          push_widget_style();
          for (int i = 0; i < static_cast<int>(v.material_colors.size()); i++) {
            const auto& mc = v.material_colors[static_cast<size_t>(i)];
            ImGui::PushID(5000 + i);
            { const ImVec2 at = ImGui::GetCursorScreenPos(); ImGui::SetCursorScreenPos(ImVec2(at.x + pad, at.y)); }
            float c[4] = {mc.rgba[0], mc.rgba[1], mc.rgba[2], mc.rgba[3]};
            if (ImGui::ColorEdit4(mc.label.c_str(), c, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs |
                                                       ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_PickerHueWheel))
              recolor_test::setMaterialColor(i, c);
            if (mc.label == "Lines (decal)" && ImGui::IsItemHovered()) ImGui::SetTooltip("The decal lines' color shows in training only for now.");
            if (mc.edited) {
              ImGui::SameLine();
              if (ImGui::SmallButton("Reset")) recolor_test::resetMaterialColor(i);
            }
            ImGui::PopID();
          }
          pop_widget_style();
          ImGui::Dummy(ImVec2(cw, 6 * S));
        }
      } else if (g_tab == 1) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        float ay = at.y + 4 * S;
        draw_auto(cl, at.x + pad, at.x + cw - pad, ay, v);
        ImGui::SetCursorScreenPos(ImVec2(at.x, ay));
        ImGui::Dummy(ImVec2(cw, 1));  // The page's height, for its scroll
      } else if (g_tab == 2) {
        if (!v.effect_colors.empty()) {  // The color slot's particle colors (fire ...). Hue only, the game's brightness kept
          ImGui::Dummy(ImVec2(cw, 8 * S));
          { const ImVec2 at = ImGui::GetCursorScreenPos(); text(cl, f_small, ImVec2(at.x + pad, at.y), MUTED, "Effect colors"); }
          ImGui::Dummy(ImVec2(cw, f_small->FontSize + 6 * S));
          push_widget_style();
          for (int i = 0; i < static_cast<int>(v.effect_colors.size()); i++) {
            const auto& ec = v.effect_colors[static_cast<size_t>(i)];
            ImGui::PushID(6000 + i);
            { const ImVec2 at = ImGui::GetCursorScreenPos(); ImGui::SetCursorScreenPos(ImVec2(at.x + pad, at.y)); }
            float c[3] = {ec.rgb[0], ec.rgb[1], ec.rgb[2]};
            if (ImGui::ColorEdit3(ec.label.c_str(), c, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel))
              recolor_test::setEffectColor(i, c, v.fx_slot);
            if ((!ec.uses.empty() || !ec.used_by.empty()) && ImGui::IsItemHovered())  // What it colors: the game's moves, and the effects taking it by name
              ImGui::SetTooltip("%s%s%s%s", ec.uses.c_str(), !ec.uses.empty() && !ec.used_by.empty() ? "\n" : "", ec.used_by.empty() ? "" : "Used by: ", ec.used_by.c_str());
            if (ec.edited) {
              ImGui::SameLine();
              if (ImGui::SmallButton("Reset")) recolor_test::resetEffectColor(i, v.fx_slot);
            }
            const int own = static_cast<int>(std::count_if(ec.per.begin(), ec.per.end(), [](const recolor_test::EffectUse& p) { return p.edited; }));
            if (ec.per.size() > 1 || own > 0) {  // Each effect taking it can have its own color over this one. A saved one shows even alone
              { const ImVec2 at = ImGui::GetCursorScreenPos(); ImGui::SetCursorScreenPos(ImVec2(at.x + pad + 14 * S, at.y)); }
              const std::string head = "Each effect (" + std::to_string(ec.per.size()) + (own ? ", " + std::to_string(own) + " changed" : std::string()) + ")###per";
              if (ImGui::TreeNodeEx(head.c_str(), own ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
                for (int k = 0; k < static_cast<int>(ec.per.size()); k++) {
                  const auto& p = ec.per[static_cast<size_t>(k)];
                  ImGui::PushID(k);
                  float pc[3] = {p.rgb[0], p.rgb[1], p.rgb[2]};
                  const std::string nm = (p.says.empty() ? p.effect : p.says) + "###c";
                  if (ImGui::ColorEdit3(nm.c_str(), pc, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel))
                    recolor_test::setEffectColorFor(i, p.effect, pc, v.fx_slot);
                  if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\n%s", p.effect.c_str(), p.edited ? "Its own color" : "Takes the color above (pick one to give it its own)");
                  if (p.edited) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Reset")) recolor_test::resetEffectColorFor(i, p.effect, v.fx_slot);
                  }
                  ImGui::PopID();
                }
                ImGui::TreePop();
              }
            }
            ImGui::PopID();
          }
          pop_widget_style();
          ImGui::Dummy(ImVec2(cw, 6 * S));
        }
        {  // Effects that play during a capture, kept per color slot along with their brightness
          ImGui::Dummy(ImVec2(cw, 8 * S));
          push_widget_style();
          { const ImVec2 at = ImGui::GetCursorScreenPos(); ImGui::SetCursorScreenPos(ImVec2(at.x + pad, at.y)); }
          if (ImGui::SmallButton(v.capturing ? "Capturing... do the move" : "Capture effects (5 s)") && !v.capturing) recolor_test::captureEffects();
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Lists effects played in the next 5 s. Changes are for this color slot, and show in training only for now.");
          ImGui::SameLine();
          if (ImGui::SmallButton("List all effects")) recolor_test::listAllEffects();
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every loaded effect of this character");
          ImGui::SameLine();
          {  // To find which effect draws something, hide them all. If it's still there, it isn't an effect.
            const bool all = std::find(v.hidden_effects.begin(), v.hidden_effects.end(), "*") != v.hidden_effects.end();
            if (ImGui::SmallButton(all ? "Show all effects" : "Hide all effects")) recolor_test::hideEffect("*", !all);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hide every effect, to find which one draws something");
          }
          {
            const bool on = !v.on_screen.empty();
            ImGui::SameLine();
            if (ImGui::SmallButton(on ? "Whole list" : "What's on screen")) recolor_test::onScreen(!on);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Only effects playing now");
            if (on) { ImGui::SameLine(); ImGui::TextDisabled("(%d playing)", static_cast<int>(v.on_screen.size())); }
          }
          if (!v.hidden_effects.empty()) {  // So a hidden effect isn't mistaken for a missing one
            { const ImVec2 at = ImGui::GetCursorScreenPos(); ImGui::SetCursorScreenPos(ImVec2(at.x + pad, at.y)); }
            const bool all = std::find(v.hidden_effects.begin(), v.hidden_effects.end(), "*") != v.hidden_effects.end();
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f), all ? "Every effect is hidden" : "%d effects hidden", static_cast<int>(v.hidden_effects.size()));
            ImGui::SameLine();
            if (ImGui::SmallButton("Show them")) for (const auto& h : v.hidden_effects) recolor_test::hideEffect(h, false);
          }
          static char cap_find[48] = "";
          { const ImVec2 at = ImGui::GetCursorScreenPos(); ImGui::SetCursorScreenPos(ImVec2(at.x + pad, at.y)); }
          ImGui::SetNextItemWidth((std::max)(cw - 2 * pad, 60.0f));
          ImGui::InputTextWithHint("##cap_find", "Search effects", cap_find, sizeof(cap_find));
          std::string find_lc = cap_find;
          for (auto& ch : find_lc) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
          // One header per effect, closed by default since a capture can list dozens of emitters. A click opens it.
          static std::map<std::string, bool> cap_open;  // Which effects are open
          std::map<std::string, std::pair<int, bool>> per;  // Effect -> number of colors, and whether any were edited
          for (const auto& cc : v.captured) { auto& e = per[cc.effect]; e.first++; e.second |= cc.edited; }
          for (const auto& [eff, pat] : v.captured_named) per[eff];  // Effects with only palette colors get a group too
          for (const auto& m : v.captured_mats) per[m.effect].second |= m.edited;  // And so do effects with only material colors
          auto shown = [&](const std::string& eff) {
            if (!v.on_screen.empty() && std::find(v.on_screen.begin(), v.on_screen.end(), eff) == v.on_screen.end()) return false;
            if (find_lc.empty()) return true;
            std::string n = eff;
            for (auto& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return n.find(find_lc) != std::string::npos;
          };
          auto mat_lines = [&](const std::string& eff) {  // The effect's material colors for this slot
            for (int k = 0; k < static_cast<int>(v.captured_mats.size()); k++) {
              const auto& m = v.captured_mats[static_cast<size_t>(k)];
              if (m.effect != eff) continue;
              ImGui::PushID(9000 + k);
              { const ImVec2 at = ImGui::GetCursorScreenPos(); ImGui::SetCursorScreenPos(ImVec2(at.x + pad + 8 * S, at.y)); }
              float c[3] = {m.rgb[0], m.rgb[1], m.rgb[2]};
              if (ImGui::ColorEdit3(m.label.c_str(), c, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel)) recolor_test::setCapturedMatColor(k, c, v.cap_gen);
              if (ImGui::IsItemHovered()) ImGui::SetTooltip("The effect's material color (every effect using this material)");
              if (m.edited) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset")) recolor_test::resetCapturedMatColor(k, v.cap_gen);
              }
              ImGui::PopID();
            }
          };
          auto named_lines = [&](const std::string& eff) {  // Lines like "uses fire_dsl01" name the slot's palette color, which is set in Effect colors
            for (const auto& [e2, pat] : v.captured_named)
              if (e2 == eff) {
                { const ImVec2 at = ImGui::GetCursorScreenPos(); ImGui::SetCursorScreenPos(ImVec2(at.x + pad + 8 * S, at.y)); }
                ImGui::TextDisabled("Uses %s", pat.c_str());
              }
          };
          std::string last;
          for (int i = 0; i < static_cast<int>(v.captured.size()); i++) {
            const auto& cc = v.captured[static_cast<size_t>(i)];
            if (!shown(cc.effect)) continue;
            if (cc.effect != last) {
              if (!last.empty() && cap_open[last]) { mat_lines(last); named_lines(last); }
              last = cc.effect;
              ImGui::Dummy(ImVec2(cw, 2 * S));
              const bool open = cap_open[cc.effect];
              const auto& e = per[cc.effect];
              const std::string h = std::string(open ? "[-]  " : "[+]  ") + cc.effect + "  (" + std::to_string(e.first) + (e.first == 1 ? " color" : " colors") +
                                    (e.second ? ", edited)" : ")");
              const ImVec2 at = ImGui::GetCursorScreenPos();
              ImGui::PushID(20000 + i);
              if (ImGui::InvisibleButton("cap_hdr", ImVec2((std::max)(pad + text_w(f_small, h) + 4 * S, 1.0f), f_small->FontSize + 4 * S))) cap_open[cc.effect] = !open;  // Only as wide as its text, so the "Set all" swatch beside it gets its own clicks
              const bool hov = ImGui::IsItemHovered();
              ImGui::PopID();
              text(cl, f_small, ImVec2(at.x + pad, at.y + 2 * S), hov ? TEXT : e.second ? LABEL : MUTED, h);
              {  // One pick sets every color of this effect (an effect can have many emitters)
                const ImVec2 after = ImGui::GetCursorScreenPos();
                ImGui::SetCursorScreenPos(ImVec2(at.x + pad + text_w(f_small, h) + 10 * S, at.y + 1 * S));
                float all[3] = {cc.rgb[0], cc.rgb[1], cc.rgb[2]};
                ImGui::PushID(20000 + i);
                if (ImGui::ColorEdit3("##cap_all", all, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_PickerHueWheel))
                  for (int k = 0; k < static_cast<int>(v.captured.size()); k++) {
                    const auto& ck = v.captured[static_cast<size_t>(k)];  // Not the color scales. They multiply the colors, so a tint there would tint them twice.
                    if (ck.effect == cc.effect && ck.label.find("color scale") == std::string::npos) recolor_test::setCapturedColor(k, all, v.cap_gen);
                  }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Set all colors");
                ImGui::PopID();
                ImGui::SameLine();
                ImGui::TextDisabled("Set all");
                {
                  const bool hid = std::find(v.hidden_effects.begin(), v.hidden_effects.end(), cc.effect) != v.hidden_effects.end();
                  ImGui::SameLine();
                  ImGui::PushID(20000 + i);
                  if (ImGui::SmallButton(hid ? "Show" : "Hide")) recolor_test::hideEffect(cc.effect, !hid);
                  ImGui::PopID();
                }
                ImGui::SetCursorScreenPos(after);
              }
            }
            if (!cap_open[cc.effect]) continue;
            ImGui::PushID(7000 + i);
            { const ImVec2 at = ImGui::GetCursorScreenPos(); ImGui::SetCursorScreenPos(ImVec2(at.x + pad + 8 * S, at.y)); }
            float c[3] = {cc.rgb[0], cc.rgb[1], cc.rgb[2]};
            if (ImGui::ColorEdit3(cc.label.c_str(), c, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel)) recolor_test::setCapturedColor(i, c, v.cap_gen);
            if (cc.edited) {
              ImGui::SameLine();
              if (ImGui::SmallButton("Reset")) recolor_test::resetCapturedColor(i, v.cap_gen);
            }
            ImGui::PopID();
          }
          if (!last.empty() && cap_open[last]) { mat_lines(last); named_lines(last); }
          {  // Effects whose colors all come from the palette, with no rows of their own
            std::vector<std::string> only;
            for (const auto& [eff, pat] : v.captured_named)  // per[] holds every effect with rows, counted above
              if (std::find(only.begin(), only.end(), eff) == only.end() && per[eff].first == 0) only.push_back(eff);
            for (const auto& m : v.captured_mats)
              if (std::find(only.begin(), only.end(), m.effect) == only.end() && per[m.effect].first == 0) only.push_back(m.effect);
            for (const std::string& eff : only) {
              if (!shown(eff)) continue;
              ImGui::Dummy(ImVec2(cw, 2 * S));
              const bool open = cap_open[eff];
              const std::string h = std::string(open ? "[-]  " : "[+]  ") + eff;
              const ImVec2 at = ImGui::GetCursorScreenPos();
              ImGui::PushID(eff.c_str());
              if (ImGui::InvisibleButton("cap_hdr", ImVec2((std::max)(pad + text_w(f_small, h) + 4 * S, 1.0f), f_small->FontSize + 4 * S))) cap_open[eff] = !open;
              const bool hov = ImGui::IsItemHovered();
              ImGui::PopID();
              text(cl, f_small, ImVec2(at.x + pad, at.y + 2 * S), hov ? TEXT : MUTED, h);
              if (open) { mat_lines(eff); named_lines(eff); }
            }
          }
          pop_widget_style();
        }
      } else if (g_tab == 4) {  // Bake: saved recolors written into paks, and the list of baked slots
        const ImVec2 at0 = ImGui::GetCursorScreenPos();
        float by = at0.y + 10 * S;
        const float bx0 = at0.x + pad, bx1 = at0.x + cw - pad;
        auto line = [&](const std::string& s, ImU32 col) {  // (wrapped, at a fixed x)
          const float h = f_small->CalcTextSizeA(f_small->FontSize, FLT_MAX, bx1 - bx0, s.c_str()).y;
          cl->AddText(f_small, f_small->FontSize, ImVec2(bx0, by), col, s.c_str(), nullptr, bx1 - bx0);
          by += h + 4 * S;
        };
        line("Outside training, a recolor only shows once it's baked. Bake again after changing it.", MUTED);
        by += 4 * S;
        const int left = recolor_test::bakeQueueLeft();
        ImGui::SetCursorScreenPos(ImVec2(bx0, by));
        push_widget_style();
        ImGui::BeginDisabled(left > 0 || v.unsaved);
        if (ImGui::SmallButton("Bake this slot")) recolor_test::bakeRecolor();
        ImGui::SameLine();
        static std::vector<recolor_test::BakeCandidate> cands;
        static std::vector<char> ticked;
        if (ImGui::SmallButton("Bake saved slots...")) {  // (a list first: nothing is baked until it's ticked and Bake is pressed)
          cands = recolor_test::bakeCandidates();
          ticked.assign(cands.size(), 0);
          for (size_t i = 0; i < cands.size(); i++) ticked[i] = (cands[i].chr + "/" + cands[i].color) == v.fx_slot;  // (the slot being edited)
          ImGui::OpenPopup("bake_pick");
        }
        ImGui::EndDisabled();
        if (ImGui::BeginPopup("bake_pick")) {
          ImGui::TextUnformatted("Tick the slots to bake, then press Bake ticked.");
          ImGui::TextDisabled("Only the slot you're editing starts ticked.");
          if (cands.empty()) ImGui::TextDisabled("No saved recolors found.");
          const float x_slot = 30 * S, x_what = x_slot + 120 * S, x_note = x_what + 260 * S;
          if (ImGui::BeginChild("bake_pick_list", ImVec2(x_note + 380 * S, (std::min)(14.0f, static_cast<float>(cands.size()) + 0.5f) * ImGui::GetFrameHeightWithSpacing()), true)) {
            for (size_t i = 0; i < cands.size(); i++) {
              const auto& cd = cands[i];
              ImGui::PushID(static_cast<int>(i));
              const float x0 = ImGui::GetCursorPosX();
              bool on = ticked[i] != 0;
              if (ImGui::Checkbox("##t", &on)) ticked[i] = on;
              ImGui::SameLine(x0 + x_slot);
              ImGui::AlignTextToFramePadding();
              ImGui::Text("%s %s", cd.chr.c_str(), recolor_test::slotLabel(cd.color).c_str());
              ImGui::SameLine(x0 + x_what);
              const std::string what = cd.parts + (cd.effects ? std::string(cd.parts.empty() ? "" : " + ") + "effect colors" : std::string());
              ImGui::TextUnformatted(what.c_str());
              ImGui::SameLine(x0 + x_note);
              if (!cd.missing.empty()) ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1), "can't bake %s yet: open %s in the editor once", cd.missing.c_str(), cd.chr.c_str());
              else ImGui::TextDisabled("ready");
              ImGui::PopID();
            }
          }
          ImGui::EndChild();
          std::vector<std::string> pick;
          for (size_t i = 0; i < cands.size(); i++) if (ticked[i]) pick.push_back(cands[i].chr + "/" + cands[i].color);
          ImGui::BeginDisabled(pick.empty());
          if (ImGui::Button(("Bake ticked (" + std::to_string(pick.size()) + ")").c_str())) { recolor_test::bakeSlots(pick); ImGui::CloseCurrentPopup(); }
          ImGui::EndDisabled();
          ImGui::SameLine();
          if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
          ImGui::EndPopup();
        }
        pop_widget_style();
        by += CHIP_H * S + 6 * S;
        line(left > 0 ? std::to_string(left) + (left == 1 ? " slot" : " slots") + " to bake..." : v.unsaved ? "Save first." : " ", left > 0 ? TEXT : DIM);
        line("Bake saved slots... lists your saved recolors. Only the ones you tick are baked.", DIM);
        by += 6 * S;
        const std::vector<recolor_test::BakedSlot> baked = recolor_test::bakedSlots();
        line("Baked slots (" + std::to_string(baked.size()) + ")", LABEL);
        // a table at fixed columns: character, slot, when, size, the button
        const float c_chr = bx0, c_slot = bx0 + text_w(f_small, "ABA_C02") + 18 * S, c_when = c_slot + text_w(f_small, "Color 99") + 18 * S,
                    c_size = c_when + text_w(f_small, "2026-11-07 17:44") + 18 * S, c_btn = c_size + text_w(f_small, "999.9 MB") + 18 * S;
        const float rh = CHIP_H * S + 4 * S;
        for (size_t i = 0; i < baked.size(); i++) {
          const auto& b = baked[i];
          const float ty = by + (CHIP_H * S - f_small->FontSize) * 0.5f;
          text(cl, f_small, ImVec2(c_chr, ty), TEXT, b.chr);
          text(cl, f_small, ImVec2(c_slot, ty), TEXT, recolor_test::slotLabel(b.color));
          text(cl, f_small, ImVec2(c_when, ty), MUTED, b.when);
          char mb[24];
          snprintf(mb, sizeof mb, "%.1f MB", b.mb);
          text(cl, f_small, ImVec2(c_size - text_w(f_small, mb) + text_w(f_small, "999.9 MB"), ty), MUTED, b.unbaked ? std::string() : std::string(mb));
          if (b.unbaked) text(cl, f_small, ImVec2(c_btn, ty), DIM, "unbaked (its files go at the next start)");
          else if (b.stale) text(cl, f_small, ImVec2(c_btn, ty), DIM, "game updated: bake again");
          else {
            ImGui::PushID(static_cast<int>(9000 + i));
            if (small_button(cl, "unbake", "Unbake", ImVec2(c_btn, by), left == 0)) recolor_test::unbake(b.chr, b.color);
            ImGui::PopID();
          }
          by += rh;
        }
        if (baked.empty()) line("None yet.", DIM);
        ImGui::SetCursorScreenPos(ImVec2(at0.x, by + 6 * S));
        ImGui::Dummy(ImVec2(cw, 1));  // The page's height, for its scroll
      } else {
        {  // The slot's whole recolor, saved to a file, loaded from one, or copied onto another slot
          ImGui::Dummy(ImVec2(cw, 10 * S));
          { const ImVec2 at = ImGui::GetCursorScreenPos(); text(cl, f_small, ImVec2(at.x + pad, at.y), MUTED, "Recolor \xC2\xB7 " + recolor_test::slotLabel(v.slot)); }
          ImGui::Dummy(ImVec2(cw, f_small->FontSize + 6 * S));
          push_widget_style();
          { const ImVec2 at = ImGui::GetCursorScreenPos(); ImGui::SetCursorScreenPos(ImVec2(at.x + pad, at.y)); }
          if (ImGui::SmallButton("Export recolor")) recolor_test::exportRecolor();
          ImGui::SameLine();
          if (ImGui::SmallButton("Import recolor...")) { recolor_test::listRecolors(); ImGui::OpenPopup("import_recolor"); }
          ImGui::SameLine();
          if (ImGui::SmallButton("Copy to slot...")) { recolor_test::listRecolors(); ImGui::OpenPopup("copy_slot"); }
          if (ImGui::BeginPopup("import_recolor")) {
            if (v.recolor_files.empty()) ImGui::TextDisabled("No exported recolors for this character yet.");
            for (const std::string& f : v.recolor_files)
              if (ImGui::MenuItem(f.c_str(), nullptr, false, !v.exporting)) recolor_test::importRecolor(f);
            ImGui::EndPopup();
          }
          if (ImGui::BeginPopup("copy_slot")) {
            ImGui::TextDisabled("Copy to (replaces it):");
            for (const std::string& s : v.slots)
              if (s != v.slot && ImGui::MenuItem(recolor_test::slotLabel(s).c_str(), nullptr, false, !v.exporting)) recolor_test::copyToSlot(s);
            ImGui::EndPopup();
          }
          {  // Palettes are whole recolors of this character, kept by name with an author and note (like IM's)
            static std::string listed_for;
            if (listed_for != v.title) { listed_for = v.title; recolor_test::listRecolors(); }  // List this character's recolors once
            static char p_name[64] = "", p_author[48] = "", p_note[160] = "";
            static std::string p_file;  // The palette being edited ("" for a new one)
            static bool p_info_only = false;
            ImGui::Dummy(ImVec2(cw, 4 * S));
            { const ImVec2 at = ImGui::GetCursorScreenPos(); ImGui::SetCursorScreenPos(ImVec2(at.x + pad, at.y)); }
            ImGui::TextDisabled("Palettes (%d)", static_cast<int>(v.palettes.size()));
            ImGui::SameLine();
            if (ImGui::SmallButton("Save as palette...")) {
              p_file.clear(); p_info_only = false;
              p_name[0] = 0; p_note[0] = 0;
              snprintf(p_author, sizeof(p_author), "%s", v.palette_author.c_str());
              ImGui::OpenPopup("palette_form");
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save this recolor as a palette");
            std::string del;
            bool open_form = false;  // Opened after the rows, because the form lives outside their IDs
            for (const auto& pal : v.palettes) {
              ImGui::PushID(pal.file.c_str());
              { const ImVec2 at = ImGui::GetCursorScreenPos(); ImGui::SetCursorScreenPos(ImVec2(at.x + pad + 8 * S, at.y)); }
              ImGui::TextUnformatted(pal.name.c_str());
              if (!pal.author.empty()) { ImGui::SameLine(); ImGui::TextDisabled("by %s", pal.author.c_str()); }
              if (ImGui::IsItemHovered())  // Hovering shows its note and the slot it was made on
                ImGui::SetTooltip("%s%s(made on %s)", pal.note.c_str(), pal.note.empty() ? "" : "\n", pal.from.empty() ? "?" : pal.from.c_str());
              ImGui::SameLine();
              ImGui::BeginDisabled(v.exporting);  // Disabled while an export reads the slot
              if (ImGui::SmallButton("Load")) recolor_test::loadPalette(pal.file);
              ImGui::EndDisabled();
              if (ImGui::IsItemHovered()) ImGui::SetTooltip("Load onto %s (the current one is backed up in recolors/palettes/%s/.trash)", recolor_test::slotLabel(v.slot).c_str(), v.title.substr(0, v.title.find(' ')).c_str());
              ImGui::SameLine();
              if (ImGui::SmallButton("Overwrite")) {
                p_file = pal.file; p_info_only = false;
                snprintf(p_name, sizeof(p_name), "%s", pal.name.c_str());
                snprintf(p_author, sizeof(p_author), "%s", pal.author.c_str());
                snprintf(p_note, sizeof(p_note), "%s", pal.note.c_str());
                open_form = true;
              }
              if (ImGui::IsItemHovered()) ImGui::SetTooltip("Replace it with %s's recolor as it is now", recolor_test::slotLabel(v.slot).c_str());
              ImGui::SameLine();
              if (ImGui::SmallButton("Edit")) {
                p_file = pal.file; p_info_only = true;
                snprintf(p_name, sizeof(p_name), "%s", pal.name.c_str());
                snprintf(p_author, sizeof(p_author), "%s", pal.author.c_str());
                snprintf(p_note, sizeof(p_note), "%s", pal.note.c_str());
                open_form = true;
              }
              if (ImGui::IsItemHovered()) ImGui::SetTooltip("Its name, author and note");
              ImGui::SameLine();
              if (ImGui::SmallButton("Delete")) ImGui::OpenPopup("palette_delete");
              if (ImGui::BeginPopup("palette_delete")) {
                ImGui::Text("Delete \"%s\"?", pal.name.c_str());
                ImGui::TextDisabled("It's moved to recolors/palettes/%s/.trash (move it back out to restore it).", v.title.substr(0, v.title.find(' ')).c_str());
                if (ImGui::SmallButton("Delete")) { del = pal.file; ImGui::CloseCurrentPopup(); }
                ImGui::SameLine();
                if (ImGui::SmallButton("Cancel")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
              }
              ImGui::PopID();
            }
            if (!del.empty()) recolor_test::deletePalette(del);
            if (open_form) ImGui::OpenPopup("palette_form");
            if (ImGui::BeginPopup("palette_form")) {
              ImGui::TextUnformatted(p_info_only ? "Palette details" : p_file.empty() ? "Save as a new palette" : "Overwrite this palette");
              ImGui::SetNextItemWidth(260 * S);
              ImGui::InputText("Name", p_name, sizeof(p_name));
              ImGui::SetNextItemWidth(260 * S);
              ImGui::InputText("Author", p_author, sizeof(p_author));
              ImGui::SetNextItemWidth(260 * S);
              ImGui::InputText("Note", p_note, sizeof(p_note));
              const bool can = p_name[0] != 0;
              if (!can) ImGui::BeginDisabled();
              if (ImGui::SmallButton(p_info_only ? "Save details" : "Save")) {
                if (p_info_only) recolor_test::editPalette(p_file, p_name, p_author, p_note);
                else recolor_test::savePalette(p_file, p_name, p_author, p_note);
                ImGui::CloseCurrentPopup();
              }
              if (!can) ImGui::EndDisabled();
              ImGui::SameLine();
              if (ImGui::SmallButton("Cancel")) ImGui::CloseCurrentPopup();
              ImGui::EndPopup();
            }
          }
          pop_widget_style();
          ImGui::Dummy(ImVec2(cw, 6 * S));
        }
        if (R > 0) {  // the slot's textures out, the shown part's in, and everything back
          ImGui::Dummy(ImVec2(cw, 4 * S));
          { const ImVec2 at = ImGui::GetCursorScreenPos(); text(cl, f_small, ImVec2(at.x + pad, at.y), MUTED, "Textures"); }
          ImGui::Dummy(ImVec2(cw, f_small->FontSize + 6 * S));
          push_widget_style();
          { const ImVec2 at = ImGui::GetCursorScreenPos(); ImGui::SetCursorScreenPos(ImVec2(at.x + pad, at.y)); }
          ImGui::BeginDisabled(v.exporting);  // One at a time. The status line shows how far it has got.
          if (ImGui::SmallButton("Export textures")) recolor_test::exportTextures();
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("All parts of this color, into a new dated folder. The _Lit/_Shadow PNGs open anywhere; alpha/ has the exact game copies.");
          ImGui::SameLine();
          if (ImGui::SmallButton("Import textures...")) tex_browser_open();
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("An export's <Part>_Lit/_Shadow PNGs and <Part>_material_colors.txt and effects.txt (each goes on its part), or any PNG, BMP or TIFF at the shown part's size. The regions take a picture's colors (an edited copy of this color's own export only brings what was changed in it); the rest comes in as paint.");
          ImGui::EndDisabled();
          ImGui::Dummy(ImVec2(cw, 6 * S));
          { const ImVec2 at = ImGui::GetCursorScreenPos(); ImGui::SetCursorScreenPos(ImVec2(at.x + pad, at.y)); }
          if (ImGui::SmallButton("Reset part")) { recolor_test::reset(-1); g_pick_region = -1; }
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("The shown part's region and material colors back to the game's (its paint stays)");
          ImGui::SameLine();
          if (ImGui::SmallButton("Reset whole color")) { recolor_test::resetColor(); g_pick_region = -1; }
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every part, paint and effect color of this slot back to the game's. The old recolor is kept in palettes/.trash.");
          pop_widget_style();
          ImGui::Dummy(ImVec2(cw, 6 * S));
        }
      }
      ImGui::EndChild();
      ImGui::PopStyleColor(2);
    }
    tex_browser_draw();
    {  // A bake's result, until OK. Fixed size: the lists scroll, nothing moves as they fill
      static recolor_test::BakeResult r;
      const bool up = recolor_test::bakeResult(r);
      if (up && !ImGui::IsPopupOpen("bake_result")) ImGui::OpenPopup("bake_result");
      push_widget_style();
      const float bw = 620 * S;
      ImGui::SetNextWindowSize(ImVec2(bw, 0), ImGuiCond_Always);
      ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
      if (ImGui::BeginPopupModal("bake_result", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
        ImGui::PushTextWrapPos(bw - ImGui::GetStyle().WindowPadding.x);
        ImGui::TextUnformatted(r.title.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        auto list = [&](const char* id, const char* head, const std::vector<std::string>& lines, float h) {
          ImGui::TextDisabled("%s (%d)", head, static_cast<int>(lines.size()));
          ImGui::BeginChild(id, ImVec2(0, h), true);
          if (lines.empty()) ImGui::TextDisabled("none");
          for (const std::string& l : lines) ImGui::TextWrapped("%s", l.c_str());
          ImGui::EndChild();
        };
        if (r.ok) list("bake_in", "Baked", r.baked, 170 * S);
        list("bake_out", r.ok ? "Not baked" : "Why", r.not_baked, (r.ok ? 110 : 200) * S);
        ImGui::Spacing();
        const float okw = 90 * S;
        ImGui::SetCursorPosX(bw - okw - ImGui::GetStyle().WindowPadding.x);
        if (ImGui::Button("OK", ImVec2(okw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) { recolor_test::bakeResultSeen(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
      }
      pop_widget_style();
    }
    // The editor's questions, shown when the window is collapsed too
    static bool recovered_answered = false;
    if (v.offer_recovery.empty()) recovered_answered = false;
    if (!v.offer_recovery.empty() && !recovered_answered) ImGui::OpenPopup("recovery_offer");
    push_widget_style();
    if (ImGui::BeginPopupModal("recovery_offer", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
      ImGui::Text("You have unsaved changes to %s from last time.", v.offer_recovery.c_str());
      ImGui::Spacing();
      if (ImGui::Button("Restore")) { recolor_test::restoreRecovery(true); recovered_answered = true; ImGui::CloseCurrentPopup(); }
      ImGui::SameLine();
      if (ImGui::Button("Discard")) { recolor_test::restoreRecovery(false); recovered_answered = true; ImGui::CloseCurrentPopup(); }
      ImGui::EndPopup();
    }
    pop_widget_style();
    static bool answered = false;  // An answer is on its way, but the view still says "asking" for a frame or two
    if (!v.confirm_close) answered = false;
    if (v.confirm_close && !answered) ImGui::OpenPopup("unsaved_close");
    push_widget_style();
    if (ImGui::BeginPopupModal("unsaved_close", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
      ImGui::TextUnformatted("You have unsaved changes.");
      ImGui::Spacing();
      if (ImGui::Button("Save")) { recolor_test::saveRecolor(); answered = true; ImGui::CloseCurrentPopup(); }
      ImGui::SameLine();
      if (ImGui::Button("Discard")) { recolor_test::discardAndClose(); answered = true; ImGui::CloseCurrentPopup(); }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) { recolor_test::cancelClose(); answered = true; ImGui::CloseCurrentPopup(); }
      ImGui::EndPopup();
    }
    pop_widget_style();
    // The first-time setup: the editor can't be used until it's done (the keys are held too)
    const bool setup_up = v.setup && recolor_test::setupShown() && v.offer_recovery.empty() && !v.confirm_close;  // (not over the other questions)
    if (setup_up && !ImGui::IsPopupOpen("first_setup")) ImGui::OpenPopup("first_setup");
    push_widget_style();
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("first_setup", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
      if (!setup_up) ImGui::CloseCurrentPopup();
      ImGui::TextUnformatted("One-time setup");
      ImGui::Spacing();
      ImGui::TextUnformatted("This is the first time the editor is used on this fighter, so it's scanning each of its parts.");
      ImGui::TextUnformatted("It only happens once per character. Please wait: the controls come back when it's done.");
      ImGui::Spacing();
      ImGui::Text("Now: %s", v.setup_part.c_str());
      ImGui::ProgressBar(v.mapping >= 0 ? v.mapping : 0.0f, ImVec2(-1.0f, 0.0f), v.mapping >= 0 ? "Scanning" : "Getting ready");
      ImGui::Spacing();
      if (ImGui::Button("Close editor")) recolor_test::toggle();
      ImGui::EndPopup();
    }
    pop_widget_style();
    end_window();
  }

  void draw() {
    // Outside a training or replay fight (menus, loading, character select): show nothing
    if (!modScreenActive() && !onlineVersionScreen() && !g_online_picker) { g_timeline_visible = false; return; }
    const MenuView v = ModMenu::instance().view();
    apply_theme(v);
    if (!modScreenActive()) {  // outside the mod's fights: the online version window, or an online fight on it
      g_timeline_visible = false;
      if (g_online_picker) { draw_online_picker(); return; }
      if (v.version_shown) draw_version(false, v.hide_window_decorations, v.version_panel - 1, v.version_title - 1, v.version_bg);
      return;
    }
    // The game's pause menu draws under us, so while it's open the overlays stay out of its
    // way, unless our own menu is open (to move them).
    const bool behind_game_menu = g_game_menu_open && !v.showing;
    if (!behind_game_menu) {
    draw_framebar(v.showing, v.hide_window_decorations, v.bar_title - 1, v.bar_panel - 1);
    draw_damage(v.showing, v.hide_window_decorations, v.damage_panel_bg - 1, v.damage_at_game, v.damage_title - 1);
    if (v.version_shown) draw_version(v.showing, v.hide_window_decorations, v.version_panel - 1, v.version_title - 1, v.version_bg);
    draw_meters(v.showing, v.meters_right, v.meters_snap, v.hide_window_decorations, v.meters_panel - 1, v.meters_split,
                v.meters_under_damage, v.meters_title - 1);
    }
    draw_timeline(v);
    use(E_MENU);
    draw_replay_status(v);
    {  // The color editor. Once it's closed (either way), its selection goes too.
      static bool was_open = false;
      const bool open = recolor_test::isOpen();
      if (was_open && !open) forget_selection();
      was_open = open;
      if (open && !behind_game_menu) draw_colors();
      static bool solo_was = false;
      static ULONGLONG solo_since = 0;
      const bool solo_now = !open && recolor_test::soloCamera() && !behind_game_menu;
      if (solo_now && !solo_was) solo_since = GetTickCount64();  // (its help shows for a few seconds each time it turns on)
      solo_was = solo_now;
      if (solo_now) {  // the free camera without the editor: its mouse, and how it works
        ImGuiIO& cio = ImGui::GetIO();
        if (!cio.WantCaptureMouse) {
          if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) recolor_test::cameraInput(cio.MouseDelta.x, cio.MouseDelta.y, 0.0f);
          if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) recolor_test::cameraPan(cio.MouseDelta.x, cio.MouseDelta.y);
          if (cio.MouseWheel != 0.0f) recolor_test::cameraInput(0.0f, 0.0f, cio.MouseWheel);
        }
        if (GetTickCount64() - solo_since < 7000) {
          const char* help = "Free camera: left drag orbits, right drag pans, wheel zooms. With the fight frozen, WASD, Q/E and the arrows move it and R shows the whole fighter.";
          ImDrawList* fg = ImGui::GetForegroundDrawList();
          const ImVec2 sz = ImGui::CalcTextSize(help);
          const ImVec2 p((cio.DisplaySize.x - sz.x) * 0.5f, 60 * S);
          fg->AddRectFilled(ImVec2(p.x - 10 * S, p.y - 6 * S), ImVec2(p.x + sz.x + 10 * S, p.y + sz.y + 6 * S), IM_COL32(0, 0, 0, 190), 4 * S);
          fg->AddText(p, IM_COL32(255, 255, 255, 255), help);
        }
      }
      if (box_editor::isOpen() && !behind_game_menu) draw_box_editor(v);
      if (open && !behind_game_menu && g_texview) draw_texture_view();
      else recolor_test::textureViewOpen(false);
    }
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
    update_check::start();  // Looks up a newer release in the background, once
    if (const std::string nv = update_check::newer(); !nv.empty()) {  // A newer release is shown beside the name. A click opens its page.
      const std::string say = "Update available: " + nv;
      const float ux = o.x + pad + text_w(f_title, "STRIVELABS") + 14 * S, uy = o.y + (f.head - f_small->FontSize) * 0.5f;
      const ImVec2 ua(ux, o.y), ub(ux + text_w(f_small, say), o.y + f.head);
      ImGui::PushID("update");
      if (clicked_rect("upd", ua, ub)) update_check::openReleases();
      const bool hov = ImGui::IsItemHovered();
      ImGui::PopID();
      text(dl, f_small, ImVec2(ux, uy), hov ? TEXT : RED, say);
      if (hov) ImGui::SetTooltip("Opens the release page on GitHub (this version: v%s)", update_check::kVersion);
    }

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
      bool logo_drawn = false;
      if (logo) {
        const float room_h = credit_y - 6 * S - (y + 8 * S);
        const float lw = std::min(nav_w - 2 * pad, room_h / std::max(logo_aspect, 0.01f));
        if (lw > 24 * S) {
          const float lh = lw * logo_aspect;
          const ImVec2 la(o.x + (nav_w - lw) * 0.5f, credit_y - 6 * S - lh);
          dl->AddImage(logo, la, ImVec2(la.x + lw, la.y + lh), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 230));
          logo_drawn = true;
          if (ImGui::IsMouseHoveringRect(la, ImVec2(la.x + lw, la.y + lh))) ImGui::SetTooltip("Built on WistfulHopes' work");
        }
      }
      if (!logo_drawn) text(dl, f_small, ImVec2(o.x + pad, credit_y), DIM, "Built on WistfulHopes' work");

      // Page header
      const float px = o.x + nav_w;
      {
        const ImVec2 a(px, body_top + 6 * S), b(o.x + w - 1, a.y + 30 * S);
        dl->AddRectFilled(a, b, CHIP);
        dl->AddRectFilled(a, ImVec2(a.x + 3 * S, b.y), RED);
        float title_end = b.x;
        // Header button that pops the page out. The meters page has none, since its Show row is the switch.
        if (!v.tension_page && v.poppable) {
          const std::string label = popped_has(v.page) ? "Close window" : "Pop out";
          const float bw = text_w(f_small, label) + 20 * S;
          const ImVec2 ba(b.x - 6 * S - bw, a.y + 4 * S), bb(b.x - 6 * S, b.y - 4 * S);
          ImGui::PushID("hdr");
          if (clicked_rect("btn", ba, bb)) toggle_popped(v.page);
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
      const float rows_end = draw_rows(dl, v.rows, v.nav_focus ? -1 : v.cursor, v.title, px, o.x + w - 1, top, bottom, main_scroll,
                                       [&v](MenuClick c) { send(v, c); });
      if (v.bar_legend) draw_bar_legend(dl, px + pad, o.x + w - pad, rows_end + 16 * S, bottom - 4 * S, v);
      const float lx = px + pad;
      if (!v.status.empty()) {
        dl->AddLine(ImVec2(lx, bottom + 2 * S), ImVec2(o.x + w - pad, bottom + 2 * S), DIVIDER);
        text(dl, f_small, ImVec2(lx, bottom + 6 * S), MUTED, v.status);
      }
    }
    end_window();

    for (const auto& pv : v.panels) draw_panel(pv);
    draw_picker(v);
  }
}
