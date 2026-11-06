// naruto - NVIDIA FXAA 3.11 for the native view
//
// A compute shader that smooths the presented frame's edges at the size it
// was rendered, before any upscaling. The algorithm is Timothy Lottes's, from
// third_party/fxaa (BSD); green stands in for luma, as the game's output has
// no luma channel. tools/build-postfx.py compiles it to src/postfx_spirv.h.

struct Constants {
  uint4 con0, con1, con2, con3;  // con0.xy: 1 / size, as floats
  uint4 rcas;
  uint4 size;  // xy: output size, zw: input size (the same)
};
[[vk::push_constant]] Constants g_constants;
[[vk::binding(0)]] Texture2D<float4> g_input;
[[vk::binding(1)]] SamplerState g_sampler;
[[vk::binding(2)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> g_output;

#define FXAA_PC 1
#define FXAA_HLSL_5 1
#define FXAA_GREEN_AS_LUMA 1
#define FXAA_QUALITY__PRESET 12
#include "FXAA3_11.h"

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= g_constants.size.xy)) {
    return;
  }
  float2 rcp = asfloat(g_constants.con0.xy);
  FxaaTex tex;
  tex.smpl = g_sampler;
  tex.tex = g_input;
  // Only the PC quality path reads the arguments after the first, texture
  // and rcp ones; the rest are its documented defaults.
  float3 color = FxaaPixelShader((float2(id.xy) + 0.5) * rcp, float4(0, 0, 0, 0), tex, tex, tex,
                                 rcp, float4(0, 0, 0, 0), float4(0, 0, 0, 0), float4(0, 0, 0, 0),
                                 0.75, 0.166, 0.0833, 8.0, 0.125, 0.05,
                                 float4(1.0, -1.0, 0.25, -0.25)).rgb;
  g_output[id.xy] = float4(color, 1.0);
}
