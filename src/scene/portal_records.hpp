#pragma once

#include "scene/lamp_lights.hpp"
#include "scene/window_light.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace ts2::scene::portalrecords {

// How the game turns windows, doors and arches into the portals that light the rooms behind them:
// each light node of the object makes one portal, filed under the room behind the light and
// shining into the room it faces, on the wall tile edge it sits at.

// A wall tile edge, walked from (x0, y0) to (x1, y1) on a level.
struct Edge {
    float x0 = 0, y0 = 0;
    int level = 0;
    float x1 = 0, y1 = 0;
};

// One light node of a portal object: where it is, its light (the light files' "intensity",
// "color" and "falloffRate" over the light resource's), and its floor and wall light settings.
struct PortalLight {
    lamplights::Placement place;
    bool hasLight = false;  // false: no light resource, the portal gives no room light
    float intensity = 0;
    std::array<float, 3> color{1, 1, 1};
    float falloffRate = 1;
    lamplights::Settings settings;  // unscaled
};

struct PortalObject {
    Edge edge;  // a wall edge the object stands in (any along its wall line)
    bool doorOrArch = false;
    std::vector<PortalLight> lights;
};

struct LotQueries {
    lamplights::LotQueries lamps;  // levels, floor heights, the lot's tiles and rooms
    std::function<bool(float x, float y)> inLot;
    std::function<bool(const Edge&)> wallAt;              // a wall stands on this edge
    std::function<std::int16_t(const Edge&)> roomInFront;  // the room on the edge's right-hand side
    std::function<bool(std::int16_t room)> roomLit;        // the room is lit to 100
};

struct Built {
    std::int16_t room = 0;  // the room it is filed under (behind the light)
    windowlight::Portal portal;
    std::array<float, 3> spillPoint{0, 0, 0};  // where its spill plane starts
};

// The portals of one object; `spillPlaneOffset` is the light files' setting of that name.
std::vector<Built> build(const PortalObject& object, const LotQueries& lot, float spillPlaneOffset);

} // namespace ts2::scene::portalrecords
