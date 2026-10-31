#include "damage_panel.h"
#include "combo_log.h"
#include "imgui_overlay.h"
#include "menu.h"
#include "combo_trials.h"

#include <Unreal/FText.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <string>
#include <vector>
#include <Windows.h>
#include <cstring>

using namespace RC;
using namespace RC::Unreal;

// In training the game shows, per side, two widgets inside its training HUD:
// DamageInfo1P (Damage, Combo Damage, Max Combo Damage) and DamageInfo1P_02 (five rows:
// Total Action, Attack Startup, Frame Advantage, Follow-up Window, Attack Interval);
// 2P the same. They're plain text blocks the game fills in, so reading their text
// gives exactly what the game shows, labels in the game's language included.
namespace damage_panel {
namespace {
  struct Row {
    FText* label = nullptr; FText* value = nullptr;
    UObject* label_block = nullptr; UObject* value_block = nullptr;  // for where the game draws them
  };
  struct Side {
    std::vector<UObject*> widgets;  // the side's two panels (opacity, visibility)
    std::vector<Row> damage, frames;  // the two panels' rows, shown in that order
    // Each part's visibility: the game collapses a part its training display has off,
    // and then leaves its placeholder numbers in it.
    uint8_t* damage_vis = nullptr;
    uint8_t* frames_vis = nullptr;
  };
  Side g_side[2];
  bool g_bound = false;
  int g_hidden = 0;  // sides whose game panel we hid (bit 1 = 1P, 2 = 2P)

  // The panel's text blocks live in its widget tree; they aren't fields of the panel.
  std::vector<UObject*> g_blocks;  // every text block, gathered once per lookup
  UObject* blockOf(UObject* widget, const TCHAR* name) {
    UObject* block = nullptr;
    if (auto** p = widget->GetValuePtrByPropertyNameInChain<UObject*>(name); p && *p) block = *p;
    if (!block) {
      auto** tree = widget->GetValuePtrByPropertyNameInChain<UObject*>(STR("WidgetTree"));
      if (!tree || !*tree) return nullptr;
      for (UObject* b : g_blocks)
        if (b->GetOuterPrivate() == *tree && b->GetName() == name) { block = b; break; }
    }
    return block;
  }
  FText* textIn(UObject* block) { return block ? block->GetValuePtrByPropertyNameInChain<FText>(STR("Text")) : nullptr; }
  Row rowOf(UObject* widget, const TCHAR* label, const TCHAR* value) {
    Row r;
    r.label_block = blockOf(widget, label);
    r.value_block = blockOf(widget, value);
    r.label = textIn(r.label_block);
    r.value = textIn(r.value_block);
    return r;
  }

  // Where the game draws a widget on screen, in pixels: its cached geometry, which is
  // Size (2 floats), Scale (float), then the absolute position (2 floats).
  bool screenRect(UObject* block, float out[4]) {
    if (!block) return false;
    static UFunction* fn = nullptr;
    if (!fn) fn = block->GetFunctionByNameInChain(STR("GetCachedGeometry"));
    if (!fn) return false;
    alignas(16) uint8_t buf[256] = {};
    block->ProcessEvent(fn, buf);
    float size[2], scale, pos[2];
    std::memcpy(size, buf, 8);
    std::memcpy(&scale, buf + 8, 4);
    std::memcpy(pos, buf + 12, 8);
    if (!(scale > 0.05f && scale < 20.0f) || !(size[0] > 0 && size[1] > 0) || size[1] * scale > 400) return false;
    out[0] = pos[0]; out[1] = pos[1]; out[2] = size[0] * scale; out[3] = size[1] * scale;
    return true;
  }

