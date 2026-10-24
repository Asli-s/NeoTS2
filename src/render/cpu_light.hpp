// The game's object lighting at one vertex, worked out on the CPU, for renderers that have no vertex
// shaders (the fixed-function one). It does what gameLight does in shaders/object_light.hlsli.
#pragma once

#include "render/frame_plan.hpp"
#include "render/math.hpp"

#include <array>

namespace ts2::render {

// The light at a vertex whose normal and place in the world are given: the ambient plus each light's
// colour times how much the normal faces it and how much of it gets there. w is 0 when the instance
// isn't lit as the game lights objects, 1 for the light itself, 2 for a family thumbnail's (already
// doubled, as its texture stage does). With `allLights` the lights the shaders light per pixel
// through the normal map are summed here too, as a renderer with no normal maps has no other place for them.
std::array<float, 4> gameLight(const VertexUniforms& frame, const Vec3& normal, const Vec3& world, bool allLights);

} // namespace ts2::render
