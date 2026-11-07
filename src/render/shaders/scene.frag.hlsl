// Scene surfaces: the material's color times its texture, lit by their light map (a lot's floors and
// walls), as the game lights objects when the vertices bring that light, otherwise by a sky and a
// sun, unless unlit.

#include "sdl_gpu.hlsli"

COMBINED Texture2D<float4> baseTexture : register(t0, space2);
COMBINED SamplerState baseSampler : register(s0, space2);
COMBINED Texture2D<float4> lightMapTexture : register(t1, space2);
COMBINED SamplerState lightMapSampler : register(s1, space2);
COMBINED Texture2D<float4> normalTexture : register(t2, space2);
COMBINED SamplerState normalSampler : register(s2, space2);
COMBINED Texture2D<float4> incidenceTexture : register(t3, space2);
COMBINED SamplerState incidenceSampler : register(s3, space2);
COMBINED Texture2D<float4> lightMapFromTexture : register(t4, space2);  // the light map being faded from
COMBINED SamplerState lightMapFromSampler : register(s4, space2);
COMBINED TextureCube<float4> envCubeTexture : register(t5, space2);
COMBINED SamplerState envCubeSampler : register(s5, space2);
#ifndef GL2
COMBINED Texture2D<float4> shadowTexture : register(t6, space2);  // the Sims' blurred shadow pictures
COMBINED SamplerState shadowSampler : register(s6, space2);
COMBINED Texture2D<float4> objectPictures : register(t7, space2);  // the lot objects' pictures from the sun
COMBINED SamplerState objectPicturesSampler : register(s7, space2);
COMBINED Texture2D<float4> objectPlaces : register(t8, space2);    // where each object lays its picture
COMBINED SamplerState objectPlacesSampler : register(s8, space2);
COMBINED Texture2D<float4> objectGridTexture : register(t9, space2);  // which places reach each tile
COMBINED SamplerState objectGridSampler : register(s9, space2);
COMBINED Texture2D<float4> causticsTexture : register(t10, space2);   // the pools' caustics tile
COMBINED SamplerState causticsSampler : register(s10, space2);
COMBINED Texture2D<float4> poolShadowTexture : register(t11, space2); // the pools' shadow map, depth along the sun
COMBINED SamplerState poolShadowSampler : register(s11, space2);
COMBINED Texture2D<float> occlusionTexture : register(t12, space2);   // how open each pixel of the view is
COMBINED SamplerState occlusionSampler : register(s12, space2);
#endif

cbuffer Material : register(b0, space3) {
    float4 color;
    float4 sunDirection;  // toward the sun, xyz, w unused
    float hasTexture;
    float alphaCutoff;    // discard below this (0 for none)
    float unlit;          // 1: color and texture as they are, without light
    float alphaReplicate; // 1: the texture's alpha stands in for its colour
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
    float4 uvTile;    // the texture coordinates times xy plus zw: one frame of a tiled texture
    float4 shadowReceiver;  // x: 1 when Sims' projected shadows fall on it, y: its level, z: 1 to fade them by the light map
    float4 caustics;        // x: 1 for a pool floor, 2 for a pool wall, 3 for the caustics alone over a pond's ground, 4 the same over a beach sea's, 0 for none, yzw: the texture light
    float4 neighborLight;   // rgb: what a neighbor's real house is tinted by, w: 1 when it is one, 2 for the ground around the lot, 3 for its ground
};

#ifndef GL2
// The Sims' projected shadows, given once a frame. Each Sim's picture is a tile of the atlas.
cbuffer Shadows : register(b1, space3) {
    row_major float4x4 shadowCameras[16];  // world to each picture's clip space
    float4 shadowCasters[16];              // xy: the tile's corner in the atlas, z: the Sim's level
    float4 shadowShade;                    // rgb: what a shadow multiplies the ground by, w: how many Sims
    float4 objectGrid;                     // the objects' grid: its first tile (xy) and its size in tiles (zw)
    row_major float4x4 poolShadow;         // world to the pool shadow map's texture coordinates and depth
    row_major float4x4 toLot;              // world to the lot
    float4 poolSun;                        // toward the sun
    float4 ambientOcclusion;               // x: 1 when these surfaces are multiplied by it (not in the original game)
    float4 neighborGround[6];              // the lots of neighbors drawn as real houses: x0, x1, y0, y1 (kMaxNeighborGrounds of them)
    float4 neighborGrounds;                // x: how many
};
#endif

