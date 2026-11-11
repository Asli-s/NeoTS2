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
    bool roofs = true;                  // roofs shown, those whose eaves rest at or below the storey looked at
    bool ceilings = false;              // the puck's Toggle Ceiling
    float eyeHeight = 1e9f;             // the camera's height in the lot
};

// The highest storey anything but a roof or an attic wall is on.
int topLevel(const Scene& scene);
// A storey above everything the lot has, roofs and attics too. Looking at it shows every
// storey, as an imposter or a house next door is drawn.
int everyLevel(const Scene& scene);
// The lowest storey the view goes down to: the lot's ground, as in the game. Under a house on a
// foundation that shows the ground with the foundation's walls and what stands beneath a deck.
// The storey below the ground (pools) is never looked at on its own.
int groundLevel(const Scene& scene);

// Whether a view cuts a wall down.
bool cutDown(const WallInfo& wall, const View& view);

// Which of the scene's instances a view shows, one flag per instance. Roofs show only with the
// walls up: those whose eaves rest on the storey looked at or below it. Attic walls go with their
// storey, the one their roof's eaves rest on. Doors and windows go with the walls
// they sit in. Floors' undersides show up to the storey looked at, and one storey above it while
// the ceilings are turned on and the camera is below that storey's floor.
std::vector<std::uint8_t> visibleInstances(const Scene& scene, const View& view);

} // namespace ts2::scene
