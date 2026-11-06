// naruto - draw capture for the native view

#include "frame_capture.h"

#include "shader_library.h"
#include "texture_decode.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <rex/logging.h>
#include <rex/memory/utils.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace frame_capture {

namespace {

// Device object fields (docs/d3d-functions.md).
constexpr uint32_t kDeviceFetchConstants = 0x480;
constexpr uint32_t kDeviceVertexConstants = 0x780;
constexpr uint32_t kDevicePixelConstants = 0x1780;
constexpr uint32_t kDeviceIndexBuffer = 0x320C;
constexpr uint32_t kDeviceRenderTargets = 0x3210;  // render target 0-3 surfaces
constexpr uint32_t kDeviceDepthStencil = 0x3220;   // depth/stencil surface
// Boolean constants (registers 0x4900-0x4907): vertex shader b0-b127 in the
// first four words, pixel shader b0-b127 in the next four.
constexpr uint32_t kDeviceBooleans = 0x2780;
constexpr uint32_t kConstantsSize = 256 * 16;

// Register shadows the draw functions flush to the GPU: registers
// 0x2000-0x200F are at 0x2880, 0x2100-0x2114 at 0x28CC, 0x2200-0x220B at
// 0x2934, four bytes each.
constexpr uint32_t kRegSurfaceInfo = 0x2880;   // RB_SURFACE_INFO (0x2000)
constexpr uint32_t kRegColorInfo = 0x2884;     // RB_COLOR_INFO (0x2001)
constexpr uint32_t kRegDepthInfo = 0x2888;     // RB_DEPTH_INFO (0x2002)
constexpr uint32_t kRegColor1Info = 0x288C;    // RB_COLOR1_INFO-RB_COLOR3_INFO (0x2003-5)
constexpr uint32_t kRegBlendControl1 = 0x2958; // RB_BLENDCONTROL1-3 (0x2209-B)
// A texture object's fetch constant (after the D3DResource header and
// MipFlush).
constexpr uint32_t kTextureFetchConstant = 0x1C;
constexpr uint32_t kRegColorMask = 0x28DC;     // RB_COLOR_MASK (0x2104)
constexpr uint32_t kRegAlphaRef = 0x2904;      // RB_ALPHA_REF (0x210E)
constexpr uint32_t kRegViewport = 0x2908;      // PA_CL_VPORT_XSCALE (0x210F) onwards
constexpr uint32_t kRegDepthControl = 0x2934;  // RB_DEPTHCONTROL (0x2200)
constexpr uint32_t kRegBlendControl = 0x2938;  // RB_BLENDCONTROL0 (0x2201)
constexpr uint32_t kRegColorControl = 0x293C;  // RB_COLORCONTROL (0x2202)
constexpr uint32_t kRegScModeControl = 0x2948;  // PA_SU_SC_MODE_CNTL (0x2205)
constexpr uint32_t kRegVteControl = 0x294C;    // PA_CL_VTE_CNTL (0x2206)
constexpr uint32_t kRegModeControl = 0x2954;   // RB_MODECONTROL (0x2208)
constexpr uint32_t kEdramModeDepthOnly = 5;

constexpr uint32_t kRestart = 0xFFFFFFFF;
// Larger index ranges are not worth copying for a debug view.
constexpr uint32_t kMaxVertexRange = 1u << 20;
constexpr size_t kMaxAttributes = 16;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(base + address);
}

// Guest physical memory is mapped 4 GB above the virtual address space.
const uint8_t* Physical(const uint8_t* base, uint32_t address) {
  return base + 0x100000000ull + (address & 0x1FFFFFFF);
}

uint32_t VirtualToPhysical(uint32_t address) {
  return (address & 0x1FFFFFFF) + ((address >> 20) >= 0xE00 ? 0x1000 : 0);
}

uint32_t RawLoad32(const uint8_t* p) {
  uint32_t value;
  std::memcpy(&value, p, 4);
  return value;
}

// Xenos endian modes: none, 8-in-16, 8-in-32, 16-in-32.
uint32_t Swap32(uint32_t value, uint32_t endian) {
  switch (endian) {
    case 1:
      return ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF);
    case 2:
      return std::byteswap(value);
    case 3:
      return std::rotl(value, 16);
    default:
      return value;
  }
}

int32_t SignExtend(uint32_t value, int bits) {
  return int32_t(value << (32 - bits)) >> (32 - bits);
}

