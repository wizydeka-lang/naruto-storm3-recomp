// naruto - 24-bit depth to float for the native view's depth resolves
//
// When the depth buffer is X8_D24 (as on the Xbox), copying it to a buffer
// gives 24-bit unorm values in the low bits of each word. This converts them
// in place to floats, which the resolve then copies into its R32_SFLOAT
// texture. tools/build-postfx.py compiles it to src/postfx_spirv.h.

struct Constants {
  uint64_t address;  // the scratch buffer
  uint count;        // words to convert
};
[[vk::push_constant]] Constants g_constants;

[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
  uint index = id.y * 65535 * 64 + id.x;
  if (index >= g_constants.count) {
    return;
  }
  uint64_t address = g_constants.address + uint64_t(index) * 4;
  uint value = vk::RawBufferLoad<uint>(address);
  vk::RawBufferStore<float>(address, float(value & 0xFFFFFF) / 16777215.0);
}
