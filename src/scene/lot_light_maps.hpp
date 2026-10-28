#pragma once

#include "scene/lot_lighting.hpp"
#include "scene/lot_shadow.hpp"
#include "scene/scene.hpp"

#include <array>
#include <functional>
#include <vector>

namespace ts2::scene::lotlightmaps {

// The lot's ground normals as the game's terrain works them out. Each tile is two triangles split
// from corner (x, y) to (x + 1, y + 1), and a corner's normal is the normalized sum of the normals
// of the lot's triangles that touch it, so the lot's edges lean on nothing outside the lot.
struct GroundNormals {
    int width = 0, height = 0;                  // tiles
    std::vector<std::array<float, 3>> corners;  // (width + 1) x (height + 1), row by row

    // The normal at a point of the ground: its tile's four corner normals blended bilinearly and
    // normalized. Straight up where there are none.
    std::array<float, 3> at(float x, float y) const;
};
// None for a lot without ground heights or larger than 256 tiles.
GroundNormals groundNormals(const lotshadow::Inputs& lot);

// The buildings' sun shadow for the outdoor sunlight: the lot's shadow grid and what it was made
// from, the fade under the shadow (hmSunLightTransitionZone), which ground-level tiles are bare
// terrain and the terrain's normal at a point.
struct Sunlight {
    const lotshadow::Grid* shadow = nullptr;
    const lotshadow::Inputs* lot = nullptr;
    float zone = 1;
    std::function<bool(int tileX, int tileY)> terrain;
    std::function<std::array<float, 3>(float x, float y)> terrainNormal;
};
// Fills a lot scene's light maps (Scene::lightMaps, laid out by addLot) with the lot's worked-out
// light, texel for texel as the game's lighting manager writes its textures: the floors of each
// level and each wall face, with the unlit room light and the outdoor sunlight added (shaded by
// `sunlight`'s shadow when given). Faces and tiles the light does not reach keep what they hold.
void fill(Scene& scene, const lotlighting::Result& light, const lotlighting::Settings& settings, const Sunlight* sunlight = nullptr);

} // namespace ts2::scene::lotlightmaps
