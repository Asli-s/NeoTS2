#pragma once

// Fewer, larger draws for parts of a scene that are only looked at, such as a neighbor's house
// shown as a real house in lot view.

#include "scene/scene.hpp"

namespace ts2::scene {

// Joins instances drawn alike (same light, light map, storey and so on) within each `cell` square: their
// opaque and cut-out primitives become one per material and sun share, vertices placed as the instances
// put them. Blended and layered primitives, skinned or morphing meshes, shadow groups, lot objects, walls
// views cut and large meshes placed more than once stay as they were. Unused meshes are dropped. Scenes
// with Sims or scripted objects (which name their instances) are left alone. Returns how many were joined.
std::size_t mergeAlike(Scene& scene, float cell);

} // namespace ts2::scene
