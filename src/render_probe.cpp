// naruto - Direct3D call probe
//
// First step towards a native renderer: hooks the game's Direct3D entry points
// (config/naruto_d3d_funcs.toml), records what each frame draws and which
// device state it uses, then calls the original function, so the Xenia GPU
// backend still renders. With NARUTO_RENDER_PROBE=1 in the environment, a
// summary is logged every 10 seconds; with NARUTO_NATIVE_VIEW=1, each draw is
// captured (frame_capture.h) and each frame handed to the native view
// (native_view.h). Otherwise the hooks only pass through.

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>

#include "frame_capture.h"
#include "native_view.h"
#include "shader_library.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace {

// Device object fields (docs/d3d-functions.md).
constexpr uint32_t kDevicePixelShader = 0x330C;
constexpr uint32_t kDeviceVertexShader = 0x3310;

uint32_t LoadGuest32(const uint8_t* base, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(base + address);
}

// Where a bound shader object keeps its microcode (docs/d3d-functions.md). A
// vertex shader holds variants (offsets at +896, eight bytes apart); the one
// bound is copied into the command buffer and its vertex fetches patched
// there for the vertex declaration (BoundVertexShader). A pixel shader has
// one copy. The object embeds
// its container's virtual part (header, constant table, element table) at
// `header`; the microcode is the container's physical part.
struct ShaderLocation {
  uint32_t header = 0;
  uint32_t address = 0;
  uint32_t size = 0;
};

ShaderLocation LocateShader(const uint8_t* base, uint32_t shader, shader_library::Stage stage,
                            uint32_t vertex_variant = 0) {
  ShaderLocation location;
  if (stage == shader_library::Stage::kVertex) {
    location.header = shader + 872;
    uint32_t variant = shader + LoadGuest32(base, shader + 896 + 8 * vertex_variant);
    location.address = LoadGuest32(base, shader + 32) + LoadGuest32(base, variant + 872);
    location.size = LoadGuest32(base, variant + 876);
  } else {
    location.header = shader + 40;
    uint32_t info = shader + LoadGuest32(base, shader + 64);
    location.address = LoadGuest32(base, shader + 24) + LoadGuest32(base, info + 40);
    location.size = LoadGuest32(base, info + 44);
  }
  return location;
}

std::vector<uint32_t> ShaderMicrocode(const uint8_t* base, const ShaderLocation& location) {
  std::vector<uint32_t> words;
  if (location.size && location.size % 4 == 0 && location.size <= 0x10000) {
    for (uint32_t i = 0; i < location.size; i += 4) {
      words.push_back(LoadGuest32(base, location.address + i));
    }
  }
  return words;
}

// The shader's container rebuilt from the object: its virtual part followed
// by the microcode at the offset the shader header gives.
std::vector<uint8_t> ShaderContainer(const uint8_t* base, const ShaderLocation& location,
                                     const std::vector<uint32_t>& words) {
  uint32_t virtual_size = LoadGuest32(base, location.header + 4);
  uint32_t shader_offset = LoadGuest32(base, location.header + 24);
  if (virtual_size > 0x10000 || shader_offset + 8 > virtual_size) {
    return {};
  }
  uint32_t code_offset = LoadGuest32(base, location.header + shader_offset);
  std::vector<uint8_t> container(size_t(virtual_size) + code_offset + words.size() * 4);
  for (uint32_t i = 0; i < virtual_size; ++i) {
    container[i] = base[location.header + i];
  }
  for (size_t w = 0; w < words.size(); ++w) {
    uint32_t value = std::byteswap(words[w]);
    std::memcpy(container.data() + virtual_size + code_offset + w * 4, &value, 4);
  }
  // The physical size covers the microcode; Direct3D's own flag bits stay.
  uint32_t physical_size = std::byteswap(uint32_t(code_offset + words.size() * 4));
  std::memcpy(container.data() + 8, &physical_size, 4);
  return container;
}

// Bound shader object -> library index (-1: no match). Objects are freed and
// their addresses reused, so a match is kept with the microcode's address and
// first words, and redone when those change.
struct ShaderMatch {
  int index = -1;
  uint32_t address = 0;
  uint32_t first_words[4] = {};
};
std::unordered_map<uint32_t, ShaderMatch> g_shader_matches;
bool g_library_loaded = false;

