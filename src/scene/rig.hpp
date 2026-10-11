#pragma once

#include "formats/anim.hpp"
#include "scene/scene.hpp"

#include <array>
#include <memory>
#include <string>
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
    };
    std::vector<Node> nodes;

    // The first node with this (lowercase) name; -1 when there is none.
    int find(const std::string& name) const;
};

// A skeleton's rig, from its model (e.g. "auskel"); empty when the model is missing or unreadable.
Rig loadRig(Assets& assets, const std::string& skeleton);

// An animation bound to a rig: per node, the channel that rotates it and the one that moves it
// (-1 for none; when an animation has more than one, the last), and its limb IK chains as rig nodes.
struct RigAnimation {
    std::shared_ptr<const anim::Animation> animation;
    std::vector<int> rotation, translation;
    // A limb (hip, knee, ankle): `begin` turns freely, `middle` bends about its x axis and `end`
    // takes the target's orientation, so that `end` reaches `target`, the knee toward `pole`.
    // A target or pole with a frame node is placed relative to that node instead of its parent.
    struct Limb {
        int begin = -1, middle = -1, end = -1;
        int target = -1, targetFrame = -1, targetRotation = -1;
        int pole = -1, poleFrame = -1;
    };
    std::vector<Limb> limbs;
};
RigAnimation bindAnimation(const Rig& rig, std::shared_ptr<const anim::Animation> animation);

// Where each of the rig's nodes is at `ms` into the animation, in the model's space; at rest
// without one.
void poseRig(const Rig& rig, const RigAnimation* animation, float ms, std::vector<Transform>& world);

// The joint matrices a skinned mesh is drawn with (see Mesh::skin), for a rig posed by `animation`
// (null: at rest); `nodes` maps each of the mesh's skin entries to a rig node (-1: the entry's joint
// isn't in the rig and stays at rest). A joint the animation doesn't move keeps the mesh's own rest
// place under its posed parent.
void skinMatrices(const Mesh& mesh, const std::vector<int>& nodes, const Rig& rig, const RigAnimation* animation,
                  const std::vector<Transform>& world, std::vector<Transform>& out);

// Each skin entry of a mesh as a node of the rig (see skinMatrices).
std::vector<int> skinNodes(const Mesh& mesh, const Rig& rig);

// Poses every actor of a scene at `seconds`, each animation looping from its start: the joint
// matrices of each skinned instance and the place of each held instance, by instance.
struct ScenePose {
    std::vector<std::pair<std::size_t, std::vector<Transform>>> joints;
    std::vector<std::pair<std::size_t, Transform>> places;
};
void poseActors(const Scene& scene, double seconds, ScenePose& out);

} // namespace ts2::scene
