#pragma once

#include <assert.h>  // some headers expect it from imgui.h, which no longer includes it

// Force-included into every file (see xmake.lua). ImGui's and fmt's asserts
// embed the full path of the header they sit in, which would put the builder's
// local folders into the release DLL. Both are compiled out here instead.
#define IM_ASSERT(_EXPR) ((void)(_EXPR))
#define FMT_ASSERT(condition, message) ((void)0)
