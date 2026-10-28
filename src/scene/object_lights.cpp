#include "scene/object_lights.hpp"

#include "scene/object_sun.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>

namespace ts2::scene::objectlights {

namespace {

// Where a room's directional light comes from, as the game turns its stored direction into the
// light's axis: the rotation from straight down to the direction by the half-way vector, with the
// direction left at its own length (so its slope comes out flatter than stored).
std::array<float, 3> towardLight(const std::array<float, 3>& direction) {
    constexpr std::array<float, 3> kDown{0, 0, -1};
    const std::array<float, 3> sum{kDown[0] + direction[0], kDown[1] + direction[1], kDown[2] + direction[2]};
    const float length2 = sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2];
    if (!(length2 > 1.1920929e-7f) || !std::isfinite(length2)) {
        return kDown;
    }
    const float inverse = 1 / std::sqrt(length2);
    const std::array<float, 3> half{sum[0] * inverse, sum[1] * inverse, sum[2] * inverse};
    const float d = kDown[0] * half[0] + kDown[1] * half[1] + kDown[2] * half[2];
    return {kDown[0] - 2 * d * half[0], kDown[1] - 2 * d * half[1], kDown[2] - 2 * d * half[2]};
}

// The light the game gives a door's or window's part in a room as its ambient: the floor light map's
// texel under the part's node on its storey, at three quarters of the light the texel stands for
// (the map keeps half of it). None off the map.
std::optional<std::array<std::uint8_t, 3>> floorTexel(const Scene& scene, int storey, float x, float y) {
    const int index = scene.lightMaps ? storey - scene.lightMaps->lowest : -1;
    if (index < 0 || static_cast<std::size_t>(index) >= scene.lightMaps->floors.size()) {
        return std::nullopt;
    }
    const int image = scene.lightMaps->floors[static_cast<std::size_t>(index)];
    if (image < 0 || static_cast<std::size_t>(image) >= scene.images.size()) {
        return std::nullopt;
    }
    const Image& map = scene.images[static_cast<std::size_t>(image)];
    const float u = std::floor(x * 8), v = std::floor(y * 8);
    if (!(u >= 0 && v >= 0 && u < static_cast<float>(map.width) && v < static_cast<float>(map.height))) {
        return std::nullopt;
    }
    const std::size_t at = (static_cast<std::size_t>(v) * map.width + static_cast<std::size_t>(u)) * 4;
    if (at + 2 >= map.rgba.size()) {
        return std::nullopt;
    }
    return std::array<std::uint8_t, 3>{map.rgba[at], map.rgba[at + 1], map.rgba[at + 2]};
}

} // namespace

ObjectLighting fromRoom(const roomlight::Lights& lights) {
    ObjectLighting out;
    out.ambient = lights.ambient;
    for (const auto& d : lights.directionals) {
        ObjectLighting::Light light;
        light.direction = towardLight(d.direction);
        light.color = d.color;
        out.lights.push_back(light);
    }
    return out;
}

namespace {

float length(const std::array<float, 3>& v) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// The light's reach: where its colour's length / (1 + falloff x d^2) falls to `cutoff`.
float rangeOf(const std::array<float, 3>& color, float falloff, float cutoff) {
    const float x = cutoff > 0 ? length(color) / cutoff : 0;
    if (!(x > 1)) {
        return 0;
    }
    return falloff <= 0 ? 10000.0f : std::sqrt((x - 1) / falloff);
}

// A spot light's cone from its full angle and penumbra (radians), as cosines.
void setCone(ObjectLighting::Light& light, const std::array<float, 3>& axis, float coneAngle, float conePenumbra) {
    const float l = length(axis);
    light.spot = true;
    light.direction = {axis[0] / l, axis[1] / l, axis[2] / l};
    light.coneInner = std::cos(coneAngle / 2);
    light.coneOuter = std::cos((coneAngle + conePenumbra) / 2);
}

} // namespace

