#include "render/shared_state.hpp"

#include <algorithm>
#include <utility>

namespace ts2::render {

void RendererState::show(std::vector<std::uint8_t> visible) {
    visible_ = std::move(visible);
}

void RendererState::showSprites(std::vector<Sprite> sprites, bool scene) {
    sprites_ = std::move(sprites);
    drawScene_ = scene;
}

void RendererState::setJoints(std::size_t instance, const std::vector<scene::Transform>& joints) {
    if (instance >= jointBase_.size() || joints.empty()) {
        return;
    }
    // An instance keeps its place in the buffer while its joints fit there, so the buffer only grows
    // when an instance needs more of them than before.
    if (jointBase_[instance] < 0 || jointCount_[instance] < static_cast<int>(joints.size())) {
        jointBase_[instance] = static_cast<int>(jointData_.size() / 12);
        jointCount_[instance] = static_cast<int>(joints.size());
        jointData_.resize(jointData_.size() + joints.size() * 12);
    }
    float* out = &jointData_[static_cast<std::size_t>(jointBase_[instance]) * 12];
    for (const auto& j : joints) {
        std::copy(j.m.begin(), j.m.end(), out);
        out += 12;
    }
    jointsChanged_ = true;
}

void RendererState::setPlace(std::size_t instance, const scene::Transform& transform) {
    if (instance < instances_.size()) {
        instances_[instance].transform = transform;
    }
}

void RendererState::setMaterials(std::size_t instance, std::vector<int> materials) {
    if (instance >= instances_.size()) {
        return;
    }
    if (instanceMaterials_.size() <= instance) {
        instanceMaterials_.resize(instance + 1);
    }
    instanceMaterials_[instance] = std::move(materials);
}

void RendererState::setInstance(const scene::Scene& scene, std::size_t index) {
    if (index > instances_.size() || index >= scene.instances.size()) {
        return;
    }
    const scene::Instance& instance = scene.instances[index];
    addLightSets(scene);
    if (index == instances_.size()) {
        instances_.push_back(instance);
        jointBase_.push_back(-1);
        jointCount_.push_back(0);
    } else {
        instances_[index] = instance;
    }
    if (index < instanceMaterials_.size()) {
        instanceMaterials_[index].clear();
    }
}

void RendererState::setLights(const scene::Scene& scene, std::size_t index) {
    if (index >= instances_.size() || index >= scene.instances.size()) {
        return;
    }
    addLightSets(scene);
    instances_[index].lightSet = scene.instances[index].lightSet;
    instances_[index].partLightSets = scene.instances[index].partLightSets;
    instances_[index].sun = scene.instances[index].sun;
    instances_[index].skirtScale = scene.instances[index].skirtScale;
}

void RendererState::addLightSets(const scene::Scene& scene) {
    if (scene.lightSets.size() > lightSets_.size()) {
        lightSets_.insert(lightSets_.end(), scene.lightSets.begin() + static_cast<std::ptrdiff_t>(lightSets_.size()), scene.lightSets.end());
    }
}

void RendererState::copyScene(PreparedBase& to, const scene::Scene& scene) {
    to.materials = scene.materials;
    to.instances = scene.instances;
    to.objectLighting = scene.objectLighting;
    to.lightSets = scene.lightSets;
}

void RendererState::swapScene(PreparedBase& prepared) {
    std::swap(materials_, prepared.materials);
    std::swap(instances_, prepared.instances);
    instanceMaterials_.clear();
    std::swap(objectLighting_, prepared.objectLighting);
    std::swap(lightSets_, prepared.lightSets);
    visible_.clear();
    jointBase_.assign(instances_.size(), -1);
    jointCount_.assign(instances_.size(), 0);
    jointData_.clear();
    shadows_ = {};
}

} // namespace ts2::render
