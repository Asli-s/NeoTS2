#pragma once

#include "scene/lot_lighting.hpp"
#include "scene/scene.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace ts2::scene::objectlights {

// A room's lights for one group as the game's object shaders take them: its ambient, and its
// directional lights pointing toward where the light comes from.
ObjectLighting fromRoom(const roomlight::Lights& lights);

// The room a point is in, 0 outdoors. `level` counts from the ground storey.
using RoomAt = std::function<std::int16_t(int level, float x, float y)>;

// Gives every lot object standing in a room the lights the room gives its group, in place of the
// daylight. Objects outdoors keep the daylight. Doors' and windows' parts go by the room their
// node stands in (Instance::partLightSets). Each part also gets the point lights of the windows and
// doors lighting its room (`portals`, their reach ending where their light falls to `rangeCutoff`),
// up to eight lights in all by the game's scores. Sims are not lit this way yet.
// `groundLevel` is the grid level of the ground storey (Instance::level counts grid levels).
void light(Scene& scene, const std::map<std::int16_t, lotlighting::Room>& rooms, const std::vector<lotlighting::PortalLight>& portals,
           float rangeCutoff, const RoomAt& roomAt, int groundLevel);

// The lights of a door's or window's parts standing at `inLot` (in lot coordinates, `storey` from the
// ground storey) as light() gives them: none for a part outdoors, which keeps the daylight, and
// empty when no part stands in a lit room.
std::vector<std::optional<ObjectLighting>> portalLights(const Scene& scene, const Instance& inLot, int storey,
                                                        const std::map<std::int16_t, lotlighting::Room>& rooms,
                                                        const std::vector<lotlighting::PortalLight>& portals, float rangeCutoff, const RoomAt& roomAt);

// The storey whose room lights a thing at `inLot` (lot coordinates) on `storey` (from the ground
// storey): the highest one whose floor under it is at or below it. The pieces of a stacked chimney
// keep their stack's storey but take the lights of the storey each reaches. `floorHeight` is
// Scene::floorHeight.
int lightStorey(const std::function<float(int, float, float)>& floorHeight, const Transform& inLot, int storey);

// The same for one thing (not a door or window) wherever it stands (see Scene::thingLights), for
// objects created or moved after the lot was lit. `roomAt` is kept, so it must hold what it uses.
ThingLights thingLights(std::map<std::int16_t, lotlighting::Room> rooms, const std::vector<lotlighting::PortalLight>& portals, float rangeCutoff,
                        RoomAt roomAt);

} // namespace ts2::scene::objectlights
