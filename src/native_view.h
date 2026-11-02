// naruto - native renderer view
//
// A second window, presented with Vulkan by the native renderer. Enabled
// with NARUTO_NATIVE_VIEW=1 in the environment; tools/run-radeon.sh
// --native-only also stops the Xenia GPU backend drawing, leaving it only the
// command stream.
//
// The game's draws, clears and resolves are captured as they happen
// (frame_capture.h) and replayed here with the precompiled shaders, pass by
// pass. The view shows the frame the game presents; with its window focused,
// Left and Right show a pass's render target instead, Home returns to the
// presented frame, and F12 saves a snapshot (below).
//
// Debugging: NARUTO_NATIVE_VIEW_DUMP=n logs the shown pass's draws in the
// n-th frame; NARUTO_NATIVE_VIEW_DEBUG bits turn off depth testing (1) and
// culling (2) and flip the winding (4); NARUTO_NATIVE_VIEW_SNAPSHOTS=dir saves
// each pass and the presented frame as PPM files in dir/<frame>/ for frame
// DUMP and every NARUTO_NATIVE_VIEW_SNAPSHOT_EVERY (300) frames after it. F12
// does the same for the current frame (into out/snapshots without SNAPSHOTS)
// and logs all its draws. NARUTO_NATIVE_VIEW_DRAW_SNAPSHOTS=p also saves pass
// p after each of its draws (pass<p>_draw<k>_rt<0|1>.ppm, its first two color
// targets, k in the logged order).

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
  // For the dump: the Xenos format and the patched and library destination
  // swizzles of the vertex fetch.
  uint32_t xenos_format = 0;
  uint32_t swizzle = 0;
  uint32_t library_swizzle = 0;
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
  // For the dump: the vertices the indices reach, and the bytes of them the
  // vertex buffers lacked (read as zero).
  uint32_t first_vertex = 0, vertex_count = 0, missing_bytes = 0;
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

// A D3DDevice_ClearF: fills the render targets and depth buffer bound at the
// time, before captured draw before_draw.
struct Clear {
  uint32_t pass = 0;
  uint32_t before_draw = 0;  // index into FrameInfo::captured
  uint32_t flags = 0;        // bits 0-3: render targets 0-3, 0x10 depth, 0x20 stencil
  uint32_t surface_info = 0;    // RB_SURFACE_INFO
  float viewport[6] = {};       // PA_CL_VPORT_* at the time
  uint32_t color_info[4] = {};  // RB_COLOR*_INFO
  uint32_t depth_info = 0;      // RB_DEPTH_INFO
  float color[4] = {};
  float depth = 1.0f;
  uint32_t stencil = 0;
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
  std::vector<Clear> clears;
  // The texture D3DDevice_Swap presented.
  bool has_front_buffer = false;
  uint32_t front_buffer[6] = {};
  std::vector<uint8_t> data;  // vertices, indices and constants
  // Guest memory, where the view reads texture data.
  const uint8_t* guest_base = nullptr;
  // Game thread time spent recording the frame's draws.
  double capture_seconds = 0;
};

bool Enabled();

// Called from the game's D3DDevice_Swap; never blocks on the view.
void SubmitFrame(FrameInfo&& frame);

// An empty buffer for the next frame's data, reusing the memory of a frame
// the view is done with so capture doesn't grow a new one each frame.
std::vector<uint8_t> RecycledData();

}  // namespace native_view