int MatchShader(const uint8_t* base, uint32_t shader, shader_library::Stage stage,
                uint32_t vertex_variant = 0) {
  if (!shader) {
    return -1;
  }
  ShaderLocation location = LocateShader(base, shader, stage, vertex_variant);
  uint32_t first_words[4] = {};
  for (uint32_t i = 0; i < 4 && i * 4 < location.size; ++i) {
    first_words[i] = LoadGuest32(base, location.address + i * 4);
  }
  auto [it, inserted] = g_shader_matches.try_emplace(shader);
  ShaderMatch& match = it->second;
  if (!inserted && match.address == location.address &&
      std::equal(std::begin(first_words), std::end(first_words), match.first_words)) {
    return match.index;
  }
  if (!g_library_loaded) {
    g_library_loaded = true;
    shader_library::Load(std::string(REXCVAR_GET(game_data_root)));
  }
  match.address = location.address;
  std::copy(std::begin(first_words), std::end(first_words), match.first_words);
  std::vector<uint32_t> words = ShaderMicrocode(base, location);
  match.index = shader_library::Match(stage, words.data(), words.size());
  if (match.index < 0) {
    REXLOG_WARN("[render probe] {} shader object {:08X} ({} words) matches no known shader",
                stage == shader_library::Stage::kVertex ? "vertex" : "pixel", shader,
                words.size());
    std::vector<uint8_t> container = ShaderContainer(base, location, words);
    if (!container.empty()) {
      shader_library::SaveRuntimeContainer(stage, container, words.data(), words.size());
    }
  }
  return match.index;
}

struct FrameStats {
  uint64_t frames = 0;
  uint64_t indexed_draws = 0;
  uint64_t vertex_draws = 0;
  uint64_t indices = 0;
  uint64_t vertices = 0;
  uint64_t clears = 0;
  uint64_t resolves = 0;
  uint64_t resolves_with_clear_color = 0;
  uint64_t texture_binds = 0;
  uint64_t captured_draws = 0;
  std::map<uint32_t, uint64_t> resolve_flags;
  std::map<uint32_t, uint64_t> primitive_types;
  std::set<uint32_t> vertex_shaders;
  std::set<uint32_t> pixel_shaders;
};

const bool g_logging = [] {
  const char* value = std::getenv("NARUTO_RENDER_PROBE");
  return value && *value && *value != '0';
}();
const bool g_enabled = g_logging || native_view::Enabled();

std::mutex g_mutex;
FrameStats g_stats;
native_view::FrameInfo g_frame;
uint32_t g_pass = 0;  // index of g_frame.passes being drawn

// Where each device's vertex shader variants were last patched for a vertex
// declaration: in place in the object's microcode (sub_8274A0F8), or, while
// the GPU may still read that, in a copy in the command buffer
// (sub_82749EF0). The patched vertex fetches give the layout draws use.
// Variant 1 is the depth-only one (RB_MODECONTROL edram mode 5) of objects
// with flag 0x20.
struct BoundVertexShader {
  uint32_t object = 0;
  uint32_t code = 0;
  uint32_t size = 0;
};
std::unordered_map<uint32_t, std::array<BoundVertexShader, 2>> g_bound_vertex_shaders;
constexpr uint32_t kRegModeControl = 0x2954;  // RB_MODECONTROL shadow

uint32_t VariantMicrocode(const uint8_t* base, uint32_t object, uint32_t variant,
                          uint32_t* size) {
  uint32_t header = object + LoadGuest32(base, object + 896 + 8 * variant);
  *size = LoadGuest32(base, header + 876);
  return LoadGuest32(base, object + 32) + LoadGuest32(base, header + 872);
}
std::set<uint32_t> g_all_vertex_shaders;
std::set<uint32_t> g_all_pixel_shaders;
auto g_window_start = std::chrono::steady_clock::now();

// Called after the original draw function, which has committed the device's
// state for it.
void RecordDraw(const uint8_t* base, uint32_t device, uint32_t primitive_type, bool indexed,
                int32_t base_vertex, uint32_t start, uint32_t count) {
  uint32_t vs = LoadGuest32(base, device + kDeviceVertexShader);
  uint32_t ps = LoadGuest32(base, device + kDevicePixelShader);
  std::lock_guard<std::mutex> lock(g_mutex);
  uint32_t variant = vs && (LoadGuest32(base, vs + 872) & 0x20) &&
                             (LoadGuest32(base, device + kRegModeControl) & 7) == 5;
  BoundVertexShader bound = g_bound_vertex_shaders[device][variant];
  if (vs && bound.object != vs) {
    bound = {vs, VariantMicrocode(base, vs, variant, &bound.size), bound.size};
  }
  int vs_index = MatchShader(base, vs, shader_library::Stage::kVertex, variant);
  int ps_index = MatchShader(base, ps, shader_library::Stage::kPixel);
  g_frame.draws++;
  if (native_view::Enabled() &&
      frame_capture::Capture(base,
                             {device, primitive_type, indexed, base_vertex, start, count, vs, ps,
                              vs_index, ps_index, g_pass, bound.code, bound.size},
                             g_frame)) {
    g_stats.captured_draws++;
  }
  (indexed ? g_stats.indexed_draws : g_stats.vertex_draws)++;
  (indexed ? g_stats.indices : g_stats.vertices) += count;
  g_stats.primitive_types[primitive_type]++;
  g_stats.vertex_shaders.insert(vs);
  g_stats.pixel_shaders.insert(ps);
}

