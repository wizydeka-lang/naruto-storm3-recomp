// naruto - draw capture for the native view
//
// Copies what one of the game's draws uses out of guest memory into the
// frame being built for the native view: its vertices (in the ranges the
// indices reach), indices, shader constants and render state, all converted
// to little-endian. Vertex layouts come from the vertex fetch instructions in
// the bound vertex shader, which Direct3D patched for the current vertex
// declaration; the device keeps the rest (docs/d3d-functions.md).

#pragma once

#include "native_view.h"

#include <cstdint>

namespace frame_capture {

struct DrawCall {
  uint32_t device;
  uint32_t primitive_type;  // Xenos primitive type
  bool indexed;
  int32_t base_vertex;
  uint32_t start;  // first index, or first vertex if not indexed
  uint32_t count;
  uint32_t vertex_shader;  // guest shader objects
  uint32_t pixel_shader;
  int vertex_shader_index;  // shader_library matches, -1 for none
  int pixel_shader_index;
  uint32_t pass;
};

// Appends the draw to frame.captured; returns false (and appends nothing) if
// the draw uses something the native view can't draw yet.
bool Capture(const uint8_t* base, const DrawCall& call, native_view::FrameInfo& frame);

// Records a D3DDevice_Resolve ending pass `pass`. rect and point are guest
// pointers (0 for none), texture the destination texture object.
void CaptureResolve(const uint8_t* base, uint32_t device, uint32_t flags, uint32_t rect,
                    uint32_t texture, uint32_t point, uint32_t level, uint32_t slice,
                    uint32_t pass, native_view::FrameInfo& frame);

// Records the texture D3DDevice_Swap presents.
void CaptureFrontBuffer(const uint8_t* base, uint32_t texture, native_view::FrameInfo& frame);

}  // namespace frame_capture
