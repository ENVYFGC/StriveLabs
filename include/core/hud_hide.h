#pragma once

namespace hud_hide {
  bool hidden();
  bool editors();
  void keepEditors(bool on);
  void gameMenu(bool open);
  void toggle(int mode);
  void tick(bool fight);
}
