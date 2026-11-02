// naruto - the game's shader libraries

#include "shader_library.h"

#include <rex/logging.h>
#include <rex/memory/utils.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

namespace shader_library {

namespace {

std::vector<Shader> g_shaders;

const char* const kLibraries[] = {"nuccMaterial", "nuccPostEffect", "runtime"};
std::filesystem::path g_runtime_dir;

uint32_t Load32(const std::vector<uint8_t>& data, size_t offset) {
  return rex::memory::load_and_swap<uint32_t>(data.data() + offset);
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
}

// The container layout XenosRecomp reads: flags (0x102A11xx, bit 0 set for a
// vertex shader), virtual and physical sizes, then table offsets; the shader
// header at shaderOffset gives the microcode's offset within the physical
// part and its size in bytes.
// A runtime container's id: a hash of its microcode, which also names its
// file.
uint32_t RuntimeId(const uint32_t* words, size_t count) {
  uint32_t hash = 2166136261u;
  for (size_t w = 0; w < count; ++w) {
    hash = (hash ^ words[w]) * 16777619u;
  }
  return hash;
}

// Scans a file of containers. For a runtime container the id from its file
// name replaces the offset.
void ScanLibrary(const std::filesystem::path& path, const std::string& name,
                 uint32_t id = UINT32_MAX) {
  std::vector<uint8_t> data = ReadFile(path);
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
    shader.offset = id != UINT32_MAX ? id : uint32_t(i);
    for (size_t w = 0; w < code_size; w += 4) {
      shader.microcode.push_back(Load32(data, code_start + w));
    }
    // A vertex shader header continues with an element table offset (in
    // words) and count; each element is address | usage << 12 | index << 16.
    if (shader.stage == Stage::kVertex && shader_offset + 36 <= virtual_size) {
      uint32_t table = Load32(data, i + shader_offset + 24);
      uint32_t count = Load32(data, i + shader_offset + 28);
      size_t elements = i + shader_offset + 36 + size_t(table) * 4;
      if (count <= 32 && elements + count * 4 <= i + virtual_size) {
        for (uint32_t e = 0; e < count; ++e) {
          uint32_t value = Load32(data, elements + e * 4);
          shader.vertex_elements.push_back(
              {value & 0xFFF, uint8_t((value >> 12) & 0xF), uint8_t((value >> 16) & 0xF)});
        }
      }
    }
    // The D3DX constant table (after its size word) lists the sampler
    // registers: count at +12, info array offset at +16, 20-byte entries of
    // name, register set (3: sampler), register index.
    uint32_t table = i + Load32(data, i + 16) + 4;
    if (table + 28 <= i + virtual_size) {
      uint32_t count = Load32(data, table + 12);
      size_t info = table + Load32(data, table + 16);
      for (uint32_t c = 0; c < count && info + (c + 1) * 20 <= i + virtual_size; ++c) {
        uint32_t set_and_index = Load32(data, info + c * 20 + 4);
        if ((set_and_index >> 16) == 3) {
          shader.samplers.push_back(uint8_t(set_and_index & 0xFF));
        }
      }
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
  if (id == UINT32_MAX) {
    REXLOG_INFO("[shader library] {}: {} shaders", name, found);
  }
}

// Cache layout: see tools/build-shaders.py.
void LoadCache(const std::filesystem::path& path) {
  std::vector<uint8_t> data = ReadFile(path);
  auto read32 = [&](size_t offset) {
    uint32_t value;
    std::memcpy(&value, data.data() + offset, 4);
    return value;
  };
  if (data.size() < 12 || std::memcmp(data.data(), "NSHC", 4) || read32(4) != 2) {
    REXLOG_WARN("[shader library] no usable shader cache at {}; run tools/build-shaders.py",
                path.string());
    return;
  }
  uint32_t count = read32(8);
  std::map<std::pair<std::string, uint32_t>, std::pair<uint32_t, uint32_t>> blobs;
  for (uint32_t e = 0; e < count && 12 + (e + 1) * 16 <= data.size(); ++e) {
    size_t entry = 12 + e * 16;
    uint8_t library = data[entry];
    uint32_t offset = read32(entry + 4), position = read32(entry + 8), size = read32(entry + 12);
    if (library < std::size(kLibraries) && size % 4 == 0 && position + size <= data.size()) {
      blobs[{kLibraries[library], offset}] = {position, size};
    }
  }
  size_t found = 0;
  for (Shader& shader : g_shaders) {
    auto it = blobs.find({shader.library, shader.offset});
    if (it != blobs.end()) {
      shader.spirv.resize(it->second.second / 4);
      std::memcpy(shader.spirv.data(), data.data() + it->second.first, it->second.second);
      ++found;
    }
  }
  REXLOG_INFO("[shader library] {} of {} shaders have SPIR-V from {}", found, g_shaders.size(),
              path.string());
}

}  // namespace

bool Load(const std::filesystem::path& game_data_root) {
  g_shaders.clear();
  for (const char* name : {kLibraries[0], kLibraries[1]}) {
    ScanLibrary(game_data_root / "data" / "system" / (std::string(name) + "_xbox360.nsh"), name);
  }
  g_runtime_dir = game_data_root.parent_path() / "out" / "shaders" / "runtime";
  std::error_code error;
  size_t runtime = 0;
  for (const auto& entry : std::filesystem::directory_iterator(g_runtime_dir, error)) {
    // vs_XXXXXXXX.bin or ps_XXXXXXXX.bin
    std::string stem = entry.path().stem().string();
    if (entry.path().extension() == ".bin" && stem.size() == 11) {
      ScanLibrary(entry.path(), kLibraries[2], uint32_t(std::strtoul(stem.c_str() + 3, nullptr, 16)));
      ++runtime;
    }
  }
  REXLOG_INFO("[shader library] runtime: {} captured shaders", runtime);
  const char* cache = std::getenv("NARUTO_SHADER_CACHE");
  LoadCache(cache && *cache ? std::filesystem::path(cache)
                            : game_data_root.parent_path() / "out" / "shaders" /
                                  "naruto_shaders.bin");
  return !g_shaders.empty();
}

const std::vector<Shader>& Shaders() {
  return g_shaders;
}

int Match(Stage stage, const uint32_t* words, size_t count) {
  // Compared three words (one instruction, or two control flow
  // instructions) at a time: only vertex fetches (opcode 0 with the
  // must-be-one bit 19) may differ, since Direct3D patches them.
  auto is_vertex_fetch = [](uint32_t w0) { return (w0 & 0x1F) == 0 && ((w0 >> 19) & 1); };
  int best = -1;
  size_t best_diff = SIZE_MAX;
  for (size_t i = 0; i < g_shaders.size(); ++i) {
    const Shader& s = g_shaders[i];
    if (s.stage != stage || s.microcode.size() != count) {
      continue;
    }
    size_t diff = 0;
    bool same = true;
    for (size_t w = 0; w < count && same; w += 3) {
      size_t n = std::min<size_t>(3, count - w);
      if (std::equal(words + w, words + w + n, s.microcode.data() + w)) {
        continue;
      }
      same = stage == Stage::kVertex && n == 3 && is_vertex_fetch(words[w]) &&
             is_vertex_fetch(s.microcode[w]);
      diff += n;
    }
    if (same && diff < best_diff) {
      best = int(i);
      best_diff = diff;
      if (!diff) {
        break;
      }
    }
  }
  return best;
}

void SaveRuntimeContainer(Stage stage, const std::vector<uint8_t>& container,
                          const uint32_t* words, size_t count) {
  if (g_runtime_dir.empty()) {
    return;
  }
  std::error_code error;
  std::filesystem::create_directories(g_runtime_dir, error);
  std::filesystem::path path =
      g_runtime_dir / fmt::format("{}_{:08x}.bin", stage == Stage::kVertex ? "vs" : "ps",
                                  RuntimeId(words, count));
  if (std::filesystem::exists(path, error)) {
    return;
  }
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(container.data()), std::streamsize(container.size()));
  REXLOG_INFO("[shader library] captured {} for tools/build-shaders.py", path.string());
}

}  // namespace shader_library
