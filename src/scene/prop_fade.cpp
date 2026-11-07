#include "prop_fade.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace ts2::scene::propfade {

namespace {

// The game counts an alpha as 1, and draws the prop as it is, when it is within 32 steps of a
// float's precision of 1.
bool nearlyOne(float a) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(a);
    const std::uint32_t sign = bits >> 31;
    const std::uint32_t ordered = (((0u - sign) >> 1) ^ bits) + sign;
    const auto diff = static_cast<std::int32_t>(ordered - std::bit_cast<std::uint32_t>(1.0f));
    return (diff >> 5) == (diff >> 31);
}

bool inside(const Box& box, const std::array<float, 3>& eye) {
    return eye[0] >= box.low[0] && eye[0] <= box.high[0] && eye[1] >= box.low[1] && eye[1] <= box.high[1] && eye[2] >= box.low[2] &&
           eye[2] <= box.high[2];
}

// Along the ground from the camera to the box: nothing on an axis the camera is within the box on,
// else to the box's middle on that axis.
std::pair<float, float> alongGround(const Box& box, const std::array<float, 3>& eye) {
    const auto nearest = [&](std::size_t k) {
        const float middle = box.low[k] + (box.high[k] - box.low[k]) * 0.5f;
        return eye[k] > box.low[k] && eye[k] < box.high[k] ? eye[k] : middle;
    };
    return {nearest(0) - eye[0], nearest(1) - eye[1]};
}

// The camera's height under the box's top, squared and signed: negative when the camera is above it.
float overTop(const Box& box, const std::array<float, 3>& eye) {
    const float above = box.high[2] - eye[2];
    return std::fabs(above) * above;
}

// The alpha for a distance squared: 1 at the reach or beyond, else (distance over the Fade Distance,
// both squared) to the curve's power, 1 from the Fade Distance on.
float faded(float distance, float reach, float fadeSquared, double curve) {
    if (!(distance < reach)) {
        return 1.0f;
    }
    const float a = distance < fadeSquared ? static_cast<float>(std::pow(static_cast<double>(distance / fadeSquared), curve)) : 1.0f;
    return nearlyOne(a) ? 1.0f : a;
}

// For a prop whose top is below this height, the camera's height over the top counts too: the
// distance grows by its square when the camera is above the top and shrinks by it when below.
constexpr float kLowPropTop = 20.0f;
// A prop whose top is above this height fades wherever it stands.
constexpr float kTallPropTop = 15.0f;
// How many neighborhood tiles around the lot a lower prop's ground must reach to fade.
constexpr int kFadeTilesAroundLot = 3;
constexpr float kTilesPerUnit = 0.1f;
// A side of a ground rectangle less than 0.07 of a tile short of the next tile line is taken to that
// line: it is more than this far past the line before it.
constexpr float kPastLine = 0.93f;
// The fade's curves: alpha is the distance squared over the Fade Distance squared, to this power.
constexpr double kPropCurve = 4.0, kImposterCurve = 2.0;

} // namespace

std::optional<Box> placedBox(const Mesh& mesh, const Transform& place) {
    constexpr float kHuge = std::numeric_limits<float>::max();
    std::array<float, 3> low{kHuge, kHuge, kHuge}, high{-kHuge, -kHuge, -kHuge};
    bool any = false;
    for (const auto& p : mesh.primitives) {
        for (std::size_t i = 0; i + 2 < p.positions.size(); i += 3) {
            for (std::size_t k = 0; k < 3; ++k) {
                const float v = p.positions[i + k];
                if (!std::isfinite(v)) {
                    continue;
                }
                low[k] = std::min(low[k], v);
                high[k] = std::max(high[k], v);
                any = true;
            }
        }
    }
    if (!any || low[0] > high[0] || low[1] > high[1] || low[2] > high[2]) {
        return std::nullopt;
    }
    std::array<float, 3> centre{}, half{};
    for (std::size_t k = 0; k < 3; ++k) {
        centre[k] = (low[k] + high[k]) * 0.5f;
        half[k] = (high[k] - low[k]) * 0.5f;
    }
    place.apply(centre.data(), true);
    Box out;
    for (std::size_t i = 0; i < 3; ++i) {
        float reachOut = 0;
        for (std::size_t k = 0; k < 3; ++k) {
            reachOut += std::fabs(place.m[i * 4 + k]) * half[k];
        }
        out.low[i] = centre[i] - reachOut;
        out.high[i] = centre[i] + reachOut;
    }
    return out;
}

std::array<int, 4> groundTiles(const std::array<float, 4>& ground) {
    const auto line = [](float units, bool low) {
        // Large enough for any neighborhood, and keeps a broken value from overflowing.
        constexpr double kLimit = 1 << 20;
        const double v = std::clamp(static_cast<double>(units * kTilesPerUnit), -kLimit, kLimit);
        if (!std::isfinite(v)) {
            return 0;
        }
        const double edge = low ? std::floor(v) : std::ceil(v);
        const double past = low ? v - edge : edge - v;
        const double shifted = past > static_cast<double>(kPastLine) ? (low ? edge + 1 : edge - 1) : edge;
        return static_cast<int>(shifted);
    };
    return {line(ground[0], true), line(ground[1], true), line(ground[2], false), line(ground[3], false)};
}

bool fadesInLotView(const Box& placed, const std::array<int, 4>& ground, const std::array<int, 4>& lotTiles) {
    if (placed.high[2] > kTallPropTop) {
        return true;
    }
    const int x0 = lotTiles[0] - kFadeTilesAroundLot, y0 = lotTiles[1] - kFadeTilesAroundLot;
    const int x1 = lotTiles[2] + kFadeTilesAroundLot, y1 = lotTiles[3] + kFadeTilesAroundLot;
    return ground[2] >= x0 && ground[0] <= x1 && ground[3] >= y0 && ground[1] <= y1;
}

std::optional<float> reach(const std::array<float, 3>& eye, const std::array<float, 2>& lotSize) {
    if (eye[0] >= 0 && eye[0] <= lotSize[0] && eye[1] >= 0 && eye[1] <= lotSize[1]) {
        return std::nullopt;
    }
    const float x = eye[0] > 0 && eye[0] < lotSize[0] ? eye[0] : lotSize[0] * 0.5f;
    const float y = eye[1] > 0 && eye[1] < lotSize[1] ? eye[1] : lotSize[1] * 0.5f;
    return (x - eye[0]) * (x - eye[0]) + (y - eye[1]) * (y - eye[1]);
}

std::optional<float> alpha(const Box& prop, const std::array<float, 3>& eye, float reach, float fadeSquared) {
    const float top = prop.high[2];
    if (top < 0) {
        return std::nullopt;
    }
    float distance = 0;
    if (!inside(prop, eye)) {
        const auto [dx, dy] = alongGround(prop, eye);
        distance = dx * dx + dy * dy;
        if (top < kLowPropTop) {
            distance -= overTop(prop, eye);
        }
    }
    return faded(distance, reach, fadeSquared, kPropCurve);
}

float imposterAlpha(const Box& imposter, const std::array<float, 3>& eye, float reach, float fadeSquared) {
    float distance = 0;
    if (!inside(imposter, eye)) {
        const auto [dx, dy] = alongGround(imposter, eye);
        distance = dx * dx + dy * dy + eye[2] * eye[2] - overTop(imposter, eye);
    }
    return faded(distance, reach, fadeSquared, kImposterCurve);
}

} // namespace ts2::scene::propfade
