// naruto - Xbox 360 texture decoding for the native view

#include "texture_decode.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace texture_decode {

namespace {

struct FormatInfo {
  uint32_t xenos;
  VkFormat format;
  uint32_t block;  // block width and height in texels
  uint32_t bytes;  // bytes per block
  // The image channel (R, G, B, A) holding each Xenos component (X, Y, Z, W).
  uint8_t channel_of[4];
};

constexpr uint8_t kRGBA[4] = {0, 1, 2, 3};

// Xenos texture formats the view understands; the _AS_16_16_16_16 variants
// only change filtering precision.
constexpr FormatInfo kFormats[] = {
    {2, VK_FORMAT_R8_UNORM, 1, 1, {0, 1, 2, 3}},                    // 8
    {3, VK_FORMAT_A1R5G5B5_UNORM_PACK16, 1, 2, {2, 1, 0, 3}},       // 1_5_5_5
    {4, VK_FORMAT_B5G6R5_UNORM_PACK16, 1, 2, {0, 1, 2, 3}},         // 5_6_5
    {6, VK_FORMAT_R8G8B8A8_UNORM, 1, 4, {0, 1, 2, 3}},              // 8_8_8_8
    {7, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, 4, {0, 1, 2, 3}},    // 2_10_10_10
    {10, VK_FORMAT_R8G8_UNORM, 1, 2, {0, 1, 2, 3}},                 // 8_8
    {15, VK_FORMAT_R4G4B4A4_UNORM_PACK16, 1, 2, {3, 2, 1, 0}},      // 4_4_4_4
    {18, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 8, {0, 1, 2, 3}},       // DXT1
    {19, VK_FORMAT_BC2_UNORM_BLOCK, 4, 16, {0, 1, 2, 3}},           // DXT2_3
    {20, VK_FORMAT_BC3_UNORM_BLOCK, 4, 16, {0, 1, 2, 3}},           // DXT4_5
    {25, VK_FORMAT_R16G16_UNORM, 1, 4, {0, 1, 2, 3}},               // 16_16
    {26, VK_FORMAT_R16G16B16A16_UNORM, 1, 8, {0, 1, 2, 3}},         // 16_16_16_16
    // The _EXPAND formats read 16-bit values as floats; render target copies
    // of 16-bit float targets use them.
    {27, VK_FORMAT_R16_SFLOAT, 1, 2, {0, 1, 2, 3}},                 // 16_EXPAND
    {28, VK_FORMAT_R16G16_SFLOAT, 1, 4, {0, 1, 2, 3}},              // 16_16_EXPAND
    {29, VK_FORMAT_R16G16B16A16_SFLOAT, 1, 8, {0, 1, 2, 3}},        // 16_16_16_16_EXPAND
    {30, VK_FORMAT_R16_SFLOAT, 1, 2, {0, 1, 2, 3}},                 // 16_FLOAT
    {31, VK_FORMAT_R16G16_SFLOAT, 1, 4, {0, 1, 2, 3}},              // 16_16_FLOAT
    {32, VK_FORMAT_R16G16B16A16_SFLOAT, 1, 8, {0, 1, 2, 3}},        // 16_16_16_16_FLOAT
    {36, VK_FORMAT_R32_SFLOAT, 1, 4, {0, 1, 2, 3}},                 // 32_FLOAT
    {37, VK_FORMAT_R32G32_SFLOAT, 1, 8, {0, 1, 2, 3}},              // 32_32_FLOAT
    {38, VK_FORMAT_R32G32B32A32_SFLOAT, 1, 16, {0, 1, 2, 3}},       // 32_32_32_32_FLOAT
    {49, VK_FORMAT_BC5_UNORM_BLOCK, 4, 16, {0, 1, 2, 3}},           // DXN
    {50, VK_FORMAT_R8G8B8A8_UNORM, 1, 4, {0, 1, 2, 3}},             // 8_8_8_8_AS_16_16_16_16
    {51, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 8, {0, 1, 2, 3}},       // DXT1_AS_16_16_16_16
    {52, VK_FORMAT_BC2_UNORM_BLOCK, 4, 16, {0, 1, 2, 3}},           // DXT2_3_AS_16_16_16_16
    {53, VK_FORMAT_BC3_UNORM_BLOCK, 4, 16, {0, 1, 2, 3}},           // DXT4_5_AS_16_16_16_16
    {54, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, 4, {0, 1, 2, 3}},   // 2_10_10_10_AS_16_16_16_16
    {59, VK_FORMAT_BC4_UNORM_BLOCK, 4, 8, {0, 1, 2, 3}},            // DXT5A
};

const FormatInfo* Find(uint32_t xenos) {
  for (const FormatInfo& info : kFormats) {
    if (info.xenos == xenos) {
      return &info;
    }
  }
  return nullptr;
}

// Texture fetch constant fields (Xenia's xe_gpu_texture_fetch_t).
struct Fetch {
  uint32_t format, endian, dimension;
  bool tiled;
  uint32_t pitch;  // texels
  uint32_t address;  // physical
  uint32_t width, height, depth;
  uint32_t swizzle;
};

Fetch Parse(const uint32_t f[6]) {
  Fetch fetch;
  fetch.tiled = f[0] >> 31;
  fetch.pitch = ((f[0] >> 22) & 0x1FF) * 32;
  fetch.format = f[1] & 0x3F;
  fetch.endian = (f[1] >> 6) & 3;
  fetch.address = f[1] & 0xFFFFF000;
  fetch.swizzle = (f[3] >> 1) & 0xFFF;
  fetch.dimension = (f[5] >> 9) & 3;
  fetch.depth = 1;
  switch (fetch.dimension) {
    case 0:  // 1D
      fetch.width = (f[2] & 0xFFFFFF) + 1;
      fetch.height = 1;
      break;
    case 2:  // 3D
      fetch.width = (f[2] & 0x7FF) + 1;
      fetch.height = ((f[2] >> 11) & 0x7FF) + 1;
      fetch.depth = (f[2] >> 22) + 1;
      break;
    default:  // 2D, cube
      fetch.width = (f[2] & 0x1FFF) + 1;
      fetch.height = ((f[2] >> 13) & 0x1FFF) + 1;
      break;
  }
  return fetch;
}

// Block index of block (x, y) in a tiled surface pitch blocks wide (the
// XGAddress2DTiledOffset formula).
uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t bytes_per_block) {
  uint32_t aligned = (pitch + 31) & ~31u;
  uint32_t log_bpp = (bytes_per_block >> 2) + ((bytes_per_block >> 1) >> (bytes_per_block >> 2));
  uint32_t macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log_bpp + 7);
  uint32_t micro = ((x & 7) + ((y & 6) << 2)) << log_bpp;
  uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + log_bpp)) +
                    ((y & 1) << 4);
  return (((offset & ~511u) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) +
          (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >>
         log_bpp;
}

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

struct Layout {
  uint32_t blocks_w, blocks_h, pitch_blocks;
  uint32_t layer_size;  // guest bytes per layer or slice
};

Layout LayoutOf(const Fetch& fetch, const FormatInfo& info) {
  Layout layout;
  layout.blocks_w = (fetch.width + info.block - 1) / info.block;
  layout.blocks_h = (fetch.height + info.block - 1) / info.block;
  layout.pitch_blocks = std::max((fetch.pitch + info.block - 1) / info.block, layout.blocks_w);
  uint32_t size;
  if (fetch.tiled) {
    size = ((layout.pitch_blocks + 31) & ~31u) * ((layout.blocks_h + 31) & ~31u) * info.bytes;
  } else {
    size = ((layout.pitch_blocks * info.bytes + 255) & ~255u) * layout.blocks_h;
  }
  layout.layer_size = (size + 4095) & ~4095u;
  return layout;
}

const uint8_t* Physical(const uint8_t* guest_base, uint32_t address) {
  return guest_base + 0x100000000ull + (address & 0x1FFFFFFF);
}

}  // namespace

