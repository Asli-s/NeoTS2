#pragma once

#include "scene/scene.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace ts2::scene {

// The game's wall views: all walls up; cutaway, which cuts down the walls on the storey looked at
// that stand between the camera and a room behind them; and all walls on that storey down.
enum class WallView { Up, Cutaway, Down };

struct View {
    WallView walls = WallView::Up;
    int level = 0;                      // the storey looked at; everything above it is hidden
    std::array<float, 2> facing{0, 1};  // the way the camera looks, along the ground
    bool roofs = true;                  // roofs shown (with walls up, on the top storey)
};

// The highest storey anything but a roof is on.
int topLevel(const Scene& scene);
// The lowest storey worth looking at on its own: the lowest with a wall that can be cut down (a
// house's ground floor stands a storey up on its foundation); 0 without such walls.
int groundLevel(const Scene& scene);

// Whether a view cuts a wall down.
bool cutDown(const WallInfo& wall, const View& view);

// Which of the scene's instances a view shows, one flag per instance. Roofs show only with walls
// up on the top storey; doors and windows go with the walls they sit in.
std::vector<std::uint8_t> visibleInstances(const Scene& scene, const View& view);

} // namespace ts2::scene
