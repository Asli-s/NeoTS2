#pragma once

#include "sim/walk_plan.hpp"

#include <optional>

namespace ts2::sim {

// Besides testing each step's place, the game looks ahead along a walking Sim's route for other
// Sims: from the last check to about ten sweep steps past now, both Sims move in straight lines
// between their places at the ends of each sweep step, and the first contact blocks the walk.

// A Sim's place at one moment of a sweep: its centre in tiles and the unit vector of the way it
// walks (0, 0 when it stands).
struct SweepPlace {
    float x = 0, y = 0;
    float dx = 0, dy = 0;
};

// How far the walker's front point lies ahead of its centre, and the contact distances: centre to
// centre, and the front point to the other's centre (for ordinary Sims, pets differ).
inline constexpr float kSweepFront = 0.35f;
inline constexpr float kSweepCentres = 0.3165f;
inline constexpr float kSweepFrontToCentre = 0.204f;
// The window reaches this many sweep steps past now.
inline constexpr float kSweepSteps = 10.02f;
// The most sweep steps looked at in one check. The window gives about eleven.
inline constexpr int kMaxSweepSteps = 32;

// The length of one sweep step in ms for a walk speed in tiles per second: the time to walk 0.05
// tiles, at least 1 ms. It is 200 ms at 0.01 tiles a second or less.
int sweepStepMs(float tilesPerSecond);

// The walk's top speed in tiles per second.
float walkTopSpeed(const WalkPlan& plan);

// The first share (0 to 1) of a sweep step at which two points moving in straight lines, from p0
// to p1 and from q0 to q1, come within `r` of each other, none when they don't.
std::optional<float> pointsMeet(float p0x, float p0y, float p1x, float p1y, float q0x, float q0y, float q1x, float q1y, float r);

// The first share of a sweep step at which the walker, moving from `from` to `to`, touches another
// Sim moving from `otherFrom` to `otherTo`: centres within 0.3165 tiles, or the walker's point
// 0.35 ahead within 0.204 of the other's centre. None when they don't touch.
std::optional<float> sweepContact(const SweepPlace& from, const SweepPlace& to, const SweepPlace& otherFrom, const SweepPlace& otherTo);

} // namespace ts2::sim
