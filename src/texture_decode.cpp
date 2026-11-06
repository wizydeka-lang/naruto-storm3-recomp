// naruto - Xbox 360 texture decoding for the native view

#include "texture_decode.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <string>

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
  bool packed_mips;
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
  fetch.packed_mips = (f[5] >> 11) & 1;
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
  // Where the base level starts, in blocks: with packed mips, a base level
  // 16 texels or less on its shorter side lies in the mip tail, beside (or,
  // for wide textures, below) where the 16x16 mip would be (Xenia's
  // GetPackedMipOffset for level 0).
  uint32_t offset_x = 0, offset_y = 0;
  uint32_t layer_size;  // guest bytes per layer or slice
};

uint32_t Log2Ceil(uint32_t value) { return value <= 1 ? 0 : 32 - std::countl_zero(value - 1); }

Layout LayoutOf(const Fetch& fetch, const FormatInfo& info) {
  Layout layout;
  layout.blocks_w = (fetch.width + info.block - 1) / info.block;
  layout.blocks_h = (fetch.height + info.block - 1) / info.block;
  layout.pitch_blocks = std::max((fetch.pitch + info.block - 1) / info.block, layout.blocks_w);
  uint32_t log2_w = Log2Ceil(fetch.width), log2_h = Log2Ceil(fetch.height);
  if (fetch.packed_mips && fetch.dimension != 0 && std::min(log2_w, log2_h) <= 4) {
    if (log2_w > std::max(log2_h, 1u)) {
      layout.offset_y = 16 / info.block;
    } else {
      layout.offset_x = 16 / info.block;
    }
  }
  uint32_t extent_w = layout.offset_x + layout.blocks_w, extent_h = layout.offset_y + layout.blocks_h;
  layout.pitch_blocks = std::max(layout.pitch_blocks, extent_w);
  uint32_t size;
  if (fetch.tiled) {
    size = ((layout.pitch_blocks + 31) & ~31u) * ((extent_h + 31) & ~31u) * info.bytes;
  } else {
    size = ((layout.pitch_blocks * info.bytes + 255) & ~255u) * extent_h;
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
        uint32_t sx = x + layout.offset_x, sy = y + layout.offset_y;
        uint32_t offset = fetch.tiled ? TiledOffset(sx, sy, layout.pitch_blocks, info.bytes) *
                                            info.bytes
                                      : sy * linear_row + sx * info.bytes;
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

namespace {

// The four colours of a BC1 colour block, as RGBA8. BC2 and BC3 blocks always
// use four opaque colours; BC1 uses three and transparent black when the
// first endpoint isn't the greater.
void ColorBlock(const uint8_t* block, bool bc1, uint8_t colors[4][4]) {
  uint16_t c0 = uint16_t(block[0] | block[1] << 8), c1 = uint16_t(block[2] | block[3] << 8);
  for (int e = 0; e < 2; ++e) {
    uint16_t c = e ? c1 : c0;
    colors[e][0] = uint8_t((c >> 11) * 255 / 31);
    colors[e][1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    colors[e][2] = uint8_t((c & 31) * 255 / 31);
    colors[e][3] = 255;
  }
  for (int i = 0; i < 4; ++i) {
    if (c0 > c1 || !bc1) {
      colors[2][i] = uint8_t((2 * colors[0][i] + colors[1][i]) / 3);
      colors[3][i] = uint8_t((colors[0][i] + 2 * colors[1][i]) / 3);
    } else {
      colors[2][i] = uint8_t((colors[0][i] + colors[1][i]) / 2);
      colors[3][i] = 0;
    }
  }
}

// The 16 values of a BC3 alpha (or BC4/BC5 channel) block.
void ValueBlock(const uint8_t* block, uint8_t values[16]) {
  uint8_t v[8] = {block[0], block[1]};
  for (int i = 2; i < 8; ++i) {
    v[i] = v[0] > v[1] ? uint8_t(((8 - i) * v[0] + (i - 1) * v[1]) / 7)
           : i < 6     ? uint8_t(((6 - i) * v[0] + (i - 1) * v[1]) / 5)
           : i == 6    ? 0
                       : 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) {
    bits |= uint64_t(block[2 + i]) << (8 * i);
  }
  for (int i = 0; i < 16; ++i) {
    values[i] = v[(bits >> (3 * i)) & 7];
  }
}

}  // namespace

uint32_t DecompressedFormat(uint32_t format) {
  switch (format) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_BC4_UNORM_BLOCK:
      return VK_FORMAT_R8_UNORM;
    case VK_FORMAT_BC5_UNORM_BLOCK:
      return VK_FORMAT_R8G8_UNORM;
    default:
      return 0;
  }
}

void Decompress(Texture& texture) {
  uint32_t plain = DecompressedFormat(texture.format);
  if (!plain) {
    return;
  }
  uint32_t texel = plain == VK_FORMAT_R8G8B8A8_UNORM ? 4 : plain == VK_FORMAT_R8G8_UNORM ? 2 : 1;
  uint32_t block_bytes = texture.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ||
                                 texture.format == VK_FORMAT_BC4_UNORM_BLOCK
                             ? 8
                             : 16;
  uint32_t blocks_w = (texture.width + 3) / 4, blocks_h = (texture.height + 3) / 4;
  uint32_t images = texture.layers * texture.depth;
  std::vector<uint8_t> out(size_t(texture.width) * texture.height * texel * images);
  const uint8_t* block = texture.data.data();
  for (uint32_t image = 0; image < images; ++image) {
    uint8_t* base = out.data() + size_t(texture.width) * texture.height * texel * image;
    for (uint32_t by = 0; by < blocks_h; ++by) {
      for (uint32_t bx = 0; bx < blocks_w; ++bx, block += block_bytes) {
        uint8_t texels[16][4] = {};
        switch (texture.format) {
          case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
          case VK_FORMAT_BC2_UNORM_BLOCK:
          case VK_FORMAT_BC3_UNORM_BLOCK: {
            bool bc1 = texture.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
            const uint8_t* color = bc1 ? block : block + 8;
            uint8_t colors[4][4];
            ColorBlock(color, bc1, colors);
            uint32_t indices = uint32_t(color[4] | color[5] << 8 | color[6] << 16 | color[7] << 24);
            for (int i = 0; i < 16; ++i) {
              std::memcpy(texels[i], colors[(indices >> (2 * i)) & 3], 4);
            }
            if (texture.format == VK_FORMAT_BC2_UNORM_BLOCK) {
              for (int i = 0; i < 16; ++i) {
                texels[i][3] = uint8_t(((block[i / 2] >> (4 * (i & 1))) & 15) * 17);
              }
            } else if (texture.format == VK_FORMAT_BC3_UNORM_BLOCK) {
              uint8_t alpha[16];
              ValueBlock(block, alpha);
              for (int i = 0; i < 16; ++i) {
                texels[i][3] = alpha[i];
              }
            }
            break;
          }
          default: {  // BC4, BC5: one or two channels
            uint8_t values[16];
            for (uint32_t c = 0; c < texel; ++c) {
              ValueBlock(block + 8 * c, values);
              for (int i = 0; i < 16; ++i) {
                texels[i][c] = values[i];
              }
            }
            break;
          }
        }
        // The block's texels inside the texture (edge blocks overhang).
        for (uint32_t y = 0; y < 4 && by * 4 + y < texture.height; ++y) {
          for (uint32_t x = 0; x < 4 && bx * 4 + x < texture.width; ++x) {
            std::memcpy(base + (size_t(by * 4 + y) * texture.width + bx * 4 + x) * texel,
                        texels[y * 4 + x], texel);
          }
        }
      }
    }
  }
  texture.data = std::move(out);
  texture.format = plain;
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
  // The alpha the view samples, beside it as a PGM.
  FILE* alpha = std::fopen((std::string(path) + ".alpha.pgm").c_str(), "wb");
  if (alpha) {
    std::fprintf(alpha, "P5 %u %u 255\n", texture.width, texture.height);
  }
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
    if (alpha) {
      uint32_t s = texture.swizzle[3];
      uint8_t a = s >= VK_COMPONENT_SWIZZLE_R ? rgba[s - VK_COMPONENT_SWIZZLE_R]
                  : s == VK_COMPONENT_SWIZZLE_ONE ? 255 : 0;
      std::fputc(a, alpha);
    }
  }
  std::fclose(file);
  if (alpha) {
    std::fclose(alpha);
  }
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
  // Textures up to 1 MB are hashed whole: games rewrite parts of a texture
  // in place, such as an eye's iris, or a character portrait streamed into
  // an atlas as the cursor nears it. Larger ones are hashed 64 bytes in
  // every 512, close enough that no rewritten block of a tile is missed.
  constexpr uint64_t kWholeBytes = 1 << 20;
  if (span <= kWholeBytes) {
    return XXH3_64bits(data, span);
  }
  constexpr uint32_t kStride = 512, kSampleBytes = 64;
  XXH3_state_t state;
  XXH3_64bits_reset(&state);
  for (uint64_t at = 0; at + kSampleBytes <= span; at += kStride) {
    XXH3_64bits_update(&state, data + at, kSampleBytes);
  }
  return XXH3_64bits_digest(&state);
}

}  // namespace texture_decode
