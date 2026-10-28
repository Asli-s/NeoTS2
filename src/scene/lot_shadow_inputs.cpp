#include "scene/lot_shadow.hpp"

#include "content/lot.hpp"
#include "scene/lot_parts.hpp"
#include "scene/scene.hpp"
#include "scene/wall_runs.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace ts2::scene::lotshadow {

Inputs fromLot(const content::Lot& lot, const std::function<std::optional<float>(std::uint32_t covering)>& fenceHeight) {
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
        if (const auto height = fenceHeight ? fenceHeight(w.covering) : std::nullopt) {
            sw.fence = true;
            sw.fenceHeight = *height;
        }
        in.walls.push_back(sw);
    }
    return in;
}

float fenceTop(const Scene& scene, const std::string& post, const std::string& rail) {
    auto same = [](const std::string& a, const std::string& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               });
    };
    float top = 0;
    for (const auto& mesh : scene.meshes) {
        if (!same(mesh.name, post.empty() ? rail : post)) continue;
        for (const auto& p : mesh.primitives) {
            for (std::size_t i = 2; i < p.positions.size(); i += 3) {
                // Past 16 is a broken model, not a fence.
                if (std::isfinite(p.positions[i]) && p.positions[i] <= 16) top = std::max(top, p.positions[i]);
            }
        }
    }
    return top;
}

} // namespace ts2::scene::lotshadow
