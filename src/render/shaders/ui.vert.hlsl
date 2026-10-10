// UI vertices: screen pixels from the top left.

cbuffer Screen : register(b0, space1) {
    float2 size;  // the screen's width and height in pixels
    float2 padding;
};

struct Input {
    float2 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float4 color : TEXCOORD2;
};

struct Output {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

Output main(Input input) {
    Output output;
    output.position = float4(input.position.x / size.x * 2 - 1, 1 - input.position.y / size.y * 2, 0, 1);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
