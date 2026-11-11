// Not in the original game: the pool-style sea's haze, shared by terrain.frag (the neighborhood's
// ground, also around the lot in lot view) and scene.frag (what stands in the sea, and lot view's
// skirt ground). In the shallows the bottom
// keeps its own light, only tinted toward HOOD_TINT, as clear shallow water shows it. Deeper, a light
// murk thickens toward HOOD_LIGHT_MAX (HOOD_LIGHT_SCALE units to come most of the way), and a dark one
// past 0.8 down.
#define HOOD_TINT float3(0.78, 0.9, 0.88)
#define HOOD_LIGHT float3(0.34, 0.47, 0.42)
#define HOOD_DEEP float3(0.05, 0.11, 0.11)
#define HOOD_LIGHT_MAX 0.75
#define HOOD_LIGHT_SCALE 1.2
// Lot view's haze comes in over the first LOT_FADE units of depth and spreads its murk over
// LOT_STRETCH times the depth, the same on the skirt and on the neighborhood's ground around it.
#define LOT_FADE 1.5
#define LOT_STRETCH 1.5

// What lies `depth` under that sea becomes its colour times `scale` plus `added`. `fadeIn` (0 to 1)
// brings the whole haze in at the shore, and `light` is how bright the sea's light is now. `stretch`
// spreads the murk over more depth (1 for none): lot view, seen close up, takes 1.5, so a steep bank
// fades softly into the water.
struct PoolSeaHaze {
    float3 scale;
    float3 added;
    float clear;  // how much of a light added under the sea still shows through
};

PoolSeaHaze poolSeaHazeAt(float depth, float fadeIn, float light, float stretch) {
    const float d = max(depth, 0.0) / stretch;
    // The tint comes in over the first 0.6 down, the light murk only from 0.2 down, fully past 1.7.
    const float tint = saturate(d / 0.6) * fadeIn;
    const float murk = HOOD_LIGHT_MAX * (1 - exp(-d / HOOD_LIGHT_SCALE)) * saturate((d - 0.2) / 1.5) * fadeIn;
    const float deep = 0.95 * (1 - exp(-max(d - 0.8, 0.0) / 2)) * fadeIn;
    PoolSeaHaze h;
    h.scale = lerp(float3(1, 1, 1), HOOD_TINT, tint) * (1 - murk) * (1 - deep);
    h.added = (HOOD_LIGHT * murk * (1 - deep) + HOOD_DEEP * deep) * light;
    h.clear = (1 - murk) * (1 - deep);
    return h;
}
