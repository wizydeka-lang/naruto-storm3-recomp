// naruto - the game's shader libraries
//
// All of the game's shaders are Xbox 360 shader containers in two files,
// data/system/nuccMaterial_xbox360.nsh and nuccPostEffect_xbox360.nsh. The
// native renderer uses one precompiled host shader per container, so every
// shader the game binds has to be matched to its container. Vertex shaders are
// bound in variants whose vertex fetch instructions Direct3D patched for the
// vertex layout, so matching tolerates a few differing words.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace shader_library {

enum class Stage { kVertex, kPixel };

// A vertex shader input: the vertex fetch instruction at instruction index
// address reads it. The converted shader declares element i at location i.
struct VertexElement {
  uint32_t address;
  uint8_t usage;  // D3DDECLUSAGE
  uint8_t usage_index;
};

struct Shader {
  Stage stage;
  std::string library;  // "nuccMaterial" or "nuccPostEffect"
  uint32_t offset;      // container offset in the library file
  std::vector<uint32_t> microcode;  // host-endian instruction words
  std::vector<VertexElement> vertex_elements;
  std::vector<uint32_t> spirv;  // from the shader cache; empty if not built
};

// Loads both libraries from the game's data folder, and the SPIR-V shader
// cache tools/build-shaders.py writes (NARUTO_SHADER_CACHE, or
// out/shaders/naruto_shaders.bin next to the game folder). Returns false if
// neither library was found.
bool Load(const std::filesystem::path& game_data_root);

const std::vector<Shader>& Shaders();

// Index of the library shader matching the given microcode, or -1. words are
// host-endian.
int Match(Stage stage, const uint32_t* words, size_t count);

}  // namespace shader_library
