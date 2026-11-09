#include "scene/effect_terrain.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

namespace ts2::scene {

namespace {

// Where a point falls on the vertex grid: the tile's corner and how far across it, each 0 to 1.
struct GridPlace {
    std::size_t column = 0, row = 0;
    float across = 0, up = 0;
};

GridPlace gridPlace(const PaintedTerrain& t, float x, float y) {
    const float u = std::clamp(std::isfinite(x) ? x / t.tileSize : 0.0f, 0.0f, static_cast<float>(t.columns - 1));
    const float v = std::clamp(std::isfinite(y) ? y / t.tileSize : 0.0f, 0.0f, static_cast<float>(t.rows - 1));
    GridPlace out;
    out.column = std::min(static_cast<std::size_t>(u), static_cast<std::size_t>(t.columns - 2));
    out.row = std::min(static_cast<std::size_t>(v), static_cast<std::size_t>(t.rows - 2));
    out.across = u - static_cast<float>(out.column);
    out.up = v - static_cast<float>(out.row);
    return out;
}

} // namespace

EffectTerrain effectTerrain(const PaintedTerrain& terrain) {
    EffectTerrain out;
    const std::size_t corners = static_cast<std::size_t>(terrain.columns) * terrain.rows;
    if (terrain.columns < 2 || terrain.rows < 2 || !(terrain.tileSize > 0) || terrain.heights.size() < corners ||
        terrain.normals.size() < corners * 3) {
        return out;
    }
    out.width = static_cast<float>(terrain.columns - 1) * terrain.tileSize;
    out.depth = static_cast<float>(terrain.rows - 1) * terrain.tileSize;
    out.waterLevel = terrain.waterLevel;
    // A copy, so the functions outlive any change to the scene's terrain.
    const auto grid = std::make_shared<PaintedTerrain>();
    grid->columns = terrain.columns;
    grid->rows = terrain.rows;
    grid->tileSize = terrain.tileSize;
    grid->heights = terrain.heights;
    grid->normals = terrain.normals;
    out.height = [grid](float x, float y) {
        const PaintedTerrain& t = *grid;
        const GridPlace g = gridPlace(t, x, y);
        const std::size_t at = g.row * t.columns + g.column;
        const float low = t.heights[at] + (t.heights[at + 1] - t.heights[at]) * g.across;
        const float high = t.heights[at + t.columns] + (t.heights[at + t.columns + 1] - t.heights[at + t.columns]) * g.across;
        return low + (high - low) * g.up;
    };
    out.normal = [grid](float x, float y) {
        const PaintedTerrain& t = *grid;
        const GridPlace g = gridPlace(t, x, y);
        const std::size_t at = g.row * t.columns + g.column;
        std::array<float, 3> n{};
        for (std::size_t i = 0; i < 3; ++i) {
            const float a = t.normals[at * 3 + i], b = t.normals[(at + 1) * 3 + i];
            const float c = t.normals[(at + t.columns) * 3 + i], d = t.normals[(at + t.columns + 1) * 3 + i];
            const float low = a + (b - a) * g.across, high = c + (d - c) * g.across;
            n[i] = low + (high - low) * g.up;
        }
        const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (!(length > 0) || !std::isfinite(length)) {
            return std::array<float, 3>{0, 0, 1};
        }
        return std::array<float, 3>{n[0] / length, n[1] / length, n[2] / length};
    };
    return out;
}

} // namespace ts2::scene
