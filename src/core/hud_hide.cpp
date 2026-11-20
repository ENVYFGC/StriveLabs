#include "hud_hide.h"
#include "combo_log.h"
#include "imgui_overlay.h"
#include "object_scan.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FMemory.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

class AREDGameState_Battle;
AREDGameState_Battle* getGameState();

namespace hud_hide {
namespace {
  struct Kept {
    UObject* obj = nullptr;
    const uint8_t* item = nullptr;
    UClass* cls = nullptr;
    bool alive() const {
      return item && *reinterpret_cast<UObject* const*>(item) == obj &&
             !(*reinterpret_cast<const int32_t*>(item + 8) & ((1 << 28) | (1 << 29))) && obj->GetClassPrivate() == cls;
    }
  };
  Kept keep(UObject* o) { return {o, o ? reinterpret_cast<const uint8_t*>(o->GetObjectItem()) : nullptr, o ? o->GetClassPrivate() : nullptr}; }

  struct Widget { Kept k; uint8_t vis = 0; bool common = false; float opacity = 1.0f; bool faded = false; int reshown = 0, refaded = 0; };
  struct Arr { UObject** data; int32_t num, max; };

  constexpr uint8_t kCollapsed = 1, kHidden = 2;
  constexpr float kMark = 0.0003f;
  constexpr int kEvery = 15;
  constexpr int32_t kBuf = 64;
  constexpr const TCHAR* kHudPrefixes[] = {STR("00_Allset_Hud_Gauge"), STR("00_Allset_Bottom_Gauge"), STR("Battle_"), STR("REDWidgetBattle"),
                                           STR("Allset_Mission_Battle"), STR("Hud_"), STR("3on_")};

  std::atomic<bool> g_hidden{false};
  std::atomic<bool> g_keep_editors{false};
  std::atomic<bool> g_menu_open{false};
  int g_wait = 0, g_mode = -1;
  std::vector<Widget> g_widgets;
  std::vector<Kept> g_huds;
  std::vector<Kept> g_common;
  std::vector<StringType> g_world_classes;
  std::string g_world_names, g_left_names;
  bool g_common_scanned = false;

  struct Engine {
    bool tried = false, widgets_ok = false, huds_ok = false, common_ok = false;
    FProperty* slot = nullptr;
    UFunction* list_widgets = nullptr;
    UObject* widget_lib = nullptr;
    UClass* user_widget = nullptr;
    int32_t w_world = 0, w_found = 0, w_class = 0;
    FBoolProperty* w_top = nullptr;
    UFunction* get_vis = nullptr;
    UFunction* set_vis = nullptr;
    int32_t gv_ret = 0, sv_in = 0;
    UFunction* get_op = nullptr;
    UFunction* set_op = nullptr;
    int32_t go_ret = 0, so_in = 0;
    bool opacity_ok = false;
    UFunction* list_actors = nullptr;
    UObject* actor_lib = nullptr;
    UClass* hud = nullptr;
    int32_t a_world = 0, a_class = 0, a_out = 0;
    FBoolProperty* show_hud = nullptr;
  } g;

  bool param(UFunction* f, const TCHAR* name, int32_t size, int32_t& at) {
    FProperty* p = f ? f->GetPropertyByName(name) : nullptr;
    if (!p || p->GetSize() != size || !p->IsInContainer(kBuf) || f->GetPropertiesSize() > kBuf) return false;
    at = p->GetOffset_Internal();
    return true;
  }

