#include "scene/pick.hpp"

#include "scene/rig.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ts2::scene {

namespace {

using Vec = std::array<float, 3>;

Vec sub(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec cross(const Vec& a, const Vec& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Where along the ray it enters the box, if it does before `limit`.
bool crossesBox(const Vec& origin, const Vec& direction, const Vec& low, const Vec& high, float limit) {
    float near = 0, far = limit;
    for (std::size_t k = 0; k < 3; ++k) {
        if (std::abs(direction[k]) < 1e-12f) {
            if (origin[k] < low[k] || origin[k] > high[k]) {
                return false;
            }
            continue;
        }
        float t0 = (low[k] - origin[k]) / direction[k], t1 = (high[k] - origin[k]) / direction[k];
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        near = std::max(near, t0);
        far = std::min(far, t1);
        if (near > far) {
            return false;
        }
    }
    return true;
}

// Where along the ray it hits the triangle (either side), if it does.
std::optional<float> hitsTriangle(const Vec& origin, const Vec& direction, const Vec& a, const Vec& b, const Vec& c) {
    const Vec e1 = sub(b, a), e2 = sub(c, a);
    const Vec p = cross(direction, e2);
    const float det = dot(e1, p);
    if (std::abs(det) < 1e-12f) {
        return std::nullopt;
    }
    const float inv = 1 / det;
    const Vec s = sub(origin, a);
    const float u = dot(s, p) * inv;
    if (u < 0 || u > 1) {
        return std::nullopt;
    }
    const Vec q = cross(s, e1);
    const float v = dot(direction, q) * inv;
    if (v < 0 || u + v > 1) {
        return std::nullopt;
    }
    const float t = dot(e2, q) * inv;
    return t > 0 ? std::optional<float>(t) : std::nullopt;
}

Vec placed(const Transform& transform, const std::vector<float>& positions, std::size_t vertex) {
    Vec v{positions[3 * vertex], positions[3 * vertex + 1], positions[3 * vertex + 2]};
    transform.apply(v.data(), true);
    return v;
}

// Vertices whose weights add up to a little over 1 still count as bounded, with the box grown to match.
constexpr float kWeightSlack = 1e-3f;

} // namespace

const Picker::SkinBound& Picker::skinBound(std::size_t mesh, std::size_t primitive, const Primitive& p) const {
    const std::size_t vertices = p.positions.size() / 3;
    auto [it, first] = skinBounds_.try_emplace({mesh, primitive});
    SkinBound& bound = it->second;
    if (!first && bound.vertices == vertices) {
        return bound;
    }
    bound = SkinBound{};
    bound.vertices = vertices;
    bound.low = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    bound.high = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    std::vector<bool> used(256, false);
    bound.bounded = isSkinned(p) && vertices > 0;
    for (std::size_t v = 0; v < vertices && bound.bounded; ++v) {
        for (std::size_t k = 0; k < 3; ++k) {
            bound.low[k] = std::min(bound.low[k], p.positions[3 * v + k]);
            bound.high[k] = std::max(bound.high[k], p.positions[3 * v + k]);
        }
        float sum = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            const float w = p.weights[4 * v + k];
            if (w > 0) {
                sum += w;
                used[p.joints[4 * v + k]] = true;
            }
        }
        // A primitive with a position that isn't a number is tested in full.
        bound.bounded = sum <= 1 + kWeightSlack && std::isfinite(p.positions[3 * v]) && std::isfinite(p.positions[3 * v + 1]) &&
                        std::isfinite(p.positions[3 * v + 2]);
    }
    for (std::size_t j = 0; j < used.size(); ++j) {
        if (used[j]) {
            bound.joints.push_back(static_cast<std::uint8_t>(j));
        }
    }
    return bound;
}

// The nearest triangle of a Sim's body instance hit by the ray before `nearest`, from its triangles
// where `pose` places them. Skinned primitives without joints in the pose are not hit. A skinned
// primitive is posed only when the ray crosses a box round it worked out from its joints: each
// vertex is a weighted sum of its rest place moved by its joints, so it lies within its rest box
// moved by each joint it uses, or toward the model's origin when its weights add up to less than 1.
std::optional<float> Picker::posedHit(const Scene& scene, const ScenePose& pose, std::size_t i, const Vec& origin, const Vec& direction,
                                      float nearest) const {
    const Instance& instance = scene.instances[i];
    if (instance.mesh < 0 || static_cast<std::size_t>(instance.mesh) >= scene.meshes.size()) {
        return std::nullopt;
    }
    const auto posed = std::find_if(pose.joints.begin(), pose.joints.end(), [&](const auto& j) { return j.first == i; });
    const std::vector<Transform>* joints = posed != pose.joints.end() ? &posed->second : nullptr;
    const auto placed = std::find_if(pose.places.begin(), pose.places.end(), [&](const auto& p) { return p.first == i; });
    const Transform place = placed != pose.places.end() ? placed->second : instance.transform;
    const auto& primitives = scene.meshes[static_cast<std::size_t>(instance.mesh)].primitives;
    std::optional<float> best;
    std::vector<float> positions;
    for (std::size_t n = 0; n < primitives.size(); ++n) {
        const Primitive& primitive = primitives[n];
        if (isSkinned(primitive) && joints == nullptr) {
            continue;
        }
        if (isSkinned(primitive)) {
            if (const SkinBound& bound = skinBound(static_cast<std::size_t>(instance.mesh), n, primitive); bound.bounded) {
                Vec low{0, 0, 0}, high{0, 0, 0};  // the model's origin is always in
                for (const std::uint8_t j : bound.joints) {
                    if (j >= joints->size()) {
                        continue;  // posedPositions leaves such joints out
                    }
                    for (int c = 0; c < 8; ++c) {
                        Vec corner{(c & 1) ? bound.high[0] : bound.low[0], (c & 2) ? bound.high[1] : bound.low[1], (c & 4) ? bound.high[2] : bound.low[2]};
                        (*joints)[j].apply(corner.data(), true);
                        for (std::size_t k = 0; k < 3; ++k) {
                            low[k] = std::min(low[k], corner[k]);
                            high[k] = std::max(high[k], corner[k]);
                        }
                    }
                }
                const float reach = std::max({-low[0], -low[1], -low[2], high[0], high[1], high[2]}) * kWeightSlack;
                Vec placedLow{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
                Vec placedHigh{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
                for (int c = 0; c < 8; ++c) {
                    Vec corner{(c & 1) ? high[0] + reach : low[0] - reach, (c & 2) ? high[1] + reach : low[1] - reach,
                               (c & 4) ? high[2] + reach : low[2] - reach};
                    place.apply(corner.data(), true);
                    for (std::size_t k = 0; k < 3; ++k) {
                        placedLow[k] = std::min(placedLow[k], corner[k]);
                        placedHigh[k] = std::max(placedHigh[k], corner[k]);
                    }
                }
                if (!crossesBox(origin, direction, placedLow, placedHigh, best ? *best : nearest)) {
                    continue;
                }
            }
            posedPositions(primitive, *joints, positions);
        } else {
            // Copied, as they are moved into place below. The vector keeps its room between primitives.
            positions.assign(primitive.positions.begin(), primitive.positions.end());
        }
        const std::size_t vertices = positions.size() / 3;
        if (vertices == 0) {
            continue;
        }
        Vec low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        Vec high{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
        for (std::size_t v = 0; v < vertices; ++v) {
            place.apply(&positions[3 * v], true);
            for (std::size_t k = 0; k < 3; ++k) {
                low[k] = std::min(low[k], positions[3 * v + k]);
                high[k] = std::max(high[k], positions[3 * v + k]);
            }
        }
        const float limit = best ? *best : nearest;
        if (!crossesBox(origin, direction, low, high, limit)) {
            continue;
        }
        const auto& indices = primitive.indices;
        for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
            if (indices[t] >= vertices || indices[t + 1] >= vertices || indices[t + 2] >= vertices) {
                continue;
            }
            const auto corner = [&](std::uint32_t v) { return Vec{positions[3 * v], positions[3 * v + 1], positions[3 * v + 2]}; };
            const auto hit = hitsTriangle(origin, direction, corner(indices[t]), corner(indices[t + 1]), corner(indices[t + 2]));
            if (hit && *hit < (best ? *best : nearest)) {
                best = hit;
            }
        }
    }
    return best;
}


void Picker::prepare(const Scene& scene) {
    boxes_.assign(scene.instances.size(), Box{});
    skinBounds_.clear();
    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        update(scene, i);
    }
}

void Picker::update(const Scene& scene, std::size_t i) {
    if (i >= scene.instances.size()) {
        return;
    }
    if (boxes_.size() < scene.instances.size()) {
        boxes_.resize(scene.instances.size());
    }
    boxes_[i] = Box{};
    for (const auto& actor : scene.actors) {
        const bool posed = std::any_of(actor.skinned.begin(), actor.skinned.end(), [&](const auto& s) { return s.instance == i; }) ||
                           std::any_of(actor.held.begin(), actor.held.end(), [&](const auto& h) { return h.instance == i; });
        if (posed) {
            return;
        }
    }
    const Instance& instance = scene.instances[i];
    if (instance.mesh < 0 || static_cast<std::size_t>(instance.mesh) >= scene.meshes.size()) {
        return;
    }
    Vec low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec high{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    bool any = false;
    for (const auto& primitive : scene.meshes[static_cast<std::size_t>(instance.mesh)].primitives) {
        for (std::size_t v = 0; v + 2 < primitive.positions.size(); v += 3) {
            for (std::size_t k = 0; k < 3; ++k) {
                low[k] = std::min(low[k], primitive.positions[v + k]);
                high[k] = std::max(high[k], primitive.positions[v + k]);
            }
            any = true;
        }
    }
    if (!any) {
        return;
    }
    // The box around the model's box where the instance stands.
    Box& box = boxes_[i];
    box.low = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    box.high = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    for (int corner = 0; corner < 8; ++corner) {
        Vec p{corner & 1 ? high[0] : low[0], corner & 2 ? high[1] : low[1], corner & 4 ? high[2] : low[2]};
        instance.transform.apply(p.data(), true);
        for (std::size_t k = 0; k < 3; ++k) {
            box.low[k] = std::min(box.low[k], p[k]);
            box.high[k] = std::max(box.high[k], p[k]);
        }
    }
    box.pickable = true;
}

std::optional<Picker::Hit> Picker::pick(const Scene& scene, std::span<const std::uint8_t> shown, const std::array<float, 3>& origin,
                                         const std::array<float, 3>& direction, const ScenePose* pose) const {
    std::optional<Hit> best;
    float nearest = std::numeric_limits<float>::max();
    const std::size_t count = std::min(boxes_.size(), scene.instances.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (!boxes_[i].pickable || (i < shown.size() && !shown[i]) ||
            !crossesBox(origin, direction, boxes_[i].low, boxes_[i].high, nearest)) {
            continue;
        }
        const Instance& instance = scene.instances[i];
        if (instance.mesh < 0 || static_cast<std::size_t>(instance.mesh) >= scene.meshes.size()) {
            continue;
        }
        for (const auto& primitive : scene.meshes[static_cast<std::size_t>(instance.mesh)].primitives) {
            const std::size_t vertices = primitive.positions.size() / 3;
            const auto& indices = primitive.indices;
            for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
                if (indices[t] >= vertices || indices[t + 1] >= vertices || indices[t + 2] >= vertices) {
                    continue;
                }
                const auto hit = hitsTriangle(origin, direction, placed(instance.transform, primitive.positions, indices[t]),
                                              placed(instance.transform, primitive.positions, indices[t + 1]),
                                              placed(instance.transform, primitive.positions, indices[t + 2]));
                if (hit && *hit < nearest) {
                    nearest = *hit;
                    best = Hit{i, *hit, instance.object};
                }
            }
        }
    }
    if (pose == nullptr) {
        return best;
    }
    for (const auto& actor : scene.actors) {
        for (const auto& s : actor.skinned) {
            if (s.instance >= scene.instances.size() || (s.instance < shown.size() && !shown[s.instance])) {
                continue;
            }
            if (const auto hit = posedHit(scene, *pose, s.instance, origin, direction, nearest)) {
                nearest = *hit;
                best = Hit{s.instance, *hit, actor.object};
            }
        }
    }
    return best;
}

float hoverAmbient(double seconds) {
    constexpr double kTwoPi = 6.283185307179586;
    const double phase = seconds - std::floor(seconds);
    return static_cast<float>(0.1 * (0.9 - 0.1 * std::cos(kTwoPi * phase)));
}

} // namespace ts2::scene