void WarnOnce(const char* reason, uint32_t value) {
  static std::set<std::pair<std::string, uint32_t>> warned;
  if (warned.insert({reason, value}).second) {
    REXLOG_WARN("[native view] not drawing: {} ({:#x})", reason, value);
  }
}

uint32_t Align(std::vector<uint8_t>& data, size_t alignment) {
  data.resize((data.size() + alignment - 1) & ~(alignment - 1));
  return uint32_t(data.size());
}

// One loop per endian mode, so the compiler can vectorize each.
template <uint32_t kEndian>
void SwapWords(uint8_t* out, const uint8_t* source, size_t size) {
  for (size_t i = 0; i + 4 <= size; i += 4) {
    uint32_t value = Swap32(RawLoad32(source + i), kEndian);
    std::memcpy(out + i, &value, 4);
  }
}

uint32_t AppendSwapped(std::vector<uint8_t>& data, const uint8_t* source, size_t size,
                       uint32_t endian) {
  uint32_t offset = Align(data, 16);
  data.resize(offset + size);
  uint8_t* out = data.data() + offset;
  switch (endian) {
    case 1:
      SwapWords<1>(out, source, size);
      break;
    case 2:
      SwapWords<2>(out, source, size);
      break;
    case 3:
      SwapWords<3>(out, source, size);
      break;
    default:
      std::memcpy(out, source, size & ~size_t(3));
  }
  return offset;
}

