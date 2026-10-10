#include "scene/view.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::scene {

int topLevel(const Scene& scene) {
    int top = 0;
    for (const auto& instance : scene.instances) {
        if (instance.part != Part::Roof) {
            top = std::max(top, instance.level);
        }
    }
    return top;
}

int groundLevel(const Scene& scene) {
    int ground = -1;
    for (const auto& wall : scene.walls) {
        if (wall.cuttable && (ground < 0 || wall.level < ground)) {
            ground = wall.level;
        }
    }
    return std::max(ground, 0);
}

bool cutDown(const WallInfo& wall, const View& view) {
    if (!wall.cuttable || wall.level != view.level || view.walls == WallView::Up) {
        return false;
    }
    if (view.walls == WallView::Down) {
        return true;
    }
    const float dx = wall.x1 - wall.x0, dy = wall.y1 - wall.y0, len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0) {
        return false;
    }
    // Looking toward the left side (dy, -dx), the room behind the wall is the left one.
    const float toward = (dy * view.facing[0] - dx * view.facing[1]) / len;
    if (std::abs(toward) < 0.25f) {
        return wall.roomLeft && wall.roomRight;  // seen edge-on: cut only between rooms
    }
    return toward > 0 ? wall.roomLeft : wall.roomRight;
}

std::vector<std::uint8_t> visibleInstances(const Scene& scene, const View& view) {
    std::vector<std::uint8_t> cut(scene.walls.size());
    for (std::size_t i = 0; i < scene.walls.size(); ++i) {
        cut[i] = cutDown(scene.walls[i], view);
    }
    auto isCut = [&](int wall) { return wall >= 0 && static_cast<std::size_t>(wall) < cut.size() && cut[static_cast<std::size_t>(wall)]; };
    const int top = topLevel(scene);
    std::vector<std::uint8_t> visible(scene.instances.size());
    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        const Instance& instance = scene.instances[i];
        bool show = instance.part == Part::Other || instance.level <= view.level;
        switch (instance.part) {
        case Part::Roof:
            show = show && view.roofs && view.walls == WallView::Up && view.level >= top;
            break;
        case Part::Wall:
        case Part::WallHub: {
            // The cut version shows when all its walls are cut, the full one otherwise.
            const bool allCut = !instance.walls.empty() && std::all_of(instance.walls.begin(), instance.walls.end(), isCut);
            show = show && instance.cut == allCut;
            break;
        }
        case Part::Object:
            show = show && std::none_of(instance.walls.begin(), instance.walls.end(), isCut);
            break;
        default:
            break;
        }
        visible[i] = show;
    }
    return visible;
}

} // namespace ts2::scene
