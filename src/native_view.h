// naruto - native renderer view
//
// A second window, presented with Vulkan by the native renderer while the
// Xenia GPU backend keeps running the game in the main window. Enabled with
// NARUTO_NATIVE_VIEW=1 in the environment.
//
// The game's draws are captured as they happen (frame_capture.h) and drawn
// here with the precompiled shaders. The game renders a frame in passes, one
// per render target, separated by resolves; the view draws one pass at a
// time. By default it shows the pass with the most indices; with the view's
// window focused, Left and Right pick another and Home returns to the default.
//
// Debugging: NARUTO_NATIVE_VIEW_DUMP=n logs the shown pass's draws in the
// n-th frame; NARUTO_NATIVE_VIEW_DEBUG=1 turns depth testing off.

#pragma once

#include <cstdint>
#include <vector>

namespace native_view {

struct VertexBinding {
  uint32_t data_offset;  // in FrameInfo::data
  uint32_t stride;       // bytes
};

struct VertexAttribute {
  uint32_t location;
  uint32_t binding;  // index into the draw's bindings
  uint32_t offset;
  uint32_t format;  // VkFormat
};

// A texture a draw's pixel shader samples: its sampler register and the
// texture fetch constant bound there (host-endian words).
struct TextureBinding {
  uint32_t sampler;
  uint32_t fetch[6];
};

struct Draw {
  int vertex_shader = -1;  // shader_library index
  int pixel_shader = -1;   // -1: no pixel shader (depth only)
  uint32_t pass = 0;
  uint32_t topology = 0;  // VkPrimitiveTopology
  bool primitive_restart = false;
  uint32_t index_offset = 0;  // in FrameInfo::data, 32-bit indices
  uint32_t index_count = 0;
  uint32_t first_binding = 0;
  uint32_t binding_count = 0;
  uint32_t first_attribute = 0;
  uint32_t attribute_count = 0;
  uint32_t first_texture = 0;
  uint32_t texture_count = 0;
  // Offsets in FrameInfo::data of the shaders' constants, little-endian. The
  // view fills the shared constants' texture and sampler indices.
  uint32_t vertex_constants = 0;
  uint32_t pixel_constants = 0;
  uint32_t shared_constants = 0;
  // Xenos register values for the draw.
  uint32_t surface_info = 0;   // RB_SURFACE_INFO
  uint32_t color_info[4] = {}; // RB_COLOR_INFO, RB_COLOR1_INFO...
  uint32_t depth_info = 0;     // RB_DEPTH_INFO
  uint32_t depth_control = 0;  // RB_DEPTHCONTROL
  uint32_t blend_control[4] = {};  // RB_BLENDCONTROL0-3
  uint32_t color_mask = 0;     // RB_COLOR_MASK, four bits per render target
  uint32_t vte_control = 0;    // PA_CL_VTE_CNTL
  uint32_t mode_control = 0;   // PA_SU_SC_MODE_CNTL
  bool alpha_test = false;
  float viewport[6] = {};  // PA_CL_VPORT_{X,Y,Z}{SCALE,OFFSET}, interleaved
};

// A Resolve: copies a render target (or the depth buffer) into a texture.
// It ends pass `pass`.
struct Resolve {
  uint32_t pass = 0;
  uint32_t flags = 0;  // bits 0-2: render target 0-3, or 4 for depth
  uint32_t surface_info = 0;
  uint32_t source_info = 0;  // RB_COLOR*_INFO or RB_DEPTH_INFO of the source
  bool has_rect = false;
  int32_t rect[4] = {};  // source x1, y1, x2, y2
  int32_t point[2] = {};  // destination x, y
  uint32_t level = 0;
  uint32_t slice = 0;
  uint32_t fetch[6] = {};  // the destination texture's fetch constant
};

// How a pass's render target starts: the Resolve that ended the previous
// pass clears it.
struct PassInfo {
  bool has_clear_color = false;
  float clear_color[4] = {0, 0, 0, 1};
  float clear_depth = 1.0f;
};

struct FrameInfo {
  uint32_t draws = 0;  // all draws, including ones that could not be captured
  std::vector<PassInfo> passes = std::vector<PassInfo>(1);
  std::vector<Draw> captured;
  std::vector<VertexBinding> bindings;
  std::vector<VertexAttribute> attributes;
  std::vector<TextureBinding> textures;
  std::vector<Resolve> resolves;
  // The texture D3DDevice_Swap presented.
  bool has_front_buffer = false;
  uint32_t front_buffer[6] = {};
  std::vector<uint8_t> data;  // vertices, indices and constants
  // Guest memory, where the view reads texture data.
  const uint8_t* guest_base = nullptr;
};

bool Enabled();

// Called from the game's D3DDevice_Swap; never blocks on the view.
void SubmitFrame(FrameInfo&& frame);

}  // namespace native_view