bool Describe(const uint32_t fetch_words[6], Texture& texture, const char** reason) {
  Fetch fetch = Parse(fetch_words);
  const FormatInfo* info = Find(fetch.format);
  if (!info) {
    *reason = "texture format";
    return false;
  }
  if (fetch.dimension == 2 && fetch.tiled) {
    *reason = "tiled 3D texture";
    return false;
  }
  if (fetch.width > 8192 || fetch.height > 8192) {
    *reason = "texture size";
    return false;
  }
  texture.dimension = fetch.dimension == 2   ? Dimension::k3D
                      : fetch.dimension == 3 ? Dimension::kCube
                                             : Dimension::k2D;
  texture.format = info->format;
  texture.width = fetch.width;
  texture.height = fetch.height;
  texture.depth = fetch.dimension == 2 ? fetch.depth : 1;
  texture.layers = fetch.dimension == 3 ? 6 : 1;
  Layout layout = LayoutOf(fetch, *info);
  uint64_t end = uint64_t(fetch.address & 0x1FFFFFFF) +
                 uint64_t(layout.layer_size) * texture.layers * texture.depth;
  if (end > 0x20000000) {
    *reason = "texture outside physical memory";
    return false;
  }
  // Fetch swizzle per output component: 0-3 select X-W, 4 is 0, 5 is 1.
  for (int c = 0; c < 4; ++c) {
    uint32_t select = (fetch.swizzle >> (3 * c)) & 7;
    texture.swizzle[c] = select < 4   ? VK_COMPONENT_SWIZZLE_R + info->channel_of[select]
                         : select == 5 ? VK_COMPONENT_SWIZZLE_ONE
                                       : VK_COMPONENT_SWIZZLE_ZERO;
  }
  return true;
}