#ifndef GL2
// Whether a lot object on the receiver's storey lays its picture from the sun over this point: its
// picture holds more than 100/255 there. The grid over the lot's tiles names the objects to look at.
bool objectShadowAt(float3 world) {
    const int2 tile = int2(floor(world.xy - objectGrid.xy));
    if (tile.x < 0 || tile.y < 0 || tile.x >= (int)objectGrid.z || tile.y >= (int)objectGrid.w) {
        return false;
    }
    [unroll] for (int half = 0; half < 2; ++half) {
        const float4 slots = objectGridTexture.Load(int3(tile.x * 2 + half, tile.y, 0));
        [unroll] for (int k = 0; k < 4; ++k) {
            const int place = (int)slots[k];
            if (place < 0) {
                return false;
            }
            const float4 picture = objectPlaces.Load(int3(3, place, 0));  // xy: tile corner, z: share, w: level
            const float4 rect = objectPlaces.Load(int3(4, place, 0));
            if (abs(picture.w - shadowReceiver.y) > 0.5 || world.x < rect.x || world.x > rect.y || world.y < rect.z || world.y > rect.w) {
                continue;
            }
            const float4 w = float4(world, 1);
            const float2 p = float2(dot(objectPlaces.Load(int3(0, place, 0)), w), dot(objectPlaces.Load(int3(1, place, 0)), w));
            if (abs(p.x) > 1 || abs(p.y) > 1) {
                continue;
            }
            // Kept half a texel inside the picture, so the filter never reaches past it.
            const float texels = picture.z * 2048;  // the atlas is 2048 texels square
            const float2 inPicture = clamp(float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5), 0.5 / texels, 1 - 0.5 / texels);
            if (objectPictures.SampleLevel(objectPicturesSampler, picture.xy + inPicture * picture.z, 0).a > 100.0 / 255) {
                return true;
            }
        }
    }
    return false;
}
#endif

// What a Sim's or a lot object's projected shadow multiplies this point of a floor or the terrain
// by: its colour where a caster on the same storey has a blurred picture holding more than 100/255.
// On the ground storey it fades to none where the buildings' shadow already takes the sun (`sun`,
// the light map's alpha). Shadows don't add up.
float3 simShadow(float3 world, float sun) {
#ifndef GL2
    if (shadowReceiver.x < 0.5) {
        return float3(1, 1, 1);
    }
    if (objectShadowAt(world)) {
        return lerp(float3(1, 1, 1), shadowShade.rgb, shadowReceiver.z > 0.5 ? sun : 1);
    }
    const int count = (int)shadowShade.w;
    [loop] for (int i = 0; i < count; ++i) {
        if (abs(shadowCasters[i].z - shadowReceiver.y) > 0.5) {
            continue;
        }
        const float4 p = mul(shadowCameras[i], float4(world, 1));
        if (abs(p.x) > 1 || abs(p.y) > 1) {
            continue;
        }
        // Kept half a texel inside the tile, so the filter never reaches the next one.
        const float2 inTile = clamp(float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5), 0.5 / 128, 1 - 0.5 / 128);
        if (shadowTexture.SampleLevel(shadowSampler, shadowCasters[i].xy + inTile * 0.25, 0).a > 100.0 / 255) {
            return lerp(float3(1, 1, 1), shadowShade.rgb, shadowReceiver.z > 0.5 ? sun : 1);
        }
    }
#endif
    return float3(1, 1, 1);
}

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

#ifndef GL2
// How much of the sun reaches a point of a pool's floor or wall past what the pool shadow map holds,
// 0 to 1: sixteen texels around it, each counted with the game's weight when its depth lies beyond
// the point's (less `bias`).
float poolSunAt(float3 world, float bias) {
    const float3 at = mul(poolShadow, float4(world, 1)).xyz;
    const float depth = at.z - bias;
    static const float kNear = 0.000244141, kFar = 0.000732422;
    static const float4 kWeights[4] = {float4(0.0479224, 0.0615335, 0.0615335, 0.0479224), float4(0.0615335, 0.0790106, 0.0790106, 0.0615335),
                                       float4(0.0615335, 0.0790106, 0.0790106, 0.0615335), float4(0.0479224, 0.0615335, 0.0615335, 0.0479224)};
    static const float kSteps[4] = {-kFar, -kNear, kNear, kFar};
    float sun = 0;
    [unroll] for (int j = 0; j < 4; ++j) {
        [unroll] for (int k = 0; k < 4; ++k) {
            const float stored = poolShadowTexture.SampleLevel(poolShadowSampler, at.xy + float2(kSteps[j], kSteps[k]), 0).r;
            sun += stored > depth ? kWeights[j][k] : 0;
        }
    }
    return sun;
}

