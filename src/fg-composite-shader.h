#pragma once

// UI-preserving recomposition of a neural-rendered HUD-less frame.
//   t0 = original HUD-less, t1 = NR(HUD-less), u0 = backbuffer in / composite out.
// The NR delta is applied fully where the UI did not touch the pixel and fades
// out with the UI's contribution, so translucent panels keep a soft, continuous
// scene under them instead of an un-enhanced cut-out; opaque UI stays untouched.
static const char kFgCompositeShader[] =
    "Texture2D<float4>   hudless : register(t0);\n"
    "Texture2D<float4>   nr      : register(t1);\n"
    "RWTexture2D<float4> back    : register(u0);\n"
    "static const float kUiFade = 0.25;\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "    uint w, h; back.GetDimensions(w, h);\n"
    "    if (id.x >= w || id.y >= h) return;\n"
    "    float4 b = back[id.xy];\n"
    "    float3 s = hudless[id.xy].rgb;\n"
    "    float3 d = abs(b.rgb - s);\n"
    "    float ui = max(d.r, max(d.g, d.b));\n"
    "    float weight = saturate(1.0 - ui / kUiFade);\n"
    "    back[id.xy] = float4(saturate(b.rgb + (nr[id.xy].rgb - s) * weight), b.a);\n"
    "}\n";
