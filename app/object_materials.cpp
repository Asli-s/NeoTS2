#include "object_materials.hpp"
#include "formats/hash.hpp"

#include <algorithm>
#include <climits>
#include <cmath>

namespace app {

void ObjectMaterials::update(ts2::sim::World& world, ts2::scene::Scene& scene) {
    changed_.clear();
    for (const auto& placed : scene.placedObjects) {
        if (placed.object > static_cast<std::uint32_t>(INT16_MAX) || placed.instance >= scene.instances.size()) {
            continue;
        }
        const auto id = static_cast<std::int16_t>(placed.object);
        const auto* object = world.find(id);
        if (object == nullptr || object->materialVersion == versions_[id]) {
            continue;
        }
        versions_[id] = object->materialVersion;
        const int mesh = scene.instances[placed.instance].mesh;
        if (mesh < 0 || static_cast<std::size_t>(mesh) >= scene.meshes.size()) {
            continue;
        }
        changed_.emplace_back(placed.instance, materialsOf(*object, scene.meshes[static_cast<std::size_t>(mesh)], scene));
    }
}

// The mesh's own materials, then each change in order on the primitives of its mesh group (all of
// them for the whole model). A "_null" material hides them, as it leaves groups out of a model.
std::vector<int> ObjectMaterials::materialsOf(const ts2::sim::SimObject& object, const ts2::scene::Mesh& mesh,
                                              ts2::scene::Scene& scene) {
    std::vector<int> out;
    out.reserve(mesh.primitives.size());
    for (const auto& p : mesh.primitives) {
        out.push_back(p.material);
    }
    for (const auto& change : object.materialChanges) {
        const std::string group = ts2::scene::lower(change.group);
        const int m = ts2::scene::lower(change.material).find("_null") != std::string::npos ? ts2::scene::kHiddenMaterial
                                                                                           : material(change.material, scene);
        if (m == -1) {
            continue;
        }
        for (std::size_t i = 0; i < mesh.primitives.size(); ++i) {
            if (group.empty() || mesh.primitives[i].group == group) {
                out[i] = m;
            }
        }
    }
    return out;
}

int ObjectMaterials::material(const std::string& name, ts2::scene::Scene& scene) {
    if (!lotMaterialsKnown_) {
        lotMaterialsKnown_ = true;
        for (std::size_t i = 0; i < scene.materials.size(); ++i) {
            materials_.try_emplace(ts2::scene::lower(scene.materials[i].name), static_cast<int>(i));
        }
    }
    const std::string key = ts2::scene::lower(name);
    if (const auto it = materials_.find(key); it != materials_.end()) {
        return it->second;
    }
    return materials_[key] = builderMaterial(builder_.material(name), scene);
}

int ObjectMaterials::builderMaterial(int index, ts2::scene::Scene& scene) {
    if (index < 0 || static_cast<std::size_t>(index) >= builder_.scene().materials.size()) {
        return -1;
    }
    if (const auto it = builderMaterials_.find(index); it != builderMaterials_.end()) {
        return it->second;
    }
    ts2::scene::Material m = builder_.scene().materials[static_cast<std::size_t>(index)];
    m.image = image(m.image, scene);
    m.normalMap = image(m.normalMap, scene);
    m.envCube = envCube(m.envCube, scene);
    scene.materials.push_back(std::move(m));
    return builderMaterials_[index] = static_cast<int>(scene.materials.size() - 1);
}

int ObjectMaterials::model(const std::string& name, ts2::scene::Scene& scene) {
    const std::string key = ts2::scene::lower(name);
    if (const auto it = models_.find(key); it != models_.end()) {
        return it->second;
    }
    const int built = builder_.model(name, {});
    if (built < 0) {
        return models_[key] = -1;
    }
    // Moved out: models_ asks the builder for each model only once.
    ts2::scene::Mesh mesh = std::move(builder_.scene().meshes[static_cast<std::size_t>(built)]);
    for (auto& p : mesh.primitives) {
        p.material = builderMaterial(p.material, scene);
    }
    scene.meshes.push_back(std::move(mesh));
    return models_[key] = static_cast<int>(scene.meshes.size() - 1);
}

int ObjectMaterials::skinnedModel(const std::string& name, const std::vector<ts2::scenegraph::MaterialOverride>& overrides,
                                  ts2::scene::Scene& scene) {
    std::string key = ts2::scene::lower(name);
    for (const auto& o : overrides) {
        key += "|" + ts2::scene::lower(o.group) + "=" + ts2::scene::lower(o.material);
    }
    if (const auto it = skinnedModels_.find(key); it != skinnedModels_.end()) {
        return it->second;
    }
    const int built = builder_.skinnedModel(name, overrides);
    if (built < 0 || builder_.scene().meshes[static_cast<std::size_t>(built)].skin.empty()) {
        return skinnedModels_[key] = -1;
    }
    ts2::scene::Mesh mesh = builder_.scene().meshes[static_cast<std::size_t>(built)];
    for (auto& p : mesh.primitives) {
        p.material = builderMaterial(p.material, scene);
    }
    scene.meshes.push_back(std::move(mesh));
    return skinnedModels_[key] = static_cast<int>(scene.meshes.size() - 1);
}

ts2::scene::Actor ObjectMaterials::sim(const ts2::scene::SimLook& look, const std::string& name, const ts2::scene::Transform& at,
                                      int level, const std::string& animation, ts2::scene::Scene& scene,
                                      std::vector<std::size_t>& reuse) {
    auto& built = builder_.scene();
    const std::size_t first = built.instances.size();
    std::vector<ts2::scene::Transform> world;
    ts2::scene::Actor actor = ts2::scene::buildSim(builder_, simRigs_, look, at, level, name, animation, world);
    std::map<std::size_t, std::size_t> moved;
    for (std::size_t i = first; i < built.instances.size(); ++i) {
        ts2::scene::Instance instance = std::move(built.instances[i]);
        auto [mesh, added] = simMeshes_.try_emplace(instance.mesh, -1);
        if (added) {
            ts2::scene::Mesh copy = built.meshes[static_cast<std::size_t>(instance.mesh)];
            for (auto& p : copy.primitives) {
                p.material = builderMaterial(p.material, scene);
            }
            scene.meshes.push_back(std::move(copy));
            mesh->second = static_cast<int>(scene.meshes.size() - 1);
        }
        instance.mesh = mesh->second;
        if (!reuse.empty() && reuse.back() < scene.instances.size()) {
            moved[i] = reuse.back();
            reuse.pop_back();
            scene.instances[moved[i]] = std::move(instance);
        } else {
            moved[i] = scene.instances.size();
            scene.instances.push_back(std::move(instance));
        }
    }
    // Only the meshes stay in the builder, for the next Sim wearing them.
    built.instances.resize(first);
    for (auto& s : actor.skinned) {
        s.instance = moved.at(s.instance);
    }
    return actor;
}

std::shared_ptr<const ts2::scene::Rig> ObjectMaterials::objectRig(const std::string& model, const std::vector<ts2::cobject::SavedJoint>& saved) {
    auto [it, first] = rigs_.try_emplace(ts2::scene::lower(model));
    if (first) {
        it->second = std::make_shared<ts2::scene::Rig>(ts2::scene::loadRig(assets_, model));
    }
    if (saved.empty() || it->second->nodes.empty()) {
        return it->second;
    }
    auto rig = std::make_shared<ts2::scene::Rig>(*it->second);
    for (const auto& j : saved) {
        const int node = rig->find(ts2::scene::lower(j.name));
        if (node < 0 || !std::ranges::all_of(j.translation, [](float v) { return std::isfinite(v); }) ||
            !std::ranges::all_of(j.rotation, [](float v) { return std::isfinite(v); })) {
            continue;
        }
        rig->nodes[static_cast<std::size_t>(node)].translation = j.translation;
        rig->nodes[static_cast<std::size_t>(node)].rotation = j.rotation;
    }
    return rig;
}

// Images move into the scene: the builder only needs their place in its own list.
int ObjectMaterials::image(int builderImage, ts2::scene::Scene& scene) {
    auto& images = builder_.scene().images;
    if (builderImage < 0 || static_cast<std::size_t>(builderImage) >= images.size()) {
        return -1;
    }
    if (const auto it = images_.find(builderImage); it != images_.end()) {
        return it->second;
    }
    scene.images.push_back(std::move(images[static_cast<std::size_t>(builderImage)]));
    return images_[builderImage] = static_cast<int>(scene.images.size() - 1);
}

// Environment cubes move into the scene the same way.
int ObjectMaterials::envCube(int builderCube, ts2::scene::Scene& scene) {
    auto& cubes = builder_.scene().envCubes;
    if (builderCube < 0 || static_cast<std::size_t>(builderCube) >= cubes.size()) {
        return -1;
    }
    if (const auto it = envCubes_.find(builderCube); it != envCubes_.end()) {
        return it->second;
    }
    scene.envCubes.push_back(std::move(cubes[static_cast<std::size_t>(builderCube)]));
    return envCubes_[builderCube] = static_cast<int>(scene.envCubes.size() - 1);
}

float clipMs(const ts2::sim::PlayingAnimation& playing, double length, double nowMs) {
    double ms = std::max(0.0, std::min<double>(nowMs - playing.startMs, playing.lengthMs) * playing.speed);
    if (length > 0) {
        const double wrapped = std::fmod(ms, length);
        ms = !playing.loops ? std::min(ms, length) : ms > 0 && wrapped == 0 ? length : wrapped;
    }
    return static_cast<float>(ms);
}

std::optional<ts2::scene::Transform> ObjectMaterials::jointPose(const ts2::sim::SimObject& object, double nowMs,
                                                                ts2::scene::AnimationLibrary& animations, const std::string& model,
                                                                const std::string& joint,
                                                                std::vector<std::pair<std::uint32_t, ts2::scene::Transform>>* bones) {
    if (!object.animation || object.isPerson()) {
        return std::nullopt;
    }
    const auto& playing = *object.animation;
    auto [it, first] = rigs_.try_emplace(ts2::scene::lower(model));
    if (first) {
        it->second = std::make_shared<ts2::scene::Rig>(ts2::scene::loadRig(assets_, model));
    }
    const int node = it->second->find(joint);
    const auto bound = node >= 0 ? animations.bound(it->second, playing.name, playing.flipped) : nullptr;
    if (!bound || !bound->animation) {
        return std::nullopt;
    }
    ts2::scene::poseRig(*it->second, bound.get(), clipMs(playing, bound->animation->duration, nowMs), poseScratch_);
    if (bones != nullptr) {
        bones->clear();
        for (std::size_t i = 0; i < it->second->nodes.size() && i < poseScratch_.size(); ++i) {
            if (!it->second->nodes[i].name.empty()) {
                bones->emplace_back(ts2::hash::crc32(it->second->nodes[i].name), poseScratch_[i]);
            }
        }
    }
    return poseScratch_[static_cast<std::size_t>(node)];
}

} // namespace app