  void find() {
    if (g.tried) return;
    g.tried = true;
    std::string missing;
    auto need = [&](bool ok, const char* what) { if (!ok) missing += (missing.empty() ? "" : ", ") + std::string(what); return ok; };

    g.list_widgets = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/UMG.WidgetBlueprintLibrary:GetAllWidgetsOfClass"));
    g.widget_lib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
    g.user_widget = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.UserWidget"));
    g.get_vis = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/UMG.Widget:GetVisibility"));
    g.set_vis = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/UMG.Widget:SetVisibility"));
    bool w = need(g.list_widgets && g.widget_lib, "WidgetBlueprintLibrary:GetAllWidgetsOfClass");
    w = need(g.user_widget != nullptr, "UMG.UserWidget") && w;
    if (g.list_widgets) {
      g.w_top = CastField<FBoolProperty>(g.list_widgets->GetPropertyByName(STR("TopLevelOnly")));
      const bool layout = param(g.list_widgets, STR("WorldContextObject"), 8, g.w_world) && param(g.list_widgets, STR("FoundWidgets"), sizeof(Arr), g.w_found) &&
                          param(g.list_widgets, STR("WidgetClass"), 8, g.w_class) && g.w_top && g.w_top->IsInContainer(kBuf);
      w = need(layout, "GetAllWidgetsOfClass's parameters") && w;
    }
    FProperty* ret = g.get_vis ? g.get_vis->GetReturnProperty() : nullptr;
    if (need(ret && ret->GetSize() == 1 && ret->IsInContainer(kBuf) && g.get_vis->GetPropertiesSize() <= kBuf, "Widget:GetVisibility")) g.gv_ret = ret->GetOffset_Internal();
    else w = false;
    w = need(param(g.set_vis, STR("InVisibility"), 1, g.sv_in), "Widget:SetVisibility") && w;
    g.get_op = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/UMG.Widget:GetRenderOpacity"));
    g.set_op = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/UMG.Widget:SetRenderOpacity"));
    {
      FProperty* oret = g.get_op ? g.get_op->GetReturnProperty() : nullptr;
      g.opacity_ok = need(oret && oret->GetSize() == 4 && oret->IsInContainer(kBuf) && g.get_op->GetPropertiesSize() <= kBuf && param(g.set_op, STR("InOpacity"), 4, g.so_in),
                          "Widget render opacity (a gauge the game keeps showing can't be kept out of sight)");
      if (g.opacity_ok) g.go_ret = oret->GetOffset_Internal();
    }
    g.widgets_ok = w;
    UClass* widget = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.Widget"));
    g.slot = widget ? widget->GetPropertyByName(STR("Slot")) : nullptr;
    g.common_ok = need(g.slot && g.slot->GetSize() == 8, "Widget.Slot") && w;

    g.list_actors = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.GameplayStatics:GetAllActorsOfClass"));
    g.actor_lib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
    g.hud = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.HUD"));
    bool h = need(g.list_actors && g.actor_lib, "GameplayStatics:GetAllActorsOfClass");
    if (g.list_actors)
      h = need(param(g.list_actors, STR("WorldContextObject"), 8, g.a_world) && param(g.list_actors, STR("ActorClass"), 8, g.a_class) &&
               param(g.list_actors, STR("OutActors"), sizeof(Arr), g.a_out), "GetAllActorsOfClass's parameters") && h;
    g.show_hud = g.hud ? CastField<FBoolProperty>(g.hud->GetPropertyByName(STR("bShowHUD"))) : nullptr;
    h = need(g.show_hud != nullptr, "HUD.bShowHUD") && h;
    g.huds_ok = h;

    if (!missing.empty())
      COMBO_LOG("hide all HUD: not found in the game: " + missing + (g.widgets_ok ? "" : "; the game's widgets stay as they are") +
                (g.huds_ok ? "" : "; the HUD actors stay as they are") + ". StriveLabs' own windows still hide.");
  }

  template <class F> void takeList(uint8_t* p, int32_t at, F&& each) {
    Arr a;
    std::memcpy(&a, p + at, sizeof(a));
    std::memset(p + at, 0, sizeof(a));
    for (int32_t i = 0; a.data && i < a.num; i++)
      if (a.data[i]) each(a.data[i]);
    if (a.data) (*GMalloc)->Free(a.data);
  }

  uint8_t visibility(UObject* w) {
    alignas(16) uint8_t p[kBuf] = {};
    w->ProcessEvent(g.get_vis, p);
    return p[g.gv_ret];
  }
  void setVisibility(UObject* w, uint8_t v) {
    alignas(16) uint8_t p[kBuf] = {};
    p[g.sv_in] = v;
    w->ProcessEvent(g.set_vis, p);
  }
  bool shows(uint8_t v) { return v != kCollapsed && v != kHidden; }
  float opacity(UObject* w) {
    alignas(16) uint8_t p[kBuf] = {};
    w->ProcessEvent(g.get_op, p);
    float v;
    std::memcpy(&v, p + g.go_ret, 4);
    return v;
  }
  void setOpacity(UObject* w, float v) {
    alignas(16) uint8_t p[kBuf] = {};
    std::memcpy(p + g.so_in, &v, 4);
    w->ProcessEvent(g.set_op, p);
  }
  bool menuWidget(UObject* w) {
    UClass* c = w->GetClassPrivate();
    return c && c->GetName().rfind(STR("SMenu_"), 0) == 0;
  }

