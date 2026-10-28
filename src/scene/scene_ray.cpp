#include "scene/scene_ray.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace ts2::scene {

namespace {

using Vec = std::array<float, 3>;

constexpr float kAheadTiles = 128;

Vec applied(const Transform& t, Vec v, bool point) {
    t.apply(v.data(), point);
    return v;
}
Vec minus(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec cross(const Vec& a, const Vec& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The inverse of a 3x4 transform, none when it flattens space.
std::optional<Transform> inverse(const Transform& t) {
    const auto& m = t.m;
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(det) || std::abs(det) < 1e-12f) {
        return std::nullopt;
    }
    const float s = 1 / det;
    Transform r;
    r.m = {(e * i - f * h) * s, (c * h - b * i) * s, (b * f - c * e) * s, 0,
           (f * g - d * i) * s, (a * i - c * g) * s, (c * d - a * f) * s, 0,
           (d * h - e * g) * s, (b * g - a * h) * s, (a * e - b * d) * s, 0};
    const Vec back = applied(r, {m[3], m[7], m[11]}, false);
    r.m[3] = -back[0];
    r.m[7] = -back[1];
    r.m[11] = -back[2];
    return r;
}

// Whether the segment from `o` along `d` (t from 0 to 1) passes through the box.
bool crossesBox(const Vec& o, const Vec& d, const Vec& low, const Vec& high) {
    float enter = 0, leave = 1;
    for (std::size_t k = 0; k < 3; ++k) {
        if (std::abs(d[k]) < 1e-12f) {
            if (o[k] < low[k] || o[k] > high[k]) {
                return false;
            }
            continue;
        }
        float t0 = (low[k] - o[k]) / d[k], t1 = (high[k] - o[k]) / d[k];
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        enter = std::max(enter, t0);
        leave = std::min(leave, t1);
        if (enter > leave) {
            return false;
        }
    }
    return true;
}

// Where along the segment it crosses the triangle (either side), if it does.
std::optional<float> crossesTriangle(const Vec& o, const Vec& d, const Vec& a, const Vec& b, const Vec& c) {
    const Vec ab = minus(b, a), ac = minus(c, a);
    const Vec p = cross(d, ac);
    const float det = dot(ab, p);
    if (std::abs(det) < 1e-12f) {
        return std::nullopt;
    }
    const float inv = 1 / det;
    const Vec s = minus(o, a);
    const float u = dot(s, p) * inv;
    if (u < 0 || u > 1) {
        return std::nullopt;
    }
    const Vec q = cross(s, ab);
    const float v = dot(d, q) * inv;
    if (v < 0 || u + v > 1) {
        return std::nullopt;
    }
    const float t = dot(ac, q) * inv;
    if (!(t >= 0 && t <= 1)) {
        return std::nullopt;
    }
    return t;
}

Vec vertex(const std::vector<float>& positions, std::uint32_t index) {
    const std::size_t at = static_cast<std::size_t>(index) * 3;
    return {positions[at], positions[at + 1], positions[at + 2]};
}

// Each triangle of a primitive whose positions are `positions` (in the space `o` and `d` are in).
void addCrossings(const Primitive& p, const std::vector<float>& positions, const Vec& o, const Vec& d,
                  SceneRays::Hit hit, std::vector<SceneRays::Hit>& out) {
    const std::size_t vertices = positions.size() / 3;
    for (std::size_t i = 0; i + 2 < p.indices.size(); i += 3) {
        const auto a = p.indices[i], b = p.indices[i + 1], c = p.indices[i + 2];
        if (a >= vertices || b >= vertices || c >= vertices) {
            continue;
        }
        if (const auto t = crossesTriangle(o, d, vertex(positions, a), vertex(positions, b), vertex(positions, c))) {
            hit.t = *t;
            out.push_back(hit);
        }
    }
}

} // namespace

const SceneRays::Box& SceneRays::primitiveBox(std::size_t mesh, std::size_t primitive) {
    if (boxes_.size() < scene_.meshes.size()) {
        boxes_.resize(scene_.meshes.size());
    }
    auto& boxes = boxes_[mesh];
    if (boxes.size() < scene_.meshes[mesh].primitives.size()) {
        boxes.resize(scene_.meshes[mesh].primitives.size());
    }
    auto& box = boxes[primitive];
    if (!box) {
        Box b;
        const auto& positions = scene_.meshes[mesh].primitives[primitive].positions;
        for (std::size_t i = 0; i + 2 < positions.size(); i += 3) {
            for (std::size_t k = 0; k < 3; ++k) {
                b.low[k] = i == 0 ? positions[k] : std::min(b.low[k], positions[i + k]);
                b.high[k] = i == 0 ? positions[k] : std::max(b.high[k], positions[i + k]);
            }
        }
        box = b;
    }
    return *box;
}

std::optional<Transform> SceneRays::locate(const Point& point, const ScenePose& pose, bool& slotMissing, bool& objectMissing,
                                           bool targetingSlots) {
    const auto placeOf = [&](std::size_t instance) {
        const auto it = std::find_if(pose.places.begin(), pose.places.end(), [&](const auto& p) { return p.first == instance; });
        return it != pose.places.end() ? it->second : scene_.instances[instance].transform;
    };
    const auto meshOf = [&](std::size_t instance) -> const Mesh* {
        const int mesh = scene_.instances[instance].mesh;
        return mesh >= 0 && static_cast<std::size_t>(mesh) < scene_.meshes.size() ? &scene_.meshes[static_cast<std::size_t>(mesh)] : nullptr;
    };
    const Mesh* mesh = nullptr;
    Transform base;
    std::optional<Transform> slot;
    if (const auto placed = std::find_if(scene_.placedObjects.begin(), scene_.placedObjects.end(),
                                         [&](const PlacedObject& p) { return p.object == point.object; });
        placed != scene_.placedObjects.end() && placed->instance < scene_.instances.size()) {
        mesh = meshOf(placed->instance);
        base = placeOf(placed->instance);
        if (point.local) {
            slot = base * *point.local;
        } else if (point.kind == 3) {
            if (point.slot >= placed->slots.size()) {
                slotMissing = true;
            } else {
                slot = placed->slots[point.slot] ? base * *placed->slots[point.slot] : base;
            }
        } else if (targetingSlots && point.kind != 0) {
            slotMissing = true;
        }
    } else if (const auto actor = std::find_if(scene_.actors.begin(), scene_.actors.end(),
                                               [&](const Actor& a) { return a.object == point.object; });
               actor != scene_.actors.end()) {
        base = actor->place;
        if (!actor->skinned.empty() && actor->skinned.front().instance < scene_.instances.size()) {
            mesh = meshOf(actor->skinned.front().instance);
        }
        if ((point.kind == 1 && targetingSlots) || point.kind == 3) {
            const auto& nodes = point.kind == 1 ? actor->targetNodes : actor->slotNodes;
            if (point.slot >= nodes.size() || (point.kind == 1 && nodes[point.slot] < 0)) {
                slotMissing = true;
            } else {
                const int node = nodes[point.slot];
                const auto index = static_cast<std::size_t>(actor - scene_.actors.begin());
                const auto posed = std::find_if(pose.nodes.begin(), pose.nodes.end(), [&](const auto& n) { return n.first == index; });
                const bool known = node >= 0 && posed != pose.nodes.end() && static_cast<std::size_t>(node) < posed->second.size();
                slot = known ? actor->place * posed->second[static_cast<std::size_t>(node)] : actor->place;
            }
        } else if (targetingSlots && point.kind != 0) {
            slotMissing = true;
        }
    } else {
        objectMissing = true;
        return std::nullopt;
    }
    if (slotMissing) {
        return std::nullopt;
    }
    if (slot) {
        return slot;
    }
    Vec low{0, 0, 0}, high{0, 0, 0};
    for (std::size_t p = 0; mesh != nullptr && p < mesh->primitives.size(); ++p) {
        const auto& positions = mesh->primitives[p].positions;
        for (std::size_t i = 0; i + 2 < positions.size(); i += 3) {
            for (std::size_t k = 0; k < 3; ++k) {
                low[k] = std::min(low[k], positions[i + k]);
                high[k] = std::max(high[k], positions[i + k]);
            }
        }
    }
    Transform at = base;
    const Vec middle = applied(base, {(low[0] + high[0]) * 0.5f, (low[1] + high[1]) * 0.5f, (low[2] + high[2]) * 0.75f}, true);
    at.m[3] = middle[0];
    at.m[7] = middle[1];
    at.m[11] = middle[2];
    return at;
}

SceneRays::Result SceneRays::trace(const Point& from, const std::optional<Point>& to, const ScenePose& pose,
                                   const std::vector<std::uint8_t>& visible) {
    Result result;
    std::map<std::size_t, const Transform*> places;
    for (const auto& [instance, place] : pose.places) {
        places[instance] = &place;
    }
    std::map<std::size_t, const std::vector<Transform>*> joints;
    for (const auto& [instance, js] : pose.joints) {
        joints[instance] = &js;
    }
    auto placeOf = [&](std::size_t instance) {
        const auto it = places.find(instance);
        return it != places.end() ? *it->second : scene_.instances[instance].transform;
    };
    auto meshOf = [&](std::size_t instance) -> const Mesh* {
        const int mesh = scene_.instances[instance].mesh;
        return mesh >= 0 && static_cast<std::size_t>(mesh) < scene_.meshes.size() ? &scene_.meshes[static_cast<std::size_t>(mesh)] : nullptr;
    };

    // Where a point is in the world, and the transform it faces by.
    auto locate = [&](const Point& point) {
        bool slotMissing = false, objectMissing = false;
        auto at = this->locate(point, pose, slotMissing, objectMissing);
        result.slotMissing = result.slotMissing || slotMissing;
        result.objectMissing = result.objectMissing || objectMissing;
        return at;
    };

    const auto start = locate(from);
    if (!start) {
        return result;
    }
    const Vec origin{start->m[3], start->m[7], start->m[11]};
    Vec end;
    if (to) {
        const auto finish = locate(*to);
        if (!finish) {
            return result;
        }
        end = {finish->m[3], finish->m[7], finish->m[11]};
    } else {
        const Vec ahead = applied(*start, {0, kAheadTiles, 0}, false);
        end = {origin[0] + ahead[0], origin[1] + ahead[1], origin[2] + ahead[2]};
    }
    const Vec along = minus(end, origin);

    // Which object each Sim's body instances draw.
    std::map<std::size_t, std::uint32_t> actorParts;
    for (const auto& actor : scene_.actors) {
        for (const auto& s : actor.skinned) {
            actorParts[s.instance] = actor.object;
        }
    }
    std::vector<float> skinned;
    for (std::size_t i = 0; i < scene_.instances.size(); ++i) {
        if (!visible.empty() && (i >= visible.size() || !visible[i])) {
            continue;
        }
        const Instance& instance = scene_.instances[i];
        const Mesh* mesh = meshOf(i);
        if (mesh == nullptr) {
            continue;
        }
        Hit hit;
        if (instance.part == Part::Wall || instance.part == Part::WallHub) {
            hit.kind = Hit::Kind::Wall;
        } else if (instance.part == Part::Floor) {
            hit.kind = Hit::Kind::Floor;
        } else if (instance.part == Part::Roof || instance.part == Part::Skirt) {
            continue;
        } else {
            const auto actor = actorParts.find(i);
            hit.object = actor != actorParts.end() ? actor->second : instance.object;
            if (hit.object == 0) {
                continue;
            }
        }
        const Transform place = placeOf(i);
        const auto posed = joints.find(i);
        const auto back = inverse(place);
        if (!back) {
            continue;
        }
        const Vec o = applied(*back, origin, true), d = applied(*back, along, false);
        for (std::size_t p = 0; p < mesh->primitives.size(); ++p) {
            const Primitive& primitive = mesh->primitives[p];
            if (isSkinned(primitive)) {
                if (posed != joints.end()) {
                    posedPositions(primitive, *posed->second, skinned);
                    addCrossings(primitive, skinned, o, d, hit, result.hits);
                }
                continue;
            }
            const Box& box = primitiveBox(static_cast<std::size_t>(instance.mesh), p);
            if (crossesBox(o, d, box.low, box.high)) {
                addCrossings(primitive, primitive.positions, o, d, hit, result.hits);
            }
        }
    }
    std::stable_sort(result.hits.begin(), result.hits.end(), [](const Hit& a, const Hit& b) { return a.t < b.t; });
    return result;
}

} // namespace ts2::scene
