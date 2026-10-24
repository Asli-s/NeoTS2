#pragma once

#include "orbit.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace app {

// The height the game's live camera keeps its look-at point at, from LiveCamera.txt's settings:
// the ground, roofs and floors under a point, kept between frames for the corners it samples.
class CameraHeight {
public:
    float cornerOffset = 1, cornerRatio = 0.5f;  // cornersampleoffset, cornersampleratio
    float floorRange = 6, floorInnerRange = 2;    // floorheightsamplerange, floorheightsampleinnerrange

    // Sets the orbit's goal height for `level` (as Instance::level counts them) on a lot placed by
    // `lotPlace`: the height under the goal, in full when zoomed all the way in and none when all the
    // way out, raised to the height under the point just ahead of the camera at its near clip
    // distance so the view never starts inside a floor.
    void apply(Orbit& orbit, const ts2::scene::Scene& scene, std::span<const std::uint8_t> shown,
               const ts2::scene::Transform& lotPlace, int level) {
        if (!orbit.goal || !orbit.rig || orbit.rig->levels.empty()) {
            return;
        }
        Vec3& goal = *orbit.goal;
        const auto toLot = lotPlace.rigidInverse();
        // Heights are worked out in the lot's own coordinates, as the game keeps them.
        auto lotHeightAt = [&](float x, float y, std::array<float, 4>& corners) {
            float at[3] = {x, y, 0};
            toLot.apply(at, true);
            return heightAt(scene, shown, lotPlace, level, at[0], at[1], corners);
        };
        auto toWorldZ = [&](float x, float y, float lotZ) {
            float at[3] = {x, y, 0};
            toLot.apply(at, true);
            at[2] = lotZ;
            lotPlace.apply(at, true);
            return at[2];
        };
        const float nearest = orbit.rig->levels.back().distance, farthest = orbit.rig->levels.front().distance;
        const float zoom = std::clamp(orbit.distanceGoal, nearest, farthest);
        const float share = farthest - nearest < 0.001f ? 0.0f : 1 - (zoom - nearest) / (farthest - nearest);
        float z = lotHeightAt(goal[0], goal[1], under_) * share;
        goal[2] = toWorldZ(goal[0], goal[1], z);
        // The camera as it will be once it reaches its goals.
        Orbit settled = orbit;
        settled.previous.reset();
        settled.target = goal;
        settled.yaw = orbit.yawGoal;
        settled.pitchBlend = orbit.pitchBlendGoal;
        settled.zoomTo(orbit.distanceGoal);
        const auto camera = settled.camera();
        Vec3 ahead = {goal[0] - camera.eye[0], goal[1] - camera.eye[1], goal[2] - camera.eye[2]};
        const float length = std::sqrt(ahead[0] * ahead[0] + ahead[1] * ahead[1] + ahead[2] * ahead[2]);
        if (length > 0) {
            const float nearClip = ts2::camera::nearClipAt(*orbit.rig, orbit.distanceGoal) / length;
            const float probe = lotHeightAt(camera.eye[0] + ahead[0] * nearClip, camera.eye[1] + ahead[1] * nearClip, ahead_);
            z = std::max(z, probe);
        }
        goal[2] = toWorldZ(goal[0], goal[1], z);
    }

    // The free camera's ground at the world position (x, y), as a world height: the highest of the
    // ground or roof straight down and `ratio` times each of four corners `offset` away, one of which
    // is sampled a call, in turn by `counter`, while the other three keep their last height in
    // `corners`. Between -200 and 200 in the lot's coordinates.
    float groundBelow(const ts2::scene::Scene& scene, std::span<const std::uint8_t> shown, const ts2::scene::Transform& lotPlace, float x,
                      float y, std::array<float, 4>& corners, unsigned& counter, float offset, float ratio) {
        constexpr float kTop = 200, kNone = -200;
        const auto toLot = lotPlace.rigidInverse();
        float at[3] = {x, y, 0};
        toLot.apply(at, true);
        float h = surfaceBelow(scene, shown, lotPlace, at[0], at[1]).value_or(kNone);
        const std::size_t i = counter++ & 3;
        const float cx = at[0] + (i == 1 || i == 2 ? offset : -offset), cy = at[1] + (i >= 2 ? offset : -offset);
        corners[i] = surfaceBelow(scene, shown, lotPlace, cx, cy).value_or(kNone);
        for (const float c : corners) {
            h = std::max(h, c * ratio);
        }
        at[2] = std::clamp(h, kNone, kTop);
        lotPlace.apply(at, true);
        return at[2];
    }

private:
    // The game samples one corner around each point a frame, in turn, and keeps the other three.
    std::array<float, 4> under_{-200, -200, -200, -200}, ahead_{-200, -200, -200, -200};
    unsigned corner_ = 0;

