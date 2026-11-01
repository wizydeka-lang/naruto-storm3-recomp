// naruto - the game's shader libraries

#include "shader_library.h"

#include <rex/logging.h>
#include <rex/memory/utils.h>

#include <algorithm>
#include <fstream>
#include <iterator>

namespace shader_library {

namespace {

std::vector<Shader> g_shaders;

uint32_t Load32(const std::vector<uint8_t>& data, size_t offset) {
  return rex::memory::load_and_swap<uint32_t>(data.data() + offset);
}

// The container layout XenosRecomp reads: flags (0x102A11xx, bit 0 set for a
// vertex shader), virtual and physical sizes, then table offsets; the shader
// header at shaderOffset gives the microcode's offset within the physical
// part and its size in bytes.
void ScanLibrary(const std::filesystem::path& path, const std::string& name) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
  size_t found = 0;
  for (size_t i = 0; i + 36 <= data.size(); i += 4) {
    uint32_t flags = Load32(data, i);
    if ((flags & 0xFFFFFF00) != 0x102A1100 || Load32(data, i + 28) || Load32(data, i + 32)) {
      continue;
    }
    uint32_t virtual_size = Load32(data, i + 4);
    uint32_t physical_size = Load32(data, i + 8);
    uint32_t shader_offset = Load32(data, i + 24);
    size_t total = size_t(virtual_size) + physical_size;
    if (!total || total > data.size() - i || shader_offset + 8 > virtual_size) {
      continue;
    }
    uint32_t code_offset = Load32(data, i + shader_offset);
    uint32_t code_size = Load32(data, i + shader_offset + 4);
    size_t code_start = i + virtual_size + code_offset;
    if (!code_size || code_size % 4 || code_start + code_size > i + total) {
      continue;
    }
    Shader shader;
    shader.stage = (flags & 1) ? Stage::kVertex : Stage::kPixel;
    shader.library = name;
    shader.offset = uint32_t(i);
    for (size_t w = 0; w < code_size; w += 4) {
      shader.microcode.push_back(Load32(data, code_start + w));
    }
    bool duplicate = std::any_of(g_shaders.begin(), g_shaders.end(), [&](const Shader& s) {
      return s.stage == shader.stage && s.microcode == shader.microcode;
    });
    if (!duplicate) {
      g_shaders.push_back(std::move(shader));
      ++found;
    }
    i += total - 4;
  }
  REXLOG_INFO("[shader library] {}: {} shaders", name, found);
}

}  // namespace

bool Load(const std::filesystem::path& game_data_root) {
  g_shaders.clear();
  for (const char* name : {"nuccMaterial", "nuccPostEffect"}) {
    ScanLibrary(game_data_root / "data" / "system" / (std::string(name) + "_xbox360.nsh"), name);
  }
  return !g_shaders.empty();
}

const std::vector<Shader>& Shaders() {
  return g_shaders;
}

int Match(Stage stage, const uint32_t* words, size_t count) {
  // Patched vertex fetch instructions changed at most 37 words in the shaders
  // measured; anything closer than a quarter of the shader is the same one.
  size_t limit = std::max<size_t>(40, count / 4);
  int best = -1;
  size_t best_diff = limit + 1;
  for (size_t i = 0; i < g_shaders.size(); ++i) {
    const Shader& s = g_shaders[i];
    if (s.stage != stage || s.microcode.size() != count) {
      continue;
    }
    size_t diff = 0;
    for (size_t w = 0; w < count && diff < best_diff; ++w) {
      diff += s.microcode[w] != words[w];
    }
    if (diff < best_diff) {
      best = int(i);
      best_diff = diff;
      if (!diff) {
        break;
      }
    }
  }
  return best;
}

}  // namespace shader_library