// The format a vertex fetch returns, as a Vulkan vertex format: fraction
// formats normalize, integer formats convert to float unnormalized.
VkFormat VertexFormat(uint32_t format, bool is_signed, bool integer) {
  auto pick = [&](VkFormat unorm, VkFormat snorm, VkFormat uscaled, VkFormat sscaled) {
    return integer ? (is_signed ? sscaled : uscaled) : (is_signed ? snorm : unorm);
  };
  switch (format) {
    case 6:  // 8_8_8_8
      return pick(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM,
                  VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED);
    case 7:  // 2_10_10_10
      return pick(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32,
                  VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32);
    case 25:  // 16_16
      return pick(VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_USCALED,
                  VK_FORMAT_R16G16_SSCALED);
    case 26:  // 16_16_16_16
      return pick(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM,
                  VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED);
    case 31:
      return VK_FORMAT_R16G16_SFLOAT;
    case 32:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 36:
      return VK_FORMAT_R32_SFLOAT;
    case 37:
      return VK_FORMAT_R32G32_SFLOAT;
    case 57:
      return VK_FORMAT_R32G32B32_SFLOAT;
    case 38:
      return VK_FORMAT_R32G32B32A32_SFLOAT;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

// Reads the draw's indices as 32-bit values; strips keep their reset index
// as kRestart.
bool ReadIndices(const uint8_t* base, const DrawCall& call, bool strip,
                 std::vector<uint32_t>& indices) {
  indices.resize(call.count);
  if (!call.indexed) {
    for (uint32_t i = 0; i < call.count; ++i) {
      indices[i] = call.start + i;
    }
    return true;
  }
  uint32_t buffer = Load32(base, call.device + kDeviceIndexBuffer);
  if (!buffer) {
    return false;
  }
  // Index buffer header: Common (bit 31: 32-bit indices, bits 29-30:
  // endian), then the data's address at +0x18.
  uint32_t common = Load32(base, buffer);
  uint32_t endian = (common >> 29) & 3;
  bool wide = common >> 31;
  const uint8_t* data = Physical(base, VirtualToPhysical(Load32(base, buffer + 0x18)));
  if (wide) {
    if (endian != 0 && endian != 2) {
      WarnOnce("32-bit index endian", endian);
      return false;
    }
    for (uint32_t i = 0; i < call.count; ++i) {
      indices[i] = Swap32(RawLoad32(data + size_t(call.start + i) * 4), endian);
    }
  } else {
    if (endian > 1) {
      WarnOnce("16-bit index endian", endian);
      return false;
    }
    for (uint32_t i = 0; i < call.count; ++i) {
      uint16_t value;
      std::memcpy(&value, data + size_t(call.start + i) * 2, 2);
      if (endian) {
        value = std::byteswap(value);
      }
      indices[i] = strip && value == 0xFFFF ? kRestart : value;
    }
  }
  return true;
}

// Constants copied for the frame, reused while the guest's are unchanged.
struct ConstantCopy {
  std::vector<uint8_t> guest;
  uint32_t offset = 0;
};

uint32_t CopyConstants(const uint8_t* source, size_t size, ConstantCopy& copy,
                       std::vector<uint8_t>& data, bool new_frame) {
  if (new_frame || copy.guest.size() != size || std::memcmp(copy.guest.data(), source, size)) {
    copy.guest.assign(source, source + size);
    copy.offset = AppendSwapped(data, source, size, 2);
  }
  return copy.offset;
}

struct Fetch {
  uint32_t location;
  uint32_t fetch_constant;
  uint32_t stride;  // bytes
  int32_t offset;   // bytes
  VkFormat format;
  uint32_t xenos_format;
  uint32_t swizzle;          // destination swizzle Direct3D patched in
  uint32_t library_swizzle;  // the one the converted shader applies
  bool is_signed;
};

// Component count and bytes per component of the vertex formats whose
// components can be reordered; 0 for others.
std::pair<uint32_t, uint32_t> Components(uint32_t xenos_format) {
  switch (xenos_format) {
    case 6:  // 8_8_8_8
      return {4, 1};
    case 25:  // 16_16
    case 31:  // 16_16_FLOAT
      return {2, 2};
    case 26:  // 16_16_16_16
    case 32:  // 16_16_16_16_FLOAT
      return {4, 2};
    case 33:  // 32
    case 36:  // 32_FLOAT
      return {1, 4};
    case 34:  // 32_32
    case 37:  // 32_32_FLOAT
      return {2, 4};
    case 57:  // 32_32_32_FLOAT
      return {3, 4};
    case 35:  // 32_32_32_32
    case 38:  // 32_32_32_32_FLOAT
      return {4, 4};
    default:
      return {0, 0};
  }
}

// The bytes of a component holding 1 in the format.
uint32_t One(const Fetch& fetch) {
  switch (fetch.xenos_format) {
    case 6:
      return fetch.is_signed ? 0x7F : 0xFF;
    case 25:
    case 26:
      return fetch.is_signed ? 0x7FFF : 0xFFFF;
    case 31:
    case 32:
      return 0x3C00;  // half 1.0
    case 33:
    case 34:
    case 35:
      return 1;
    default:
      return 0x3F800000;  // float 1.0
  }
}

template <typename T>
void ReorderComponents(uint8_t* at, const uint8_t* end, size_t step, uint32_t count,
                       const int source[4], T one) {
  for (; at + count * sizeof(T) <= end; at += step) {
    T in[4];
    for (uint32_t c = 0; c < count; ++c) {
      std::memcpy(&in[c], at + c * sizeof(T), sizeof(T));
    }
    for (uint32_t c = 0; c < count; ++c) {
      T value = source[c] >= 0 ? in[source[c]] : source[c] == -2 ? one : T(0);
      std::memcpy(at + c * sizeof(T), &value, sizeof(T));
    }
  }
}

// Reorders an attribute's components so that the converted shader, which
// applies the library microcode's destination swizzle, sees what the patched
// swizzle selects: Direct3D patches the swizzle for the vertex declaration
// (16-bit components of a byte-swapped word come out second first). The
// data itself is laid out as the GPU reads it, first component in the low
// bits. A lane the patched swizzle sets to 0 or 1 gets that constant; one it
// leaves unwritten keeps the register's earlier value, 0 in the converted
// shader, as does a component the format lacks (1 for w).
void Reorder(std::vector<uint8_t>& data, size_t begin, size_t end, uint32_t stride,
             const Fetch& fetch) {
  auto [count, size] = Components(fetch.xenos_format);
  int source[4] = {0, 1, 2, 3};  // data component, or -1 for 0, -2 for 1
  bool identity = true;
  for (uint32_t d = 0; d < 4; ++d) {
    uint32_t wanted = (fetch.library_swizzle >> (3 * d)) & 7;
    uint32_t actual = (fetch.swizzle >> (3 * d)) & 7;
    if (wanted >= count || wanted == actual) {
      continue;
    }
    source[wanted] = actual < count ? int(actual) : (actual == 5 || actual == 3) ? -2 : -1;
    identity = false;
  }
  if (identity || begin >= end) {
    return;
  }
  uint32_t one = One(fetch);
  uint8_t* at = data.data() + begin;
  const uint8_t* last = data.data() + end;
  size_t step = stride ? stride : end - begin;
  switch (size) {
    case 1:
      ReorderComponents<uint8_t>(at, last, step, count, source, uint8_t(one));
      break;
    case 2:
      ReorderComponents<uint16_t>(at, last, step, count, source, uint16_t(one));
      break;
    default:
      ReorderComponents<uint32_t>(at, last, step, count, source, one);
  }
}


// A vertex buffer range a draw reads, and the components to reorder in it.
struct VertexRange {
  uint32_t address;  // physical
  uint32_t size;
  uint32_t endian;
  uint32_t stride;
  std::vector<Fetch> reorders;
};

// Appends a vertex range to data, little-endian and with its components in
// the order the converted shaders expect; returns its offset.
uint32_t AppendVertices(std::vector<uint8_t>& data, const uint8_t* source,
                        const VertexRange& range) {
  uint32_t offset = AppendSwapped(data, source, range.size, range.endian);
  for (const Fetch& fetch : range.reorders) {
    Reorder(data, offset + fetch.offset, offset + range.size, range.stride, fetch);
  }
  return offset;
}

// Frames a vertex range has to stay unchanged before it is cached, and may
// go unused before it is dropped.
constexpr uint64_t kVertexStableFrames = 2;
constexpr uint64_t kVertexIdleFrames = 600;

// Converted vertex ranges, by where they are and how they're converted.
struct CachedVertices {
  uint64_t content = 0;       // hash of the guest bytes
  uint64_t stable_since = 0;  // frame the content last changed
  uint64_t last_used = 0;     // frame
  std::shared_ptr<const native_view::VertexData> data;
  native_view::VertexBinding binding{};  // in frame last_used
};
std::unordered_map<uint64_t, CachedVertices> g_vertex_cache;
uint64_t g_vertex_data_ids = 0;

// The binding for a vertex range: cached data if it has been unchanged for
// a few frames, else a copy in the frame's data. Ranges a frame binds again
// reuse the first binding.
native_view::VertexBinding BindVertices(const uint8_t* base, const VertexRange& range,
                                        native_view::FrameInfo& frame) {
  XXH3_state_t key_state;
  XXH3_64bits_reset(&key_state);
  uint32_t where[4] = {range.address, range.size, range.endian, range.stride};
  XXH3_64bits_update(&key_state, where, sizeof(where));
  for (const Fetch& fetch : range.reorders) {
    uint32_t how[4] = {uint32_t(fetch.offset), fetch.xenos_format | fetch.is_signed << 8,
                       fetch.swizzle, fetch.library_swizzle};
    XXH3_64bits_update(&key_state, how, sizeof(how));
  }
  auto [it, added] = g_vertex_cache.try_emplace(XXH3_64bits_digest(&key_state));
  CachedVertices& cached = it->second;
  if (!added && cached.last_used == frame.number) {
    return cached.binding;
  }
  const uint8_t* source = Physical(base, range.address);
  uint64_t content = XXH3_64bits(source, range.size);
  if (added || content != cached.content) {
    cached.content = content;
    cached.stable_since = frame.number;
    cached.data.reset();
  }
  cached.last_used = frame.number;
  if (!cached.data && frame.number - cached.stable_since >= kVertexStableFrames) {
    auto data = std::make_shared<native_view::VertexData>();
    data->id = ++g_vertex_data_ids;
    AppendVertices(data->bytes, source, range);
    cached.data = std::move(data);
  }
  if (cached.data) {
    cached.binding = {0, range.stride, int32_t(frame.vertex_data.size())};
    frame.vertex_data.push_back(cached.data);
  } else {
    cached.binding = {AppendVertices(frame.data, source, range), range.stride, -1};
  }

  static uint64_t swept = 0;
  if (frame.number >= swept + 300) {
    swept = frame.number;
    std::erase_if(g_vertex_cache, [&](const auto& entry) {
      return entry.second.last_used + kVertexIdleFrames < frame.number;
    });
  }
  return cached.binding;
}

}  // namespace

// The fingerprint of a texture's memory, once per frame for each texture.
uint64_t TextureFingerprint(const uint8_t* base, const uint32_t fetch[6],
                            native_view::FrameInfo& frame) {
  static std::unordered_map<uint64_t, uint64_t> fingerprints;  // by fetch constant
  static uint64_t frame_number = UINT64_MAX;
  if (frame.number != frame_number) {
    frame_number = frame.number;
    fingerprints.clear();
  }
  auto [it, added] = fingerprints.try_emplace(XXH3_64bits(fetch, 6 * sizeof(uint32_t)));
  if (added) {
    it->second = texture_decode::Fingerprint(base, fetch);
  }
  return it->second;
}

bool Capture(const uint8_t* base, const DrawCall& call, native_view::FrameInfo& frame) {
  const auto& shaders = shader_library::Shaders();
  if (call.vertex_shader_index < 0 || (call.pixel_shader && call.pixel_shader_index < 0)) {
    return false;
  }
  const shader_library::Shader& vs = shaders[call.vertex_shader_index];
  if (vs.spirv.empty() ||
      (call.pixel_shader_index >= 0 && shaders[call.pixel_shader_index].spirv.empty())) {
    WarnOnce("shader missing from the shader cache", call.vertex_shader);
    return false;
  }
  if (vs.vertex_elements.size() > kMaxAttributes) {
    WarnOnce("too many vertex elements", uint32_t(vs.vertex_elements.size()));
    return false;
  }

  native_view::Draw draw;
  draw.vertex_shader = call.vertex_shader_index;
  draw.pixel_shader = call.pixel_shader_index;
  draw.pass = call.pass;
  bool strip = false;
  bool quads = false;
  switch (call.primitive_type) {
    case 1:
      draw.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      break;
    case 2:
      draw.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      break;
    case 3:
      draw.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      strip = true;
      break;
    case 4:
      draw.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      break;
    case 5:
      draw.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
      strip = true;
      break;
    case 6:
      draw.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      strip = true;
      break;
    case 13:  // Quad list, drawn as two triangles per quad.
      draw.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      quads = true;
      break;
    default:
      WarnOnce("primitive type", call.primitive_type);
      return false;
  }
  draw.primitive_restart = strip;

  thread_local std::vector<uint32_t> indices;
  if (!ReadIndices(base, call, strip, indices)) {
    return false;
  }
  uint32_t min_index = UINT32_MAX, max_index = 0;
  for (uint32_t index : indices) {
    if (index != kRestart) {
      min_index = std::min(min_index, index);
      max_index = std::max(max_index, index);
    }
  }
  if (min_index > max_index || max_index - min_index >= kMaxVertexRange ||
      int64_t(min_index) + call.base_vertex < 0) {
    return false;
  }
  uint32_t first_vertex = uint32_t(int64_t(min_index) + call.base_vertex);
  uint32_t last_vertex = uint32_t(int64_t(max_index) + call.base_vertex);

  // The vertex fetches come from the bound variant's microcode as Direct3D
  // last patched it for a vertex declaration.
  uint32_t code = call.vertex_code;
  uint32_t code_size = call.vertex_code_size;

  // Decode the patched vertex fetches in program order: a mini fetch reuses
  // the fetch constant and stride of the full fetch before it. Direct3D
  // rewrites the fetches in the vertex declaration's order, so an element's
  // fetch is the one writing its library fetch's destination register and
  // lanes, not necessarily the one at its address.
  struct Patched {
    uint32_t w0, w1, w2;
    uint32_t constant, stride;
    bool used;
  };
  auto destination = [](uint32_t w0, uint32_t w1) {
    uint32_t lanes = 0;
    for (uint32_t c = 0; c < 4; ++c) {
      lanes |= (((w1 >> (3 * c)) & 7) != 7) << c;
    }
    return ((w0 >> 12) & 0x3F) | lanes << 8;  // register and written lanes
  };
  std::vector<uint32_t> order(vs.vertex_elements.size());
  for (uint32_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    return vs.vertex_elements[a].address < vs.vertex_elements[b].address;
  });
  std::vector<Patched> patched;
  bool have_full = false;
  uint32_t full_constant = 0, full_stride = 0;
  for (uint32_t element : order) {
    uint32_t address = vs.vertex_elements[element].address * 12;
    if (address + 12 > code_size) {
      WarnOnce("vertex element outside the microcode", vs.vertex_elements[element].address);
      return false;
    }
    uint32_t w0 = Load32(base, code + address);
    uint32_t w1 = Load32(base, code + address + 4);
    uint32_t w2 = Load32(base, code + address + 8);
    if ((w0 & 0x1F) != 0) {
      WarnOnce("vertex element is not a vertex fetch", w0);
      return false;
    }
    if (!((w1 >> 30) & 1)) {
      have_full = true;
      full_constant = ((w0 >> 20) & 0x1F) * 3 + ((w0 >> 25) & 3);
      full_stride = (w2 & 0xFF) * 4;
    } else if (!have_full) {
      WarnOnce("mini vertex fetch without a full fetch", w1);
      return false;
    }
    patched.push_back({w0, w1, w2, full_constant, full_stride, false});
  }
  std::vector<Fetch> fetches;
  for (uint32_t i = 0; i < order.size(); ++i) {
    uint32_t element = order[i];
    uint32_t address = vs.vertex_elements[element].address * 12;
    uint32_t library_w0 = vs.microcode[address / 4], library_w1 = vs.microcode[address / 4 + 1];
    uint32_t wanted = destination(library_w0, library_w1);
    // The fetch at the element's address if it matches, else the first
    // unused one that does.
    Patched* match = nullptr;
    if (!patched[i].used && destination(patched[i].w0, patched[i].w1) == wanted) {
      match = &patched[i];
    }
    for (uint32_t j = 0; j < patched.size() && !match; ++j) {
      if (!patched[j].used && destination(patched[j].w0, patched[j].w1) == wanted) {
        match = &patched[j];
      }
    }
    if (!match) {
      WarnOnce("no patched vertex fetch for element", vs.vertex_elements[element].address);
      return false;
    }
    match->used = true;
    uint32_t w1 = match->w1, w2 = match->w2;
    if (SignExtend((w1 >> 24) & 0x3F, 6) != 0) {
      WarnOnce("vertex fetch exponent adjust", w1);
      return false;
    }
    uint32_t xenos_format = (w1 >> 16) & 0x3F;
    VkFormat format = VertexFormat(xenos_format, (w1 >> 12) & 1, (w1 >> 13) & 1);
    if (format == VK_FORMAT_UNDEFINED) {
      WarnOnce("vertex format", xenos_format);
      return false;
    }
    int32_t offset = SignExtend((w2 >> 8) & 0x7FFFFF, 23) * 4;
    if (offset < 0 || offset >= 2048) {
      WarnOnce("vertex fetch offset", uint32_t(offset));
      return false;
    }
    fetches.push_back({element, match->constant, match->stride, offset, format, xenos_format,
                       w1 & 0xFFF, library_w1 & 0xFFF, bool((w1 >> 12) & 1)});
  }

  bool new_frame = frame.captured.empty();
  draw.first_vertex = first_vertex;
  draw.vertex_count = last_vertex - first_vertex + 1;
  // The vertex buffers the fetches read, each the range of vertices the
  // indices reach.
  std::vector<VertexRange> ranges;
  std::map<uint32_t, uint32_t> range_of;  // fetch constant -> binding
  draw.first_attribute = uint32_t(frame.attributes.size());
  for (const Fetch& fetch : fetches) {
    auto [it, added] = range_of.try_emplace(fetch.fetch_constant, uint32_t(ranges.size()));
    if (added) {
      // Vertex fetch constant: address | 3 (type), then size in words << 2 |
      // endian; the address is physical.
      uint32_t constant = call.device + kDeviceFetchConstants + fetch.fetch_constant * 8;
      uint32_t d0 = Load32(base, constant), d1 = Load32(base, constant + 4);
      uint32_t size = ((d1 >> 2) & 0xFFFFFF) * 4;
      if ((d0 & 3) != 3) {
        WarnOnce("fetch constant is not a vertex buffer", fetch.fetch_constant);
        frame.attributes.resize(draw.first_attribute);
        return false;
      }
      uint64_t begin = 0, end = std::min<uint64_t>(size, 256);
      if (fetch.stride) {
        begin = uint64_t(first_vertex) * fetch.stride;
        end = std::min<uint64_t>(size, uint64_t(last_vertex + 1) * fetch.stride);
        draw.missing_bytes += uint32_t(uint64_t(last_vertex + 1) * fetch.stride - end);
      }
      if (begin >= end) {
        frame.attributes.resize(draw.first_attribute);
        return false;
      }
      ranges.push_back({(d0 & ~3u) + uint32_t(begin), uint32_t(end - begin), d1 & 3,
                        fetch.stride, {}});
    }
    frame.attributes.push_back({fetch.location, it->second, uint32_t(fetch.offset), fetch.format,
                                fetch.xenos_format, fetch.swizzle, fetch.library_swizzle});
    if (fetch.swizzle != fetch.library_swizzle) {
      if (Components(fetch.xenos_format).first) {
        ranges[it->second].reorders.push_back(fetch);
      } else {
        WarnOnce("vertex fetch swizzle", fetch.swizzle | fetch.xenos_format << 12);
      }
    }
  }
  draw.first_binding = uint32_t(frame.bindings.size());
  for (const VertexRange& range : ranges) {
    frame.bindings.push_back(BindVertices(base, range, frame));
  }
  draw.binding_count = uint32_t(ranges.size());
  draw.attribute_count = uint32_t(frame.attributes.size()) - draw.first_attribute;

  // Indices, rebased to the first copied vertex.
  draw.index_offset = Align(frame.data, 16);
  draw.index_count = uint32_t(quads ? indices.size() / 4 * 6 : indices.size());
  frame.data.resize(draw.index_offset + size_t(draw.index_count) * 4);
  uint8_t* out = frame.data.data() + draw.index_offset;
  auto append_index = [&](uint32_t index) {
    uint32_t value = index == kRestart ? kRestart : index - min_index;
    std::memcpy(out, &value, 4);
    out += 4;
  };
  if (quads) {
    for (size_t q = 0; q + 4 <= indices.size(); q += 4) {
      for (size_t corner : {0, 1, 2, 0, 2, 3}) {
        append_index(indices[q + corner]);
      }
    }
  } else {
    for (uint32_t index : indices) {
      append_index(index);
    }
  }

  thread_local ConstantCopy vertex_constants, pixel_constants, shared_constants;
  draw.vertex_constants = CopyConstants(base + call.device + kDeviceVertexConstants,
                                        kConstantsSize, vertex_constants, frame.data, new_frame);
  draw.pixel_constants = CopyConstants(base + call.device + kDevicePixelConstants,
                                       kConstantsSize, pixel_constants, frame.data, new_frame);

  auto reg = [&](uint32_t offset) { return Load32(base, call.device + offset); };
  draw.surface_info = reg(kRegSurfaceInfo);
  draw.depth_control = reg(kRegDepthControl);
  draw.color_info[0] = reg(kRegColorInfo);
  draw.depth_info = reg(kRegDepthInfo);
  draw.blend_control[0] = reg(kRegBlendControl);
  for (int i = 1; i < 4; ++i) {
    draw.color_info[i] = reg(kRegColor1Info + 4 * (i - 1));
    draw.blend_control[i] = reg(kRegBlendControl1 + 4 * (i - 1));
  }
  draw.color_mask = reg(kRegColorMask);
  draw.vte_control = reg(kRegVteControl);
  draw.mode_control = reg(kRegScModeControl);
  if ((reg(kRegModeControl) & 7) == kEdramModeDepthOnly || call.pixel_shader_index < 0) {
    draw.color_mask = 0;
  }
  for (int i = 0; i < 6; ++i) {
    draw.viewport[i] = std::bit_cast<float>(reg(kRegViewport + 4 * i));
  }

  // Shared constants (XenosRecomp's layout): texture and sampler indices,
  // filled in by the view, then booleans at 256 (vertex shader b0-b15 in the
  // low half, pixel shader b0-b15 in the high half) and the alpha test
  // threshold at 272. Stored guest-endian so the copy swaps them like the
  // other constants.
  uint32_t color_control = reg(kRegColorControl);
  uint32_t alpha_function = color_control & 7;
  draw.alpha_test = (color_control & 8) && (alpha_function == 4 || alpha_function == 6);
  uint8_t shared[288] = {};
  uint32_t booleans = std::byteswap((reg(kDeviceBooleans) & 0xFFFF) |
                                    (reg(kDeviceBooleans + 16) << 16));
  std::memcpy(shared + 256, &booleans, 4);
  if (draw.alpha_test) {
    uint32_t threshold = std::byteswap(reg(kRegAlphaRef));
    std::memcpy(shared + 272, &threshold, 4);
  }
  draw.shared_constants = CopyConstants(shared, sizeof(shared), shared_constants, frame.data,
                                        new_frame);

  // Texture fetch constants (type 2) for the pixel shader's samplers.
  draw.first_texture = uint32_t(frame.textures.size());
  if (call.pixel_shader_index >= 0) {
    for (uint8_t sampler : shaders[call.pixel_shader_index].samplers) {
      if (sampler >= 16) {
        continue;
      }
      native_view::TextureBinding binding{sampler, {}};
      for (int w = 0; w < 6; ++w) {
        binding.fetch[w] = Load32(base, call.device + kDeviceFetchConstants + sampler * 24 + 4 * w);
      }
      if ((binding.fetch[0] & 3) == 2) {
        binding.fingerprint = TextureFingerprint(base, binding.fetch, frame);
        frame.textures.push_back(binding);
      } else {
        static std::set<std::pair<int, uint8_t>> warned;
        if (warned.insert({call.pixel_shader_index, sampler}).second) {
          REXLOG_WARN("[native view] pixel shader {} sampler {} has no texture: fetch {:08x} "
                      "{:08x} {:08x} {:08x} {:08x} {:08x}",
                      call.pixel_shader_index, sampler, binding.fetch[0], binding.fetch[1],
                      binding.fetch[2], binding.fetch[3], binding.fetch[4], binding.fetch[5]);
        }
      }
    }
  }
  draw.texture_count = uint32_t(frame.textures.size()) - draw.first_texture;
  frame.guest_base = base;

  frame.captured.push_back(draw);
  return true;
}