  void bind() {
    release();
    std::vector<UObject*> found;
    g_blocks.clear();
    UObjectGlobals::FindAllOf(STR("TextBlock"), g_blocks);
    UObjectGlobals::FindAllOf(STR("Battle_TrainingDamage_C"), found);
    UObjectGlobals::FindAllOf(STR("Battle_TrainingDamage02_C"), found);
    for (UObject* w : found) {
      // Only the panel in the running game: the blueprints keep template copies too,
      // filled with placeholder numbers, which live under the asset's own path.
      const auto full = w->GetFullName();
      if (full.find(STR("/Engine/Transient")) == StringType::npos) continue;
      if (full.find(STR("WidgetArchetype")) != StringType::npos || full.find(STR("Default__")) != StringType::npos) continue;
      const auto name = w->GetName();
      const int side = name.find(STR("2P")) != StringType::npos ? 1 : name.find(STR("1P")) != StringType::npos ? 0 : -1;
      if (side < 0) continue;
      Side& s = g_side[side];
      s.widgets.push_back(w);
      if (name.find(STR("_02")) == StringType::npos) {
        if (!s.damage.empty()) continue;  // one panel per side
        s.damage_vis = w->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Visibility"));
        const TCHAR* names[3][2] = {{STR("TXT_Damage"), STR("TXT_DamageVal")}, {STR("TXT_ComboDamage"), STR("TXT_ComboDamageVal")},
                                    {STR("TXT_MaxDamage"), STR("TXT_MaxDamageVal")}};
        for (auto& n : names) s.damage.push_back(rowOf(w, n[0], n[1]));
      } else {
        if (!s.frames.empty()) continue;
        s.frames_vis = w->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Visibility"));
        const TCHAR* names[5][2] = {{STR("TXT_01"), STR("TXT_01_Num")}, {STR("TXT_02"), STR("TXT_02_Num")}, {STR("TXT_03"), STR("TXT_03_Num")},
                                    {STR("TXT_04"), STR("TXT_04_Num")}, {STR("TXT_05"), STR("TXT_05_Num")}};
        for (auto& n : names) s.frames.push_back(rowOf(w, n[0], n[1]));
      }
    }
    g_blocks.clear();
    g_bound = !g_side[0].widgets.empty() || !g_side[1].widgets.empty();
    auto rows = [](const Side& s) {
      int n = 0;
      for (const auto* v : {&s.damage, &s.frames}) for (const Row& r : *v) n += r.label && r.value;
      return n;
    };
    std::string vis;
    for (int side = 0; side < 2; side++)
      for (UObject* w : g_side[side].widgets)
        if (auto* v = w->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Visibility"))) vis += " " + std::to_string(side + 1) + "P:" + std::to_string(*v);
    COMBO_LOG(g_bound ? "damage panel: found the game's panel, rows P1 " + std::to_string(rows(g_side[0])) + " P2 " + std::to_string(rows(g_side[1])) + ", widgets P1 " + std::to_string(g_side[0].widgets.size()) + " P2 " + std::to_string(g_side[1].widgets.size()) + ", visibility" + vis
                      : std::string("damage panel: the game's panel isn't here"));
  }

  // The frame info the game shows under the damage (Total Action, Attack Startup, Frame
  // Advantage, Follow-up Window, Attack Interval): five ints per side in training's
  // manager at +0x42A4, side * 20. The game writes them into its panel only while that
  // side's frame info is on (+0x4288), else its text keeps placeholder numbers; read
  // here, they show either way. Clamped like the game does.
  std::string frameInfo(int side, int row) {
    const uintptr_t m = combo_trials::trainingManager();
    if (!m || side < 0 || side > 1 || row < 0 || row > 4) return {};
    int v = *reinterpret_cast<const int*>(m + 0x42A4 + side * 20 + row * 4);
    const int lo = row < 3 ? -999 : 0;
    v = v < lo ? lo : v > 999 ? 999 : v;
    return std::to_string(v);
  }

  std::string utf8(const StringType& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string u(static_cast<size_t>(n), char(0));
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), u.data(), n, nullptr, nullptr);
    return u;
  }
  int g_retry = 0;  // frames until looking for the game's panel again

  // The side has something to show: any row with a value.
  bool shown(const Side& s) {
    for (const auto* rows : {&s.damage, &s.frames})
      for (const Row& r : *rows)
        if (r.value && !r.value->ToString().empty()) return true;
    return false;
  }

  void setOpacity(int side, float a) {
    for (UObject* w : g_side[side].widgets)
      if (auto* fn = w->GetFunctionByNameInChain(STR("SetRenderOpacity"))) {
        struct { float InOpacity; } p{a};
        w->ProcessEvent(fn, &p);
      }
  }
}

void update(bool round_started) {
  const int sides = ModMenu::instance().damagePanel();
  const bool on = sides != 0;
  // At each fight's start, and if the panel wasn't there yet, about every two seconds.
  // Off at a reset: unbound (a new fight's widgets are found again when it's on), once the
  // game's panel was shown again below.
  if (round_started && !on && !g_hidden) g_bound = false;
  if ((round_started && on) || (on && !g_bound && --g_retry <= 0)) { bind(); g_retry = 120; }
  if (!g_bound) return;
  // The game's panel is hidden only for the sides shown in our windows.
  const int hide = ModMenu::instance().hideGameDamagePanel() ? sides : 0;
  for (int side = 0; side < 2; side++)
    if (((hide ^ g_hidden) >> side) & 1) setOpacity(side, (hide >> side) & 1 ? 0.0f : 1.0f);
  g_hidden = hide;
  for (int side = 0; side < 2; side++) {
    std::vector<imgui_overlay::DamageRow> out;
    if (((sides >> side) & 1) && shown(g_side[side]))
      for (const auto* rows : {&g_side[side].damage, &g_side[side].frames}) {
        if (rows == &g_side[side].frames && !out.empty()) out.emplace_back();  // the game's gap between its two parts
        for (size_t i = 0; i < rows->size(); i++) {
          const Row& r = (*rows)[i];
          if (!r.label || !r.value) continue;
          imgui_overlay::DamageRow d;
          d.label = utf8(r.label->ToString());
          d.value = utf8(r.value->ToString());
          if (rows == &g_side[side].frames)
            if (std::string v = frameInfo(side, static_cast<int>(i)); !v.empty()) d.value = v;
          d.placed = screenRect(r.label_block, d.label_rect) && screenRect(r.value_block, d.value_rect);
          out.push_back(std::move(d));
        }
      }
    imgui_overlay::setDamageRows(side, out);
  }
}

void release() {
  for (auto& s : g_side) { s.widgets.clear(); s.damage.clear(); s.frames.clear(); }
  g_bound = false;
  g_hidden = 0;  // the widgets are gone; new ones start visible
  imgui_overlay::setDamageRows(0, {});
  imgui_overlay::setDamageRows(1, {});
}
}  // namespace damage_panel