void LogWindow() {
  auto now = std::chrono::steady_clock::now();
  double seconds = std::chrono::duration<double>(now - g_window_start).count();
  if (seconds < 10.0) {
    return;
  }
  const FrameStats& s = g_stats;
  double frames = s.frames ? double(s.frames) : 1.0;
  g_all_vertex_shaders.insert(s.vertex_shaders.begin(), s.vertex_shaders.end());
  size_t matched = 0;
  for (const auto& [object, match] : g_shader_matches) {
    matched += match.index >= 0;
  }
  g_all_pixel_shaders.insert(s.pixel_shaders.begin(), s.pixel_shaders.end());
  std::string primitives;
  for (auto [type, n] : s.primitive_types) {
    primitives += fmt::format(" {}:{:.1f}", type, n / frames);
  }
  std::string resolve_flags;
  for (auto [flags, n] : s.resolve_flags) {
    resolve_flags += fmt::format(" {:#x}:{:.1f}", flags, n / frames);
  }
  REXLOG_INFO(
      "[render probe] {:.1f} fps | draws/frame {:.1f} indexed + {:.1f} non-indexed "
      "({:.0f} indices, {:.0f} vertices) | primitive types/frame{} | clears/frame {:.2f} "
      "resolves/frame {:.2f} (with clear color {:.2f}; flags{}) texture binds/frame {:.1f} | "
      "shaders this window {} VS {} PS, all {} VS {} PS, library matches {}/{} | "
      "captured draws/frame {:.1f}",
      s.frames / seconds, s.indexed_draws / frames, s.vertex_draws / frames, s.indices / frames,
      s.vertices / frames, primitives, s.clears / frames, s.resolves / frames,
      s.resolves_with_clear_color / frames, resolve_flags, s.texture_binds / frames, s.vertex_shaders.size(), s.pixel_shaders.size(),
      g_all_vertex_shaders.size(), g_all_pixel_shaders.size(), matched,
      g_shader_matches.size(), s.captured_draws / frames);
  g_stats = {};
  g_window_start = now;
}

// NARUTO_UNLOCK_ALL=1 marks every playable character unlocked. The game
// keeps one bit per player id at 0x82A0F508 (sub_821DB0E0, ccIsPlayerUnlock;
// part of the save data at offset 0x1BEA0); ids 16-143 hold the unlockable
// characters. Setting them each frame survives the save being loaded, and
// the next save keeps them.
void UnlockAllPlayers(uint8_t* base) {
  static const bool enabled = [] {
    const char* value = std::getenv("NARUTO_UNLOCK_ALL");
    return value && *value && *value != '0';
  }();
  constexpr uint32_t kPlayerUnlockBits = 0x82A0F508;
  if (enabled) {
    std::memset(base + kPlayerUnlockBits + 2, 0xFF, 16);
  }
}

template <typename Field>
void Count(Field FrameStats::*field) {
  if (!g_enabled) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_stats.*field += 1;
}

}  // namespace

extern "C" {
REX_FUNC(__imp__sub_82746748);
REX_FUNC(__imp__sub_827518D8);
REX_FUNC(__imp__sub_827514C0);
REX_FUNC(__imp__sub_8275CED8);
REX_FUNC(__imp__sub_8275BD48);
REX_FUNC(__imp__sub_8274D2C8);
REX_FUNC(__imp__sub_82749EF0);
REX_FUNC(__imp__sub_8274A0F8);
}

// Patches a vertex shader variant in place for the vertex declaration:
// (device, object, declaration, variant); returns 0 if the GPU may still read
// the microcode.
REX_HOOK_RAW(sub_8274A0F8) {
  uint32_t device = ctx.r3.u32, object = ctx.r4.u32, variant = ctx.r6.u32;
  __imp__sub_8274A0F8(ctx, base);
  if (g_enabled && ctx.r3.u32 && variant < 2) {
    uint32_t size;
    uint32_t code = VariantMicrocode(base, object, variant, &size);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_bound_vertex_shaders[device][variant] = {object, code, size};
  }
}