void CaptureResolve(const uint8_t* base, uint32_t device, uint32_t flags, uint32_t rect,
                    uint32_t texture, uint32_t point, uint32_t level, uint32_t slice,
                    uint32_t pass, native_view::FrameInfo& frame) {
  if (!texture) {
    return;
  }
  native_view::Resolve resolve;
  resolve.pass = pass;
  resolve.flags = flags;
  resolve.surface_info = Load32(base, device + kRegSurfaceInfo);
  uint32_t source = flags & 7;
  resolve.source_info = source == 4   ? Load32(base, device + kRegDepthInfo)
                        : source == 0 ? Load32(base, device + kRegColorInfo)
                                      : Load32(base, device + kRegColor1Info + 4 * (source - 1));
  if (rect) {
    resolve.has_rect = true;
    for (int i = 0; i < 4; ++i) {
      resolve.rect[i] = int32_t(Load32(base, rect + 4 * i));
    }
  }
  if (point) {
    resolve.point[0] = int32_t(Load32(base, point));
    resolve.point[1] = int32_t(Load32(base, point + 4));
  }
  resolve.level = level;
  resolve.slice = slice;
  for (int w = 0; w < 6; ++w) {
    resolve.fetch[w] = Load32(base, texture + kTextureFetchConstant + 4 * w);
  }
  frame.resolves.push_back(resolve);
}

