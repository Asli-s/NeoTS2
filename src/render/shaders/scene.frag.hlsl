// Scene surfaces: the material's color times its texture, lit by their light map (a lot's floors and
// walls), as the game lights objects when the vertices bring that light, otherwise by a sky and a
// sun, unless unlit.

Texture2D<float4> baseTexture : register(t0, space2);
SamplerState baseSampler : register(s0, space2);
Texture2D<float4> lightMapTexture : register(t1, space2);
SamplerState lightMapSampler : register(s1, space2);
Texture2D<float4> normalTexture : register(t2, space2);
SamplerState normalSampler : register(s2, space2);
Texture2D<float4> incidenceTexture : register(t3, space2);
SamplerState incidenceSampler : register(s3, space2);
Texture2D<float4> lightMapFromTexture : register(t4, space2);  // the light map being faded from
SamplerState lightMapFromSampler : register(s4, space2);
TextureCube<float4> envCubeTexture : register(t5, space2);
SamplerState envCubeSampler : register(s5, space2);

cbuffer Material : register(b0, space3) {
    float4 color;
    float4 sunDirection;  // toward the sun, xyz, w unused
    float hasTexture;
    float alphaCutoff;    // discard below this (0 for none)
    float unlit;          // 1: color and texture as they are, without light
    float padding;
    float4 lightMap;      // x: the gain on the light map (0: none), y: 1 to leave the color out, z: 1 for terrain, w: 1 with a normal map
    float4 pixelLightColors[3];  // the colours of the lights lit per pixel through the normal map
    float pixelLightCount;       // how many there are (0: none)
    float normalMapped;          // 1: the normal from the normal map, 0: straight out
    float simLighting;           // 1: lit as the game's Sim materials
    float lightMapFade;          // how much of the light map is the current one (below 1 while it fades in)
    float4 envCube;              // rgb: the environment cube's colour scale, w: 1 when the material reflects it
    // The camera's right, up and forward, to look the cube up in the camera's space (cameraRight.w 1)
    // instead of the world's.
    float4 cameraRight;
    float4 cameraUp;
    float4 cameraForward;
    float4 emissive;  // rgb added to the light before the texture multiplies it, w: 1 when on
};

struct Input {
    float4 position : SV_Position;
    float3 normal : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float3 world : TEXCOORD2;
    float alpha : TEXCOORD3;
    float4 light : TEXCOORD4;
    float2 lightUv : TEXCOORD5;
    float3 color : TEXCOORD6;  // the vertices' color (white unless they have one)
    float3 lightVector0 : TEXCOORD7;
    float3 lightVector1 : TEXCOORD8;
    float3 lightVector2 : TEXCOORD9;
    float3 reflection : TEXCOORD10;
    float3 specular : TEXCOORD11;
    bool front : SV_IsFrontFace;
};

// The light of the lights lit per pixel, as the game's pixel shader adds it to the vertices': the
// normal from the normal map (normalized, straight out without one) against each light's
// interpolated tangent-space vector, clamped, times the light's colour.
float3 normalMappedLight(Input input) {
    const float3 n = normalMapped > 0.5 ? normalize(normalTexture.Sample(normalSampler, input.uv).xyz * 2 - 1) : float3(0, 0, 1);
    float3 sum = saturate(dot(n, input.lightVector0)) * pixelLightColors[0].rgb;
    if (pixelLightCount > 1.5) {
        sum += saturate(dot(n, input.lightVector1)) * pixelLightColors[1].rgb;
    }
    if (pixelLightCount > 2.5) {
        sum += saturate(dot(n, input.lightVector2)) * pixelLightColors[2].rgb;
    }
    return sum;
}

