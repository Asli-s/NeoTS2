#pragma once

#include "scene/lot_lighting.hpp"
#include "scene/lot_shadow.hpp"
#include "scene/scene.hpp"

#include <array>
#include <functional>
#include <vector>

namespace ts2::scene::lotlightmaps {

// A sloping ground's normal `depth` under the sea, turned toward straight up, fully at 1 down, so a
// steep sea bed isn't shaded as a wall facing away from the sun (smooth lot skirts, not in the game).
std::array<float, 3> underwaterNormal(const std::array<float, 3>& normal, float depth);

struct GroundNormals {
    int width = 0, height = 0;                  // tiles
    int steps = 1;                              // points a tile each way
    std::vector<std::array<float, 3>> corners;  // (width * steps + 1) x (height * steps + 1), row by row

    // The normal at a point of the ground: its tile's four corner normals blended bilinearly and
    // normalized. Straight up where there are none.
    std::array<float, 3> at(float x, float y) const;
};
// The lot's ground normals as the game's terrain works them out. Each tile is two triangles split from
// corner (x, y) to (x + 1, y + 1), and a corner's normal is the normalized sum of its lot triangles'
// normals, so the edges lean on nothing outside. None for a lot without ground heights or over 256 tiles.
GroundNormals groundNormals(const lotshadow::Inputs& lot);
// The normals of a lot's ground as drawn (see Scene::drawnGround), from each point's slope to its
// neighbours. Under the sea they turn up toward straight up by depth (underwaterNormal), as the
// light there is scattered. None when it is empty or malformed.
GroundNormals groundNormals(const Scene::DrawnGround& drawn);
// The drawn ground's height at a point (lot tiles), blended between its points. 0 when it is empty
// or malformed.
float drawnHeight(const Scene::DrawnGround& drawn, float x, float y);

// The buildings' sun shadow for the outdoor sunlight: the lot's shadow grid and what it was made
// from, the fade under the shadow (hmSunLightTransitionZone), which ground-level tiles are bare
// terrain and the terrain's normal at a point.
struct Sunlight {
    const lotshadow::Grid* shadow = nullptr;
    const lotshadow::Inputs* lot = nullptr;
    float zone = 1;
    std::function<bool(int tileX, int tileY)> terrain;
    std::function<std::array<float, 3>(float x, float y)> terrainNormal;
    // The shadow with the ground around the lot (lotshadow::around), for the lot skirt.
    const lotshadow::Grid* around = nullptr;
};
// Fills a lot scene's light maps (Scene::lightMaps, laid out by addLot) with the lot's worked-out
// light, texel for texel as the game's lighting manager writes its textures: the floors of each
// level and each wall face, with the unlit room light and the outdoor sunlight added (shaded by
// `sunlight`'s shadow when given). Faces and tiles the light does not reach keep what they hold.
void fill(Scene& scene, const lotlighting::Result& light, const lotlighting::Settings& settings, const Sunlight* sunlight = nullptr);

// The lamps' and windows' light and the buildings' sun shadow past the lot's edge, onto the lot
// skirt, which the game leaves unlit and unshadowed by them. addSkirtLamps gives a lit lot scene its black skirt lamp image (fill fills it).
// linkSkirtLamps points the scene's skirt ground and roads at it, once the skirt is added, the
// lot standing at `place` in the scene.
void addSkirtLamps(Scene& scene);
void linkSkirtLamps(Scene& scene, const Transform& place);

} // namespace ts2::scene::lotlightmaps