  std::string narrow(const StringType& s) {
    std::string o;
    for (auto c : s) o += c < 0x80 ? static_cast<char>(c) : '?';
    return o;
  }
  bool hudName(const StringType& n) {
    for (const TCHAR* p : kHudPrefixes)
      if (n.rfind(p, 0) == 0) return true;
    return false;
  }
  void scanCommon() {
    std::vector<UObject*> found;
    object_scan::allOf(STR("UserWidget"), found);
    g_common.clear();
    std::string names;
    for (UObject* o : found) {
      bool owned = false, hud_parent = false;
      UObject* outer = o->GetOuterPrivate();
      for (int up = 0; up < 6 && outer && !owned; up++) {
        UClass* oc = outer->GetClassPrivate();
        const StringType on = oc ? oc->GetName() : StringType();
        owned = on == STR("REDGameCommon");
        if (!owned && hudName(on)) hud_parent = true;
        outer = outer->GetOuterPrivate();
      }
      if (!owned) continue;
      if (*reinterpret_cast<UObject* const*>(reinterpret_cast<const uint8_t*>(o) + g.slot->GetOffset_Internal())) continue;
      UClass* c = o->GetClassPrivate();
      const StringType n = c ? c->GetName() : StringType();
      if (n.rfind(STR("SMenu_"), 0) == 0 || !(hudName(n) || hud_parent)) continue;
      g_common.push_back(keep(o));
      names += (names.empty() ? "" : ", ") + narrow(n);
    }
    COMBO_LOG("hide all HUD: " + std::to_string(g_common.size()) + " HUD roots owned by the game's UI manager: " + (names.empty() ? std::string("none") : names));
    g_common_scanned = true;
  }

  UObject* fightObject() { return reinterpret_cast<UObject*>(getGameState()); }

  void fade(Widget& r) {
    if (!g.opacity_ok) return;
    const float o = opacity(r.k.obj);
    if (o == kMark || o == 0.0f) return;
    if (!r.faded) { r.opacity = o; r.faded = true; }
    else { r.refaded++; r.opacity = o; }
    setOpacity(r.k.obj, kMark);
  }

  void pass(bool first) {
    UObject* world = fightObject();
    if (g.widgets_ok) {
      g_widgets.erase(std::remove_if(g_widgets.begin(), g_widgets.end(), [](const Widget& r) { return !r.k.alive(); }), g_widgets.end());
      for (Widget& r : g_widgets) {
        fade(r);
        const uint8_t v = visibility(r.k.obj);
        if (v == kHidden) continue;
        r.vis = v;
        if (shows(v)) { r.reshown++; setVisibility(r.k.obj, kHidden); }
      }
      if (g.common_ok)
        for (const Kept& k : g_common) {
          if (!first && g_menu_open.load(std::memory_order_relaxed)) break;
          if (!k.alive() || std::any_of(g_widgets.begin(), g_widgets.end(), [&](const Widget& r) { return r.k.obj == k.obj; })) continue;
          const uint8_t v = visibility(k.obj);
          if (!shows(v)) continue;
          g_widgets.push_back({k, v, true});
          setVisibility(k.obj, kHidden);
          fade(g_widgets.back());
        }
      if (world && (first || !g_menu_open.load(std::memory_order_relaxed))) {
        alignas(16) uint8_t p[kBuf] = {};
        std::memcpy(p + g.w_world, &world, sizeof(world));
        std::memcpy(p + g.w_class, &g.user_widget, sizeof(g.user_widget));
        g.w_top->SetPropertyValueInContainer(p, true);
        g.widget_lib->ProcessEvent(g.list_widgets, p);
        takeList(p, g.w_found, [first](UObject* w) {
          for (const Widget& r : g_widgets)
            if (r.k.obj == w) return;
          if (menuWidget(w)) return;
          const uint8_t v = visibility(w);
          if (!shows(v)) return;
          UClass* wc = w->GetClassPrivate();
          const StringType wn = wc ? wc->GetName() : StringType();
          const bool seen = std::find(g_world_classes.begin(), g_world_classes.end(), wn) != g_world_classes.end();
          if (first) {
            if (!seen) { g_world_classes.push_back(wn); g_world_names += (g_world_names.empty() ? "" : ", ") + narrow(wn); }
          } else if (!seen) {
            const std::string nn = narrow(wn);
            if (g_left_names.find(nn) == std::string::npos && g_left_names.size() < 600) g_left_names += (g_left_names.empty() ? "" : ", ") + nn;
            return;
          }
          g_widgets.push_back({keep(w), v});
          setVisibility(w, kHidden);
          fade(g_widgets.back());
        });
      }
    }
    if (g.huds_ok && world) {
      g_huds.erase(std::remove_if(g_huds.begin(), g_huds.end(), [](const Kept& k) { return !k.alive(); }), g_huds.end());
      for (const Kept& k : g_huds)
        if (g.show_hud->GetPropertyValueInContainer(k.obj)) g.show_hud->SetPropertyValueInContainer(k.obj, false);
      alignas(16) uint8_t p[kBuf] = {};
      std::memcpy(p + g.a_world, &world, sizeof(world));
      std::memcpy(p + g.a_class, &g.hud, sizeof(g.hud));
      g.actor_lib->ProcessEvent(g.list_actors, p);
      takeList(p, g.a_out, [](UObject* a) {
        for (const Kept& k : g_huds)
          if (k.obj == a) return;
        if (!g.show_hud->GetPropertyValueInContainer(a)) return;
        g_huds.push_back(keep(a));
        g.show_hud->SetPropertyValueInContainer(a, false);
      });
    }
  }

