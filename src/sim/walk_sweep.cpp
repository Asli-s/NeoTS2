#include "sim/walk_sweep.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::sim {

int sweepStepMs(float tilesPerSecond) {
    constexpr float kStillSpeed = 0.01f;
    constexpr int kStillStepMs = 200;
    constexpr float kStepTiles = 0.05f;  // a sweep step is the time to walk this far
    if (!(tilesPerSecond > kStillSpeed)) {
        return kStillStepMs;
    }
    // Above 0.01 tiles a second this is at most 5000 ms, so it fits an int.
    return std::max(1, static_cast<int>(kStepTiles * 1000.0f / tilesPerSecond));
}

float walkTopSpeed(const WalkPlan& plan) {
    float top = 0;
    for (const SpeedPart& part : plan.speed) {
        top = std::max({top, part.from, part.to});
    }
    return std::isfinite(top) ? top * 1000.0f : 0.0f;
}

std::optional<float> pointsMeet(float p0x, float p0y, float p1x, float p1y, float q0x, float q0y, float q1x, float q1y, float r) {
    const float sx = p0x - q0x, sy = p0y - q0y;
    const float vx = (p1x - q1x) - sx, vy = (p1y - q1y) - sy;
    const float c = sx * sx + sy * sy - r * r;
    if (!std::isfinite(c)) {
        return std::nullopt;
    }
    if (c <= 0) {
        return 0.0f;
    }
    const float a = vx * vx + vy * vy, b = 2 * (sx * vx + sy * vy);
    const float disc = b * b - 4 * a * c;
    if (!(a > 0) || disc < 0) {
        return std::nullopt;
    }
    const float f = (-b - std::sqrt(disc)) / (2 * a);
    return f >= 0 && f <= 1 ? std::optional<float>(f) : std::nullopt;
}

std::optional<float> sweepContact(const SweepPlace& from, const SweepPlace& to, const SweepPlace& otherFrom, const SweepPlace& otherTo) {
    const auto centres = pointsMeet(from.x, from.y, to.x, to.y, otherFrom.x, otherFrom.y, otherTo.x, otherTo.y, kSweepCentres);
    const auto front = pointsMeet(from.x + from.dx * kSweepFront, from.y + from.dy * kSweepFront, to.x + to.dx * kSweepFront,
                                  to.y + to.dy * kSweepFront, otherFrom.x, otherFrom.y, otherTo.x, otherTo.y, kSweepFrontToCentre);
    if (centres && front) {
        return std::min(*centres, *front);
    }
    return centres ? centres : front;
}

} // namespace ts2::sim
