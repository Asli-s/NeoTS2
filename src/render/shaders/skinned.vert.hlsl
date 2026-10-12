// Skinned scene vertices: each follows up to four joints (weighted), then world placement and the
// camera. Joint matrices are 3x4, three rows each, starting at jointBase.

cbuffer Frame : register(b0, space1) {
    row_major float4x4 viewProjection;  // applied to column vectors
    row_major float4x4 model;           // the instance's placement
    float4 ambient;                     // w: 1 when lit as the game lights objects
    float4 lightDirections[8];          // toward each light
    float4 lightColors[8];
    float4 thumbnail;                   // rgb: the material's diffuse; w: the thumbnail scale (0: not a thumbnail)
    uint jointBase;
    uint3 padding;
};

StructuredBuffer<float4> joints : register(t0, space0);

struct Input {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float alpha : TEXCOORD3;
    uint4 joint : TEXCOORD4;   // 255: none
    float4 weight : TEXCOORD5;
    float2 lightUv : TEXCOORD6;  // unused: skinned surfaces have no light map
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
    // A family thumbnail: the game's vertex colour there, clamped, then doubled by its texture stage.
    if (thumbnail.w > 0) {
        return float4(2 * saturate(sum * thumbnail.rgb * thumbnail.w), 2);
    }
    return float4(sum, 1);
}

float3x4 jointMatrix(uint j) {
    const uint row = (jointBase + j) * 3;
    return float3x4(joints[row], joints[row + 1], joints[row + 2]);
}

Output main(Input input) {
    float3 position = 0, normal = 0;
    [unroll] for (int k = 0; k < 4; ++k) {
        if (input.joint[k] != 255 && input.weight[k] > 0) {
            const float3x4 m = jointMatrix(input.joint[k]);
            position += input.weight[k] * mul(m, float4(input.position, 1));
            normal += input.weight[k] * mul((float3x3)m, input.normal);
        }
    }
    Output output;
    const float4 world = mul(model, float4(position, 1));
    output.position = mul(viewProjection, world);
    output.normal = mul((float3x3)model, normalize(normal));
    output.uv = input.uv;
    output.world = world.xyz;
    output.alpha = input.alpha;
    output.light = gameLight(output.normal);
    output.lightUv = float2(0, 0);
    return output;
}
