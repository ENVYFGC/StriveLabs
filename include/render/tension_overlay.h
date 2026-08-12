#pragma once

#include "common.h"

class TensionOverlay {
  PIMPL

public:
  ~TensionOverlay();
  TensionOverlay();

  void reset();
  void draw();
};
