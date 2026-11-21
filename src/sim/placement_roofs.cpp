#include "sim/placement_roofs.hpp"

#include "scene/lot_parts.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace ts2::sim {

struct PlacementRoofs::Roof {
    std::int32_t level = 0;  // the level the roof is saved on
    std::uint32_t type = 0;
    std::optional<scene::RoofGrid> grid;
};

PlacementRoofs::~PlacementRoofs() = default;
PlacementRoofs::PlacementRoofs(PlacementRoofs&&) noexcept = default;
PlacementRoofs& PlacementRoofs::operator=(PlacementRoofs&&) noexcept = default;

PlacementRoofs::PlacementRoofs(const content::Lot& lot) {
    firstLevel_ = lot.roomGraph ? lot.roomGraph->firstLevel : 0;
    if (const auto h = lot.grids.find(content::kFloorHeightGridInstance); h != lot.grids.end() && h->second.elementSize == 4) {
        heights_ = std::make_shared<const array3d::Array3D>(h->second);
    }
    // The eaves stand on the roof's storey, as the scene builds them.
    const scene::Heights heights(lot);
    for (const auto& r : lot.roofs) {
        Roof roof{r.level0, r.type, std::nullopt};
        const float eave = heights.corner(content::gridIndex(lot, r.level0), r.x0, r.y0);
        roof.grid = scene::RoofGrid::of(r, eave);
        roofs_.push_back(std::move(roof));
    }
}

// The floor at the corner the game rounds the spot to (half a tile down, to the nearest, ties to even).
float PlacementRoofs::floorHeight(float x, float y, std::int32_t level) const {
    if (!heights_) {
        return 0;
    }
    const auto cx = static_cast<std::int32_t>(std::nearbyint(x - 0.5f)), cy = static_cast<std::int32_t>(std::nearbyint(y - 0.5f));
    const std::int32_t g = level - firstLevel_;
    return heights_->contains(g, cx, cy) ? heights_->f32(g, cx, cy, 0) : 0.0f;
}

bool PlacementRoofs::tooLow(float x, float y, std::int32_t level) const {
    constexpr float kClearance = 0.1f, kStorey = 3.0f, kType15Height = 0.2f;
    constexpr std::uint32_t kType15 = 15;
    if (level < 0) {
        return false;
    }
    const bool covered = std::ranges::any_of(roofs_, [&](const Roof& r) { return r.grid && r.grid->inGrid(x, y); });
    if (!covered) {
        return false;
    }
    const float floor = floorHeight(x, y, level);
    float highest = 0;
    for (const Roof& r : roofs_) {
        if (r.level < std::max(level - 1, 0) || r.level > level + 2) {
            continue;
        }
        float h = 0;
        if (r.type == kType15) {
            h = floor + kType15Height;
        } else if (r.grid) {
            h = r.grid->heightFrom(x, y, level);
        } else {
            continue;
        }
        highest = std::max(highest, h);
    }
    return floor + kClearance < highest && highest < floor + kStorey;
}

} // namespace ts2::sim
