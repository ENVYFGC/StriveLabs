/*
  Utility for watching for keyboard inputs on another thread
*/
#pragma once

#include <vector>

namespace BindWatcherI {
  // Added to a key code when Windows repeats a held key. Only frame step uses
  // repeats (hold to keep stepping); every other bind reacts to the press alone.
  constexpr int REPEAT = 0x10000;
  void addToFilter(unsigned long code);
  // While on, Esc is kept from the game and only reported through takeEscape().
  void setEscapeCapture(bool on);
  bool takeEscape();
  void setCaptureAll(bool on);  // while on, every key is reported, not just filtered ones
  std::vector<int> getInputs(bool await = false); // returns inputs since last call. If await: block until there is an input to return
};