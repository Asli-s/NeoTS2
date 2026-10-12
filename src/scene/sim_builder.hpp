#pragma once

#include "scene/builder.hpp"
#include "scene/rig.hpp"
#include "scene/sims.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ts2::scene {

// Skeleton rigs and the animations bound to them, each loaded once.
class RigLibrary {
public:
    explicit RigLibrary(Assets& assets) : assets_(assets) {}

    // A skeleton's rig (e.g. "auskel"); null when missing or empty.
    std::shared_ptr<const Rig> rig(const std::string& skeleton);
    // An animation (without "_anim") bound to a skeleton's rig; null when either is missing.
    std::shared_ptr<const RigAnimation> animation(const std::string& skeleton, const std::string& name);

private:
    Assets& assets_;
    std::map<std::string, std::shared_ptr<const Rig>> rigs_;
    std::map<std::string, std::shared_ptr<const RigAnimation>> animations_;
};

// Puts a Sim into the scene in what it wears (`look`): each part skinned to the Sim's rig, a top and
// a bottom sharing one texture over the skin as the game draws them, standing at `at` on `level`
// and posed by `animation` at its start (at rest when it has none). Instances are named "<name>
// <part model>". Returns the Sim's actor; `world` gets the rig's pose.
Actor buildSim(SceneBuilder& builder, RigLibrary& rigs, const SimLook& look, const Transform& at, int level,
               const std::string& name, const std::string& animation, std::vector<Transform>& world);

} // namespace ts2::scene