std::vector<PointLight> pointLights(const std::vector<lotlighting::PortalLight>& portals, const std::vector<LampLight>& lamps, float cutoff) {
    std::vector<PointLight> out;
    for (const auto& lamp : lamps) {
        PointLight p;
        p.room = lamp.room;
        p.light.point = true;
        p.light.position = lamp.position;
        p.light.color = lamp.color;
        p.light.falloff = lamp.falloff;
        p.light.range = rangeOf(lamp.color, lamp.falloff, cutoff);
        p.strength = 0.25f * length(lamp.color);
        if (lamp.spot) {
            if (!(length(lamp.axis) > 0)) {
                continue;
            }
            setCone(p.light, lamp.axis, lamp.coneAngle, lamp.conePenumbra);
        }
        if (p.strength > 0 && p.light.range > 0 && std::isfinite(p.strength) && std::isfinite(p.light.range)) {
            out.push_back(p);
        }
    }
    for (const auto& portal : portals) {
        if (!portal.lights) {
            continue;
        }
        auto add = [&](const windowlight::Light& from, bool spill) {
            PointLight p;
            p.room = portal.into;
            p.light.point = true;
            p.light.position = portal.lightPosition;
            p.light.color = from.color;
            p.light.falloff = from.falloff;
            p.light.range = rangeOf(from.color, from.falloff, cutoff);
            p.strength = 0.25f * length(from.color);
            p.plane = spill;
            p.planePoint = portal.spillPoint;
            p.planeNormal = {portal.portal.nx, portal.portal.ny};
            if (!spill && portal.spot) {
                if (!(length(portal.lightAxis) > 0)) {
                    return;
                }
                setCone(p.light, portal.lightAxis, portal.coneAngle, portal.conePenumbra);
            }
            if (p.strength > 0 && p.light.range > 0 && std::isfinite(p.strength) && std::isfinite(p.light.range)) {
                out.push_back(p);
            }
        };
        add(portal.lights->beam, false);
        if (portal.lights->spill) {
            add(*portal.lights->spill, true);
        }
    }
    return out;
}

