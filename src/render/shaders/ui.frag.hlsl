// UI pixels: the texture times the quad's color (text is white glyphs tinted to its color).

#include "sdl_gpu.hlsli"

COMBINED Texture2D<float4> image : register(t0, space2);
COMBINED SamplerState imageSampler : register(s0, space2);

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

float4 main(Input input) : SV_Target {
    return image.Sample(imageSampler, input.uv) * input.color;
}