float4 shade(Input input) {
    const float4 texel = hasTexture > 0.5 ? baseTexture.Sample(baseSampler, input.uv) : float4(1, 1, 1, 1);
    float4 albedo = color * texel;
    albedo.rgb *= input.color;
    albedo.a *= input.alpha;
    if (albedo.a < alphaCutoff) {
        discard;
    }
    // A lot's floor or wall, as the game draws it: its texture (floors: times the material's diffuse,
    // walls: alone) times the light map times 2, and no other light. The game draws walls and floors
    // with its own light-mapped material, so a pattern whose material is unlit is light-mapped too.
    if (lightMap.x > 0) {
        const float3 base = lightMap.y > 0.5 && hasTexture > 0.5 ? texel.rgb : albedo.rgb;
        float3 lit = lightMapTexture.Sample(lightMapSampler, input.lightUv).rgb;
        if (lightMapFade < 1) {
            lit = lerp(lightMapFromTexture.Sample(lightMapFromSampler, input.lightUv).rgb, lit, lightMapFade);
        }
        // A bump-mapped floor or wall: times how much the normal map faces the incidence map's
        // direction (where the light map grows brighter, straight out where it is even).
        float bump = 1;
        if (lightMap.w > 0.5) {
            const float3 n = normalTexture.Sample(normalSampler, input.uv).xyz * 2 - 1;
            const float3 toward = incidenceTexture.Sample(incidenceSampler, input.lightUv).xyz * 2 - 1;
            bump = saturate(dot(n, toward));
        }
        // The lot's terrain: darkened by its light map (a multiply pass in the game, so at most 1).
        if (lightMap.z > 0.5) {
            return float4(base * saturate(lit * lightMap.x), albedo.a);
        }
        return float4(saturate(base * lit * lightMap.x * bump), albedo.a);
    }
    if (unlit > 0.5) {
        return albedo;
    }
    // A family thumbnail: the texture times the light, which already holds the material's diffuse.
    if (input.light.w > 1.5) {
        return float4((hasTexture > 0.5 ? texel.rgb : color.rgb) * input.light.rgb, albedo.a);
    }
    if (input.light.w > 0.5) {
        const float3 pixel = pixelLightCount > 0.5 ? normalMappedLight(input) : float3(0, 0, 0);
        // A Sim: the vertex light times the diffuse scaled by 0.8, the per-pixel light times 0.8
        // alone, then the texture (the diffuse stands in for a missing one).
        if (simLighting > 0.5) {
            const float3 base = (hasTexture > 0.5 ? texel.rgb : color.rgb) * input.color;
            return float4(base * (input.light.rgb * color.rgb * 0.8 + pixel * 0.8), albedo.a);
        }
        return float4(albedo.rgb * (input.light.rgb + pixel), albedo.a);
    }
    float3 n = normalize(input.normal);
    if (!input.front) {
        n = -n;  // two-sided surfaces light their back as a front
    }
    // Sky from above, a little light from the ground, and the sun.
    const float sky = lerp(0.45, 0.85, saturate(n.z * 0.5 + 0.5));
    const float sun = saturate(dot(n, sunDirection.xyz)) * 0.75;
    return float4(albedo.rgb * (sky + sun), albedo.a);
}

// The specular, then the environment cube, are added over the lit colour, leaving its alpha. The
// game scales the cube by its texture light, white by day. Its night value is not known yet, so it
// is white here too.
float4 main(Input input) : SV_Target {
    float4 lit = shade(input);
    if (emissive.w > 0.5) {
        lit.rgb += emissive.rgb * (hasTexture > 0.5 ? baseTexture.Sample(baseSampler, input.uv).rgb : float3(1, 1, 1)) * input.color;
    }
    lit.rgb += input.specular;
    if (envCube.w > 0.5) {
        float3 r = input.reflection;
        if (cameraRight.w > 0.5) {
            r = float3(dot(r, cameraRight.xyz), dot(r, cameraUp.xyz), dot(r, cameraForward.xyz));
        }
        lit.rgb += envCubeTexture.Sample(envCubeSampler, r).rgb * envCube.rgb;
    }
    return lit;
}