namespace {

using Corners = std::array<std::array<float, 3>, 8>;

// The corners of a primitive's box (its node origin when it has no vertices), placed in the world.
Corners boxCorners(const Primitive& primitive, const Transform& transform) {
    std::array<float, 3> low = primitive.origin, high = primitive.origin;
    if (primitive.positions.size() >= 3) {
        low = high = {primitive.positions[0], primitive.positions[1], primitive.positions[2]};
        for (std::size_t i = 0; i + 2 < primitive.positions.size(); i += 3) {
            for (std::size_t k = 0; k < 3; ++k) {
                low[k] = std::min(low[k], primitive.positions[i + k]);
                high[k] = std::max(high[k], primitive.positions[i + k]);
            }
        }
    }
    Corners out{};
    for (int c = 0; c < 8; ++c) {
        float v[3] = {c & 1 ? high[0] : low[0], c & 2 ? high[1] : low[1], c & 4 ? high[2] : low[2]};
        transform.apply(v, true);
        out[static_cast<std::size_t>(c)] = {v[0], v[1], v[2]};
    }
    return out;
}

// The game's score of a point light for a box: the best of its corners.
float score(const PointLight& p, const Corners& corners) {
    float best = 0;
    for (const auto& c : corners) {
        if (p.plane && p.planeNormal[0] * (c[0] - p.planePoint[0]) + p.planeNormal[1] * (c[1] - p.planePoint[1]) < 0) {
            continue;
        }
        const float d[3] = {c[0] - p.light.position[0], c[1] - p.light.position[1], c[2] - p.light.position[2]};
        const float d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (!(d2 < p.light.range * p.light.range)) {
            continue;
        }
        float s = p.strength / (d2 * p.light.falloff + 1);
        if (p.light.spot) {
            // How far along the beam the corner lies, as the cosine of its angle off the beam.
            const float distance = std::sqrt(d2);
            const auto& a = p.light.direction;
            const float along = -(d[0] * a[0] + d[1] * a[1] + d[2] * a[2]);
            if (!(d2 > 0) || !(along > distance * p.light.coneOuter)) {
                continue;
            }
            if (along < distance * p.light.coneInner) {
                s *= (along / distance - p.light.coneOuter) / (p.light.coneInner - p.light.coneOuter);
            }
        }
        best = std::max(best, s);
    }
    return best;
}

// A room's light for one group (`lit`), with the room's average as its texture light. `ambient`
// replaces the group's own (doors' and windows' parts).
ObjectLighting roomBase(const lotlighting::Room& lit, int group, const std::optional<std::array<float, 3>>& ambient) {
    ObjectLighting base = fromRoom(*lit.groups[group]);
    base.textureLight = lit.average;
    if (ambient) {
        base.ambient = *ambient;
    }
    return base;
}

// The lights a part in room `room` (0 outdoors) gets, as the game picks them: the directional lights
// of `base` and the room's point lights, the eight that score best, directionals first.
ObjectLighting choose(ObjectLighting base, const std::vector<PointLight>& points, std::int16_t room, const Corners& corners) {
    constexpr std::size_t kMaxLights = 8;
    std::vector<std::pair<float, ObjectLighting::Light>> scored;
    for (const auto& light : base.lights) {
        scored.push_back({0.25f * length(light.color), light});
    }
    for (const auto& p : points) {
        if (p.room == room) {
            scored.push_back({score(p, corners), p.light});
        }
    }
    std::erase_if(scored, [](const auto& s) { return !(s.first > 0); });
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (scored.size() > kMaxLights) {
        scored.resize(kMaxLights);
    }
    std::stable_partition(scored.begin(), scored.end(), [](const auto& s) { return !s.second.point; });
    base.lights.clear();
    for (const auto& s : scored) {
        base.lights.push_back(s.second);
    }
    return base;
}

bool hasPointLight(const ObjectLighting& set) {
    return std::any_of(set.lights.begin(), set.lights.end(), [](const ObjectLighting::Light& l) { return l.point; });
}

// What tells two light sets apart, to share equal ones.
std::vector<float> key(const ObjectLighting& set) {
    std::vector<float> out(set.ambient.begin(), set.ambient.end());
    out.insert(out.end(), set.textureLight.begin(), set.textureLight.end());
    for (const auto& l : set.lights) {
        out.insert(out.end(), {l.point ? 1.0f : 0.0f, l.direction[0], l.direction[1], l.direction[2], l.color[0], l.color[1], l.color[2],
                               l.position[0], l.position[1], l.position[2], l.falloff, l.range, l.spot ? 1.0f : 0.0f, l.coneInner, l.coneOuter});
    }
    return out;
}

// A thing's light sets in the room at `at` (x, y in the lot), one per part: the game picks the lights
// for each part by the part's box. Outdoors the parts pick from `outdoor` (with the Sim-only lights
// for a Sim) and the outdoor lamps and windows. Empty outdoors when no lamp or window reaches any
// part, so the thing keeps the daylight as it fades.
std::vector<ObjectLighting> thingSets(const std::map<std::int16_t, lotlighting::Room>& rooms, const ObjectLighting& outdoor,
                                      const std::vector<PointLight>& points, const RoomAt& roomAt, const Mesh& mesh, const Transform& inLot,
                                      int storey, const std::array<float, 2>& at, LightGroup group) {
    const std::int16_t id = roomAt(storey, at[0], at[1]);
    const auto room = rooms.find(id);
    const int kind = group == LightGroup::Sims ? lotlighting::kSims : lotlighting::kObjects;
    ObjectLighting base;
    if (id == 0) {
        base = group == LightGroup::Sims ? withSimLights(outdoor) : outdoor;
    } else if (room != rooms.end() && room->second.groups[kind]) {
        base = roomBase(room->second, kind, std::nullopt);
    } else {
        return {};
    }
    std::vector<ObjectLighting> out;
    for (const auto& primitive : mesh.primitives) {
        out.push_back(choose(base, points, id, boxCorners(primitive, inLot)));
    }
    if (id == 0 && std::none_of(out.begin(), out.end(), hasPointLight)) {
        out.clear();
    }
    return out;
}

} // namespace

