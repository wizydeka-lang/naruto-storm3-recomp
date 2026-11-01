// naruto - native renderer view
//
// A second window, presented with Vulkan by the native renderer while the
// Xenia GPU backend keeps running the game in the main window. Enabled with
// NARUTO_NATIVE_VIEW=1 in the environment.

#pragma once

#include <cstdint>

namespace native_view {

struct FrameInfo {
  uint32_t draws = 0;
  // The color of the frame's last Resolve that cleared a render target.
  bool has_clear_color = false;
  float clear_color[4] = {0, 0, 0, 1};
};

bool Enabled();

// Called from the game's D3DDevice_Swap; never blocks on the view.
void SubmitFrame(const FrameInfo& frame);

}  // namespace native_view
