#pragma once

#include "formats/terrain.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace ts2::scene::terrainwater {

// The water's alpha (0-255) by its depth, as Terrain.ini's [WaterRenderParameters]
// AlphaVsDepthCurve gives it: points of (depth, alpha) in rising depth, joined by straight lines and
// held flat past either end.
struct AlphaCurve {
    std::vector<std::pair<float, float>> points;
    [[nodiscard]] float alpha(float depth) const;
};

// The curve from the text of Terrain.ini. Empty when the file has none, or a broken one: points not
// in rising depth, numbers that aren't, or more than 64 points.
[[nodiscard]] AlphaCurve readAlphaCurve(std::string_view ini);

// The water standing in the lot's dug terrain (ponds), as the game builds it: each tile with a corner
// under the water gets four triangles around its centre. A vertex lies at the water's height there
// (a tile's centre at the average of its corners), and its alpha follows the curve by the water's
// depth over the terrain (at the centre, the averages' difference), rounded down to a whole step out of 255.
// Texture coordinates are the vertex's place in tiles.
struct Mesh {
    std::vector<float> positions;  // x, y, z per vertex, in the lot's tiles
    std::vector<float> uvs;
    std::vector<float> alphas;     // 0-1
    std::vector<std::uint32_t> indices;
};

// `water` is the lot's water height per terrain corner (content::kWaterHeightGridInstance) and
// `terrain(x, y)` the ground's height at a corner. Lots over 1024 tiles a side build none.
[[nodiscard]] Mesh build(const terrain::Array2D& water, const std::function<float(int x, int y)>& terrain, const AlphaCurve& curve);

// The water's height at a corner. Empty off the grid, and for a value that isn't a finite number, so
// a broken file can't put a corner at infinity.
[[nodiscard]] std::optional<float> cornerHeight(const terrain::Array2D& water, int x, int y);

// The water's height at a point of the lot, from its corners as the game reads it: the tile is cut
// along its diagonal from corner (0, 0) to (1, 1), and the height is flat on each half. Empty off the
// grid.
[[nodiscard]] std::optional<float> heightAt(const terrain::Array2D& water, float x, float y);

} // namespace ts2::scene::terrainwater
