#include "scene/object_sun.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace ts2::scene::objectsun {

namespace {

constexpr float kAboveBox = 0.1f;

struct Box {
    std::array<float, 3> low, high;
};

// The box around the mesh as placed: the box around its eight corners turned and moved into place.
std::optional<Box> placedBox(const Mesh& mesh, const Transform& transform) {
    constexpr float kFar = std::numeric_limits<float>::max();
    Box local{{kFar, kFar, kFar}, {-kFar, -kFar, -kFar}};
    for (const auto& p : mesh.primitives) {
        for (std::size_t i = 0; i + 2 < p.positions.size(); i += 3) {
            for (std::size_t k = 0; k < 3; ++k) {
                local.low[k] = std::min(local.low[k], p.positions[i + k]);
                local.high[k] = std::max(local.high[k], p.positions[i + k]);
            }
        }
    }
    if (!(local.low[0] <= local.high[0] && local.low[1] <= local.high[1] && local.low[2] <= local.high[2])) {
        return std::nullopt;
    }
    Box placed{{kFar, kFar, kFar}, {-kFar, -kFar, -kFar}};
    for (int corner = 0; corner < 8; ++corner) {
        float v[3] = {corner & 1 ? local.high[0] : local.low[0], corner & 2 ? local.high[1] : local.low[1],
                      corner & 4 ? local.high[2] : local.low[2]};
        transform.apply(v, true);
        for (std::size_t k = 0; k < 3; ++k) {
            placed.low[k] = std::min(placed.low[k], v[k]);
            placed.high[k] = std::max(placed.high[k], v[k]);
        }
    }
    return placed;
}

} // namespace

float at(const lotshadow::Grid& shadow, float zone, const std::array<float, 3>& point) {
    const float x = point[0] * lotshadow::kSamplesPerTile, y = point[1] * lotshadow::kSamplesPerTile;
    if (!(x >= 0 && y >= 0 && x <= static_cast<float>(shadow.width - 1) && y <= static_cast<float>(shadow.height - 1)) ||
        !std::isfinite(point[2])) {
        return 1;
    }
    const float share = lotshadow::sunVisible(shadow.shadowAt(point[0], point[1]) - point[2], zone);
    return std::isfinite(share) ? std::clamp(share, 0.0f, 1.0f) : 1.0f;
}

std::vector<std::array<float, 3>> testPoints(const Scene& scene, const Instance& instance) {
    if (instance.mesh < 0 || static_cast<std::size_t>(instance.mesh) >= scene.meshes.size()) {
        return {};
    }
    const Mesh& mesh = scene.meshes[static_cast<std::size_t>(instance.mesh)];
    const auto& m = instance.transform.m;
    std::array<float, 3> point{m[3], m[7], m[11]};
    if (instance.sunTest == SunTest::NodePosition) {
        std::vector<std::array<float, 3>> out;
        for (const auto& p : mesh.primitives) {
            float v[3] = {p.origin[0], p.origin[1], p.origin[2]};
            instance.transform.apply(v, true);
            out.push_back({v[0], v[1], v[2]});
        }
        return out;
    }
    if (const auto box = placedBox(mesh, instance.transform)) {
        point = {(box->low[0] + box->high[0]) * 0.5f, (box->low[1] + box->high[1]) * 0.5f,
                 instance.sunTest == SunTest::AboveBox ? box->high[2] + kAboveBox : (box->low[2] + box->high[2]) * 0.5f};
    }
    return std::vector<std::array<float, 3>>(mesh.primitives.size(), point);
}

namespace {

std::vector<float> sunOf(const Scene& scene, const Instance& instance, const lotshadow::Grid& shadow, float zone) {
    std::vector<float> out;
    if (instance.sunTest == SunTest::Never) {
        return out;
    }
    if (instance.sunTest == SunTest::NoSun) {
        if (instance.mesh >= 0 && static_cast<std::size_t>(instance.mesh) < scene.meshes.size()) {
            out.assign(scene.meshes[static_cast<std::size_t>(instance.mesh)].primitives.size(), 0.0f);
        }
        return out;
    }
    for (const auto& point : testPoints(scene, instance)) {
        out.push_back(at(shadow, zone, point));
    }
    return out;
}

} // namespace

void shade(Scene& scene, const lotshadow::Grid& shadow, float zone) {
    for (auto& instance : scene.instances) {
        if (instance.part == Part::Object || instance.litOutdoors) {
            instance.sun = sunOf(scene, instance, shadow, zone);
        }
    }
}

ThingSun thingSun(std::shared_ptr<const lotshadow::Grid> shadow, float zone) {
    if (!shadow || shadow->width <= 0) {
        return {};
    }
    return [shadow = std::move(shadow), zone](const Scene& scene, const Instance& inLot) { return sunOf(scene, inLot, *shadow, zone); };
}

} // namespace ts2::scene::objectsun
