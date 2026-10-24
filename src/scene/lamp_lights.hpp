#pragma once

#include "content/lighting.hpp"
#include "formats/scenegraph.hpp"
#include "scene/light_source.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ts2::scene::lamplights {

// How an object's lamp feeds the floor and wall light: its settings come from the light files by
// the light's name, are scaled by the lighting state for lamps inside or outside, then placed
// where the light hangs on the lot.

// A lamp's floor and wall light settings ("diffusionSource..."), with the game's defaults for
// those no line sets.
struct Settings {
    std::array<float, 2> min{-0.5f, -0.5f}, max{0.5f, 0.5f};  // the lit area around the light, in tiles
    float intensity = 0;
    std::array<float, 3> color{1, 1, 1};
    bool directional = false;
    float falloff = 1;
    float directionalFalloff = 0;
    float wallScale = 1, floorScale = 1;
    float interFloorFalloff = 0.5f;
};

Settings settingsFor(const content::Lighting::LightAttributes* attributes);

// The settings for one lamp of one object now: intensity times the state's diffusion strength for
// lamps there and the object's own light strength (0 when its lights are off), falloff times the
// state's diffusion falloff.
Settings scaled(Settings settings, const content::Lighting::Modifier& modifier, float strength);

// Where a lamp's light node is on the lot: position in tiles (z in level heights of 3) and
// rotation as a quaternion x, y, z, w.
struct Placement {
    std::array<float, 3> position{0, 0, 0};
    std::array<float, 4> rotation{0, 0, 0, 1};
};

// v turned by the rotation q (x, y, z, w), in the game's order of operations.
std::array<float, 3> turned(const std::array<float, 4>& q, const std::array<float, 3>& v);

// An object's turn for its saved facing (0..7, 45 degrees a step), as the game builds it.
std::array<float, 4> facingRotation(int direction);

// The lights of a model standing at `object` (its position and turn): each light node's name and
// its place in the world, through the nodes above it.
struct LightNode {
    std::string name;
    Placement place;
};
std::vector<LightNode> lightNodes(const scenegraph::ResourceNode& model, const Placement& object);

// What the record needs from the lot: the floor height of a level at a point, whether a tile
// is on the lot, and the room at a point.
struct LotQueries {
    int lowestLevel = 0, highestLevel = 0;
    std::function<float(int level, float x, float y)> floorHeight;
    std::function<bool(int x, int y, int level)> tileOnLot;
    std::function<std::int16_t(float x, float y, int level)> roomAt;
};

// The level a point is on: the highest whose floor is below it (by more than 0.01).
int levelAt(const LotQueries& lot, float x, float y, float z);

// The light source a lamp gives the floor and wall light, as the game makes it. None when it
// has no colour, stands off the lot, or its placement or area is broken (not finite).
[[nodiscard]] std::optional<LightSource> sourceFor(const Settings& settings, const Placement& placement, const LotQueries& lot);

} // namespace ts2::scene::lamplights
