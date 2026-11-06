// naruto - Xbox 360 texture decoding for the native view
//
// Reads a texture from guest memory as a texture fetch constant describes it
// and converts it to data a Vulkan image can be filled from: endian-swapped,
// untiled, with rows of blocks packed together. Only the base mip level is
// decoded for now.

#pragma once

#include <cstdint>
#include <vector>

namespace texture_decode {

enum class Dimension { k2D, k3D, kCube };  // 1D textures are 2D, one row high

struct Texture {
  Dimension dimension = Dimension::k2D;
  uint32_t format = 0;  // VkFormat
  uint32_t width = 0, height = 0, depth = 1;
  uint32_t layers = 1;  // 6 for cube maps
  // VkComponentSwizzle for each of the image view's R, G, B and A.
  uint32_t swizzle[4] = {};
  std::vector<uint8_t> data;  // layer after layer (or slice after slice)
};

// Describes the texture without reading it. Returns false (with a reason) for
// textures the view can't use yet.
bool Describe(const uint32_t fetch[6], Texture& texture, const char** reason);

// Fills texture.data from guest memory.
void Decode(const uint8_t* guest_base, const uint32_t fetch[6], Texture& texture);

// The uncompressed VkFormat Decompress turns a block-compressed format into
// (BC1-3 to RGBA8, BC4 to R8, BC5 to R8G8), or 0 for other formats.
uint32_t DecompressedFormat(uint32_t format);

// Converts decoded BC1-5 blocks to plain texels, for GPUs that can't sample
// them (phone GPUs, which have ASTC and ETC2 instead). Changes the format.
void Decompress(Texture& texture);

// Writes an uncompressed 2D texture as a binary PPM (development aid);
// returns false for formats it doesn't handle.
bool WritePreview(const Texture& texture, const char* path);

// A fingerprint of the texture's guest memory, to notice new contents at the
// same address: a hash of all of it up to 1 MB, of 64 bytes in every 512 for
// larger ones.
uint64_t Fingerprint(const uint8_t* guest_base, const uint32_t fetch[6]);

}  // namespace texture_decode
