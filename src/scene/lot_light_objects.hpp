#pragma once

#include "content/lighting.hpp"
#include "scene/light_source.hpp"
#include "scene/object_lights.hpp"
#include "scene/portal_records.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

namespace ts2::content {
struct Lot;
}

namespace ts2::scene {
class Assets;
}

namespace ts2::scene::lotlightobjects {

// The objects of a lot that light it: its lamps (anything with lights that is not a door or a
// window) as floor and wall light sources, and its windows, doors and arches as portals.
struct Queries {
    // The room at a point on a level, 0 outdoors.
    std::function<std::int16_t(int level, float x, float y)> roomAt;
    // How strongly an object's lights shine now (the Change Light primitive, 1 when never set).
    std::function<float(std::uint32_t object)> strength;
    // Whether a room's light is at 100 percent (the game skips lamps in, and portals into, rooms
    // dimmed below that).
    std::function<bool(std::int16_t room)> roomLit;
};

struct Found {
    std::vector<LightSource> lamps;
    std::vector<portalrecords::Built> portals;
    std::vector<objectlights::LampLight> lampLights;  // their light on things
};

// `lotPackage` is the lot's file (its objects' scene records). `state` is the lighting state now.
Found gather(Assets& assets, const content::Lot& lot, const std::filesystem::path& lotPackage, const content::Lighting::State& state,
             const Queries& queries);

} // namespace ts2::scene::lotlightobjects
