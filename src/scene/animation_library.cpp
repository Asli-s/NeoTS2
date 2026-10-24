#include "scene/animation_library.hpp"

#include "scene/sims.hpp"

#include <algorithm>
#include <cmath>

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

std::shared_ptr<const RigAnimation> AnimationLibrary::bound(const std::shared_ptr<const Rig>& rig, const std::string& name, bool mirrored) {
    if (!rig) {
        return nullptr;
    }
    const auto key = std::make_tuple(rig.get(), name, mirrored);
    if (const auto it = bound_.find(key); it != bound_.end()) {
        return it->second;
    }
    const auto animation = find(name);
    if (!animation) {
        return bound_[key] = nullptr;
    }
    return bound_[key] = std::make_shared<const RigAnimation>(bindAnimation(*rig, animation, mirrored));
}

std::optional<float> AnimationLibrary::rootTurn(const std::string& name, float ms) {
    constexpr float kRadiansPerDegree = 3.14159265358979f / 180.0f;
    const auto animation = find(name);
    if (!animation) {
        return std::nullopt;
    }
    for (const auto& channel : animation->channels) {
        if (channel.kind() == anim::ChannelKind::Rotation && channel.name == "root_rot" && channel.components.size() >= 2) {
            return channel.components[1].sample(ms) * kRadiansPerDegree;
        }
    }
    return 0.0f;
}

std::optional<float> AnimationLibrary::length(const std::string& name) {
    const auto animation = find(name);
    return animation ? std::optional<float>(animation->duration) : std::nullopt;
}

bool AnimationLibrary::hasTargetType(const std::string& name, std::uint16_t type) {
    const auto animation = find(name);
    return animation && std::find(animation->targetTypes.begin(), animation->targetTypes.end(), type) != animation->targetTypes.end();
}

std::optional<float> AnimationLibrary::eventTime(const std::string& name, int id) {
    if (const auto animation = find(name)) {
        for (const auto& event : animation->events) {
            if (anim::simAnticsEventId(event) == id) {
                return static_cast<float>(event.timeMs);
            }
        }
    }
    return std::nullopt;
}

std::vector<std::pair<float, int>> AnimationLibrary::simAnticsEvents(const std::string& name) {
    std::vector<std::pair<float, int>> out;
    if (const auto animation = find(name)) {
        for (const auto& event : animation->events) {
            if (const auto id = anim::simAnticsEventId(event)) {
                out.emplace_back(static_cast<float>(event.timeMs), *id);
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

} // namespace ts2::scene