    // The height under the lot position (x, y), in lot coordinates: the ground or roof straight down,
    // the corners' heights times the corner ratio, and the highest floor at or below `level` on the
    // tiles within the floor range, in full within the inner range and fading to nothing at its edge. Between -200 and 200.
    float heightAt(const ts2::scene::Scene& scene, std::span<const std::uint8_t> shown,
                   const ts2::scene::Transform& lotPlace, int level, float x, float y, std::array<float, 4>& corners) {
        constexpr float kTop = 200, kNone = -200;
        float h = surfaceBelow(scene, shown, lotPlace, x, y).value_or(kNone);
        const std::size_t i = corner_++ & 3;
        const float cx = x + (i == 1 || i == 2 ? cornerOffset : -cornerOffset), cy = y + (i >= 2 ? cornerOffset : -cornerOffset);
        const auto hit = surfaceBelow(scene, shown, lotPlace, cx, cy);
        corners[i] = hit && *hit >= h ? *hit : kNone;
        for (const float c : corners) {
            h = std::max(h, c * cornerRatio);
        }
        h = std::clamp(h, kNone, kTop);
        if (!scene.floorCorner) {
            return h;
        }
        const int tx = static_cast<int>(std::floor(x)), ty = static_cast<int>(std::floor(y));
        const int reach = static_cast<int>(std::ceil(floorRange));
        for (int dx = -reach; dx <= reach; ++dx) {
            for (int dy = -reach; dy <= reach; ++dy) {
                const float distance = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                if (distance >= floorRange) {
                    continue;
                }
                for (int l = level; l >= 0; --l) {
                    const auto floor = scene.floorCorner(l, tx + dx, ty + dy);
                    if (!floor) {
                        continue;
                    }
                    float height = *floor;
                    if (distance > floorInnerRange) {
                        height *= 1 - (distance - floorInnerRange) / (floorRange - floorInnerRange);
                    }
                    h = std::max(h, height);
                    break;
                }
            }
        }
        return h;
    }

    // The ground (the lot's and the skirt's) and roof triangles in lot coordinates, sorted into the
    // 1 x 1 cells they cover, so a ray straight down only tests the few in its cell. Built once for a scene.
    struct Triangle {
        std::array<float, 9> corners{};
        std::size_t instance = 0;
    };
    const ts2::scene::Scene* surfacesOf_ = nullptr;
    std::vector<Triangle> triangles_;
    std::vector<std::vector<std::uint32_t>> cells_;
    int cellX_ = 0, cellY_ = 0, cellsWide_ = 0, cellsHigh_ = 0;

