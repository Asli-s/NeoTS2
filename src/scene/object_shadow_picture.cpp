#include "scene/object_shadow_picture.hpp"

#include "diag/log.hpp"
#include "scene/object_lights.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace ts2::scene::objectshadow {

namespace {

using Vec = std::array<float, 3>;

float dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The picture's axes for a sun line, as the Sims' pictures have them: across, up (as near the
// world's up as the sun allows) and along the light.
struct Axes {
    Vec right, up, forward;
};

std::optional<Axes> axesFor(const Vec& toSun) {
    const float length = std::sqrt(dot(toSun, toSun));
    if (!(length > 1e-6f) || !std::isfinite(length)) {
        return std::nullopt;
    }
    const Vec d{toSun[0] / length, toSun[1] / length, toSun[2] / length};
    // Under the horizon or straight above, no ground shadow is drawn.
    if (!(d[2] > 1e-3f) || d[2] > 1 - 1e-6f) {
        return std::nullopt;
    }
    Axes a;
    a.forward = {-d[0], -d[1], -d[2]};
    Vec up{d[2] * a.forward[0], d[2] * a.forward[1], 1 + d[2] * a.forward[2]};
    const float u = std::sqrt(dot(up, up));
    a.up = {up[0] / u, up[1] / u, up[2] / u};
    a.right = {a.forward[1] * a.up[2] - a.forward[2] * a.up[1], a.forward[2] * a.up[0] - a.forward[0] * a.up[2],
               a.forward[0] * a.up[1] - a.forward[1] * a.up[0]};
    return a;
}

constexpr float kMargin = 0.5f;    // spare room across and up, either side
constexpr float kSmallest = 0.15f;  // models smaller than this in every direction cast none
// Broken model or placement data can give boxes and places far past any lot. They cast none, so the
// renderer never meets a size it can't count in tiles.
constexpr float kLargest = 1e4f;
constexpr float kFarthest = 1e6f;
constexpr int kMaxPictures = 256;   // more models and facings than any lot shows
constexpr int kMaxPlaces = 1024;

// The box around a mesh's vertices, as the box around that box turned by a facing (the same box
// for quarter turns, a bigger one between them).
bool turnedBox(const Mesh& mesh, int facing, Vec& low, Vec& high) {
    Vec lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec hi{-lo[0], -lo[1], -lo[2]};
    bool any = false;
    for (const auto& p : mesh.primitives) {
        for (std::size_t i = 0; i + 2 < p.positions.size(); i += 3) {
            const Vec v{p.positions[i], p.positions[i + 1], p.positions[i + 2]};
            if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) {
                continue;
            }
            for (std::size_t k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], v[k]);
                hi[k] = std::max(hi[k], v[k]);
            }
            any = true;
        }
    }
    if (!any) {
        return false;
    }
    const double a = facing * 3.14159265358979323846 / 4;
    const float c = static_cast<float>(std::cos(a)), s = static_cast<float>(std::sin(a));
    const float cx = (lo[0] + hi[0]) / 2, cy = (lo[1] + hi[1]) / 2, hx = (hi[0] - lo[0]) / 2, hy = (hi[1] - lo[1]) / 2;
    const float x = c * cx - s * cy, y = s * cx + c * cy;
    const float ex = std::abs(c) * hx + std::abs(s) * hy, ey = std::abs(s) * hx + std::abs(c) * hy;
    low = {x - ex, y - ey, lo[2]};
    high = {x + ex, y + ey, hi[2]};
    return true;
}

// A row of the camera: the axis scaled, with its offset.
void setRow(std::array<float, 16>& m, int row, const Vec& axis, float scale, float offset) {
    m[static_cast<std::size_t>(row * 4 + 0)] = axis[0] * scale;
    m[static_cast<std::size_t>(row * 4 + 1)] = axis[1] * scale;
    m[static_cast<std::size_t>(row * 4 + 2)] = axis[2] * scale;
    m[static_cast<std::size_t>(row * 4 + 3)] = offset;
}

} // namespace

