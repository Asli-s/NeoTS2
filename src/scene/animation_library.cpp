#include "scene/animation_library.hpp"

#include "scene/sims.hpp"

namespace ts2::scene {

std::shared_ptr<const anim::Animation> AnimationLibrary::find(const std::string& name) {
    const auto it = animations_.find(name);
    if (it != animations_.end()) {
        return it->second;
    }
    std::shared_ptr<const anim::Animation> found;
    if (auto loaded = loadAnimation(assets_, name)) {
        found = std::make_shared<const anim::Animation>(std::move(*loaded));
    }
    return animations_[name] = found;
}

std::shared_ptr<const RigAnimation> AnimationLibrary::bound(const std::shared_ptr<const Rig>& rig, const std::string& name) {
    if (!rig) {
        return nullptr;
    }
    const auto key = std::make_pair(rig.get(), name);
    if (const auto it = bound_.find(key); it != bound_.end()) {
        return it->second;
    }
    const auto animation = find(name);
    return bound_[key] = animation ? std::make_shared<const RigAnimation>(bindAnimation(*rig, animation)) : nullptr;
}

std::optional<float> AnimationLibrary::length(const std::string& name) {
    const auto animation = find(name);
    return animation ? std::optional<float>(animation->duration) : std::nullopt;
}

} // namespace ts2::scene
