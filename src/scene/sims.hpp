#pragma once

#include "formats/anim.hpp"
#include "formats/scenegraph.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ts2::scene {

// One part a Sim wears (hair, face, body...): a model and the materials replacing its groups' own.
struct SimPart {
    std::uint32_t part = 0;  // 1 hair, 2 face, 8 body...
    std::string model;       // resource node name, without "_cres"
    std::vector<scenegraph::MaterialOverride> overrides;
};

// What a Sim wears, from its character file: the parts its age data (AGED) lists for the outfit
// category it has active (1 everyday, 8 formal, 16 pajamas...), each through its part property set
// (GZPS) and that set's reference list. Leaves the character file in use in `assets`.
struct SimLook {
    std::uint32_t category = 0;
    std::uint32_t age = 0;     // 4 teen, 8 adult...
    std::uint32_t gender = 0;  // 1 female, 2 male
    std::string skinTone;      // e.g. "00000004-0000-0000-0000-000000000000"
    std::string skeleton;      // its skeleton's model (e.g. "auskel"), without "_cres"
    std::vector<SimPart> parts;
    std::vector<std::string> problems;
};
SimLook simLook(Assets& assets, const std::filesystem::path& characterFile);

// The material whose texture is the Sim's bare skin under its body outfit, for its age, gender
// and skin tone (e.g. "amtopnaked_nude_s4"); empty when not known (other ages, custom skin tones).
std::string skinUnderClothes(const SimLook& look);

// The material for a body outfit's material drawn over that skin, named as the game names the
// composite ("_amtopnaked_nude_s4_ambodyovershirtpantsshoes_blue"); `material` when there is no skin.
std::string clothesOverSkin(SceneBuilder& builder, Assets& assets, const SimLook& look, const std::string& material);

// The bone (lowercase) a Sim's slot `slot` sits at: the Sim's SLOT resource names it through the
// Sim's "Model - Bones" string table, both in its character file; empty when unknown.
std::string simSlotBone(const std::filesystem::path& characterFile, std::size_t slot);

// An animation by name (without "_anim"); nullopt when missing or unreadable.
std::optional<anim::Animation> loadAnimation(Assets& assets, const std::string& name);

// A skeleton's joints with an animation applied at `ms`: each joint the animation has a rotation
// (Euler degrees, x then y then z) or translation for takes those instead of its own; the others
// keep the skeleton's. Without an animation, the bind pose.
Pose posedSkeleton(Assets& assets, const std::string& skeleton, const anim::Animation* animation, float ms);

} // namespace ts2::scene
