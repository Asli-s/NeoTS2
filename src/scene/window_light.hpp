#pragma once

#include "scene/light_source.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace ts2::scene::windowlight {

// How the game lights rooms through their windows, doors and arches ("portals"): each one
// gives the room behind it a sun beam and a soft spill of sky light, and adds two light
// sources to the floor and wall light.

// A "modifiers" line of the lighting state: how strongly, and how far, portal light reaches.
struct Modifiers {
    float practical = 0;          // the room lights' strength
    float practicalFalloff = 1;
    float diffusion = 0;          // the floor and wall light's strength
    float diffusionFalloff = 1;
};

// One of a lighting state's outdoor lights, for the sky's light from a direction.
struct OutdoorLight {
    std::array<float, 3> color{0, 0, 0};
    float strength = 0;
    std::array<float, 3> direction{0, 0, 1};  // toward the light, normalized
};

// What a lighting state says about portals.
struct Daylight {
    std::array<float, 3> sunColor{0, 0, 0};
    float sunStrength = 0;
    std::array<float, 3> sunDirection{0, 0, 1};  // toward the sun, normalized
    std::array<float, 3> sky{0, 0, 0};
    // Whether the state has a "skylight" line. Without one, windows take the sky's light from
    // outside them in place of the sun and the skylight.
    bool skylight = true;
    // How much of the sun and skylight windows keep, the rest coming from the sky's light outside
    // them (portalSaturation, 1 in every shipped state).
    float saturation = 1;
    // The sky's light from a direction: these lights (the sun, sky and fill1 lines) by how much
    // the direction faces each, the rest of the way the skylight, times outdoorStrength.
    std::vector<OutdoorLight> outdoorLights;
    float outdoorStrength = 1;
    Modifiers insidePortals, outsidePortals, insidePortalSpills, outsidePortalSpills;
    float doorOrArchDirect = 0;   // doorOrArchDirectMultiplier
    bool suppressionDimsRoomLight = true;  // portalSuppressionAffectsRoomLighting
};

// Windows close together on the same wall share their light: each takes away part of the
// others' (portalSuppressionRadius, portalSuppressionAmount, portalCornerSuppressionAmount).
struct Suppression {
    float radius = 4;
    float facing = 0.2f;          // for windows facing the same way
    float corner = 0.2f;          // for windows at right angles
};

struct Portal {
    std::int16_t otherRoom = 0;   // the room it lets light into
    float nx = 0, ny = 0;         // horizontal normal, into the other room
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // its ends, in tiles
    float height = 0;             // portals only share light at the same height
    bool doorOrArch = false;
    float size = 1;               // wider portals give more light
    std::array<float, 3> tint{1, 1, 1};
    float reach = 1;              // scales the room lights' falloff
    LightSource source;           // the portal's own floor and wall light, in the world, before colour
    float suppression = 1;        // what is left after its neighbours took their share
};

// What a portal gives the room behind it.
struct Light {
    std::array<float, 3> color{0, 0, 0};  // already times its strength
    float falloff = 0;
};
struct PortalLights {
    Light beam;                       // straight sunlight (black when the portal faces away)
    std::optional<Light> spill;       // soft light spreading from the portal's plane
    std::array<float, 3> roomColor{0, 0, 0};  // what the room samples see through it
    std::vector<LightSource> sources; // for the floor and wall light
};

// Sets each portal's suppression from the others in the same list (one room's portals).
void suppress(std::vector<Portal>& portals, const Suppression& s);

// The sky's light from `direction` (see Daylight::outdoorLights).
std::array<float, 3> skyLight(const Daylight& day, const std::array<float, 3>& direction);

// The lights of a portal of a room (`fromOutside`: the room is the outdoors, room 0). `lightMap`
// is the wall light at the portal before portal light was added.
[[nodiscard]] PortalLights lightsFor(const Portal& p, bool fromOutside, const Daylight& day, const std::array<float, 3>& lightMap);

} // namespace ts2::scene::windowlight
