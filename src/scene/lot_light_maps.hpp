#pragma once

#include "scene/lot_lighting.hpp"
#include "scene/lot_shadow.hpp"
#include "scene/scene.hpp"

#include <array>
#include <functional>

namespace ts2::scene::lotlightmaps {

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
