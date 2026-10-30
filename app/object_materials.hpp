#pragma once

#include "scene/animation_library.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"
#include "scene/scene.hpp"
#include "scene/sim_builder.hpp"
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
    explicit ObjectMaterials(ts2::scene::Assets& assets) : assets_(assets), builder_(assets), simRigs_(assets) {}

    // The scene mesh of a game model (e.g. "puddle_cres"), added with its materials on first use.
    // -1 when it has nothing to draw.
    int model(const std::string& name, ts2::scene::Scene& scene);
    // The scene mesh of a lot object's model with the materials the lot put on it, skinned to the
    // model's joints. -1 when it has nothing to draw or follows no joint.
    int skinnedModel(const std::string& name, const std::vector<ts2::scenegraph::MaterialOverride>& overrides, ts2::scene::Scene& scene);
    // A Sim created during play put into the scene in what it wears (see buildSim), with its meshes and
    // materials. The actor's instances are the scene's new ones, or take the places in `reuse` first
    // (instances drawing nothing, left by removed Sims).
    ts2::scene::Actor sim(const ts2::scene::SimLook& look, const std::string& name, const ts2::scene::Transform& at, int level,
                          const std::string& animation, ts2::scene::Scene& scene, std::vector<std::size_t>& reuse);
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

    // Where joint `joint` (lower case) of the object's model `model` sits at `nowMs` as the object's
    // clip moves it, in the model's space. Joints the clip doesn't move are at rest. Nullopt when it
    // plays none or the model has no such joint. The clips name a skeleton of their own ("chairdining"
    // for every dining chair), but drive the joints of the model that plays them. `bones` gets every
    // named joint as posed, in the model's space, by its name's hash.
    std::optional<ts2::scene::Transform> jointPose(const ts2::sim::SimObject& object, double nowMs,
                                                   ts2::scene::AnimationLibrary& animations, const std::string& model,
                                                   const std::string& joint,
                                                   std::vector<std::pair<std::uint32_t, ts2::scene::Transform>>* bones = nullptr);

private:
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
    ts2::scene::RigLibrary simRigs_;
    std::map<int, int> simMeshes_;                   // builder mesh -> scene mesh, for Sims
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
