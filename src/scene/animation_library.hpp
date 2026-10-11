#pragma once

#include "formats/anim.hpp"
#include "scene/rig.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace ts2::scene {

class Assets;

// The game's animations by name, each read once when first asked for, and bound to the rigs that
// play them.
class AnimationLibrary {
public:
    explicit AnimationLibrary(Assets& assets) : assets_(assets) {}

    // An animation by name (without "_anim"); null when the game has none by that name.
    std::shared_ptr<const anim::Animation> find(const std::string& name);

    // The animation bound to a rig; null when missing.
    std::shared_ptr<const RigAnimation> bound(const std::shared_ptr<const Rig>& rig, const std::string& name);

    // How long an animation lasts, in milliseconds; nullopt when missing.
    std::optional<float> length(const std::string& name);

private:
    Assets& assets_;
    std::map<std::string, std::shared_ptr<const anim::Animation>> animations_;  // null: known missing
    std::map<std::pair<const Rig*, std::string>, std::shared_ptr<const RigAnimation>> bound_;
};

} // namespace ts2::scene
