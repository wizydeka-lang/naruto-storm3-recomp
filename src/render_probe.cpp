// naruto - Direct3D call probe
//
// First step towards a native renderer: hooks the game's Direct3D entry points
// (config/naruto_d3d_funcs.toml), records what each frame draws and which
// device state it uses, then calls the original function, so the Xenia GPU
// backend still renders. With NARUTO_RENDER_PROBE=1 in the environment, a
// summary is logged every 10 seconds; otherwise the hooks only pass through.

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>

namespace {

// Device object fields (docs/d3d-functions.md).
constexpr uint32_t kDevicePixelShader = 0x330C;
constexpr uint32_t kDeviceVertexShader = 0x3310;

uint32_t LoadGuest32(const uint8_t* base, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(base + address);
}

struct FrameStats {
  uint64_t frames = 0;
  uint64_t indexed_draws = 0;
  uint64_t vertex_draws = 0;
  uint64_t indices = 0;
  uint64_t vertices = 0;
  uint64_t clears = 0;
  uint64_t resolves = 0;
  uint64_t texture_binds = 0;
  std::map<uint32_t, uint64_t> primitive_types;
  std::set<uint32_t> vertex_shaders;
  std::set<uint32_t> pixel_shaders;
};

const bool g_enabled = [] {
  const char* value = std::getenv("NARUTO_RENDER_PROBE");
  return value && *value && *value != '0';
}();

std::mutex g_mutex;
FrameStats g_stats;
std::set<uint32_t> g_all_vertex_shaders;
std::set<uint32_t> g_all_pixel_shaders;
auto g_window_start = std::chrono::steady_clock::now();

void RecordDraw(const uint8_t* base, uint32_t device, uint32_t primitive_type, bool indexed,
                uint32_t count) {
  uint32_t vs = LoadGuest32(base, device + kDeviceVertexShader);
  uint32_t ps = LoadGuest32(base, device + kDevicePixelShader);
  std::lock_guard<std::mutex> lock(g_mutex);
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
  g_all_pixel_shaders.insert(s.pixel_shaders.begin(), s.pixel_shaders.end());
  std::string primitives;
  for (auto [type, n] : s.primitive_types) {
    primitives += fmt::format(" {}:{:.1f}", type, n / frames);
  }
  REXLOG_INFO(
      "[render probe] {:.1f} fps | draws/frame {:.1f} indexed + {:.1f} non-indexed "
      "({:.0f} indices, {:.0f} vertices) | primitive types/frame{} | clears/frame {:.2f} "
      "resolves/frame {:.2f} texture binds/frame {:.1f} | shaders this window {} VS {} PS, "
      "all {} VS {} PS",
      s.frames / seconds, s.indexed_draws / frames, s.vertex_draws / frames, s.indices / frames,
      s.vertices / frames, primitives, s.clears / frames, s.resolves / frames,
      s.texture_binds / frames, s.vertex_shaders.size(), s.pixel_shaders.size(),
      g_all_vertex_shaders.size(), g_all_pixel_shaders.size());
  g_stats = {};
  g_window_start = now;
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
REX_FUNC(__imp__sub_8275D010);
REX_FUNC(__imp__sub_8275BD48);
REX_FUNC(__imp__sub_8274D2C8);
}

// D3DDevice_Swap
REX_HOOK_RAW(sub_82746748) {
  if (g_enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stats.frames++;
    LogWindow();
  }
  __imp__sub_82746748(ctx, base);
}

// D3DDevice_DrawIndexedVertices(device, primitive type, base vertex, start index, index count)
REX_HOOK_RAW(sub_827518D8) {
  if (g_enabled) {
    RecordDraw(base, ctx.r3.u32, ctx.r4.u32, true, ctx.r7.u32);
  }
  __imp__sub_827518D8(ctx, base);
}

// D3DDevice_DrawVertices(device, primitive type, start vertex, vertex count)
REX_HOOK_RAW(sub_827514C0) {
  if (g_enabled) {
    RecordDraw(base, ctx.r3.u32, ctx.r4.u32, false, ctx.r6.u32);
  }
  __imp__sub_827514C0(ctx, base);
}

// D3DDevice_Clear
REX_HOOK_RAW(sub_8275D010) {
  Count(&FrameStats::clears);
  __imp__sub_8275D010(ctx, base);
}

// D3DDevice_Resolve
REX_HOOK_RAW(sub_8275BD48) {
  Count(&FrameStats::resolves);
  __imp__sub_8275BD48(ctx, base);
}

// D3DDevice_SetTexture
REX_HOOK_RAW(sub_8274D2C8) {
  Count(&FrameStats::texture_binds);
  __imp__sub_8274D2C8(ctx, base);
}