ThingLights thingLights(std::map<std::int16_t, lotlighting::Room> rooms, ObjectLighting outdoor, const std::vector<lotlighting::PortalLight>& portals,
                        const std::vector<LampLight>& lamps, float rangeCutoff, RoomAt roomAt) {
    if (!roomAt) {
        return {};
    }
    return [rooms = std::move(rooms), outdoor = std::move(outdoor), points = pointLights(portals, lamps, rangeCutoff), roomAt = std::move(roomAt)](
               const Mesh& mesh, const Transform& inLot, int storey, LightGroup group) {
        return thingSets(rooms, outdoor, points, roomAt, mesh, inLot, storey, {inLot.m[3], inLot.m[7]}, group);
    };
}

namespace {

// Each part of a door or window by the room its node stands in: in a room the room's light for doors
// and windows with the floor light map under the node as its ambient, outdoors `outdoor` with the
// outdoor lamps and windows. None (the daylight) for a part outdoors that no lamp or window reaches.
// Empty when no part has a light set.
std::vector<std::optional<ObjectLighting>> portalParts(const Scene& scene, const Instance& instance, const Mesh& mesh, int storey,
                                                       const std::map<std::int16_t, lotlighting::Room>& rooms, const ObjectLighting& outdoor,
                                                       const std::vector<PointLight>& points, const RoomAt& roomAt) {
    std::vector<std::optional<ObjectLighting>> out;
    const auto nodes = objectsun::testPoints(scene, instance);
    bool lit = false;
    for (std::size_t i = 0; i < nodes.size() && i < mesh.primitives.size(); ++i) {
        const auto& node = nodes[i];
        const std::int16_t id = roomAt(storey, node[0], node[1]);
        const auto room = rooms.find(id);
        if (id == 0) {
            auto set = choose(outdoor, points, id, boxCorners(mesh.primitives[i], instance.transform));
            if (hasPointLight(set)) {
                out.push_back(std::move(set));
                lit = true;
            } else {
                out.emplace_back();
            }
            continue;
        }
        if (room == rooms.end() || !room->second.groups[lotlighting::kPortals]) {
            out.emplace_back();
            continue;
        }
        std::optional<std::array<float, 3>> ambient;
        if (const auto texel = floorTexel(scene, storey, node[0], node[1])) {
            ambient = std::array<float, 3>{};
            for (std::size_t k = 0; k < 3; ++k) {
                (*ambient)[k] = 0.75f * 2 * static_cast<float>((*texel)[k]) / 255;
            }
        }
        const auto corners = boxCorners(mesh.primitives[i], instance.transform);
        out.push_back(choose(roomBase(room->second, lotlighting::kPortals, ambient), points, id, corners));
        lit = true;
    }
    if (!lit) {
        out.clear();
    }
    return out;
}

} // namespace

std::vector<std::optional<ObjectLighting>> portalLights(const Scene& scene, const Instance& inLot, int storey,
                                                        const std::map<std::int16_t, lotlighting::Room>& rooms, const ObjectLighting& outdoor,
                                                        const std::vector<PointLight>& points, const RoomAt& roomAt) {
    if (!roomAt || inLot.mesh < 0 || static_cast<std::size_t>(inLot.mesh) >= scene.meshes.size()) {
        return {};
    }
    return portalParts(scene, inLot, scene.meshes[static_cast<std::size_t>(inLot.mesh)], storey, rooms, outdoor, points, roomAt);
}