void Decode(const uint8_t* guest_base, const uint32_t fetch_words[6], Texture& texture) {
  Fetch fetch = Parse(fetch_words);
  const FormatInfo& info = *Find(fetch.format);
  Layout layout = LayoutOf(fetch, info);
  uint32_t images = texture.layers * texture.depth;
  uint32_t row = layout.blocks_w * info.bytes;
  uint32_t linear_row = (layout.pitch_blocks * info.bytes + 255) & ~255u;
  texture.data.resize(size_t(row) * layout.blocks_h * images);
  uint8_t* out = texture.data.data();
  for (uint32_t image = 0; image < images; ++image) {
    const uint8_t* source = Physical(guest_base, fetch.address + image * layout.layer_size);
    for (uint32_t y = 0; y < layout.blocks_h; ++y) {
      for (uint32_t x = 0; x < layout.blocks_w; ++x, out += info.bytes) {
        uint32_t offset = fetch.tiled ? TiledOffset(x, y, layout.pitch_blocks, info.bytes) *
                                            info.bytes
                                      : y * linear_row + x * info.bytes;
        if (info.bytes >= 4) {
          for (uint32_t w = 0; w < info.bytes; w += 4) {
            uint32_t value;
            std::memcpy(&value, source + offset + w, 4);
            value = Swap32(value, fetch.endian);
            std::memcpy(out + w, &value, 4);
          }
        } else {
          // Smaller texels are swapped with the 32-bit word holding them.
          uint32_t word;
          std::memcpy(&word, source + (offset & ~3u), 4);
          word = Swap32(word, fetch.endian);
          std::memcpy(out, reinterpret_cast<const uint8_t*>(&word) + (offset & 3), info.bytes);
        }
      }
    }
  }
}

bool WritePreview(const Texture& texture, const char* path) {
  if (texture.dimension != Dimension::k2D ||
      (texture.format != VK_FORMAT_B5G6R5_UNORM_PACK16 &&
       texture.format != VK_FORMAT_R8G8B8A8_UNORM)) {
    return false;
  }
  FILE* file = std::fopen(path, "wb");
  if (!file) {
    return false;
  }
  std::fprintf(file, "P6 %u %u 255\n", texture.width, texture.height);
  for (uint32_t i = 0; i < texture.width * texture.height; ++i) {
    uint8_t rgba[4];
    if (texture.format == VK_FORMAT_R8G8B8A8_UNORM) {
      std::memcpy(rgba, texture.data.data() + i * 4, 4);
    } else {
      uint16_t v;
      std::memcpy(&v, texture.data.data() + i * 2, 2);
      rgba[0] = uint8_t((v & 31) * 255 / 31);
      rgba[1] = uint8_t(((v >> 5) & 63) * 255 / 63);
      rgba[2] = uint8_t((v >> 11) * 255 / 31);
      rgba[3] = 255;
    }
    // Apply the view's swizzle (R, G, B selectors).
    uint8_t out[3];
    for (int c = 0; c < 3; ++c) {
      uint32_t s = texture.swizzle[c];
      out[c] = s >= VK_COMPONENT_SWIZZLE_R ? rgba[s - VK_COMPONENT_SWIZZLE_R]
               : s == VK_COMPONENT_SWIZZLE_ONE ? 255 : 0;
    }
    std::fwrite(out, 1, 3, file);
  }
  std::fclose(file);
  return true;
}

uint64_t Fingerprint(const uint8_t* guest_base, const uint32_t fetch_words[6]) {
  Fetch fetch = Parse(fetch_words);
  const FormatInfo* info = Find(fetch.format);
  if (!info) {
    return 0;
  }
  uint64_t span = uint64_t(LayoutOf(fetch, *info).layer_size) * (fetch.dimension == 3 ? 6 : 1) *
                  (fetch.dimension == 2 ? fetch.depth : 1);
  if ((fetch.address & 0x1FFFFFFF) + span > 0x20000000) {
    return 0;
  }
  const uint8_t* data = Physical(guest_base, fetch.address);
  // Small textures are hashed whole: games swap small details, such as an
  // eye's iris, by rewriting a few blocks of a texture in place. Larger
  // ones are sampled evenly.
  constexpr uint64_t kWholeBytes = 256 << 10;
  if (span <= kWholeBytes) {
    return XXH3_64bits(data, span);
  }
  constexpr uint32_t kSamples = 512, kSampleBytes = 64;
  XXH3_state_t state;
  XXH3_64bits_reset(&state);
  for (uint32_t s = 0; s < kSamples; ++s) {
    uint64_t at = std::min<uint64_t>(span * s / kSamples, span - kSampleBytes);
    XXH3_64bits_update(&state, data + at, kSampleBytes);
  }
  return XXH3_64bits_digest(&state);
}

}  // namespace texture_decode
