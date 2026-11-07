#pragma once

#include "scene/builder.hpp"
#include "scene/rig.hpp"
#include "scene/sims.hpp"

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ts2::scene {

// Skeleton rigs and the animations bound to them, each loaded once.
class RigLibrary {
public:
    explicit RigLibrary(Assets& assets) : assets_(assets) {}

    // A skeleton's rig (e.g. "auskel") with its bones stretched (see stretched), with a teen human's
    // arms when `teenArms` (see stretchTeenArms). Null when missing or empty.
    std::shared_ptr<const Rig> rig(const std::string& skeleton, float stretch = 1, bool teenArms = false);
    // An animation (without "_anim") bound to that rig. Null when either is missing.
    std::shared_ptr<const RigAnimation> animation(const std::string& skeleton, float stretch, const std::string& name,
                                                  bool teenArms = false);

private:
    Assets& assets_;
    std::map<std::tuple<std::string, float, bool>, std::shared_ptr<const Rig>> rigs_;
    std::map<std::tuple<std::string, float, bool, std::string>, std::shared_ptr<const RigAnimation>> animations_;
};

// Sets the joints each part of `look` names in its model's bone_adjust lists, in the order the game
// attaches parts: by part bit, hair first, a top before a bottom, so the bottom's hip and thigh
// targets win. Joints no part names keep their places.
void wearPartJoints(Rig& rig, const SimLook& look);

// Puts a Sim into the scene in what it wears (`look`): each part skinned to the Sim's rig, a top and
// a bottom sharing one texture over the skin as the game draws them, standing at `at` on `level`
// and posed by `animation` at its start (at rest when it has none). Instances are named "<name>
// <part model>". Returns the Sim's actor. `world` gets the rig's pose.
Actor buildSim(SceneBuilder& builder, RigLibrary& rigs, const SimLook& look, const Transform& at, int level,
               const std::string& name, const std::string& animation, std::vector<Transform>& world);

} // namespace ts2::scene