// A pool's floor or wall as the game's caustics shaders draw it: the texture lit by the light map
// at night (by day by half, so as it is), most of it whatever the sun, the rest and the caustics
// tile where the sun reaches. On walls only faces toward the sun get it, and the caustics fade
// toward the wall's top.
float4 poolCaustics(Input input, float4 texel, float3 map) {
    const float3 base = hasTexture > 0.5 ? texel.rgb : color.rgb;
    const float3 lightMapped = caustics.y < 0.5 ? map : float3(0.5, 0.5, 0.5);
    const float3 result = base * saturate(lightMapped * 2);
    const float3 lot = mul(toLot, float4(input.world, 1)).xyz;
    const float3 textureLight = caustics.yzw;
    if (caustics.x > 2.5) {
        // The ground under a pond drawn like a pool (not in the original game): the floors' caustics
        // alone, added over it. The pattern is cast along the sun, so it lies unbroken over the bottom
        // and the banks, and fades softly to a third on banks turned from the sun.
        const float3 toSun = normalize(mul((float3x3)toLot, poolSun.xyz));
        const float3 across = normalize(cross(toSun, abs(toSun.z) < 0.99 ? float3(0, 0, 1) : float3(1, 0, 0)));
        const float3 along = cross(toSun, across);
        const float3 caustic = causticsTexture.Sample(causticsSampler, 0.3 * float2(dot(lot, across), dot(lot, along))).rgb;
        const float facing = saturate(dot(normalize(mul((float3x3)toLot, input.normal)), toSun));
        // The vertex alpha is the depth under the water over 0.75: 0 at the waterline, 1 at 0.75 down.
        // The murky water dims them deeper down.
        const float murk = exp(-0.45 * max(input.alpha, 0));
        // A beach sea's (4) light steep faces as fully as the bottom.
        const float turned = caustics.x > 3.5 ? 1 : lerp(0.35, 1, sqrt(facing));
        const float sun = poolSunAt(input.world, 0.003) * turned * saturate(input.alpha) * murk;
        return float4(caustic * sun * textureLight, 1);
    }
    if (caustics.x < 1.5) {
        const float2 uv = 0.3 * float2(lot.x - lot.y, lot.x + lot.y);
        const float sun = poolSunAt(input.world, 0.003);
        const float3 caustic = causticsTexture.Sample(causticsSampler, uv).rgb;
        return float4(result * 0.9 + caustic * sun * textureLight + 0.1 * result * sun, texel.a);
    }
    const float2 uv = 0.3 * float2(lot.x - 2.5 * lot.y, lot.x + lot.y + lot.z);
    const float facing = dot(input.normal, poolSun.xyz) > 0.11 ? 1 : 0;
    const float sun = poolSunAt(input.world, 0.011) * facing;
    const float3 caustic = causticsTexture.Sample(causticsSampler, uv).rgb;
    return float4(result * 0.8 + caustic * textureLight * sun * input.uv.y + 0.2 * result * sun, texel.a);
}
#endif

float4 shade(Input input) {
    float4 texel = hasTexture > 0.5 ? baseTexture.Sample(baseSampler, input.uv) : float4(1, 1, 1, 1);
    if (alphaReplicate > 0.5) {
        texel.rgb = texel.aaa;
    }
    float4 albedo = color * texel;
    albedo.rgb *= input.color;
    albedo.a *= input.alpha;
    if (albedo.a < alphaCutoff) {
        discard;
    }
#ifndef GL2
    // The pools' caustics alone over ground with no light map of its own (a beach sea drawn with
    // them, not in the original game).
    if (caustics.x > 2.5 && lightMap.x <= 0) {
        return poolCaustics(input, texel, float3(0, 0, 0));
    }
#endif
    // A lot's floor or wall, as the game draws it: its texture (floors: times the material's diffuse,
    // walls: alone) times the light map times 2, and no other light. The game draws walls and floors
    // with its own light-mapped material, so a pattern whose material is unlit is light-mapped too.
    if (lightMap.x > 0) {
        const float3 base = lightMap.y > 0.5 && hasTexture > 0.5 ? texel.rgb : albedo.rgb;
        float4 map = lightMapTexture.Sample(lightMapSampler, input.lightUv);
        if (lightMapFade < 1) {
            map = lerp(lightMapFromTexture.Sample(lightMapFromSampler, input.lightUv), map, lightMapFade);
        }
        const float3 lit = map.rgb;
#ifndef GL2
        if (caustics.x > 0.5) {
            return poolCaustics(input, texel, lit);
        }
#endif
        const float3 shadow = simShadow(input.world, map.a);
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
            return float4(base * saturate(lit * lightMap.x) * shadow, albedo.a);
        }
        return float4(saturate(base * lit * lightMap.x * bump) * shadow, albedo.a);
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
// cube's colour scale already holds the texture light.
float4 main(Input input) : SV_Target {
#ifndef GL2
    // A neighbor drawn as a real house brings its own ground.
    if (neighborLight.w > 1.5 && neighborLight.w < 2.5) {
        const int count = (int)neighborGrounds.x;
        for (int i = 0; i < count; ++i) {
            const float4 lot = neighborGround[i];
            if (input.world.x > lot.x && input.world.x < lot.y && input.world.y > lot.z && input.world.y < lot.w) {
                discard;
            }
        }
    }
#endif
    input.uv = input.uv * uvTile.xy + uvTile.zw;
    float4 lit = shade(input);
#ifndef GL2
    // Not on Sims, who still darken what is around them. Their low-poly arms sit so close to their bodies
    // that those sides would read as fully enclosed, and turn black and speckled.
    if (ambientOcclusion.x > 0.5 && simLighting < 0.5) {
        lit.rgb *= occlusionTexture.Load(int3(input.position.xy, 0));
    }
#endif
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
    if (neighborLight.w > 0.5 && neighborLight.w < 1.5) {
        lit.rgb *= neighborLight.rgb;
    } else if (neighborLight.w > 2.5) {
        // Its light map keeps the day's shadows, and the light is the hour's, as on the ground around the lot.
        lit.rgb *= input.light.rgb;
    }
    return lit;
}
