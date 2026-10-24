#pragma once

#include "formats/anim.hpp"
#include "scene/rig.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace ts2::scene {

class Assets;

// The game's animations by name, each read once when first asked for, and bound to the rigs that
// play them.
class AnimationLibrary {
public:
    explicit AnimationLibrary(Assets& assets) : assets_(assets) {}

    // An animation by name (without "_anim"). Null when the game has none by that name.
    std::shared_ptr<const anim::Animation> find(const std::string& name);

    // The animation bound to a rig. Null when missing. Mirrored: played on the other side of the body
    // (see RigAnimation).
    std::shared_ptr<const RigAnimation> bound(const std::shared_ptr<const Rig>& rig, const std::string& name, bool mirrored = false);

    // How long an animation lasts, in milliseconds. Nullopt when missing.
    [[nodiscard]] std::optional<float> length(const std::string& name);
    // When an animation's SimAntics event `id` comes, in milliseconds. Nullopt when it has none.
    [[nodiscard]] std::optional<float> eventTime(const std::string& name, int id);
    // An animation's SimAntics events as (time in milliseconds, number), earliest first.
    [[nodiscard]] std::vector<std::pair<float, int>> simAnticsEvents(const std::string& name);
    // Whether an animation has a target (skeleton) of this type (2: a carry pose). False when missing.
    [[nodiscard]] bool hasTargetType(const std::string& name, std::uint16_t type);

    // How far an animation has turned its root about the up axis after `ms`, in radians (the
    // root_rot channel's y). Nullopt when missing.
    [[nodiscard]] std::optional<float> rootTurn(const std::string& name, float ms);

private:
    Assets& assets_;
    std::map<std::string, std::shared_ptr<const anim::Animation>> animations_;  // null: known missing
    std::map<std::tuple<const Rig*, std::string, bool>, std::shared_ptr<const RigAnimation>> bound_;
};

} // namespace ts2::scene
