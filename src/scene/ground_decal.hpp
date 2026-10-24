#pragma once

#include "scene/scene.hpp"

#include <array>

namespace ts2::scene {

// The widest ground decal drawn: a broken file's decal must not cover more than the largest
// neighborhood (128 tiles of 10).
constexpr float kMaxGroundDecalWidth = 1280;
// The narrowest: below it the square is lost in the precision of its place.
constexpr float kMinGroundDecalWidth = 0.01f;

// A ground decal's mesh: the terrain's triangles under a square `width` wide at `centre`, turned by
// `turns` (full turns), split as the terrain's tiles are and cut to the square. Its texture spans the
// square, u along the turned x axis and v a quarter turn on. Empty for a broken terrain, or a
// square that isn't finite, is outside the width limits above or lies far off the terrain.
Primitive groundDecal(const PaintedTerrain& terrain, const std::array<float, 2>& centre, float width, float turns, int material);

} // namespace ts2::scene