int lightStorey(const std::function<float(int, float, float)>& floorHeight, const Transform& inLot, int storey) {
    if (!floorHeight) {
        return storey;
    }
    const auto& m = inLot.m;
    constexpr float kOnFloor = 0.01f;   // standing on a floor counts as above it
    constexpr int kMaxStoreys = 16;     // a broken height grid cannot loop on
    for (int i = 0; i < kMaxStoreys && floorHeight(storey + 2, m[3], m[7]) <= m[11] + kOnFloor; ++i) {
        ++storey;
    }
    return storey;
}

namespace {

// A thing's light sets (not a door's or window's) as light() gives them. A fence's rail or post
// stands in the room at its box's centre. A rail starts on a tile corner, which may be the corner of
// a room it only touches.
std::vector<ObjectLighting> propParts(const Scene& scene, const Instance& instance, const Mesh& mesh, int storey,
                                      const std::map<std::int16_t, lotlighting::Room>& rooms, const ObjectLighting& outdoor,
                                      const std::vector<PointLight>& points, const RoomAt& roomAt) {
    std::array<float, 2> at{instance.transform.m[3], instance.transform.m[7]};
    if (instance.sunTest == SunTest::AboveBox) {
        if (const auto centre = objectsun::testPoints(scene, instance); !centre.empty()) {
            at = {centre[0][0], centre[0][1]};
        }
    }
    return thingSets(rooms, outdoor, points, roomAt, mesh, instance.transform, lightStorey(scene.floorHeight, instance.transform, storey), at,
                     LightGroup::Props);
}

} // namespace

std::vector<ObjectLighting> propLights(const Scene& scene, const Instance& inLot, int storey, const std::map<std::int16_t, lotlighting::Room>& rooms,
                                       const ObjectLighting& outdoor, const std::vector<PointLight>& points, const RoomAt& roomAt) {
    if (!roomAt || inLot.mesh < 0 || static_cast<std::size_t>(inLot.mesh) >= scene.meshes.size()) {
        return {};
    }
    return propParts(scene, inLot, scene.meshes[static_cast<std::size_t>(inLot.mesh)], storey, rooms, outdoor, points, roomAt);
}

void light(Scene& scene, const std::map<std::int16_t, lotlighting::Room>& rooms, const ObjectLighting& outdoor,
           const std::vector<lotlighting::PortalLight>& portals, const std::vector<LampLight>& lamps, float rangeCutoff, const RoomAt& roomAt,
           int groundLevel) {
    const auto points = pointLights(portals, lamps, rangeCutoff);
    std::map<std::vector<float>, int> setOf;
    auto add = [&](ObjectLighting&& set) {
        auto [it, added] = setOf.emplace(key(set), static_cast<int>(scene.lightSets.size()));
        if (added) {
            scene.lightSets.push_back(std::move(set));
        }
        return it->second;
    };
    scene.lightSets.clear();
    for (auto& instance : scene.instances) {
        instance.lightSet = -1;
        instance.partLightSets.clear();
        if (instance.part != Part::Object || !roomAt || instance.mesh < 0 || static_cast<std::size_t>(instance.mesh) >= scene.meshes.size()) {
            continue;
        }
        const Mesh& mesh = scene.meshes[static_cast<std::size_t>(instance.mesh)];
        const int storey = instance.level - groundLevel;
        if (instance.lightGroup == LightGroup::Portals) {
            for (auto& part : portalParts(scene, instance, mesh, storey, rooms, outdoor, points, roomAt)) {
                instance.partLightSets.push_back(part ? add(std::move(*part)) : -1);
            }
            continue;
        }
        if (instance.lightGroup != LightGroup::Props) {
            continue;
        }
        for (auto& set : propParts(scene, instance, mesh, storey, rooms, outdoor, points, roomAt)) {
            instance.partLightSets.push_back(add(std::move(set)));
        }
        if (!instance.partLightSets.empty() &&
            std::all_of(instance.partLightSets.begin(), instance.partLightSets.end(), [&](int set) { return set == instance.partLightSets[0]; })) {
            instance.lightSet = instance.partLightSets[0];
            instance.partLightSets.clear();
        }
    }
}

} // namespace ts2::scene::objectlights
