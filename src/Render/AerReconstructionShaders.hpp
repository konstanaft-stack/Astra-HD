#pragma once
// Reconstructed shader math; pinned DXBC is used only by the external test oracle.
namespace cvr::render::aer_shaders {
inline constexpr char Vertex[] = R"AER(
// Semantic reconstruction of the audited VS at RVA5778E0, SHA F340B9A2...33B1AAD.
// Constant-byte layout and four-vertex path match the recovered contract.
// This vertex contract is validated by the independent pinned-DXBC WARP fixture.
cbuffer srcTransf : register(b0) {
    float4 srcViewport;                  // byte0
    float4 col0;                         // byte16
    column_major float4x4 clip2clipRot0; // byte32
    float4 col1;                         // byte96
    column_major float4x4 clip2clipRot1; // byte112
};
struct Varyings {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};
Varyings WarpVS(uint id : SV_VertexID) {
    bool second = id >= 5;
    uint k = second ? (uint)max((int)id-6,0) : min(id,3u);
    float2 xy = float2((float)(k&2u)-1.0f,2.0f*(float)(k&1u)-1.0f);
    float4 clip = float4(xy,0.01f,1.0f);
    Varyings o;
    o.position = second ? mul(clip2clipRot1,clip) : mul(clip2clipRot0,clip);
    o.uv = xy*srcViewport.xy+srcViewport.zw;
    o.color = second ? col1 : col0;
    return o;
}

)AER";
inline constexpr char Pixel[] = R"AER(
cbuffer FilterControl : register(b0) {
    // Emphasis, 4 * Deblur, Contrast, Vibrance. Do NOT saturate these parameters.
    // Reference INI Sharpness=0, Sharpmix=.5, Contrast=Vibrance=0 -> (0,2,0,0).
    float4 filterParam;
};
Texture2D<float4> sourceTexture : register(t0);
SamplerState pointBorder : register(s0);

SamplerState linearBorder : register(s1);
float3 DecodeColor(float3 v) {
#if AER_DECODE
 return lerp(v / 12.92, pow((v + .055) / 1.055, 2.4), step(.04045,v));
#else
 return v;
#endif
}
float3 EncodeColor(float3 v) {
#if AER_ENCODE
 return lerp(v * 12.92, 1.055 * pow(max(v,0), 1.0/2.4) - .055, step(.0031308,v));
#else
 return v;
#endif
}
struct PixelInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

// The four Gather positions read the 4x4 footprint surrounding the four central
// source texels. Gather component order is LL, LR, UR, UL, before swizzling.
void GatherSquare(float2 originUv, int2 offset, out float3 ul, out float3 ur,
                  out float3 ll, out float3 lr) {
    float4 r = sourceTexture.GatherRed(pointBorder, originUv, offset);
    float4 g = sourceTexture.GatherGreen(pointBorder, originUv, offset);
    float4 b = sourceTexture.GatherBlue(pointBorder, originUv, offset);
    ul = DecodeColor(float3(r.w, g.w, b.w));
    ur = DecodeColor(float3(r.z, g.z, b.z));
    ll = DecodeColor(float3(r.x, g.x, b.x));
    lr = DecodeColor(float3(r.y, g.y, b.y));
}

void AccumulateKernel(float3 a, float3 b, float3 c,
                      float3 d, float3 e, float3 f,
                      float3 g, float3 h, float3 i,
                      float bilinearWeight,
                      inout float3 numerator, inout float3 denominator) {
    float3 minCross = min(e, min(min(b, d), min(f, h)));
    float3 maxCross = max(e, max(max(b, d), max(f, h)));
    float3 minimum = minCross + min(minCross, min(min(a, c), min(g, i)));
    float3 maximum = maxCross + max(maxCross, max(max(a, c), max(g, i)));
    float3 amplitude = saturate(min(minimum, 2.0 - maximum) / maximum);
    // Written in the reciprocal-square-root form used by the bytecode.
    float3 weight = -rcp((8.0 - 3.0 * filterParam.x) * rsqrt(amplitude));
    float confidence = bilinearWeight / (0.03125 + maximum.g - minimum.g);
    numerator += confidence * (e + weight * (b + d + f + h));
    denominator += confidence * (1.0 + 4.0 * weight);
}

float3 ApplyColorControls(float3 value) {
    value += filterParam.z * (value * value * (3.0 - 2.0 * value) - value);
    float luminance = dot(value, float3(0.299, 0.587, 0.114));
    float range = max(value.r, max(value.g, value.b)) - min(value.r, min(value.g, value.b));
    float gain = 1.0 + filterParam.w * (1.0 - range);
    return saturate(luminance + gain * (value - luminance));
}

float4 LukeCasWarpPS(PixelInput input) : SV_Target {
#if AER_BYPASS
    float4 plainSample = sourceTexture.Sample(linearBorder,input.uv);
#if AER_DECODE
    uint bw,bh;sourceTexture.GetDimensions(bw,bh);
    float2 pixel=input.uv*float2(bw,bh)-.5, phase=frac(pixel);
    float3 ul,ur,ll,lr;GatherSquare(floor(pixel)/float2(bw,bh),int2(1,1),ul,ur,ll,lr);
    plainSample.rgb=lerp(lerp(ul,ur,phase.x),lerp(ll,lr,phase.x),phase.y);
#endif
    return float4(EncodeColor(plainSample.rgb),plainSample.a)*input.color;
#else
    uint width, height;
    sourceTexture.GetDimensions(width, height);
    float2 size = float2(width, height);
    float2 sourcePosition = input.uv * size - 0.5;
    float2 phase = frac(sourcePosition);
    float2 originUv = floor(sourcePosition) / size;
    float3 a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p;
    GatherSquare(originUv, int2(0,0), a,b,e,f);
    GatherSquare(originUv, int2(2,0), c,d,g,h);
    GatherSquare(originUv, int2(0,2), i,j,m,n);
    GatherSquare(originUv, int2(2,2), k,l,o,p);
    float4 bilinear = float4((1-phase.x)*(1-phase.y), phase.x*(1-phase.y),
                            (1-phase.x)*phase.y, phase.x*phase.y);
    float3 plain = bilinear.x*f + bilinear.y*g + bilinear.z*j + bilinear.w*k;
    float3 numerator=0, denominator=0;
    AccumulateKernel(a,b,c,e,f,g,i,j,k, bilinear.x, numerator, denominator);
    AccumulateKernel(b,c,d,f,g,h,j,k,l, bilinear.y, numerator, denominator);
    AccumulateKernel(e,f,g,i,j,k,m,n,o, bilinear.z, numerator, denominator);
    AccumulateKernel(f,g,h,j,k,l,n,o,p, bilinear.w, numerator, denominator);
    float3 sharpened = saturate(numerator / denominator);
    float3 mixed = saturate(plain + filterParam.y * (sharpened - plain));
    return float4(EncodeColor(ApplyColorControls(mixed)), 1.0) * input.color;
#endif
}

)AER";
}