void CaptureClear(const uint8_t* base, uint32_t device, uint32_t flags, uint32_t color,
                  float depth, uint32_t stencil, uint32_t pass, native_view::FrameInfo& frame) {
  native_view::Clear clear;
  clear.pass = pass;
  clear.before_draw = uint32_t(frame.captured.size());
  // Only bound targets are cleared; the RB_COLOR*_INFO shadows of unbound
  // ones still describe earlier targets.
  for (int i = 0; i < 4; ++i) {
    if (!Load32(base, device + kDeviceRenderTargets + 4 * i)) {
      flags &= ~(1u << i);
    }
  }
  if (!Load32(base, device + kDeviceDepthStencil)) {
    flags &= ~0x30u;
  }
  clear.flags = flags;
  clear.surface_info = Load32(base, device + kRegSurfaceInfo);
  for (int i = 0; i < 6; ++i) {
    clear.viewport[i] = std::bit_cast<float>(Load32(base, device + kRegViewport + 4 * i));
  }
  clear.color_info[0] = Load32(base, device + kRegColorInfo);
  for (int i = 1; i < 4; ++i) {
    clear.color_info[i] = Load32(base, device + kRegColor1Info + 4 * (i - 1));
  }
  clear.depth_info = Load32(base, device + kRegDepthInfo);
  if (color) {
    for (int i = 0; i < 4; ++i) {
      uint32_t bits = Load32(base, color + 4 * i);
      std::memcpy(&clear.color[i], &bits, 4);
    }
  }
  clear.depth = depth;
  clear.stencil = stencil;
  frame.clears.push_back(clear);
}

void FinishFrame(const uint8_t* base, uint32_t texture, native_view::FrameInfo& frame) {
  if (!texture) {
    return;
  }
  frame.has_front_buffer = true;
  for (int w = 0; w < 6; ++w) {
    frame.front_buffer[w] = Load32(base, texture + kTextureFetchConstant + 4 * w);
  }
}

}  // namespace frame_capture