  std::string modeText(int mode) {
    if (mode == 8) return "training";
    if (mode == 0x17) return "a replay";
    return "game mode " + std::to_string(mode);
  }

  void off(const char* why) {
    int back = 0, changed = 0, gone = 0, huds = 0, refaded = 0;
    std::string kept_showing;
    for (const Widget& r : g_widgets) {
      if (!r.k.alive()) { gone++; continue; }
      refaded += r.refaded;
      if (r.reshown > 0) {
        UClass* c = r.k.obj->GetClassPrivate();
        kept_showing += (kept_showing.empty() ? "" : ", ") + (c ? narrow(c->GetName()) : std::string("?")) + " x" + std::to_string(r.reshown);
      }
      if (r.faded) {
        if (opacity(r.k.obj) == kMark) setOpacity(r.k.obj, r.opacity);
        else changed++;
      }
      if (visibility(r.k.obj) != kHidden) { changed++; continue; }
      setVisibility(r.k.obj, r.vis);
      back++;
    }
    for (const Kept& k : g_huds) {
      if (!k.alive()) { gone++; continue; }
      g.show_hud->SetPropertyValueInContainer(k.obj, true);
      huds++;
    }
    g_widgets.clear();
    g_huds.clear();
    g_hidden = false;
    COMBO_LOG(std::string("hide all HUD: off (") + why + ") in " + modeText(g_mode) + ", " + std::to_string(back) + " game widgets shown again, " +
              std::to_string(changed) + " left as the game set them, " + std::to_string(huds) + " HUD actors shown again, " + std::to_string(gone) + " gone" +
              (kept_showing.empty() ? std::string() : "; the game kept showing: " + kept_showing + (g.opacity_ok ? " (kept out of sight through opacity)" : "")) +
              (refaded ? "; the game put opacity back " + std::to_string(refaded) + " times" : std::string()) +
              (g_left_names.empty() ? std::string() : "; opened meanwhile and left alone: " + g_left_names));
  }
}

bool hidden() { return g_hidden.load(std::memory_order_relaxed); }
bool editors() { return g_hidden.load(std::memory_order_relaxed) && g_keep_editors.load(std::memory_order_relaxed); }
void gameMenu(bool open) { g_menu_open.store(open, std::memory_order_relaxed); }
void keepEditors(bool on) {
  if (g_keep_editors.exchange(on, std::memory_order_relaxed) != on) COMBO_LOG(std::string("hide all HUD: editor panels stay usable: ") + (on ? "on" : "off"));
}

void toggle(int mode) {
  if (g_hidden) { off("the key"); return; }
  if (g_menu_open.load(std::memory_order_relaxed)) {
    COMBO_LOG("hide all HUD: not turned on while the game's own menu is open");
    imgui_overlay::note("Hide all HUD: close the game's menu first");
    return;
  }
  find();
  g_mode = mode;
  if (g.common_ok) scanCommon();
  g_hidden = true;
  g_world_classes.clear();
  g_world_names.clear();
  g_left_names.clear();
  pass(true);
  g_wait = kEvery;
  const auto common = std::count_if(g_widgets.begin(), g_widgets.end(), [](const Widget& r) { return r.common; });
  COMBO_LOG("hide all HUD: on in " + modeText(mode) + ", " + std::to_string(g_widgets.size()) + " game widgets hidden (" + std::to_string(common) +
            " of them the UI manager's), " + std::to_string(g_huds.size()) + " HUD actors hidden" +
            (g.widgets_ok && g.huds_ok && g.common_ok ? "" : " (part of the game's HUD wasn't found, see above)") +
            (g_world_names.empty() ? std::string() : "; the fight's own: " + g_world_names));
}

void tick(bool fight) {
  if (!g_hidden.load(std::memory_order_relaxed)) return;
  if (!fight) { off("no fight running"); return; }
  if (--g_wait > 0) return;
  g_wait = kEvery;
  pass(false);
}
}
