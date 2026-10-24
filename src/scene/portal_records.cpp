#include "scene/portal_records.hpp"

#include "diag/log.hpp"

#include <bit>
#include <cmath>
#include <cstdint>

// The game works these out on the x87 in double precision and rounds to float only where it
// stores a value, so the code below does the same: double in between, float where the game
// keeps a float.

namespace ts2::scene::portalrecords {

namespace {

constexpr float kLevelHeight = 3.0f;
constexpr int kMaxLevelSteps = 64;
constexpr double kFarAlong = 4.0e6;  // further along its wall than this a light is not a portal's

float r24(double v) {
    return static_cast<float>(v);
}

Edge reversed(const Edge& e) {
    return {e.x1, e.y1, e.level, e.x0, e.y0};
}

// The edge's right-hand normal, (dy, -dx) made unit.
std::array<float, 2> normalOf(const Edge& e) {
    const float a = r24(static_cast<double>(e.y1) - e.y0);
    const float b = -r24(static_cast<double>(e.x1) - e.x0);
    const double length = std::sqrt(static_cast<double>(b) * b + static_cast<double>(a) * a);
    if (!(length > 0)) {
        return {a, b};
    }
    const double inverse = 1.0 / length;
    return {r24(a * inverse), r24(inverse * b)};
}

// The floor height of a level at a point. Levels above and below the lot's go on in steps of a
// level's height, and are 0 off the lot.
float elevation(const LotQueries& lot, float x, float y, int level) {
    const auto& q = lot.lamps;
    if (!q.floorHeight) {
        return 0;
    }
    if (level > q.highestLevel || level < q.lowestLevel) {
        if (!lot.inLot || !lot.inLot(x, y)) {
            return 0;
        }
        const bool above = level > q.highestLevel;
        float h = q.floorHeight(above ? q.highestLevel : q.lowestLevel, x, y);
        const int steps = above ? level - q.highestLevel : q.lowestLevel - level;
        for (int i = 0; i < steps && i < kMaxLevelSteps; ++i) {
            h = above ? h + kLevelHeight : h - kLevelHeight;
        }
        return h;
    }
    return q.floorHeight(level, x, y);
}

// The whole number of edge lengths t lies along: the game rounds t - 0.5 to an integer the
// float way, then corrects it upward when t is exactly the next integer.
int stepsAlong(double t) {
    const float shifted = r24((t - 0.5) + 12582912.0);
    const int i = static_cast<int>(std::bit_cast<std::int32_t>(shifted) - 0x4B400000);
    return t == static_cast<float>(i + 1) ? i + 1 : i;
}

} // namespace

std::vector<Built> build(const PortalObject& object, const LotQueries& lot, float spillPlaneOffset) {
    std::vector<Built> out;
    const Edge& a = object.edge;
    const Edge b = reversed(a);
    if (!lot.roomInFront || (lot.roomLit && (!lot.roomLit(lot.roomInFront(a)) || !lot.roomLit(lot.roomInFront(b))))) {
        return out;
    }
    const auto nA = normalOf(a), nB = normalOf(b);
    float midX = r24(r24(static_cast<double>(a.x1) + a.x0) * 0.5f);
    float midY = r24(r24(static_cast<double>(a.y1) + a.y0) * 0.5f);
    for (const auto& light : object.lights) {
        const auto& pos = light.place.position;
        const auto facing = lamplights::turned(light.place.rotation, {-0.0f, -0.0f, -1.0f});
        if (!lot.inLot || !lot.inLot(pos[0], pos[1]) || elevation(lot, pos[0], pos[1], lot.lamps.lowestLevel) > pos[2]) {
            continue;
        }
        // Move the edge along its own line onto the tile the light is at.
        const float dx = r24(static_cast<double>(a.x1) - a.x0), dy = r24(static_cast<double>(a.y1) - a.y0);
        const float px = r24(static_cast<double>(pos[0]) - a.x0), py = r24(static_cast<double>(pos[1]) - a.y0);
        const float lengthSquared = r24(static_cast<double>(dy) * dy + static_cast<double>(dx) * dx);
        if (lengthSquared < 1e-6) {
            continue;
        }
        const double t = (static_cast<double>(dy) * py + static_cast<double>(dx) * px) / lengthSquared;
        if (!(std::fabs(t) < kFarAlong)) {
            continue;
        }
        float x0 = a.x0, y0 = a.y0;
        if (!(0 <= t && t < 1)) {
            const float k = static_cast<float>(stepsAlong(t));
            midX = r24(midX + static_cast<double>(r24(static_cast<double>(dx) * k)));
            midY = r24(midY + static_cast<double>(r24(static_cast<double>(dy) * k)));
            x0 = r24(static_cast<double>(dx) * k + a.x0);
            y0 = r24(static_cast<double>(dy) * k + a.y0);
        }
        // The level the light is on: the highest whose floor is not above it.
        if (pos[2] < 0) {
            continue;
        }
        int upper = a.level + 1, steps = 0;
        float top = elevation(lot, x0, y0, upper);
        bool lost = false;
        while (!(top >= pos[2])) {
            if (top <= 0 || ++steps > kMaxLevelSteps) {
                if (steps > kMaxLevelSteps) {
                    diag::warnOnce("portal light: a light more than 64 levels above its portal is skipped");
                }
                lost = true;
                break;
            }
            top = elevation(lot, x0, y0, ++upper);
        }
        if (lost || top <= 0) {
            continue;
        }
        int level = upper - 1;
        float floor = elevation(lot, x0, y0, level);
        for (steps = 0; floor > pos[2] && steps < kMaxLevelSteps; ++steps) {
            floor = elevation(lot, x0, y0, --level);
        }
        if (floor > pos[2]) {
            diag::warnOnce("portal light: a light more than 64 levels below its portal is skipped");
            continue;
        }
        const Edge here{x0, y0, level, r24(static_cast<double>(dx) + x0), r24(static_cast<double>(dy) + y0)};
        const Edge back = reversed(here);
        if (!lot.wallAt || !lot.wallAt(here)) {
            continue;
        }
        const std::int16_t roomA = lot.roomInFront(here), roomB = lot.roomInFront(back);
        // The light shines into the room on the side it faces. It is filed under the other.
        const double dot = static_cast<double>(nA[1]) * facing[1] + static_cast<double>(nA[0]) * facing[0];
        Built built;
        built.lightPosition = pos;
        built.lightAxis = lamplights::turned(light.place.rotation, {0.0f, 0.0f, 1.0f});
        built.spot = light.spot;
        built.coneAngle = light.coneAngle;
        built.conePenumbra = light.conePenumbra;
        const Edge* corners = nullptr;
        std::array<float, 2> normal{};
        if (dot > 0) {
            corners = &back;
            built.room = roomB;
            built.portal.otherRoom = roomA;
            normal = nA;
        } else if (dot < 0) {
            corners = &here;
            built.room = roomA;
            built.portal.otherRoom = roomB;
            normal = nB;
        } else {
            continue;
        }
        auto& p = built.portal;
        p.x0 = corners->x0;
        p.y0 = corners->y0;
        p.x1 = corners->x1;
        p.y1 = corners->y1;
        p.height = static_cast<float>(level);
        p.nx = normal[0];
        p.ny = normal[1];
        p.doorOrArch = object.doorOrArch;
        p.size = light.hasLight ? light.intensity : 0.0f;
        p.tint = light.hasLight ? light.color : std::array<float, 3>{1, 1, 1};
        p.reach = light.hasLight ? light.falloffRate : 1.0f;
        p.suppression = 1;
        // Its own floor and wall light, made from the light node with the settings unscaled.
        p.source = lamplights::sourceFor(light.settings, light.place, lot.lamps).value_or(LightSource{});
        // The spill plane starts a little into the room it lights, halfway up the level.
        const float middle = r24((static_cast<double>(floor) + top) * 0.5);
        built.spillPoint = {r24(static_cast<double>(r24(static_cast<double>(normal[0]) * spillPlaneOffset)) + midX),
                            r24(static_cast<double>(r24(static_cast<double>(normal[1]) * spillPlaneOffset)) + midY),
                            r24(static_cast<double>(r24(0.0 * spillPlaneOffset)) + middle)};
        out.push_back(built);
    }
    return out;
}

} // namespace ts2::scene::portalrecords
