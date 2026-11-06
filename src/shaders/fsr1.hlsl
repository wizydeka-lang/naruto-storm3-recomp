// naruto - AMD FidelityFX Super Resolution 1 for the native view
//
// Compute shaders that upscale the presented frame to the window: EASU
// (EASU=1) scales g_input into g_output, then RCAS (EASU=0) sharpens it.
// The algorithm is AMD's, from third_party/ffx-fsr (MIT); the view computes
// the constants. tools/build-postfx.py compiles both to src/postfx_spirv.h.

#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"

struct Constants {
  uint4 con0, con1, con2, con3;  // FsrEasuCon
  uint4 rcas;                    // FsrRcasCon
  uint4 size;                    // xy: output size, zw: input size
};
[[vk::push_constant]] Constants g_constants;
[[vk::binding(0)]] Texture2D<float4> g_input;
[[vk::binding(1)]] SamplerState g_sampler;
[[vk::binding(2)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> g_output;

#if EASU

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return g_input.GatherRed(g_sampler, p); }
AF4 FsrEasuGF(AF2 p) { return g_input.GatherGreen(g_sampler, p); }
AF4 FsrEasuBF(AF2 p) { return g_input.GatherBlue(g_sampler, p); }
#include "ffx_fsr1.h"

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= g_constants.size.xy)) {
    return;
  }
  AF3 color;
  FsrEasuF(color, id.xy, g_constants.con0, g_constants.con1, g_constants.con2, g_constants.con3);
  g_output[id.xy] = AF4(color, 1.0);
}

#else

#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) {
  return g_input.Load(int3(clamp(p, ASU2(0, 0), ASU2(g_constants.size.zw) - 1), 0));
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_fsr1.h"

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= g_constants.size.xy)) {
    return;
  }
  AF1 r, g, b;
  FsrRcasF(r, g, b, id.xy, g_constants.rcas);
  g_output[id.xy] = AF4(r, g, b, 1.0);
}

#endif
