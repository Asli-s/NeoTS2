#include "render/terrain_mesh.hpp"

#include <algorithm>
#include <utility>

namespace ts2::render {

IndexedMesh terrainGrid(const scene::PaintedTerrain& t) {
    IndexedMesh m;
    m.vertices.resize(static_cast<std::size_t>(t.columns) * t.rows * kTerrainFloats);
    for (std::uint32_t y = 0; y < t.rows; ++y) {
        for (std::uint32_t x = 0; x < t.columns; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * t.columns + x;
            float* p = &m.vertices[i * kTerrainFloats];
            p[0] = static_cast<float>(x) * t.tileSize;
            p[1] = static_cast<float>(y) * t.tileSize;
            p[2] = t.heights[i];
            std::copy_n(&t.normals[i * 3], 3, p + 3);
            p[6] = static_cast<float>(x);
            p[7] = static_cast<float>(y);
        }
    }
    // Two triangles per tile, split from its (x, y) corner to its (x + 1, y + 1) corner.
    m.indices.reserve(static_cast<std::size_t>(t.columns - 1) * (t.rows - 1) * 6);
    for (std::uint32_t y = 0; y + 1 < t.rows; ++y) {
        for (std::uint32_t x = 0; x + 1 < t.columns; ++x) {
            const std::uint32_t a = y * t.columns + x, b = a + 1, d = a + t.columns, e = d + 1;
            if (!t.hiddenTiles.empty() && t.hiddenTiles[static_cast<std::size_t>(y) * (t.columns - 1) + x]) {
                continue;
            }
            m.indices.insert(m.indices.end(), {a, b, e, a, e, d});
        }
    }
    return m;
}

IndexedMesh waterSheet(const scene::PaintedTerrain& t) {
    IndexedMesh m;
    const std::uint32_t tilesX = t.columns - 1;
    for (std::size_t i = 0; i < t.waterTiles.size(); ++i) {
        if (!t.waterTiles[i] || (!t.hiddenTiles.empty() && t.hiddenTiles[i])) {
            continue;
        }
        const auto x = static_cast<float>(i % tilesX), y = static_cast<float>(i / tilesX);
        const auto base = static_cast<std::uint32_t>(m.vertices.size() / kWaterFloats);
        for (const auto [cx, cy] : {std::pair{x, y}, std::pair{x + 1, y}, std::pair{x + 1, y + 1}, std::pair{x, y + 1}}) {
            m.vertices.insert(m.vertices.end(), {cx * t.tileSize, cy * t.tileSize, t.waterLevel, cx, cy});
        }
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return m;
}

RoadMesh roadQuads(const scene::PaintedTerrain& t) {
    RoadMesh r;
    r.mesh.vertices.reserve(t.roads.size() * 24);
    r.mesh.indices.reserve(t.roads.size() * 6);
    for (const auto& road : t.roads) {
        const auto base = static_cast<std::uint32_t>(r.mesh.vertices.size() / kRoadFloats);
        for (const auto& corner : road.corners) {
            r.mesh.vertices.insert(r.mesh.vertices.end(), {corner[0], corner[1], corner[2], corner[3], corner[4], static_cast<float>(road.texture)});
        }
        const auto first = static_cast<std::uint32_t>(r.mesh.indices.size());
        r.mesh.indices.insert(r.mesh.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
        if (r.runs.empty() || r.runs.back().texture != road.texture) {
            r.runs.push_back({road.texture, first, 0});
        }
        r.runs.back().indexCount += 6;
    }
    return r;
}

ImposterMesh imposterParts(const scene::PaintedTerrain& t) {
    ImposterMesh m;
    for (const auto& part : t.imposters) {
        if (part.texture >= t.imposterTextures.size()) {
            continue;
        }
        ImposterMesh::Part draw{part.kind};
        draw.texture = part.texture;
        draw.lightMap = part.lightMap >= 0 && static_cast<std::size_t>(part.lightMap) < t.imposterTextures.size() ? part.lightMap : -1;
        draw.tint = part.lightMapTint;
        draw.firstIndex = static_cast<std::uint32_t>(m.mesh.indices.size());
        draw.indexCount = static_cast<std::uint32_t>(part.indices.size());
        draw.vertexOffset = static_cast<std::int32_t>(m.mesh.vertices.size() / kImposterFloats);
        m.mesh.vertices.insert(m.mesh.vertices.end(), part.vertices.begin(), part.vertices.end());
        m.mesh.indices.insert(m.mesh.indices.end(), part.indices.begin(), part.indices.end());
        m.draws.push_back(draw);
    }
    return m;
}

IndexedMesh boundaryCurtain(const scene::Scene::BoundaryClip& b) {
    IndexedMesh m;
    const float corners[4][2] = {{b.x0, b.y0}, {b.x1, b.y0}, {b.x1, b.y1}, {b.x0, b.y1}};
    for (int i = 0; i < 4; ++i) {
        const auto& p = corners[i];
        const auto& q = corners[(i + 1) % 4];
        const auto base = static_cast<std::uint32_t>(m.vertices.size() / kCurtainFloats);
        m.vertices.insert(m.vertices.end(), {p[0], p[1], b.bottom, q[0], q[1], b.bottom, q[0], q[1], b.top, p[0], p[1], b.top});
        m.indices.insert(m.indices.end(), {base, base + 3, base + 2, base, base + 2, base + 1});
    }
    return m;
}

} // namespace ts2::render
