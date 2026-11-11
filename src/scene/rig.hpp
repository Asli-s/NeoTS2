#pragma once

#include "formats/anim.hpp"
#include "formats/scenegraph.hpp"
#include "scene/limb_ik.hpp"
#include "scene/scene.hpp"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <span>
#include <vector>

namespace ts2::scene {

class Assets;

// A skeleton ready to be posed: its nodes parents first, each with its rest place relative to its
// parent. Joints are the named transform and shape nodes (lowercase names).
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
        // How the body stretch sizes the joint's rest translation, from its bone data: one axis
        // (0 x, 1 y, 2 z), kStretchAll or kStretchNone. Y without bone data.
        int stretchAxis = 1;
        // How much of the stretch the joint takes (1 all of it). The neck and head take a quarter.
        float stretchWeight = 1;
    };
    static constexpr int kStretchNone = 3, kStretchAll = 4;
    std::vector<Node> nodes;
    float stretch = 1;  // how much longer its bones are than the skeleton's own (see stretched)

    // The first node with this (lowercase) name, -1 when there is none.
    int find(const std::string& name) const;
};

// A skeleton's rig, from its model (e.g. "auskel"). Empty when the model is missing or unreadable.
Rig loadRig(Assets& assets, const std::string& skeleton);

// The rig sized to a body stretch of `stretch`, rotations kept: how the game sizes a Sim's skeleton
// by its age data's stretch (teens are the adult skeleton at 0.94). Each joint's rest translation is
// scaled on its stretch axis by its weighted share of the change from the rig's present stretch:
// ((stretch - 1) x weight + 1) / ((present - 1) x weight + 1).
Rig stretched(Rig rig, float stretch);

// One entry of a model's "bone_adjust" lists: the rest place a worn part gives a skeleton joint
// (its surface targets, grips or hair joints), each value only when the entry has it.
struct JointAdjust {
    std::string joint;  // the joint's name, matched exactly
    std::optional<std::array<float, 3>> translation;
    std::optional<std::array<float, 4>> rotation;  // quaternion x, y, z, w
};

// The model's bone_adjust entries, in file order. Entries without a joint name or with values that
// aren't finite numbers are left out.
std::vector<JointAdjust> jointAdjusts(const std::vector<scenegraph::DataValue>& dataLists);

// Sets each entry's values on the first joint of `rig` with its name. Entries naming no joint of
// the rig change nothing. Later entries win, so parts are applied in the order the game attaches
// them.
void adjustJoints(Rig& rig, std::span<const JointAdjust> adjusts);

// Gives the upper arms ("l_upperarm", "r_upperarm") the y stretch axis. The game does this for teen
// humans before stretching, so their arms shorten with the body although the skeleton's bone data
// leaves them unstretched.
void stretchTeenArms(Rig& rig);

// A joint's rest translation when its rig, now at `rig.stretch`, is sized to `stretch` instead.
std::array<float, 3> stretchedPlace(const Rig& rig, const Rig::Node& node, float stretch);

// How a clip made for one body size moves a joint of another skeleton: the game fits each sampled
// translation on the joint's stretch axis by factor = skeleton stretch / ((clip stretch - 1) x weight + 1).
// A value of `height`'s sign moves by (factor - 1) x height, any other is scaled. Only root_trans has a
// height (its clip's root height), so a teen's root stands equally lower all through a climb. Joints
// marked unstretched, or stretched on all three axes, are left as the clip has them.
struct PlaceFit {
    int axis = -1;  // -1: the joint's places are kept
    float factor = 1;
    float height = -1;
    // root_trans only: the clip's weight channel for it (0-1), which takes the fit back as it rises.
    // At 1 the root keeps the clip's place: a seated teen sits as high as the clip has it.
    const anim::Curve* weight = nullptr;
};
std::array<float, 3> fittedPlace(const PlaceFit& fit, std::array<float, 3> t, float ms);

// An animation bound to a rig: per node, the channel that rotates it and the one that moves it
// (-1 for none, the last when an animation has more than one), and the limbs it reaches with IK.
// A mirrored animation plays on the other side of the body: each channel of a joint named "l_..."
// moves the "r_..." joint and the other way round, and every value is reflected across the axis of
// the joint it moves (see mirroredTurn). Turns one way are the clips of the other way, mirrored.
struct RigAnimation {
    std::shared_ptr<const anim::Animation> animation;
    std::vector<int> rotation, translation;
    std::vector<PlaceFit> placeFits;  // per node, for its translation channel (see PlaceFit)
    std::vector<LimbChain> limbs;
    // Per IK chain of the animation (anim::Animation::chains), the scalar channel giving its limb
    // weight (IkChain::extra), -1 for none.
    std::vector<int> chainWeights;
    bool mirrored = false;
};
RigAnimation bindAnimation(const Rig& rig, std::shared_ptr<const anim::Animation> animation, bool mirrored = false);
// The fit of node `node`'s translation channel in `animation`, none when it has no fit for it.
PlaceFit placeFitAt(const RigAnimation& animation, std::size_t node);

