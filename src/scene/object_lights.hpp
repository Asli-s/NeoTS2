#pragma once

#include "scene/lot_lighting.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace ts2::scene::objectlights {

// A room's lights for one group as the game's object shaders take them: its ambient, and its
// directional lights pointing toward where the light comes from.
ObjectLighting fromRoom(const roomlight::Lights& lights);

// A lamp's light on things (its "practical" light): a point light at its light node, or a spot
// light along the node's +z. It lights things in the lamp's own room.
struct LampLight {
    std::int16_t room = 0;
    std::array<float, 3> position{0, 0, 0};
    std::array<float, 3> axis{0, 0, 1};
    std::array<float, 3> color{0, 0, 0};
    float falloff = 1;
    bool spot = false;
    float coneAngle = 0, conePenumbra = 0;  // radians
};

// The room a point is in, 0 outdoors. `level` counts from the ground storey.
using RoomAt = std::function<std::int16_t(int level, float x, float y)>;

// A point light a lamp, window or door gives the room it lights (0 outdoors), as things there take
// it, with what the game scores it by: its strength (a quarter of its colour's length) and, for a
// window's or door's spill, the plane through the spill point facing into the room (nothing behind
// it scores).
struct PointLight {
    std::int16_t room = 0;
    ObjectLighting::Light light;
    float strength = 0;
    bool plane = false;
    std::array<float, 3> planePoint{0, 0, 0};
    std::array<float, 2> planeNormal{0, 0};
};

// The point lights of the lamps and of the windows and doors (`portals`), their reach ending where
// their light falls to `rangeCutoff`. Lights too weak to reach anything are left out.
std::vector<PointLight> pointLights(const std::vector<lotlighting::PortalLight>& portals, const std::vector<LampLight>& lamps, float rangeCutoff);

// Gives every lot object standing in a room the lights the room gives its group, in place of the
// daylight. Doors' and windows' parts go by the room their node stands in (Instance::partLightSets).
// Each part also gets the point lights of the windows and doors lighting its room (`portals`) and of
// the lamps in it (`lamps`), their reach ending where their light falls to `rangeCutoff`, up to
// eight lights in all by the game's scores. Outdoors is room 0: a part there picks from the daylight
// (`outdoor`) and the lamps and windows lighting outdoors, and keeps the plain daylight when none
// reaches it. `groundLevel` is the grid level of the ground storey (Instance::level counts grid
// levels).
void light(Scene& scene, const std::map<std::int16_t, lotlighting::Room>& rooms, const ObjectLighting& outdoor,
           const std::vector<lotlighting::PortalLight>& portals, const std::vector<LampLight>& lamps, float rangeCutoff, const RoomAt& roomAt,
           int groundLevel);

// The lights of a door's or window's parts standing at `inLot` (in lot coordinates, `storey` from the
// ground storey) as light() gives them: none for a part that keeps the daylight, and empty when every
// part does. `points` comes from pointLights(), made once for all the things lit again.
std::vector<std::optional<ObjectLighting>> portalLights(const Scene& scene, const Instance& inLot, int storey,
                                                        const std::map<std::int16_t, lotlighting::Room>& rooms, const ObjectLighting& outdoor,
                                                        const std::vector<PointLight>& points, const RoomAt& roomAt);

// The lights of a thing's parts (not a door's or window's) standing at `inLot` as light() gives them:
// empty when it keeps the daylight.
std::vector<ObjectLighting> propLights(const Scene& scene, const Instance& inLot, int storey, const std::map<std::int16_t, lotlighting::Room>& rooms,
                                       const ObjectLighting& outdoor, const std::vector<PointLight>& points, const RoomAt& roomAt);

// The storey whose room lights a thing at `inLot` (lot coordinates) on `storey` (from the ground
// storey): the highest one whose floor under it is at or below it. The pieces of a stacked chimney
// keep their stack's storey but take the lights of the storey each reaches. `floorHeight` is
// Scene::floorHeight.
int lightStorey(const std::function<float(int, float, float)>& floorHeight, const Transform& inLot, int storey);

// The same for one thing (not a door or window) or Sim wherever it stands (see Scene::thingLights),
// for objects created or moved after the lot was lit and for Sims as they move. A Sim in a room gets
// its "sim:" lights and the same windows and lamps. A Sim outdoors also picks from the daylight's
// Sim-only lights. `roomAt` is kept, so it must hold what it uses.
ThingLights thingLights(std::map<std::int16_t, lotlighting::Room> rooms, ObjectLighting outdoor, const std::vector<lotlighting::PortalLight>& portals,
                        const std::vector<LampLight>& lamps, float rangeCutoff, RoomAt roomAt);

} // namespace ts2::scene::objectlights
