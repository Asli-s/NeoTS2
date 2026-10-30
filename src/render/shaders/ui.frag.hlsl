// UI pixels: the texture times the quad's color (text is white glyphs tinted to its color).

#include "sdl_gpu.hlsli"

COMBINED Texture2D<float4> image : register(t0, space2);
COMBINED SamplerState imageSampler : register(s0, space2);

cbuffer Picture : register(b0, space3) {
    float2 texels;  // the texture's width and height
    float scale;    // the UI's size, 1 as the game draws it
    float padding;
};

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

// Where to sample so a picture enlarged with the UI stays sharp: each of its pixels shows as a
// block of one colour, blended with the next only along a window pixel at its edge. A picture also
// stretched by the game itself (the menu's blurred backdrop) stays smooth.
float2 sharpUv(float2 uv) {
    float2 texel = uv * texels;
    // Window pixels per texel, and how much of that is the game's own stretch.
    float2 shown = 1 / max(max(abs(ddx(texel)), abs(ddy(texel))), 1e-6);
    float2 sharp = max(1, min(shown, scale * scale / shown));
    float2 fromCentre = frac(texel) - 0.5;
    float2 flat = 0.5 - 0.5 / sharp;
    float2 blend = (fromCentre - clamp(fromCentre, -flat, flat)) * sharp + 0.5;
    return (floor(texel) + blend) / texels;
}

float4 main(Input input) : SV_Target {
    float2 uv = scale > 1 ? sharpUv(input.uv) : input.uv;
    return image.Sample(imageSampler, uv) * input.color;
}
