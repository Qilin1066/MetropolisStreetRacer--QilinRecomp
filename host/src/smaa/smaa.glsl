#version 450
// SMAA 1x (MIT; SMAA.hlsl fetched at configure time, THIRD_PARTY.md) for the present pass, at the HIGH
// preset: luma edge detection, blending-weight calculation and neighbourhood
// blending, each a full-screen triangle (PASS 1, 2, 3; VERTEX or FRAGMENT).
// Built into smaa_spirv.inc by tools/smaa_spirv.sh; nothing compiles at
// run time. Every sample is at level 0 (textureLod), as SMAA's are in effect.
#extension GL_GOOGLE_include_directive : enable

layout(push_constant) uniform Constants {
    vec4 metrics;  // 1 / width, 1 / height, width, height
} pc;

#define SMAA_RT_METRICS pc.metrics
#define SMAA_PRESET_HIGH 1
#define SMAA_CUSTOM_SL 1
#define SMAATexture2D(tex) sampler2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) textureLod(tex, coord, 0.0)
#define SMAASampleLevelZeroPoint(tex, coord) textureLod(tex, coord, 0.0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) textureLodOffset(tex, coord, 0.0, offset)
#define SMAASample(tex, coord) textureLod(tex, coord, 0.0)
#define SMAASamplePoint(tex, coord) textureLod(tex, coord, 0.0)
#define SMAASampleOffset(tex, coord, offset) textureLodOffset(tex, coord, 0.0, offset)
#define SMAA_FLATTEN
#define SMAA_BRANCH
#define lerp(a, b, t) mix(a, b, t)
#define saturate(a) clamp(a, 0.0, 1.0)
#define mad(a, b, c) (a * b + c)
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define bool2 bvec2
#define bool3 bvec3
#define bool4 bvec4

#if defined(VERTEX)
#define SMAA_INCLUDE_VS 1
#define SMAA_INCLUDE_PS 0
#else
#define SMAA_INCLUDE_VS 0
#define SMAA_INCLUDE_PS 1
#endif
#include "SMAA.hlsl"

#if defined(VERTEX)
layout(location = 0) out vec2 texcoord;
layout(location = 1) out vec2 pixcoord;
layout(location = 2) out vec4 offset[3];
void main() {
    // One triangle over the target: (0, 0) at its top left, as Vulkan has it.
    texcoord = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(texcoord * 2.0 - 1.0, 0.0, 1.0);
    pixcoord = vec2(0.0);
#if PASS == 1
    SMAAEdgeDetectionVS(texcoord, offset);
#elif PASS == 2
    SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);
#else
    SMAANeighborhoodBlendingVS(texcoord, offset[0]);
    offset[1] = offset[2] = vec4(0.0);
#endif
}
#else
layout(location = 0) in vec2 texcoord;
layout(location = 1) in vec2 pixcoord;
layout(location = 2) in vec4 offset[3];
layout(location = 0) out vec4 result;
layout(set = 0, binding = 0) uniform sampler2D tex0;  // 1: the colour; 2: the edges; 3: the colour
layout(set = 0, binding = 1) uniform sampler2D tex1;  // 2: the area texture; 3: the blending weights
layout(set = 0, binding = 2) uniform sampler2D tex2;  // 2: the search texture (nearest)
void main() {
#if PASS == 1
    result = vec4(SMAALumaEdgeDetectionPS(texcoord, offset, tex0), 0.0, 0.0);
#elif PASS == 2
    result = SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, tex0, tex1, tex2, vec4(0.0));
#else
    result = SMAANeighborhoodBlendingPS(texcoord, offset[0], tex0, tex1);
#endif
}
#endif