    void collectSurfaces(const ts2::scene::Scene& scene, const ts2::scene::Transform& lotPlace) {
        surfacesOf_ = &scene;
        triangles_.clear();
        cells_.clear();
        const auto toLot = lotPlace.rigidInverse();
        float low[2] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        float high[2] = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
        for (std::size_t i = 0; i < scene.instances.size(); ++i) {
            const auto& instance = scene.instances[i];
            // The lot's ground and roofs, and the ground of the skirt round it, where the camera can
            // look past the lot's edge.
            const bool ground = instance.name == "terrain" || instance.name == "lot skirt";
            if ((instance.part != ts2::scene::Part::Roof && !ground) || instance.mesh < 0 ||
                static_cast<std::size_t>(instance.mesh) >= scene.meshes.size()) {
                continue;
            }
            const auto place = toLot * instance.transform;
            for (const auto& primitive : scene.meshes[static_cast<std::size_t>(instance.mesh)].primitives) {
                const std::size_t vertices = primitive.positions.size() / 3;
                for (std::size_t t = 0; t + 2 < primitive.indices.size(); t += 3) {
                    Triangle tri{{}, i};
                    bool valid = true;
                    for (std::size_t c = 0; c < 3; ++c) {
                        const std::size_t v = primitive.indices[t + c];
                        if (v >= vertices) {
                            valid = false;
                            break;
                        }
                        float p[3] = {primitive.positions[v * 3], primitive.positions[v * 3 + 1], primitive.positions[v * 3 + 2]};
                        place.apply(p, true);
                        for (std::size_t k = 0; k < 3; ++k) {
                            tri.corners[c * 3 + k] = p[k];
                        }
                        for (std::size_t k = 0; k < 2; ++k) {
                            low[k] = std::min(low[k], p[k]);
                            high[k] = std::max(high[k], p[k]);
                        }
                    }
                    if (valid) {
                        triangles_.push_back(tri);
                    }
                }
            }
        }
        // A lot is at most a few hundred tiles across. A larger area is left without surfaces.
        constexpr int kMostCells = 1 << 20;
        if (triangles_.empty() || high[0] - low[0] > 4096 || high[1] - low[1] > 4096) {
            triangles_.clear();
            return;
        }
        cellX_ = static_cast<int>(std::floor(low[0]));
        cellY_ = static_cast<int>(std::floor(low[1]));
        cellsWide_ = static_cast<int>(std::floor(high[0])) - cellX_ + 1;
        cellsHigh_ = static_cast<int>(std::floor(high[1])) - cellY_ + 1;
        if (cellsWide_ * cellsHigh_ > kMostCells) {
            triangles_.clear();
            return;
        }
        cells_.resize(static_cast<std::size_t>(cellsWide_ * cellsHigh_));
        for (std::uint32_t t = 0; t < triangles_.size(); ++t) {
            const auto& c = triangles_[t].corners;
            const int x0 = static_cast<int>(std::floor(std::min({c[0], c[3], c[6]}))) - cellX_;
            const int x1 = static_cast<int>(std::floor(std::max({c[0], c[3], c[6]}))) - cellX_;
            const int y0 = static_cast<int>(std::floor(std::min({c[1], c[4], c[7]}))) - cellY_;
            const int y1 = static_cast<int>(std::floor(std::max({c[1], c[4], c[7]}))) - cellY_;
            for (int x = x0; x <= x1; ++x) {
                for (int y = y0; y <= y1; ++y) {
                    cells_[static_cast<std::size_t>(y * cellsWide_ + x)].push_back(t);
                }
            }
        }
    }

    // The height of the highest ground or roof at or below 200 straight down at the lot position
    // (x, y), in lot coordinates, among the instances `shown` (all when empty). On the lot where
    // there is none, the ground's height.
    std::optional<float> surfaceBelow(const ts2::scene::Scene& scene, std::span<const std::uint8_t> shown,
                                      const ts2::scene::Transform& lotPlace, float x, float y) {
        if (surfacesOf_ != &scene) {
            collectSurfaces(scene, lotPlace);
        }
        const int cx = static_cast<int>(std::floor(x)) - cellX_, cy = static_cast<int>(std::floor(y)) - cellY_;
        if (cells_.empty() || cx < 0 || cy < 0 || cx >= cellsWide_ || cy >= cellsHigh_) {
            return std::nullopt;
        }
        std::optional<float> best;
        for (const std::uint32_t t : cells_[static_cast<std::size_t>(cy * cellsWide_ + cx)]) {
            const auto& tri = triangles_[t];
            if (tri.instance < shown.size() && !shown[tri.instance]) {
                continue;
            }
            const auto& c = tri.corners;
            // Where (x, y) falls in the triangle seen from above, and its height there.
            const float d = (c[4] - c[7]) * (c[0] - c[6]) + (c[6] - c[3]) * (c[1] - c[7]);
            if (std::abs(d) < 1e-12f) {
                continue;
            }
            const float a = ((c[4] - c[7]) * (x - c[6]) + (c[6] - c[3]) * (y - c[7])) / d;
            const float b = ((c[7] - c[1]) * (x - c[6]) + (c[0] - c[6]) * (y - c[7])) / d;
            if (a < 0 || b < 0 || a + b > 1) {
                continue;
            }
            const float z = a * c[2] + b * c[5] + (1 - a - b) * c[8];
            if (z <= 200 && (!best || z > *best)) {
                best = z;
            }
        }
        // The ground is left open over a pool, where the game's cameras find the ground's own height.
        const auto& size = scene.lotSize;
        if (!best && scene.floorHeight && x >= 0 && y >= 0 && x <= size[0] && y <= size[1]) {
            best = scene.floorHeight(1, x, y);
        }
        return best;
    }
};

} // namespace app
