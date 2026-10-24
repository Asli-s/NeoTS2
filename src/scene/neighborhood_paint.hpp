#pragma once

#include "formats/nhood_paint_rules.hpp"
#include "formats/nhood_terrain.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ts2::scene {

// Up to three paints on a terrain vertex: paint ids (0 for an empty slot) and their weights,
// which add up to 250 once painting is done.
struct VertexPaints {
    std::array<std::uint8_t, 3> id{};
    std::array<std::uint16_t, 3> weight{};
};

struct TerrainPaints {
    std::uint32_t columns = 0, rows = 0;  // vertices
    std::vector<VertexPaints> vertices;   // row by row
    std::vector<float> normals;           // x, y, z per vertex
};

// The vertex normals the game uses: the six triangles around a vertex, each normalized, summed
// and normalized.
std::vector<float> terrainNormals(const nhood::Terrain& terrain, float tileSize);

// Paints the terrain as the game does when it loads a neighborhood: cliffs where it is steep,
// beach near the water, elsewhere the type's paint table by height and nearness to roads, each
// spread around by its curve. `roads` has one byte per tile, row by row (nonzero: a road on it),
// empty for none.
// `normals` (x, y, z per vertex) replaces terrainNormals when given.
TerrainPaints paintTerrain(const nhood::Terrain& terrain, const nhood::PaintRules& rules, float tileSize,
                           std::span<const std::uint8_t> roads, std::span<const float> normals = {});

} // namespace ts2::scene
