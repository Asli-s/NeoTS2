#pragma once

#include "content/lot.hpp"

#include <cstddef>
#include <string>

namespace ts2::content {

// Build mode's terrain paint brush on a lot's paints: its terrain textures' paint list and each
// paint's strength map (flat grid kFirstPaintMapInstance + its index: a byte, 0 to 255, at four points
// a tile, (tiles * 4 + 1) points each way).

// The index of `texture` in the lot's paint list, added with a strength map of nothing painted when
// it isn't there. Nothing when the lot keeps no terrain textures or its maps' size isn't known.
std::optional<std::size_t> terrainPaintIndex(Lot& lot, const std::string& texture);

// Paints with paint `paint` round (x, y) (in tiles on the grids' axes) out to `radius` tiles, fully
// within the inner 60% and fading to nothing at the edge: its strength rises to the brush's there
// and every other paint's falls by as much, so the new paint shows on top. Without a paint, it rubs
// every paint off the same way, back to the canvas. Returns whether any strength changed.
bool paintTerrain(Lot& lot, std::optional<std::size_t> paint, float x, float y, float radius);

} // namespace ts2::content
