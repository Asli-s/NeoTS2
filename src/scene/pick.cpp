#include "scene/pick.hpp"

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

} // namespace

void Picker::prepare(const Scene& scene) {
    boxes_.assign(scene.instances.size(), Box{});
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
                                         const std::array<float, 3>& direction) const {
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
                    best = Hit{i, *hit};
                }
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
