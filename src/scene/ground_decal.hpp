#pragma once

#include "scene/scene.hpp"

#include <array>

namespace ts2::scene {

// The widest ground decal drawn: a broken file's decal must not cover more than the largest
// neighborhood (128 tiles of 10).
constexpr float kMaxGroundDecalWidth = 1280;
// The narrowest: below it the square is lost in the precision of its place.
constexpr float kMinGroundDecalWidth = 0.01f;
// The least and most depth over width (a decal's aspect). Its depth must also be within the widths above.
constexpr float kMinGroundDecalAspect = 0.01f, kMaxGroundDecalAspect = 100;

// A ground decal's mesh as the game builds it: every terrain tile, whole and on its own corners, that a
// rectangle at `centre`, `width` wide and `aspect` times as deep, turned by `turns` (full turns), reaches.
// The texture spans the rectangle (u along the turned x axis, v a quarter turn on) and runs past 0 and 1 beyond it, hidden
// there by its clamped clear edge. Empty for a broken terrain or a rectangle that isn't finite, breaks
// the limits above or lies far off the terrain.
Primitive groundDecal(const PaintedTerrain& terrain, const std::array<float, 2>& centre, float width, float turns, int material,
                      float aspect = 1);

} // namespace ts2::scene
