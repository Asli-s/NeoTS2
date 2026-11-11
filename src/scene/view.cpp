#include "scene/view.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::scene {

namespace {

// A wall or hub made only of attic walls.
bool attic(const Scene& scene, const Instance& instance) {
    if ((instance.part != Part::Wall && instance.part != Part::WallHub) || instance.walls.empty()) {
        return false;
    }
    return std::all_of(instance.walls.begin(), instance.walls.end(), [&](int wall) {
        return wall >= 0 && static_cast<std::size_t>(wall) < scene.walls.size() && scene.walls[static_cast<std::size_t>(wall)].attic;
    });
}

} // namespace

int topLevel(const Scene& scene) {
    int top = 0;
    for (const auto& instance : scene.instances) {
        if (instance.part != Part::Roof && !attic(scene, instance)) {
            top = std::max(top, instance.level);
        }
    }
    return top;
}

int everyLevel(const Scene& scene) {
    int top = 0;
    for (const auto& instance : scene.instances) {
        top = std::max(top, instance.level);
    }
    return top + 1;
}

int groundLevel(const Scene& scene) {
    return scene.groundLevel;
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
    std::vector<std::uint8_t> visible(scene.instances.size());
    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        const Instance& instance = scene.instances[i];
        if (instance.hidden) {
            continue;
        }
        if (instance.underside != Underside::None) {
            // The storey above the one looked at shows them only with Toggle Ceiling on and the
            // camera below its floor. The edges above walls show only with the toggle on, on the
            // storeys below too.
            const bool above = instance.level == view.level + 1 && view.ceilings && view.eyeHeight < instance.floorHeight;
            visible[i] = instance.underside == Underside::Floor ? instance.level <= view.level || above
                                                                : view.ceilings && (instance.level <= view.level || above);
            continue;
        }
        bool show = instance.part == Part::Other || instance.level <= view.level;
        switch (instance.part) {
        case Part::Roof:
            // With the walls up, a roof shows once the storey its eaves rest on is looked at, as
            // the game turns each saved level on. Dormer roofs a storey up stay off until then, and
            // the holes the main roof keeps under them show the rooms there.
            show = show && view.roofs && view.walls == WallView::Up && instance.level < view.level;
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
