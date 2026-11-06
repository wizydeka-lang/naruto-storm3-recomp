// naruto - AMD FidelityFX Contrast Adaptive Sharpening for the native view
//
// A compute shader that sharpens the presented frame into g_output, scaling
// it up on the way when built with CAS_SCALE=1 (CAS only takes that choice
// as a constant). The algorithm is AMD's, from third_party/ffx-cas (MIT); the
// view computes the constants (CasSetup). tools/build-postfx.py compiles both
// variants to src/postfx_spirv.h.

#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"

struct Constants {
  uint4 con0, con1, con2, con3;  // con0, con1: CasSetup's const0, const1
  uint4 rcas;
  uint4 size;  // xy: output size, zw: input size
};
[[vk::push_constant]] Constants g_constants;
[[vk::binding(0)]] Texture2D<float4> g_input;
[[vk::binding(1)]] SamplerState g_sampler;
[[vk::binding(2)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> g_output;

AF3 CasLoad(ASU2 p) {
  return g_input.Load(int3(clamp(p, ASU2(0, 0), ASU2(g_constants.size.zw) - 1), 0)).rgb;
}
void CasInput(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_cas.h"

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= g_constants.size.xy)) {
    return;
  }
  AF1 r, g, b;
#if CAS_SCALE
  CasFilter(r, g, b, id.xy, g_constants.con0, g_constants.con1, false);
#else
  CasFilter(r, g, b, id.xy, g_constants.con0, g_constants.con1, true);
#endif
  g_output[id.xy] = AF4(r, g, b, 1.0);
}
