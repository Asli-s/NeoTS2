#include "scene/lot_shadow.hpp"

#include "content/lot.hpp"
#include "scene/lot_parts.hpp"
#include "scene/wall_runs.hpp"

namespace ts2::scene::lotshadow {

Inputs fromLot(const content::Lot& lot, const std::function<bool(std::uint32_t covering)>& isFence) {
    Inputs in;
    const auto floors = lot.grids.find(content::kFloorGridInstance);
    if (floors == lot.grids.end() || floors->second.elementSize != 8) {
        return in;
    }
    const auto& g = floors->second;
    // Levels count from the ground storey, as walls save them. Levels below the ground cast no shadow.
    const int ground = content::groundGridLevel(lot);
    in.width = static_cast<int>(g.width);
    in.height = static_cast<int>(g.height);
    in.levels = static_cast<int>(g.levels) - ground;
    in.corner = [heights = Heights(lot), ground](int x, int y, int level) {
        return heights.corner(level + ground, static_cast<float>(x), static_cast<float>(y));
    };
    // The floor grid keeps its quarters west, north, east, south, as the shadow numbers them.
    in.floorAt = [g, ground](int x, int y, int level, int quarter) {
        return g.contains(level + ground, x, y) && g.i16(level + ground, x, y, static_cast<std::size_t>(quarter)) != 0;
    };
    for (const auto& w : wallruns::wallsOf(lot, nullptr)) {
        Wall sw{w.level, w.x0, w.y0, w.x1, w.y1};
        sw.fence = isFence && isFence(w.covering);
        sw.fenceHeight = 1.1824f;
        in.walls.push_back(sw);
    }
    return in;
}

} // namespace ts2::scene::lotshadow