std::optional<Picture> picture(const Mesh& mesh, int meshIndex, int facing, const Vec& toSun) {
    const auto axes = axesFor(toSun);
    Vec low, high;
    if (!axes || !turnedBox(mesh, facing, low, high)) {
        return std::nullopt;
    }
    const Vec turnedLow = low, turnedHigh = high;
    // The game's box never reaches below the ground.
    low[2] = std::max(low[2], 0.0f);
    const float extent = std::max({high[0] - low[0], high[1] - low[1], high[2] - low[2]});
    if (!(extent >= kSmallest) || !(extent <= kLargest)) {
        return std::nullopt;
    }
    // The box's corners along each of the picture's axes.
    float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    float hi[3] = {-lo[0], -lo[1], -lo[2]};
    for (int k = 0; k < 8; ++k) {
        const Vec corner{(k & 1) ? high[0] : low[0], (k & 2) ? high[1] : low[1], (k & 4) ? high[2] : low[2]};
        const float along[3] = {dot(axes->right, corner), dot(axes->up, corner), dot(axes->forward, corner)};
        for (int j = 0; j < 3; ++j) {
            lo[j] = std::min(lo[j], along[j]);
            hi[j] = std::max(hi[j], along[j]);
        }
    }
    const float across = hi[0] - lo[0], depth = hi[2] - lo[2];
    if (!(across > 1e-4f) || !(depth > 1e-4f)) {
        return std::nullopt;
    }
    Picture p;
    p.mesh = meshIndex;
    p.facing = facing;
    p.low = turnedLow;
    p.high = turnedHigh;
    const float width = across + 2 * kMargin;
    const float bottom = lo[1] - kMargin, top = hi[1] + kMargin;
    // The game hands its camera the depth over the width as the picture's shape, so the picture's
    // height comes from those, not from the box's height.
    setRow(p.camera, 0, axes->right, 2 / width, -(lo[0] + hi[0]) / width);
    setRow(p.camera, 1, axes->up, 2 * across / (width * depth), -(bottom + top) / (top - bottom));
    setRow(p.camera, 2, axes->forward, 1 / depth, -lo[2] / depth);
    p.camera[15] = 1;
    p.size = extent < 0.5f ? 32 : extent < 1.5f ? 64 : 128;
    return p;
}

