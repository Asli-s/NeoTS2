// The geometry of a neighborhood's terrain, water, roads and lot imposters, and of a lot's boundary
// curtain, laid out for the GPU. Every renderer draws the same vertices; none of it needs a graphics API.
#pragma once

#include "scene/scene.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace ts2::render {

struct IndexedMesh {
    std::vector<float> vertices;
    std::vector<std::uint32_t> indices;
};

constexpr unsigned kTerrainFloats = 8;   // position, normal, grid position
constexpr unsigned kWaterFloats = 5;     // position, tile coordinates
constexpr unsigned kRoadFloats = 6;      // position, texture coordinates, which road texture
constexpr unsigned kImposterFloats = 7;  // position, texture coordinates, the second set's
constexpr unsigned kCurtainFloats = 3;   // position

// A vertex per tile corner, and two triangles per tile (but those hidden under a lot's own ground).
IndexedMesh terrainGrid(const scene::PaintedTerrain& t);
// A quad per wet tile at the water level. Empty when there is no water.
IndexedMesh waterSheet(const scene::PaintedTerrain& t);

// A quad per road tile, in the roads' order. `runs` are the stretches of consecutive quads that share a
// road texture, for renderers that bind one texture at a time.
struct RoadMesh {
    IndexedMesh mesh;
    struct Run {
        std::uint32_t texture = 0, firstIndex = 0, indexCount = 0;
    };
    std::vector<Run> runs;
};
RoadMesh roadQuads(const scene::PaintedTerrain& t);

// The lots' imposters: all parts in one buffer. `draws` are the parts that have their texture, in the order they are drawn in.
struct ImposterMesh {
    IndexedMesh mesh;
    struct Part {
        scene::PaintedTerrain::ImposterPart::Kind kind;
        std::uint32_t texture = 0;  // index into the terrain's imposterTextures
        int lightMap = -1;          // likewise, -1 for none
        std::array<float, 3> tint{1, 1, 1};
        std::uint32_t firstIndex = 0, indexCount = 0;
        std::int32_t vertexOffset = 0;  // in vertices
    };
    std::vector<Part> draws;
};
ImposterMesh imposterParts(const scene::PaintedTerrain& t);

// A lot skirt's boundary curtain: a wall on each edge, facing inwards.
IndexedMesh boundaryCurtain(const scene::Scene::BoundaryClip& clip);

} // namespace ts2::render
