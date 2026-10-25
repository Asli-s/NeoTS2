// Neighborhood terrain: the game draws the terrain black, adds each paint's texture times its
// alpha map, darkens what is under the water by depth (the underwater gradient), then multiplies
// what is above the water by the light map. This does it in one pass. Alpha and light maps are
// sampled at grid / size (so a vertex falls between two texels, as in the game). Paint textures
// repeat every few tiles, each cell of the variant grid showing one variant per paint. In the
// water's reflection only what is above the water is drawn, without the gradient.

#include "sdl_gpu.hlsli"

#ifdef GL2
// OpenGL 2 has no texture arrays, so the terrain is drawn in one pass per paint texture, added
// together (the first replaces what was there). A pass shows its texture (`slice`) weighted by the
// alpha of every layer that has the cell show it. Four layers' alpha maps share a texture, and so do
// their cells' textures (one layer in each of its four channels).
COMBINED Texture2D<float4> paint : register(t0, space2);
COMBINED SamplerState paintSampler : register(s0, space2);
COMBINED Texture2D<float4> alphas0 : register(t1, space2);
COMBINED SamplerState alpha0Sampler : register(s1, space2);
COMBINED Texture2D<float4> alphas1 : register(t2, space2);
COMBINED SamplerState alpha1Sampler : register(s2, space2);
COMBINED Texture2D<float4> cells0 : register(t3, space2);
COMBINED SamplerState cell0Sampler : register(s3, space2);
COMBINED Texture2D<float4> cells1 : register(t4, space2);
COMBINED SamplerState cell1Sampler : register(s4, space2);
COMBINED Texture2D<float4> lightMap : register(t5, space2);
COMBINED SamplerState lightSampler : register(s5, space2);
COMBINED Texture2D<float4> gradient : register(t6, space2);
COMBINED SamplerState gradientSampler : register(s6, space2);
#else
COMBINED Texture2DArray<float4> paints : register(t0, space2);
COMBINED SamplerState paintSampler : register(s0, space2);
COMBINED Texture2DArray<float4> alphas : register(t1, space2);
COMBINED SamplerState alphaSampler : register(s1, space2);
COMBINED Texture2DArray<float4> cells : register(t2, space2);
COMBINED SamplerState cellSampler : register(s2, space2);
COMBINED Texture2D<float4> lightMap : register(t3, space2);
COMBINED SamplerState lightSampler : register(s3, space2);
COMBINED Texture2D<float4> gradient : register(t4, space2);
COMBINED SamplerState gradientSampler : register(s4, space2);

#endif

cbuffer Terrain : register(b0, space3) {
    float alphaSize;
    float textureRepeat;
    float cellTiles;
    float layers;
    float cellsX;
    float cellsY;
    float lightMapScale;
    float litFrom;       // the water's height for the light map, the gradient and the reflection
    float gradientDepth;
    float reflected;     // 1 when drawing the reflection
#ifdef GL2
    float slice;         // the paint texture this pass shows
    float passAlpha;     // the alpha it writes: 1 in the pass that replaces, 0 in those that add
#else
    float2 padding;
#endif
};

struct Input {
    float4 position : SV_Position;
    float2 grid : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float height : TEXCOORD2;
};

#ifdef GL2
float4 main(Input input) : SV_Target {
    if (reflected > 0.5 && input.height < litFrom) {
        discard;
    }
    const float2 cell = clamp(floor(input.grid / cellTiles), float2(0, 0), float2(cellsX - 1, cellsY - 1));
    const float2 cellAt = (cell + 0.5) / float2(cellsX, cellsY);
    const float2 alphaAt = input.grid / alphaSize;
    const float4 shown0 = 1 - step(0.5, abs(floor(cells0.Sample(cell0Sampler, cellAt) * 255 + 0.5) - slice));
    const float4 shown1 = 1 - step(0.5, abs(floor(cells1.Sample(cell1Sampler, cellAt) * 255 + 0.5) - slice));
    const float weight = dot(alphas0.Sample(alpha0Sampler, alphaAt), shown0) + dot(alphas1.Sample(alpha1Sampler, alphaAt), shown1);
    float3 color = paint.Sample(paintSampler, input.grid / textureRepeat).rgb * weight;
    if (reflected < 0.5) {
        color *= gradient.Sample(gradientSampler, float2(0.5, (litFrom - input.height) / gradientDepth)).rgb;
    }
    if (input.height >= litFrom) {
        color *= lightMap.Sample(lightSampler, input.grid / lightMapScale).rgb;
    }
    return float4(color, passAlpha);
}
#else
float4 main(Input input) : SV_Target {
    if (reflected > 0.5 && input.height < litFrom) {
        discard;
    }
    float3 color = float3(0, 0, 0);
    const int2 cell = clamp(int2(floor(input.grid / cellTiles)), int2(0, 0), int2(int(cellsX) - 1, int(cellsY) - 1));
    for (int i = 0; i < int(layers); ++i) {
        const float a = alphas.Sample(alphaSampler, float3(input.grid / alphaSize, i)).r;
        const float slice = round(cells.Load(int4(cell, i, 0)).r * 255);
        color += paints.Sample(paintSampler, float3(input.grid / textureRepeat, slice)).rgb * a;
    }
    if (reflected < 0.5) {
        color *= gradient.Sample(gradientSampler, float2(0.5, (litFrom - input.height) / gradientDepth)).rgb;
    }
    if (input.height >= litFrom) {
        color *= lightMap.Sample(lightSampler, input.grid / lightMapScale).rgb;
    }
    return float4(color, 1);
}
#endif
