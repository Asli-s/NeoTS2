// Neighborhood terrain: the game draws the terrain black, adds each paint's texture times its
// alpha map, darkens what is under the water by depth (the underwater gradient), then multiplies
// what is above the water by the light map. This does it in one pass. Alpha and light maps are
// sampled at grid / size (so a vertex falls between two texels, as in the game). Paint textures
// repeat every few tiles, each cell of the variant grid showing one variant per paint. In the
// water's reflection only what is above the water is drawn, without the gradient.

Texture2DArray<float4> paints : register(t0, space2);
SamplerState paintSampler : register(s0, space2);
Texture2DArray<float4> alphas : register(t1, space2);
SamplerState alphaSampler : register(s1, space2);
Texture2DArray<float4> cells : register(t2, space2);
SamplerState cellSampler : register(s2, space2);
Texture2D<float4> lightMap : register(t3, space2);
SamplerState lightSampler : register(s3, space2);
Texture2D<float4> gradient : register(t4, space2);
SamplerState gradientSampler : register(s4, space2);

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
    float2 padding;
};

struct Input {
    float4 position : SV_Position;
    float2 grid : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float height : TEXCOORD2;
};

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
