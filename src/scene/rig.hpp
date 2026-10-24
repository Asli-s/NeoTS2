#pragma once

#include "formats/anim.hpp"
#include "scene/limb_ik.hpp"
#include "scene/scene.hpp"

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <span>
#include <vector>

namespace ts2::scene {

class Assets;

// A skeleton ready to be posed: its nodes parents first, each with its rest place relative to its
// parent. Joints are the named transform nodes (lowercase names).
struct Rig {
    struct Node {
        std::string name;  // empty for nodes that aren't joints
        int parent = -1;
        std::array<float, 3> translation{};
        std::array<float, 4> rotation{0, 0, 0, 1};
        // The skeleton's extra rotation for the joint (its bone data), put before what an animation
        // gives the joint. Identity for most joints.
        std::array<float, 4> animationTurn{0, 0, 0, 1};
        // The axis (0 x, 1 y, 2 z) a mirrored clip reflects the joint across, from its bone data. X
        // without bone data, -1 when the file's axis is out of range.
        int mirrorAxis = 0;
        // The axis (0 x, 1 y, 2 z) a clip's own heading turns the joint about (see turnedPlace), -1 for
        // none, as for joints without bone data. Only the root and the limb controls have one.
        int turnAxis = -1;
    };
    std::vector<Node> nodes;
    float stretch = 1;  // how much longer its bones are than the skeleton's own (see stretched)

    // The first node with this (lowercase) name, -1 when there is none.
    int find(const std::string& name) const;
};

// A skeleton's rig, from its model (e.g. "auskel"). Empty when the model is missing or unreadable.
Rig loadRig(Assets& assets, const std::string& skeleton);

// The rig with every joint's rest translation times `stretch`, rotations kept: how the game sizes a
// Sim's skeleton by its age data's stretch (teens are the adult skeleton at 0.94).
Rig stretched(Rig rig, float stretch);

// Fits a clip made for one body size to `rig`'s (stretched) skeleton. A joint's translation moves
// by its rest place's change between the two sizes (rest x (rig stretch - clip stretch)), so the
// clip's moves keep their size: a teen's hips stand lower by as much as their rest pose does, and
// a teen climbing a straight stair still rises a full storey, as in the game. Additive channels and
// channels of joints the rig lacks are scaled by the stretch over the clip's instead.
// Teens play adult clips this way.
void fitToStretch(anim::Animation& clip, const Rig& rig);

// An animation bound to a rig: per node, the channel that rotates it and the one that moves it
// (-1 for none, the last when an animation has more than one), and the limbs it reaches with IK.
// A mirrored animation plays on the other side of the body: each channel of a joint named "l_..."
// moves the "r_..." joint and the other way round, and every value is reflected across the axis of
// the joint it moves (see mirroredTurn). Turns one way are the clips of the other way, mirrored.
struct RigAnimation {
    std::shared_ptr<const anim::Animation> animation;
    std::vector<int> rotation, translation;
    std::vector<LimbChain> limbs;
    bool mirrored = false;
};
RigAnimation bindAnimation(const Rig& rig, std::shared_ptr<const anim::Animation> animation, bool mirrored = false);

// The joint a channel moves when its clip is played mirrored: "l_" and "r_" at the start of the
// name swapped (case matters), any other name unchanged.
std::string mirroredJointName(const std::string& name);
// A joint's turn as a mirrored clip gives it: the part about the joint's mirror axis kept, the
// other two negated. No axis (-1): unchanged.
std::array<float, 4> mirroredTurn(std::array<float, 4> q, int axis);
// A joint's place as a mirrored clip gives it: negated along the joint's mirror axis.
std::array<float, 3> mirroredPlace(std::array<float, 3> t, int axis);
// A joint's turn and place turned `angle` radians about its turn axis (-1: unchanged): how the game
// sets a locomotion clip's root and limb controls on the heading the clip plays from.
std::array<float, 4> turnedTurn(const std::array<float, 4>& q, int axis, float angle);
std::array<float, 3> turnedPlace(const std::array<float, 3>& t, int axis, float angle);

// The rotation `b` then `a` (the quaternion product a * b, as x, y, z, w).
std::array<float, 4> turnProduct(const std::array<float, 4>& a, const std::array<float, 4>& b);
// The rotation `q` turned only `share` of its angle (the game's weighting of additive turns).
std::array<float, 4> partialTurn(const std::array<float, 4>& q, float share);
// The rotation of Euler angles in degrees, applied x first, then y, then z (q = qz * qy * qx).
std::array<float, 4> eulerRotation(float x, float y, float z);
// The rotation an animation gives a joint from its Euler angles: the joint's animation turn, then
// the angles' rotation (the game multiplies the turn in front).
std::array<float, 4> animatedRotation(const Rig::Node& node, float x, float y, float z);
// A rotation channel's turn at `ms`, as the game samples it: the Euler angles at the 30-a-second frames
// either side, each made a rotation, and the turn slerped between them the short way (the angles
// themselves are never wrapped, so keys crossing 180 degrees don't spin).
std::array<float, 4> channelTurn(const anim::Channel& channel, float ms);
// animatedRotation of a rotation channel at `ms`.
std::array<float, 4> animatedRotation(const Rig::Node& node, const anim::Channel& channel, float ms);

// The places of all of the rig's nodes from their places relative to their parents.
void placeRig(const Rig& rig, const std::vector<Transform>& local, std::vector<Transform>& world);

// Where each of the rig's nodes is at `ms` into the animation, in the model's space. At rest
// without one.
void poseRig(const Rig& rig, const RigAnimation* animation, float ms, std::vector<Transform>& world);

// The joint matrices a skinned mesh is drawn with (see Mesh::skin), for a rig posed by `animation`
// (null: at rest). `nodes` maps each of the mesh's skin entries to a rig node (-1: the entry's joint
// isn't in the rig and stays at rest). A joint the animation doesn't move keeps the mesh's own rest
// place under its posed parent.
void skinMatrices(const Mesh& mesh, const std::vector<int>& nodes, const Rig& rig, const RigAnimation* animation,
                  const std::vector<Transform>& world, std::vector<Transform>& out);
// The same for a rig posed by several animations at once.
void skinMatrices(const Mesh& mesh, const std::vector<int>& nodes, const Rig& rig, std::span<const RigAnimation* const> animations,
                  const std::vector<Transform>& world, std::vector<Transform>& out);

// Each skin entry of a mesh as a node of the rig (see skinMatrices).
std::vector<int> skinNodes(const Mesh& mesh, const Rig& rig);

// A joint's local place and turn as last posed.
struct HeldJoint {
    std::array<float, 3> translation{};
    std::array<float, 4> rotation{0, 0, 0, 1};
};

// Poses every actor of a scene at `seconds`, each animation looping from its start: the joint
// matrices of each skinned instance and the place of each skinned and held instance, by instance,
// and each actor's rig nodes in its model's space, by actor.

struct ScenePose {
    std::vector<std::pair<std::size_t, std::vector<Transform>>> joints;
    std::vector<std::pair<std::size_t, Transform>> places;
    std::vector<std::pair<std::size_t, std::vector<Transform>>> nodes;
    // Each lot object's actor's joints as last posed, which joints no clip moves keep (see
    // poseRigLayered). Kept between calls.
    std::unordered_map<std::uint32_t, std::vector<HeldJoint>> held;
    // The heading (radians) each lot object's actor was placed at as it was last posed.
    std::unordered_map<std::uint32_t, float> heldHeading;
};
void poseActors(const Scene& scene, double seconds, ScenePose& out);

} // namespace ts2::scene