Layout layout(const Scene& scene, const Vec& toSun, const Transform& place) {
    Layout out;
    const auto axes = axesFor(toSun);
    if (!axes) {
        return out;
    }
    const Transform toLot = place.rigidInverse();
    std::map<std::pair<int, int>, std::optional<std::size_t>> byModel;
    for (const auto& instance : scene.instances) {
        if (instance.shadow != Kind::Sun || instance.shadowMesh < 0 || static_cast<std::size_t>(instance.shadowMesh) >= scene.meshes.size()) {
            continue;
        }
        const auto key = std::make_pair(instance.shadowMesh, static_cast<int>(instance.shadowFacing));
        auto found = byModel.find(key);
        if (found == byModel.end()) {
            std::optional<std::size_t> index;
            if (out.pictures.size() < kMaxPictures) {
                if (auto p = picture(scene.meshes[static_cast<std::size_t>(instance.shadowMesh)], instance.shadowMesh, instance.shadowFacing, toSun)) {
                    index = out.pictures.size();
                    out.pictures.push_back(*p);
                }
            } else {
                diag::warnOnce("object shadows: models past 256 cast no shadow");
            }
            found = byModel.emplace(key, index).first;
        }
        if (!found->second) {
            continue;
        }
        if (out.places.size() >= kMaxPlaces) {
            diag::warnOnce("object shadows: objects past 1024 cast no shadow");
            break;
        }
        const Picture& pic = out.pictures[*found->second];
        const Transform inLot = toLot * instance.transform;
        const auto& t = inLot.m;
        const Vec at{t[3], t[7], t[11]};
        Place laid;
        laid.picture = *found->second;
        laid.camera = pic.camera;
        for (int row = 0; row < 3; ++row) {
            const auto r = static_cast<std::size_t>(row * 4);
            laid.camera[r + 3] -= pic.camera[r] * at[0] + pic.camera[r + 1] * at[1] + pic.camera[r + 2] * at[2];
        }
        // Where the picture's box reaches the plane the object stands on, along the light, widened
        // toward the sun to the object's own box.
        float rect[4] = {std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                         -std::numeric_limits<float>::max()};
        const auto& m = pic.camera;
        const float sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]), sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
        const float sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
        for (int k = 0; k < 8; ++k) {
            const float cx = (k & 1) ? 1.0f : -1.0f, cy = (k & 2) ? 1.0f : -1.0f, cz = (k & 4) ? 1.0f : 0.0f;
            const float a = (cx - m[3]) / sx, b = (cy - m[7]) / sy, c = (cz - m[11]) / sz;
            Vec p{axes->right[0] * a + axes->up[0] * b + axes->forward[0] * c, axes->right[1] * a + axes->up[1] * b + axes->forward[1] * c,
                  axes->right[2] * a + axes->up[2] * b + axes->forward[2] * c};
            const float s = -p[2] / axes->forward[2];
            p = {p[0] + axes->forward[0] * s, p[1] + axes->forward[1] * s, 0};
            rect[0] = std::min(rect[0], p[0]);
            rect[1] = std::max(rect[1], p[0]);
            rect[2] = std::min(rect[2], p[1]);
            rect[3] = std::max(rect[3], p[1]);
        }
        {
            const float dx = -axes->forward[0], dy = -axes->forward[1];
            if (dx >= 0) rect[1] = std::max(rect[1], pic.high[0]); else rect[0] = std::min(rect[0], pic.low[0]);
            if (dy >= 0) rect[3] = std::max(rect[3], pic.high[1]); else rect[2] = std::min(rect[2], pic.low[1]);
        }
        // Into the scene's frame: the camera reads scene points taken back to the lot, and the rect
        // is the box around its corners carried over.
        const auto& b = toLot.m;
        for (int row = 0; row < 3; ++row) {
            const auto r = static_cast<std::size_t>(row * 4);
            const std::array<float, 4> c{laid.camera[r], laid.camera[r + 1], laid.camera[r + 2], laid.camera[r + 3]};
            for (std::size_t k = 0; k < 3; ++k) {
                laid.camera[r + k] = c[0] * b[k] + c[1] * b[4 + k] + c[2] * b[8 + k];
            }
            laid.camera[r + 3] = c[0] * b[3] + c[1] * b[7] + c[2] * b[11] + c[3];
        }
        float spanLow[2] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        float spanHigh[2] = {-spanLow[0], -spanLow[1]};
        for (int k = 0; k < 4; ++k) {
            float corner[3] = {(k & 1) ? rect[1] + at[0] : rect[0] + at[0], (k & 2) ? rect[3] + at[1] : rect[2] + at[1], 0};
            place.apply(corner, true);
            spanLow[0] = std::min(spanLow[0], corner[0]);
            spanHigh[0] = std::max(spanHigh[0], corner[0]);
            spanLow[1] = std::min(spanLow[1], corner[1]);
            spanHigh[1] = std::max(spanHigh[1], corner[1]);
        }
        laid.rect = {spanLow[0], spanHigh[0], spanLow[1], spanHigh[1]};
        const auto near = [](float v) { return std::abs(v) <= kFarthest; };
        if (!std::ranges::all_of(laid.rect, near) || !std::ranges::all_of(laid.camera, near)) {
            diag::warnOnce("object shadows: an object too far away or with a broken model casts none");
            continue;
        }
        const int storey = objectlights::lightStorey(scene.floorHeight, inLot, instance.level - scene.groundLevel);
        laid.level = storey + scene.groundLevel;
        out.places.push_back(laid);
    }
    return out;
}

} // namespace ts2::scene::objectshadow