// Loads a vertex shader variant through the command buffer: (device, ...,
// object r5, ..., variant r10). It copies the microcode there, patches the
// copy and leaves the device's write pointer (+48) on its last word.
REX_HOOK_RAW(sub_82749EF0) {
  uint32_t device = ctx.r3.u32, object = ctx.r5.u32, variant = ctx.r10.u32;
  __imp__sub_82749EF0(ctx, base);
  if (g_enabled && variant < 2) {
    uint32_t size;
    VariantMicrocode(base, object, variant, &size);
    uint32_t end = LoadGuest32(base, device + 48);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_bound_vertex_shaders[device][variant] = {object, end + 4 - size, size};
  }
}

// D3DDevice_Swap(device, front buffer texture, ...)
REX_HOOK_RAW(sub_82746748) {
  UnlockAllPlayers(base);
  if (g_enabled) {
    native_view::FrameInfo frame;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (native_view::Enabled()) {
        frame_capture::FinishFrame(base, ctx.r4.u32, g_frame);
      }
      g_stats.frames++;
      if (g_logging) {
        LogWindow();
      }
      frame = std::move(g_frame);
      g_frame = {};
      g_pass = 0;
    }
    native_view::SubmitFrame(std::move(frame));
  }
  __imp__sub_82746748(ctx, base);
}

// D3DDevice_DrawIndexedVertices(device, primitive type, base vertex, start index, index count)
REX_HOOK_RAW(sub_827518D8) {
  uint32_t device = ctx.r3.u32, primitive_type = ctx.r4.u32, start = ctx.r6.u32,
           count = ctx.r7.u32;
  int32_t base_vertex = ctx.r5.s32;
  __imp__sub_827518D8(ctx, base);
  if (g_enabled) {
    RecordDraw(base, device, primitive_type, true, base_vertex, start, count);
  }
}

// D3DDevice_DrawVertices(device, primitive type, start vertex, vertex count)
REX_HOOK_RAW(sub_827514C0) {
  uint32_t device = ctx.r3.u32, primitive_type = ctx.r4.u32, start = ctx.r5.u32,
           count = ctx.r6.u32;
  __imp__sub_827514C0(ctx, base);
  if (g_enabled) {
    RecordDraw(base, device, primitive_type, false, 0, start, count);
  }
}

// D3DDevice_ClearF(device, flags, rects, color (four floats), Z (f1), stencil
// in r8); D3DDevice_Clear and the engine's clears both end up here.
REX_HOOK_RAW(sub_8275CED8) {
  if (g_enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stats.clears++;
    if (native_view::Enabled()) {
      frame_capture::CaptureClear(base, ctx.r3.u32, ctx.r4.u32, ctx.r6.u32, float(ctx.f1.f64),
                                  ctx.r8.u32, g_pass, g_frame);
    }
  }
  __imp__sub_8275CED8(ctx, base);
}

// D3DDevice_Resolve(device, flags, source rect, destination texture,
// destination point, level, slice or face, clear color, clear Z (f1), ...)
REX_HOOK_RAW(sub_8275BD48) {
  if (g_enabled) {
    uint32_t flags = ctx.r4.u32;
    uint32_t clear_color = ctx.r10.u32;
    float clear_depth = float(ctx.f1.f64);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (native_view::Enabled()) {
      frame_capture::CaptureResolve(base, ctx.r3.u32, flags, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
                                    ctx.r8.u32, ctx.r9.u32, g_pass, g_frame);
    }
    g_stats.resolves++;
    g_stats.resolve_flags[flags]++;
    // The clear arguments only apply with D3DRESOLVE_CLEARRENDERTARGET
    // (0x100) and D3DRESOLVE_CLEARDEPTHSTENCIL (0x200); otherwise they hold
    // leftovers.
    native_view::PassInfo next;
    if ((flags & 0x200) && clear_depth >= 0.0f && clear_depth <= 1.0f) {
      next.clear_depth = clear_depth;
    }
    if (clear_color) {
      g_stats.resolves_with_clear_color++;
    }
    if (clear_color && (flags & 0x100)) {
      next.has_clear_color = true;
      for (int i = 0; i < 4; ++i) {
        next.clear_color[i] = std::bit_cast<float>(LoadGuest32(base, clear_color + 4 * i));
      }
    }
    g_frame.passes.push_back(next);
    g_pass++;
  }
  __imp__sub_8275BD48(ctx, base);
}

// D3DDevice_SetTexture
REX_HOOK_RAW(sub_8274D2C8) {
  Count(&FrameStats::texture_binds);
  __imp__sub_8274D2C8(ctx, base);
}
