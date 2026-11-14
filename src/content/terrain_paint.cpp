#include "content/terrain_paint.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::content {

namespace {

constexpr int kStepsPerTile = 4;
constexpr float kFullWithin = 0.6f;  // of the radius

terrain::Array2D* paintMap(Lot& lot, std::size_t paint) {
    const auto it = lot.flatGrids.find(kFirstPaintMapInstance + static_cast<std::uint32_t>(paint));
    return it != lot.flatGrids.end() && it->second.elementSize >= 1 ? &it->second : nullptr;
}

} // namespace

std::optional<std::size_t> terrainPaintIndex(Lot& lot, const std::string& texture) {
    if (!lot.terrainTextures) {
        return std::nullopt;
    }
    auto& paints = lot.terrainTextures->paints;
    if (const auto it = std::find(paints.begin(), paints.end(), texture); it != paints.end()) {
        return static_cast<std::size_t>(it - paints.begin());
    }
    // The map's size: the lot's, four points a tile and one more each way.
    std::uint32_t width = 0, height = 0, version = 0;
    if (const auto* first = paints.empty() ? nullptr : paintMap(lot, 0)) {
        width = first->width;
        height = first->height;
        version = first->version;
    } else if (lot.terrainTextures->width > 0 && lot.terrainTextures->height > 0) {
        width = lot.terrainTextures->width * kStepsPerTile + 1;
        height = lot.terrainTextures->height * kStepsPerTile + 1;
    }
    if (width == 0 || height == 0) {
        return std::nullopt;
    }
    terrain::Array2D map;
    map.version = version;
    map.width = width;
    map.height = height;
    map.elementSize = 1;
    map.elements.assign(static_cast<std::size_t>(width) * height, 0);
    const std::size_t index = paints.size();
    paints.push_back(texture);
    lot.flatGrids[kFirstPaintMapInstance + static_cast<std::uint32_t>(index)] = std::move(map);
    return index;
}

bool paintTerrain(Lot& lot, std::optional<std::size_t> paint, float x, float y, float radius) {
    if (!lot.terrainTextures || radius <= 0) {
        return false;
    }
    const std::size_t count = lot.terrainTextures->paints.size();
    bool changed = false;
    const float step = 1.0f / kStepsPerTile;
    const int i0 = static_cast<int>(std::floor((x - radius) * kStepsPerTile)), i1 = static_cast<int>(std::ceil((x + radius) * kStepsPerTile));
    const int j0 = static_cast<int>(std::floor((y - radius) * kStepsPerTile)), j1 = static_cast<int>(std::ceil((y + radius) * kStepsPerTile));
    for (int i = i0; i <= i1; ++i) {
        for (int j = j0; j <= j1; ++j) {
            const float d = std::hypot(static_cast<float>(i) * step - x, static_cast<float>(j) * step - y);
            if (d >= radius) {
                continue;
            }
            const float t = d <= radius * kFullWithin ? 1.0f : (radius - d) / (radius * (1 - kFullWithin));
            const auto brush = static_cast<int>(std::lround(255.0f * t));
            for (std::size_t n = 0; n < count; ++n) {
                auto* map = paintMap(lot, n);
                if (map == nullptr || i < 0 || j < 0 || static_cast<std::uint32_t>(i) >= map->width || static_cast<std::uint32_t>(j) >= map->height) {
                    continue;
                }
                auto& value = map->elements[(static_cast<std::size_t>(j) * map->width + static_cast<std::size_t>(i)) * map->elementSize];
                const int now = paint && *paint == n ? std::max<int>(value, brush) : std::min<int>(value, 255 - brush);
                if (now != value) {
                    value = static_cast<std::uint8_t>(now);
                    changed = true;
                }
            }
        }
    }
    return changed;
}

} // namespace ts2::content
