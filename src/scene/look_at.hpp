#pragma once

#include "scene/scene.hpp"

#include <array>
#include <optional>
#include <vector>

namespace ts2::scene {

struct Rig;

// The head's look-at chain (head -> head_grip), as the game's look-at IK strategy solves it.
// head_grip is the eye point, offset from the head along the head's own Y.
struct HeadChain {
    int head = -1, grip = -1;
};
// The rig's head chain, or one with -1s when the rig has no head with a head_grip under it.
HeadChain headChain(const Rig& rig);

// Turns the head so that a line from head_grip along the head's +Z passes through `target`, its +X
// square to the plane of the pole, the target and the head, and its +Y leaning toward the pole.
// `target` and `pole` are in the rig's space. Only the head's local turn changes. False (nothing
// changed) when the target is too close to the head or in line with the pole.
bool aimHead(const Rig& rig, const HeadChain& chain, const std::array<float, 3>& target, const std::array<float, 3>& pole,
             std::vector<Transform>& local, const std::vector<Transform>& world);

// Of the rig as posed (`world`, one per node). Nullopt without a head chain with a neck above it.
std::optional<HeadPlace> headPlace(const Rig& rig, const HeadChain& chain, const std::vector<Transform>& world);

} // namespace ts2::scene
