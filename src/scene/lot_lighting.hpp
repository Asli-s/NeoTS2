#pragma once

#include "content/lighting.hpp"
#include "scene/floor_light.hpp"
#include "scene/light_map_texels.hpp"
#include "scene/light_source.hpp"
#include "scene/portal_records.hpp"
#include "scene/room_light.hpp"
#include "scene/room_samples.hpp"
#include "scene/wall_light.hpp"
#include "scene/wall_runs.hpp"
#include "scene/window_light.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ts2::content {
struct Lot;
}

namespace ts2::scene::lotlighting {

// A whole lot's indoor light, worked out in the order the game's lighting manager does it: the
// floor and wall light from the lamps, the windows' and doors' light from that, the floor and wall
// light again with the windows added, the corners, then each room's samples and the lights its
// objects, Sims and doors get.

// Everything the light files say about it, for one lighting state.
struct Settings {
    // The floor and wall light ("diffusion").
    float insideFloorAmbient = 0, outsideFloorAmbient = 0, insideWallAmbient = 0, outsideWallAmbient = 0;
    std::array<float, 3> insideColor{1, 1, 1}, outsideColor{1, 1, 1};
    float floorThreshold = 0.4f, wallThreshold = 0.4f;
    int steps = 50;
    float brightWallFloor = 1, darkWallFloor = 0.5f;
    bool dimOutside = true;  // samples of floors and walls outdoors leave the outside ambient out (always, in the game)
    // Windows and doors.
    windowlight::Daylight day;
    windowlight::Suppression suppression;
    // Rooms.
    roomsamples::Settings samples;
    std::array<roomlight::Settings, 3> groups;  // object:, sim:, wall:
    content::Lighting::UnlitRoom unlit;
    // The sunlight added to outdoor floors and walls in the light maps.
    lightmaptexels::Outdoor outdoor;
    // The incidence maps made of the light maps (incidenceMapScale, incidenceMapHeightClamp).
    float incidenceScale = 20, incidenceHeightClamp = 1;
    // How much of the sun a Sim's projected shadow takes away (see simshadows::shade).
    float projectiveShadowStrength = 1;
};
enum Group { kObjects, kSims, kPortals };

// The settings of `state` (with the light files' global values where the state leaves them out).
// Nullopt when there is no such state.
[[nodiscard]] std::optional<Settings> settingsFor(const content::Lighting& lighting, const std::string& state);

struct Inputs {
    // The whole lot. Levels count from its ground storey (0), the storey below it (a pool's) is -1.
    const content::Lot* lot = nullptr;
    std::vector<LightSource> lamps;             // lamps that are on, as the game records them
    std::vector<portalrecords::Built> portals;  // the lot's windows, doors and arches
    wallruns::BlocksLight blocksLight;          // which wall coverings stop light
    // Rooms and coverings for the room samples (their floor and wall light are filled in here).
    std::function<std::int16_t(int level, int x, int y, int quarter)> quarterRoom;
    std::function<std::int16_t(int level, float x, float y)> roomAt;
    std::function<std::optional<roomsamples::Color>(bool floor, const std::string& name)> covering;
    // The game numbers rooms 1 .. roomCount - 1 with no gaps. Rooms that lost all their tiles still
    // get the unlit room light.
    int roomCount = 0;
};

struct Room {
    std::array<std::array<float, 3>, 6> samples{};  // with the unlit room light added
    std::array<std::optional<roomlight::Lights>, 3> groups;  // by Group
    std::array<float, 3> average{0, 0, 0};          // the plain average everything in it gets too
    float unlitShare = 0;                           // how much of the unlit room light it got (0..1)
};

struct PortalLight {
    std::int16_t into = 0;                          // the room it shines into
    windowlight::Portal portal;                     // after suppression
    std::array<float, 3> spillPoint{0, 0, 0};
    std::array<float, 3> lightPosition{0, 0, 0};    // its light node
    std::array<float, 3> lightAxis{0, 0, 1};        // the node's +z
    bool spot = false;                              // the beam is a spot light with this cone (radians)
    float coneAngle = 0, conePenumbra = 0;
    std::optional<windowlight::PortalLights> lights;
};

struct Result {
    std::vector<floorlight::Grid> floors;   // per level, from the lowest
    int lowest = 0;                         // the level of floors[0]: 0 the ground storey, -1 a pool's below it
    walllight::Walls walls;                 // with texels filled
    wallruns::Layout layout;
    std::map<std::int16_t, Room> rooms;
    std::vector<PortalLight> portals;
    bool complete = true;                   // false when a pass stopped at its safety limit
};

Result compute(const Inputs& in, const Settings& settings);

} // namespace ts2::scene::lotlighting
