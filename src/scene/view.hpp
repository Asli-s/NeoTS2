#pragma once

#include "scene/scene.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace ts2::scene {

// The game's wall views: all walls up, cutaway, and all walls on the storey looked at down.
// Cutaway cuts down the walls on that storey that stand between the camera and a room behind them.
enum class WallView { Up, Cutaway, Down };

struct View {
    WallView walls = WallView::Up;
    int level = 0;                      // the storey looked at, everything above it is hidden
    std::array<float, 2> facing{0, 1};  // the way the camera looks, along the ground
    bool roofs = true;                  // roofs shown (below the storey looked at, all with walls up on the top one)
};

// The highest storey anything but a roof or an attic wall is on.
int topLevel(const Scene& scene);
// The lowest storey the view goes down to: the lot's ground, as in the game. Under a house on a
// foundation that shows the ground with the foundation's walls and what stands beneath a deck.
// The storey below the ground (pools) is never looked at on its own.
int groundLevel(const Scene& scene);

// Whether a view cuts a wall down.
bool cutDown(const WallInfo& wall, const View& view);

// Which of the scene's instances a view shows, one flag per instance. Roofs show only with the
// walls up: those of the storeys below the one looked at, and all of them on the top storey. Attic
// walls show with all the roofs as well as on their own storey. Doors and windows go with the walls
// they sit in.
std::vector<std::uint8_t> visibleInstances(const Scene& scene, const View& view);

} // namespace ts2::scene
