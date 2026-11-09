#pragma once

#include "scene/rig.hpp"

#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace ts2::scene {

// One animation playing on a rig among others: where it is, how strongly it plays, on which layer
// (higher layers cover lower ones) and when it started (later ones cover earlier ones on a layer).
struct LayeredClip {
    const RigAnimation* animation = nullptr;
    float ms = 0;
    float weight = 1;
    int layer = 0;
    double start = 0;
    // Radians the clip's root and limb controls are turned by about their turn axes (Rig::Node::turnAxis):
    // a locomotion clip plays from its own heading, which may differ from the one the rig is placed at.
    float turn = 0;
    // Moves only the body's joints and limbs, not the root joints (root_trans, root_rot). A walk's last
    // pose under the animation that follows it is drawn this way, as the game's root already follows
    // that animation, and its limb targets still count as it fades out.
    bool bodyOnly = false;
    // Leaves out where it moves the root (root_trans's move) but keeps the rest. The animation before
    // a walk is drawn this way under the walk's fade-in: the game's root takes the walk's place at
    // once, so a Sim leaving a long stair at its top doesn't keep the climb's lift. Its limbs reach
    // around the drawn root as they do around its own (see LimbSource::rootMove).
    bool noRootMove = false;
    // Where the clip's IK object stands, in the rig's space: its limb targets on that object are
    // placed from there. None: in the skeleton's own space.
    std::optional<Transform> objectFrame;
    // Where a bone of the clip's IK object is, in the rig's space (see LimbSource::objectBone).
    std::function<std::optional<Transform>(std::uint32_t bone)> objectBone;
    // For a clip with a root joint, the move from where the Sim stood as it started to where it stands
    // now, in the rig's space. Its root joints and its limb targets and poles in the skeleton's space
    // move by it, so the clip plays on where it started in the world while the Sim is placed anew
    // (snapped to a slot, or walking off), as the game's clips do (see LimbSource::rootMove).
    std::optional<Transform> placeMove;
    // In place of `animation`: a pose to hold, one per rig node (a Sim's pose as it was at some moment,
    // see Actor::Clip::frozen). It moves every joint and has no limbs of its own.
    const std::vector<HeldJoint>* pose = nullptr;
};

// Poses a rig from several animations as the game mixes them, joint by joint: the highest layer moving a
// joint takes it by its clips' summed weights, the layer below gets 1 minus that, and so on. A layer whose
// clips weigh 1 or more takes all that's left, shared by weight. Limbs reach for all clips' targets,
// weighted as the joint mix weighs them. A joint no clip moves stays at rest, or with `held` (one per
// joint, empty at first) keeps what it held, as in the game. Clips below layer 0 (the starting pose)
// count only while nothing is held yet, and `held` is then set to this pose. `look` gives where the head
// looks after mixing, and it turns there on a layer above every clip.
void poseRigLayered(const Rig& rig, std::span<const LayeredClip> clips, std::vector<Transform>& world,
                    std::vector<HeldJoint>* held = nullptr,
                    const std::function<std::optional<HeadLook>(const HeadPlace&)>* look = nullptr);

// The weights the clips give a mesh's morphs (Mesh::morphs), one per morph into `weights`. A clip's
// morph channel weighs the morph of its name, case ignored, in its target's blend group ("*" any), and a
// constant channel keeps its first value. Clips mix layer by layer as on a joint, each counting only
// above 0.00075. A morph no clip weighs keeps its weight, 0 at first. With `bindings` the channels are
// matched to the mesh (`meshIndex` in the scene) once per clip.
void mixMorphWeights(const Mesh& mesh, std::span<const LayeredClip> clips, std::vector<float>& weights,
                     MorphBindings* bindings = nullptr, int meshIndex = -1);
// The clips' eye values, mixed the same way into `eyes`: the eyes' direction (x, y) and the left and
// right eye's stretch, from their eye channels. A value no clip drives keeps what it had.
void mixEyes(std::span<const LayeredClip> clips, std::array<float, 4>& eyes);

} // namespace ts2::scene
