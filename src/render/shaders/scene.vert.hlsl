// Scene vertices: world placement, then the camera.

cbuffer Frame : register(b0, space1) {
    row_major float4x4 viewProjection;  // applied to column vectors
    row_major float4x4 model;           // the instance's placement
    float4 ambient;                     // w: 1 when lit as the game lights objects
    float4 lightDirections[8];          // toward each light
    float4 lightColors[8];
    float4 thumbnail;                   // rgb: the material's diffuse; w: the thumbnail scale (0: not a thumbnail)
};

struct Input {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float alpha : TEXCOORD3;
    float2 lightUv : TEXCOORD6;
};

struct Output {
    float4 position : SV_Position;
    float3 normal : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float3 world : TEXCOORD2;
    float alpha : TEXCOORD3;
    float4 light : TEXCOORD4;
    float2 lightUv : TEXCOORD5;
};

// The game's object lighting at a vertex: the ambient plus each light's colour times how much the
// normal faces it. w is 0 when the instance isn't lit this way.
float4 gameLight(float3 normal) {
    if (ambient.w < 0.5) {
        return float4(0, 0, 0, 0);
    }
    const float3 n = normalize(normal);
    float3 sum = ambient.rgb;
    [unroll] for (int i = 0; i < 8; ++i) {
        sum += max(0, dot(n, lightDirections[i].xyz)) * lightColors[i].rgb;
    }
    // A lot skirt: the fixed-function vertex colour, clamped.
    if (ambient.w > 2.5) {
        return float4(saturate(sum), 1);
    }
    // A family thumbnail: the game's vertex colour there, clamped, then doubled by its texture stage.
    if (thumbnail.w > 0) {
        return float4(2 * saturate(sum * thumbnail.rgb * thumbnail.w), 2);
    }
    return float4(sum, 1);
}

Output main(Input input) {
    Output output;
    const float4 world = mul(model, float4(input.position, 1));
    output.position = mul(viewProjection, world);
    output.normal = mul((float3x3)model, input.normal);
    output.uv = input.uv;
    output.world = world.xyz;
    output.alpha = input.alpha;
    output.light = gameLight(output.normal);
    output.lightUv = input.lightUv;
    return output;
}
