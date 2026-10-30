#pragma once

#include "formats/anim.hpp"
#include "formats/scenegraph.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ts2::scene {

// A Sim's age in its age data (AGED "age").
constexpr std::uint32_t kToddlerAge = 0x01, kChildAge = 0x02, kTeenAge = 0x04, kAdultAge = 0x08, kElderAge = 0x10,
                        kBabyAge = 0x20, kYoungAdultAge = 0x40;

// The kinds of part a Sim wears (a property set's "parts").
constexpr std::uint32_t kHairPart = 1, kFacePart = 2, kTopPart = 4, kBodyPart = 8, kBottomPart = 16;

// One part a Sim wears (hair, face, body...): a model and the materials replacing its groups' own.
struct SimPart {
    std::uint32_t part = 0;  // one of the kinds above
    std::string model;       // resource node name, without "_cres"
    std::vector<scenegraph::MaterialOverride> overrides;
    std::shared_ptr<const MeshEdit> meshEdit;  // a change to its mesh first, or none
};

// What a Sim wears, from its character file: the parts its age data (AGED) lists for the outfit
// category it has active (1 everyday, 8 formal, 16 pajamas...), each through its part property set
// (GZPS) and that set's reference list. With no hair listed (a baby), its skin outfit's bald head.
// Leaves the character file in use in `assets`.
struct SimLook {
    std::uint32_t category = 0;
    std::uint32_t age = 0;     // one of the age flags below
    std::uint32_t gender = 0;  // 1 female, 2 male
    std::string skinTone;      // e.g. "00000004-0000-0000-0000-000000000000"
    // The materials of its bald head, bare top and bottom, and a toddler's bare body (its skin
    // outfit), e.g. "emtopnaked_nude_s5", "pubodynaked_nude_s1".
    std::string skinHair, skinTop, skinBottom, skinBody;
    // Textures drawn over the face's own, bottom first (a newborn's eyes and eyebrows).
    std::vector<std::string> faceOverlays;
    // A PlantSim's plant skin (outfit category 0x800), laid over the skin above: its head, and its
    // body under any top, bottom or full outfit. Empty for other Sims.
    std::string plantHead, plantBody;
    std::string skeleton;      // its skeleton's model (e.g. "auskel"), without "_cres"
    float stretch = 1;         // its skeleton's bones are this much longer (teens 0.94: the adult skeleton, smaller)
    std::vector<SimPart> parts;
    std::vector<std::string> problems;
};
SimLook simLook(Assets& assets, const std::filesystem::path& characterFile);

// A newborn before it has a character file: its gender (1 female, 2 male), the dominant values of
// its skin, hair and eye genes, the draw that picks its eyebrows and the change that blends its
// face from its parents' (none for the plain baby face).
struct Newborn {
    std::uint32_t gender = 0;
    std::string skinTone, hairTone, eyeColour;
    std::uint32_t browDraw = 0;
    std::shared_ptr<const MeshEdit> face;
};

// The outfit catalog's entries made for babies: part property sets and texture overlays, each with
// its key, in the order the catalog is read. Reading them takes a while, so they are read once, and
// on any thread: `skinPackages` are Assets::skinPackages().
struct BabyCatalog {
    std::vector<std::pair<scenegraph::Link, std::map<std::string, std::string>>> parts, overlays;
};
BabyCatalog readBabyCatalog(const std::vector<std::filesystem::path>& skinPackages);

// What a newborn wears, picked from the outfit catalog as the game picks it for a baby: the skin
// outfit's bald head, face and bare body of its tone, and the one baby body worn every day (the
// diaper). On the face, its eye colour and then eyebrows of its hair tone, one of the baby
// eyebrows drawn by `browDraw`.
SimLook newbornLook(Assets& assets, const BabyCatalog& catalog, const Newborn& baby);

// Whether a Sim wears only the last entry of an outfit's list of this part (kHairPart...). A Sim
// wears one hair, top, bottom and body each, so the game takes the last of those (a PlantSim's
// everyday hair list holds its old hair, then the leaf crown). Faces and accessories keep every entry.
bool wearsLastEntryOnly(std::uint32_t part);

// The material whose texture is the Sim's bare skin under its clothes, as the game picks it: its
// skin outfit's body (toddlers) or else top under a body outfit, its bottom under a top and bottom.
// Empty when it has none.
std::string skinUnderClothes(const SimLook& look, bool separates = false);

// The material for a body outfit's material drawn over that skin, named as the game names the
// composite ("_amtopnaked_nude_s4_ambodyovershirtpantsshoes_blue"). `material` when there is no skin.
std::string clothesOverSkin(SceneBuilder& builder, Assets& assets, const SimLook& look, const std::string& material);

// The material for a hair's scalp (its "hair" subset) drawn over the Sim's bald head, named as the
// game names the composite ("_amhairbald_skin_s5_amhairpompodore_grey"). `material` when there is
// no bald head.
std::string hairOverScalp(SceneBuilder& builder, Assets& assets, const SimLook& look, const std::string& material);

// The material for a face with the look's face overlays drawn over it (a newborn's eyes and
// eyebrows). `material` when there are none.
std::string faceWithOverlays(SceneBuilder& builder, const SimLook& look, const std::string& material);

// The material for a top and a bottom worn together: as the game does, the skin, the top and the
// bottom are drawn into one texture that both use, bottom last
// ("_afbottomnaked_nude_s3_aftophalter_black_afbottomcargopants_pink"). Empty when there is no skin.
std::string separatesOverSkin(SceneBuilder& builder, Assets& assets, const SimLook& look, const std::string& top,
                              const std::string& bottom);

// The bone (lowercase) a Sim's slot `slot` sits at: the Sim's SLOT resource names it through the
// Sim's "Model - Bones" string table, both in its character file. A file with no trees and no such
// table reads the template person's, as the game does. Empty when unknown.
std::string simSlotBone(Assets& assets, const std::filesystem::path& characterFile, std::size_t slot);
// All of them, by slot, read at once. With `targeting`, the bones of its targeting slots instead.
std::vector<std::string> simSlotBones(Assets& assets, const std::filesystem::path& characterFile, bool targeting = false);

// The animation a Sim of this age (SimLook::age) stands idle with, as the game's idle code names it
// for each age (adults' when its age has none). Empty when unknown.
std::string standingIdle(Assets& assets, std::uint32_t age);

// The pose the game keeps under everything a Sim of this age plays (<age>-pose-neutral-stand, a
// toddler's p-sitPose-slotRigged, adults' when its age has none). Empty when unknown.
std::string standingPose(Assets& assets, std::uint32_t age);

// An animation by name (without "_anim"). Nullopt when missing or unreadable.
[[nodiscard]] std::optional<anim::Animation> loadAnimation(Assets& assets, const std::string& name);

// A skeleton's joints with an animation applied at `ms`: each joint the animation has a rotation
// (Euler degrees, x then y then z) or translation for takes those instead of its own. The others
// keep the skeleton's. Without an animation, the bind pose.
Pose posedSkeleton(Assets& assets, const std::string& skeleton, const anim::Animation* animation, float ms);

} // namespace ts2::scene
