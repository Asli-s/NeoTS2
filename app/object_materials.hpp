#pragma once

#include "scene/animation_library.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"
#include "scene/scene.hpp"
#include "sim/world.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace app {

// Time into an object's clip of `length` ms at `nowMs`: looping clips wrap, the others hold their
// last pose, and so does a looping clip whose counted plays have ended. A stopped one holds the
// pose it was stopped at.
float clipMs(const ts2::sim::PlayingAnimation& playing, double length, double nowMs);

// Shows the materials scripts put on lot objects (Change Material): each changed object's
// materials, one per primitive of its mesh, with materials the lot didn't have added to its scene.
// Also brings in the models of objects scripts create.
class ObjectMaterials {
public:
    explicit ObjectMaterials(ts2::scene::Assets& assets) : assets_(assets), builder_(assets) {}

    // The scene mesh of a game model (e.g. "puddle_cres"), added with its materials on first use.
    // -1 when it has nothing to draw.
    int model(const std::string& name, ts2::scene::Scene& scene);
    // The scene mesh of a lot object's model with the materials the lot put on it, skinned to the
    // model's joints. -1 when it has nothing to draw or follows no joint.
    int skinnedModel(const std::string& name, const std::vector<ts2::scenegraph::MaterialOverride>& overrides, ts2::scene::Scene& scene);
    // The rig of an object's model, at rest as the lot saved its joints. Empty when the model has none.
    std::shared_ptr<const ts2::scene::Rig> objectRig(const std::string& model, const std::vector<ts2::cobject::SavedJoint>& saved);
    // Where a named node of a model sits in it, once model() has added it. Null for none.
    const ts2::scene::Transform* bone(const std::string& model, const std::string& node) const { return builder_.bone(model, node); }

    // Looks for objects whose materials changed since the last update.
    void update(ts2::sim::World& world, ts2::scene::Scene& scene);
    // The next update sends the object's materials again if scripts changed them (its instance got
    // a new mesh).
    void showAgain(std::int16_t object) { versions_.erase(object); }
    // What the last update changed: scene instance and its materials (see Renderer::setMaterials).
    const std::vector<std::pair<std::size_t, std::vector<int>>>& changed() const { return changed_; }

    // How the clip an object plays (Animate Object) moves its model at `nowMs` on the world's
    // clock, in the model's space: the plumbob's spin. Only for models whose every vertex follows
    // one joint, which the whole model turns with. Nullopt for other objects and when it plays none.
    std::optional<ts2::scene::Transform> pose(const ts2::sim::SimObject& object, double nowMs, ts2::scene::AnimationLibrary& animations);
    // Where joint `joint` (lower case) of the object's model `model` sits at `nowMs` as the object's
    // clip moves it, in the model's space. Joints the clip doesn't move are at rest. Nullopt when it
    // plays none or the model has no such joint. The clips name a skeleton of their own ("chairdining"
    // for every dining chair), but drive the joints of the model that plays them.
    std::optional<ts2::scene::Transform> jointPose(const ts2::sim::SimObject& object, double nowMs,
                                                   ts2::scene::AnimationLibrary& animations, const std::string& model,
                                                   const std::string& joint);

private:
    // A model whose vertices all follow one joint: its rig, that joint and the joint's rest place.
    struct OneJointModel {
        std::shared_ptr<const ts2::scene::Rig> rig;
        int joint = -1;
        ts2::scene::Transform rest;
    };
    // By lower-case model name. Null for models that don't qualify.
    std::map<std::string, std::shared_ptr<const OneJointModel>> oneJointModels_;
    std::map<std::string, std::shared_ptr<const ts2::scene::Rig>> rigs_;  // by lower-case model name
    std::vector<ts2::scene::Transform> poseScratch_;

    std::vector<int> materialsOf(const ts2::sim::SimObject& object, const ts2::scene::Mesh& mesh, ts2::scene::Scene& scene);
    // The scene's material of that name, added from the game's files on first use. -1 when the
    // game has no such material.
    int material(const std::string& name, ts2::scene::Scene& scene);
    int image(int builderImage, ts2::scene::Scene& scene);
    int envCube(int builderCube, ts2::scene::Scene& scene);
    // The scene's copy of one of the builder's materials.
    int builderMaterial(int index, ts2::scene::Scene& scene);

    ts2::scene::Assets& assets_;
    ts2::scene::SceneBuilder builder_;
    std::map<std::int16_t, std::uint32_t> versions_;  // per object, the materialVersion shown
    std::map<std::string, int> materials_;           // lower-case name -> scene material
    bool lotMaterialsKnown_ = false;                 // the lot's own materials are in materials_
    std::map<int, int> images_;                      // builder image -> scene image
    std::map<int, int> envCubes_;                    // builder cube -> scene cube
    std::map<int, int> builderMaterials_;            // builder material -> scene material
    std::map<std::string, int> models_;              // lower-case name -> scene mesh
    std::map<std::string, int> skinnedModels_;       // lower-case name and materials -> scene mesh
    std::vector<std::pair<std::size_t, std::vector<int>>> changed_;
};

} // namespace app
