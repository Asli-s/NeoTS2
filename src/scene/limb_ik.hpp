#pragma once

#include "formats/anim.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace ts2::scene {

struct Rig;

// A transform's turn as a quaternion (x, y, z, w).
std::array<float, 4> turnOf(const Transform& t);
// From turn `a` toward `b` by `t` (0 to 1), the short way round.
std::array<float, 4> turnToward(const std::array<float, 4>& a, const std::array<float, 4>& b, float t);

// A limb an animation reaches with IK, as the game's 7DOF strategy solves it: a leg (thigh, calf,
// foot) or an arm (upper arm, bicep, forearm, wrist, hand). Its target and pole are the animation's
// channels, which need not be skeleton joints.
// Where a limb reaches: channel indices into the animation (-1: none) and the rig node they are
// expressed in (a bone, or the skeleton's root).
struct LimbTarget {
    int position = -1, rotation = -1, extraRotation = -1;
    int weight = -1;  // blends this target with the limb's others, none: 1
    int frame = -1;
    bool onObject = false;  // on the clip's IK object (see LimbSource::objectFrame)
    std::uint32_t objectBone = 0;  // on that bone of the IK object (its name's hash), 0: on the object's own node
    // In a mirrored clip, the axes the position and turns are reflected across (see mirroredTurn),
    // -1: not reflected.
    int positionAxis = -1, rotationAxis = -1, extraRotationAxis = -1;
    // The axes a clip's own heading turns the position and the first turn about (see LimbSource::turn),
    // -1: none. The extra turn after it isn't turned, as in the game.
    int positionTurnAxis = -1, rotationTurnAxis = -1;
};

struct LimbChain {
    std::vector<int> joints;  // rig nodes from the limb's start to its end: 3 or 5
    std::vector<LimbTarget> targets;
    int pole = -1, poleFrame = -1;
    int poleAxis = -1, poleTurnAxis = -1;  // as LimbTarget::positionAxis and positionTurnAxis
    // How much the limb follows its targets rather than the animation's own turns of its joints
    // (a scalar channel, none: fully).
    int weight = -1;
};

// The animation's limb chains on this rig. Chains it can't solve are left out. Mirrored, each chain
// moves the limb on the other side and reaches for its targets reflected (see RigAnimation).
std::vector<LimbChain> bindLimbs(const Rig& rig, const anim::Animation& animation, bool mirrored = false);

// An animation whose limbs reach for its targets, at `ms`, showing with `weight` (as the layers mix
// it). Limbs with additive targets add to the others by `clipWeight`, the clip's own weight.
struct LimbSource {
    const anim::Animation* animation = nullptr;
    const std::vector<LimbChain>* limbs = nullptr;
    float ms = 0;
    float weight = 1;
    float clipWeight = 1;
    float turn = 0;  // radians the clip's targets are turned by (see LayeredClip::turn)
    // Per limb (as `limbs`), the weight it shows with instead of `weight`, empty: `weight` for all.
    std::vector<float> limbWeights;
    // Where the clip's IK object stands in the rig's space. Its targets on that object are placed
    // from there. None: they are in the skeleton's own space.
    std::optional<Transform> objectFrame;
    // Applied to its targets and poles in the skeleton's own space. For a clip whose root move is left
    // out (LayeredClip::noRootMove): the move from where the clip has its root to where the root is
    // drawn, as the game keeps a Sim's last pose whole when it moves the Sim (a long stair's climb at
    // the top). For a clip fading out after the Sim was placed anew: the move of the Sim's place
    // (LayeredClip::placeMove), so its limbs reach where they did (getting into bed).
    Transform rootMove;
    // Where a bone of the clip's IK object is in the rig's space, by its name's hash: a hug's hands go
    // to the other Sim's back. Targets on a bone it can't give are left out.
    std::function<std::optional<Transform>(std::uint32_t bone)> objectBone;
};

// Whether all the limb's target positions are additive channels (anim::Channel::additive).
bool isAdditive(const anim::Animation& a, const LimbChain& limb);

// A scalar channel's value at `ms` clamped to 0..1, 1 for none (-1): a limb or target weight.
float limbWeightAt(const anim::Animation& a, int channel, float ms);

// What a limb puts on one of its joints (index into LimbChain::joints) in the game's mix, per unit
// of its clip's weight. The joints that place the end take the limb weight times the sum of its
// targets' weights, and the limb weight for the pole. The end's orientation joints take the limb
// weight times the weights of its targets with an orientation, and nothing for the pole.
struct LimbFloor {
    float targets = 0, pole = 0;
};
LimbFloor limbFloor(const anim::Animation& a, const LimbChain& limb, std::size_t joint, float ms);

// One layer of a limb joint's mix in the game (each overlay clip counts as a layer of its own): the
// weight of its clips' turns of the joint, and what its limbs' targets and poles put on it.
struct LimbJointLayer {
    float turns = 0, targets = 0, pole = 0;
};
// The game's weighing of a limb joint, top layer first. A layer weighs its turns plus the larger of
// its targets and its pole, and leaves the rest to the layers below. Sets the factor on each layer's
// turn weights in `scales` (one per layer) and returns the IK weight, the solved turn's weight.
float weighLimbJoint(std::span<const LimbJointLayer> layers, std::span<float> scales);
// The share of the solved turn in a limb joint whose clips' turns weigh `turns` in all (after the
// scales of weighLimbJoint): none below the least weight that counts, all of it with no clip turns.
float solvedShare(float ikWeight, float turns);

// Turns each limb's joints so that its end reaches its target, bending toward its pole, and its end
// takes the target's orientation. `local` and `world` are the rig's pose, updated in place. A limb
// reaches for the weighted average of all its targets in all the animations, each weighted by how
// much its animation shows, as the game feeds them into one chain. `shares` gives, per rig node,
// the share of the solved turn in the joint (solvedShare), the rest staying with the turn it has in
// `local`. Null: the solved turns whole.
void solveLimbs(const Rig& rig, std::span<const LimbSource> sources, std::vector<Transform>& local, std::vector<Transform>& world,
                const std::vector<float>* shares = nullptr);

inline void solveLimbs(const Rig& rig, const anim::Animation& animation, const std::vector<LimbChain>& limbs, float ms,
                       std::vector<Transform>& local, std::vector<Transform>& world, const std::vector<float>* shares = nullptr) {
    const LimbSource one{&animation, &limbs, ms, 1, 1};
    solveLimbs(rig, std::span<const LimbSource>(&one, 1), local, world, shares);
}

} // namespace ts2::scene
