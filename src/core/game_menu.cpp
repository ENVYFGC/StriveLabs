#include "game_menu.h"
#include "combo_log.h"
#include "object_scan.h"
#include "imgui_overlay.h"

#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <string>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

// The pause menu in a fight is the game's start menu, SMenu_Layout: a few copies kept
// for the whole session, shown when the menu opens.
namespace game_menu {
namespace {
  std::vector<UObject*> g_menus;
  bool g_bound = false;
  int g_retry = 0;  // frames until looking again, while not found
  int g_state = -1;  // last reported: bit per copy, on screen and visible
  UFunction* g_in_viewport = nullptr;
  UFunction* g_is_visible = nullptr;

  bool ask(UObject* w, UFunction* fn) {
    if (!fn) return false;
    struct { bool ReturnValue = false; } p;
    w->ProcessEvent(fn, &p);
    return p.ReturnValue;
  }

  void bind() {
    g_menus.clear();
    std::vector<UObject*> found;
    object_scan::allOf(STR("SMenu_Layout_C"), found);
    for (UObject* w : found) {
      const auto full = w->GetFullName();
      if (full.find(STR("/Engine/Transient")) == StringType::npos) continue;  // not the templates
      g_menus.push_back(w);
    }
    g_bound = !g_menus.empty();
    g_in_viewport = g_bound ? g_menus[0]->GetFunctionByNameInChain(STR("IsInViewport")) : nullptr;
    g_is_visible = g_bound ? g_menus[0]->GetFunctionByNameInChain(STR("IsVisible")) : nullptr;
    g_state = -1;
    COMBO_LOG("game menu: " + std::to_string(g_menus.size()) + " pause menu copies found");
  }
}

void update() {
  if (!g_bound) {
    if (--g_retry > 0) return;
    bind();
    g_retry = 120;
    if (!g_bound) { imgui_overlay::setGameMenuOpen(false); return; }
  }
  int state = 0;
  for (size_t i = 0; i < g_menus.size() && i < 16; i++) {
    const bool in_view = ask(g_menus[i], g_in_viewport);
    const bool visible = ask(g_menus[i], g_is_visible);
    if (in_view) state |= 1 << (2 * i);
    if (visible) state |= 2 << (2 * i);
  }
  if (state != g_state) {
    std::string s;
    for (size_t i = 0; i < g_menus.size() && i < 16; i++)
      s += " " + std::to_string(i) + ":" + ((state >> (2 * i)) & 1 ? "in" : "out") + ((state >> (2 * i)) & 2 ? "/shown" : "/hidden");
    COMBO_LOG("game menu: copies" + s);
    g_state = state;
  }
  bool open = false;
  for (size_t i = 0; i < g_menus.size() && i < 16; i++)
    if (((state >> (2 * i)) & 3) == 3) open = true;
  imgui_overlay::setGameMenuOpen(open);
}

void release() {
  g_menus.clear();
  g_bound = false;
  g_in_viewport = g_is_visible = nullptr;
  g_retry = 0;
  imgui_overlay::setGameMenuOpen(false);
}
}  // namespace game_menu
