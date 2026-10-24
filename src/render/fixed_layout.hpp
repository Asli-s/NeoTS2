// A neighborhood's terrain, roads and water laid out for drawing without shaders. The shaders work out
// per pixel what the fixed pipeline can't: the paints' alpha maps and variant cells, the underwater
// gradient and the sky the water reflects. Here they are worked out once, on the CPU, when the scene is
// uploaded. Nothing here depends on a graphics API.
#pragma once

#include "render/terrain_mesh.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace ts2::render {

constexpr unsigned kFixedTerrainFloats = 9;  // position, then the texture coordinates of the paint, of its weights and of the light map
constexpr unsigned kFixedRoadFloats = 7;     // position, texture coordinates, the light map's

// The terrain grid's vertices with the texture coordinates of the three textures a pass draws with.
std::vector<float> fixedTerrainVertices(const IndexedMesh& grid, const scene::PaintedTerrain& t);

// The colour of each grid vertex: the underwater gradient at its height (white without one), as RGBA bytes.
std::vector<std::uint8_t> fixedTerrainColors(const IndexedMesh& grid, const scene::PaintedTerrain& t);

// The colour of each grid vertex in the pass for paint texture `slice`, when a pass has only one texture
// to draw with: that paint's weight, the underwater gradient and (at or above the water) the light map,
// all multiplied out at the vertex and so blended linearly across each tile, as RGBA bytes.
std::vector<std::uint8_t> fixedSliceColors(const IndexedMesh& grid, const scene::PaintedTerrain& t, unsigned slice);

// The terrain light map at each road vertex, as RGBA bytes (white without a light map).
std::vector<std::uint8_t> fixedRoadColors(const RoadMesh& roads, const scene::PaintedTerrain& t, float lightMapSize);

// How much paint texture `slice` shows at each texel of the alpha maps: the alpha of every layer whose
// cell there shows that texture, in the red, green and blue of an alphaSize square RGBA picture.
std::vector<std::uint8_t> fixedPaintWeights(const scene::PaintedTerrain& t, unsigned slice);

// The road quads' vertices with the light map's texture coordinates, which the world position gives.
std::vector<float> fixedRoadVertices(const RoadMesh& roads, float lightMapSize);

// The average colour of the images (RGB, 0 to 1), looking at a spread of their texels. White for none.
std::array<float, 3> averageColor(const std::vector<scene::Image>& images);

} // namespace ts2::render
