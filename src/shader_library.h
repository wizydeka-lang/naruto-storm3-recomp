// naruto - the game's shader libraries
//
// Most of the game's shaders are Xbox 360 shader containers in two files,
// data/system/nuccMaterial_xbox360.nsh and nuccPostEffect_xbox360.nsh; the
// rest (2D and UI shaders among them) come from packed archives and are
// captured from memory into out/shaders/runtime/ when first bound. The native
// renderer uses one precompiled host shader per container, so every shader the
// game binds has to be matched to its container. Vertex shaders are bound in
// variants whose vertex fetch instructions Direct3D patched for the vertex
// layout, so matching ignores vertex fetch instructions.

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
  std::string library;  // "nuccMaterial", "nuccPostEffect" or "runtime"
  uint32_t offset;      // container offset in the library file; for runtime, its id
  std::vector<uint32_t> microcode;  // host-endian instruction words
  std::vector<VertexElement> vertex_elements;
  std::vector<uint8_t> samplers;  // sampler registers, which are fetch constant slots
  std::vector<uint32_t> spirv;  // from the shader cache; empty if not built
};

// Loads both libraries from the game's data folder, the captured containers
// in out/shaders/runtime/ next to it, and the SPIR-V shader cache
// tools/build-shaders.py writes (NARUTO_SHADER_CACHE, or
// out/shaders/naruto_shaders.bin). Returns false if no shader was found.
bool Load(const std::filesystem::path& game_data_root);

const std::vector<Shader>& Shaders();

// Index of the library shader matching the given microcode, or -1. words are
// host-endian.
int Match(Stage stage, const uint32_t* words, size_t count);

// Saves a container captured from memory to out/shaders/runtime/ (once per
// shader) for tools/build-shaders.py to convert; the next run can match it.
void SaveRuntimeContainer(Stage stage, const std::vector<uint8_t>& container,
                          const uint32_t* words, size_t count);

}  // namespace shader_library