// The factor on a clip's weight for one of its joint channels at `ms`. A channel tagged with an IK
// chain (anim::Channel::chain) gives way to the chain's solve: 1 minus the chain's limb weight, or
// negative (the channel isn't used at all) when the chain has no limb weight channel. 1 otherwise.
float channelShare(const RigAnimation& animation, int channel, float ms);

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

struct HeldJoint;
// Where each of the rig's nodes is at `ms` into the animation, in the model's space. At rest
// without one. With `held` (one per node), the nodes the animation doesn't move keep those instead.
void poseRig(const Rig& rig, const RigAnimation* animation, float ms, std::vector<Transform>& world,
             const std::vector<HeldJoint>* held = nullptr);

// The joint matrices a skinned mesh is drawn with (see Mesh::skin), for a rig posed by `animation`
// (null: at rest). `nodes` maps each of the mesh's skin entries to a rig node (-1: the entry's joint
// isn't in the rig and stays at rest). A joint the animation doesn't move keeps the mesh's own rest
// place under its posed parent.
void skinMatrices(const Mesh& mesh, const std::vector<int>& nodes, const Rig& rig, const RigAnimation* animation,
                  const std::vector<Transform>& world, std::vector<Transform>& out);
// The same for a rig posed by several animations at once. With `ownRig` (a lot object's rig, made
// from the mesh's own model), every joint is placed as the rig is posed.
void skinMatrices(const Mesh& mesh, const std::vector<int>& nodes, const Rig& rig, std::span<const RigAnimation* const> animations,
                  const std::vector<Transform>& world, std::vector<Transform>& out, bool ownRig = false);

// Each skin entry of a mesh as a node of the rig (see skinMatrices).
std::vector<int> skinNodes(const Mesh& mesh, const Rig& rig);

// Whether a primitive has four joints and four weights per vertex.
bool isSkinned(const Primitive& p);
// A skinned primitive's positions as `joints` pose them, as the skinned vertex shader does, into
// `out` (all 0 for a primitive that isn't skinned).
void posedPositions(const Primitive& p, const std::vector<Transform>& joints, std::vector<float>& out);

// A joint's local place and turn as last posed.
struct HeldJoint {
    std::array<float, 3> translation{};
    std::array<float, 4> rotation{0, 0, 0, 1};
};

// Which morph of a mesh each morph channel of a clip weighs (see mixMorphWeights), found once per
// mesh (by its index in the scene) and clip. Each entry holds its clip, so its key stays its own,
// and the mesh's morphs it was found for, so it is found again for another mesh at that index.
struct MorphBindings {
    struct Binding {
        std::uint16_t target = 0, channel = 0;
        std::uint8_t morph = 0;  // an index into Mesh::morphs
    };
    struct Entry {
        std::shared_ptr<const anim::Animation> animation;
        std::vector<MorphName> morphs;
        std::vector<Binding> bound;
    };
    std::map<std::pair<int, const anim::Animation*>, Entry> entries;
};

// Poses every actor of a scene at `seconds`, each animation looping from its start: the joint
// matrices of each skinned instance and the place of each skinned and held instance, by instance,
// and each Sim actor's rig nodes in its model's space, by actor.

struct ScenePose {
    std::vector<std::pair<std::size_t, std::vector<Transform>>> joints;
    std::vector<std::pair<std::size_t, Transform>> places;
    std::vector<std::pair<std::size_t, std::vector<Transform>>> nodes;
    // Each lot object's actor's joints as last posed, which joints no clip moves keep (see
    // poseRigLayered). Kept between calls.
    std::unordered_map<std::uint32_t, std::vector<HeldJoint>> held;
    // Each lot object's actor's frozen clip (see Actor::Clip::frozen): its start (seconds) and the
    // pose it holds. Kept between calls.
    std::unordered_map<std::uint32_t, std::pair<double, std::vector<HeldJoint>>> frozen;
    // Where each carried Sim's actor was placed as last posed (see Actor::carrier), by its object.
    std::unordered_map<std::uint32_t, Transform> carried;
    // Where each Sim was placed and each held object hung as last posed, by object.
    std::unordered_map<std::uint32_t, Transform> drawnAt;
    // The rig each actor was last posed with, by object, so a pose kept from another rig is dropped.
    std::unordered_map<std::uint32_t, const Rig*> heldRig;
    // Each skinned instance's morph weights (by Mesh::morphs), by instance. Kept between calls, as a
    // morph no clip weighs keeps its weight.
    std::unordered_map<std::size_t, std::vector<float>> morphs;
    // Each instance with eyes' eye values (see mixEyes), by instance, kept the same way.
    std::unordered_map<std::size_t, std::array<float, 4>> eyes;
    MorphBindings morphBindings;  // kept between calls
};
void poseActors(const Scene& scene, double seconds, ScenePose& out);

} // namespace ts2::scene
