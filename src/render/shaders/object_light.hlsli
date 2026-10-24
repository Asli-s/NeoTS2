// The game's object lighting, shared by the scene and skinned vertex shaders. The including shader
// declares the Frame constants first (ambient, lights, thumbnail, pixelLights).

struct Output {
    float4 position : SV_Position;
    float3 normal : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float3 world : TEXCOORD2;
    float alpha : TEXCOORD3;
    float4 light : TEXCOORD4;
    float2 lightUv : TEXCOORD5;
    float3 color : TEXCOORD6;
    // Toward each light lit per pixel through the normal map, in the vertex's tangent space, times
    // the light's falloff.
    float3 lightVector0 : TEXCOORD7;
    float3 lightVector1 : TEXCOORD8;
    float3 lightVector2 : TEXCOORD9;
    float3 reflection : TEXCOORD10;  // the view reflected about the normal, in the world
    float3 specular : TEXCOORD11;
};

// The direction toward the camera from `world` reflected about the normal, per vertex as the
// game's standard material looks up its environment cube.
float3 reflectedView(float3 normal, float3 world) {
    const float3 n = normalize(normal);
    const float3 v = normalize(eye.xyz - world);
    return 2 * n * dot(n, v) - v;
}

// Toward light i from `world` (normalized), and how much of the light gets there: a point light's
// 1 / (1 + falloff x distance squared), none beyond its range, and a spot light's only within its
// cone. All of a directional light's gets there.
float3 towardLight(int i, float3 world, out float reach) {
    if (lightDirections[i].w > 0) {
        const float3 v = lightPositions[i].xyz - world;
        const float d2 = dot(v, v);
        const float3 l = v * rsqrt(max(d2, 1e-12));
        reach = d2 <= lightDirections[i].w ? 1 / (1 + lightPositions[i].w * d2) : 0;
        if (lightCones[i].z > 0.5) {
            reach *= saturate((dot(l, lightDirections[i].xyz) - lightCones[i].x) * lightCones[i].y);
        }
        return l;
    }
    reach = 1;
    return lightDirections[i].xyz;
}

// Whether light i is lit per pixel through the normal map instead of at the vertices.
bool perPixel(int i) {
    return (pixelLights.x > 0 && i == pixelLights.y) || (pixelLights.x > 1 && i == pixelLights.z) ||
           (pixelLights.x > 2 && i == pixelLights.w);
}

// The light at a vertex: the ambient plus each light's colour times how much the normal faces it
// and how much of it gets there, leaving out lights lit per pixel. w is 0 when the instance isn't
// lit this way.
float4 gameLight(float3 normal, float3 world) {
    if (ambient.w < 0.5) {
        return float4(0, 0, 0, 0);
    }
    const float3 n = normalize(normal);
    float3 sum = ambient.rgb;
    [unroll] for (int i = 0; i < 8; ++i) {
        if (!perPixel(i)) {
            float reach;
            const float3 l = towardLight(i, world, reach);
            sum += max(0, dot(n, l) * reach) * lightColors[i].rgb;
        }
    }
    // A lot skirt: the fixed-function vertex colour, clamped, times its material's colour scale.
    if (ambient.w > 2.5) {
        return float4(saturate(sum) * thumbnail.rgb, 1);
    }
    // A family thumbnail: the game's vertex colour there, clamped, then doubled by its texture stage.
    if (thumbnail.w > 0) {
        return float4(2 * saturate(sum * thumbnail.rgb * thumbnail.w), 2);
    }
    return float4(sum, 1);
}

// The standard material's specular at a vertex lit as the game lights objects (`lit` 1): each
// directional light's colour times the reflected view against it to the material's power, where
// the surface faces the light. Lamps add none. Times the material's specular scale.
float3 gameSpecular(float3 normal, float3 reflection, float lit) {
    if (specular.w <= 0 || lit < 0.5 || lit > 1.5) {
        return float3(0, 0, 0);
    }
    const float3 n = normalize(normal);
    float3 sum = 0;
    [unroll] for (int i = 0; i < 8; ++i) {
        const float3 l = lightDirections[i].xyz;
        if (lightDirections[i].w <= 0 && dot(n, l) > 0.0001) {
            sum += pow(max(0, dot(reflection, l)), specular.w) * lightColors[i].rgb;
        }
    }
    return sum * specular.rgb;
}

// As the game's vertex shader sets up per-pixel lights: each light's direction in the tangent
// space of the (unnormalized) tangent, the binormal tangent x normal and the normal, times how
// much of the light gets there.
float3 tangentLight(int k, float3 n, float3 t, float3 b, float3 world) {
    if (k >= pixelLights.x) {
        return float3(0, 0, 0);
    }
    const int i = k == 0 ? pixelLights.y : k == 1 ? pixelLights.z : pixelLights.w;
    float reach;
    const float3 l = towardLight(i, world, reach);
    return reach * float3(dot(l, t), dot(l, b), dot(l, n));
}

void pixelLightVectors(float3 normal, float3 tangent, float3 world, inout Output output) {
    const float3 n = normalize(normal);
    const float3 b = cross(tangent, n);
    output.lightVector0 = tangentLight(0, n, tangent, b, world);
    output.lightVector1 = tangentLight(1, n, tangent, b, world);
    output.lightVector2 = tangentLight(2, n, tangent, b, world);
}
