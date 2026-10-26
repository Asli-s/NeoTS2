#include "scene/sims.hpp"

#include "content/animation_lists.hpp"
#include "diag/log.hpp"
#include "formats/dbpf.hpp"
#include "formats/objd.hpp"
#include "formats/propset.hpp"
#include "formats/reference_list.hpp"
#include "formats/resource_types.hpp"
#include "formats/slot.hpp"
#include "formats/str.hpp"
#include "scene/rig.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <locale>
#include <optional>
#include <sstream>
#include <string_view>

namespace sg = ts2::scenegraph;

namespace ts2::scene {

namespace {

constexpr std::uint32_t kAgeData = 0xAC598EAC;
constexpr std::uint32_t kPartProperties = 0xEBCF3E27;
constexpr std::uint32_t kSkinCategory = 0x80;  // the outfit category of the bare skin
constexpr std::uint32_t kPlantCategory = 0x800;  // a PlantSim's plant skin, drawn over the bare skin
constexpr std::uint32_t kLocalGroup = 0xFFFFFFFF;
constexpr std::uint32_t kTxmt = 0x49596978;
constexpr std::uint32_t kCres = 0xE519C933;

// A property as a number (decimal, or hex with 0x), 0 when missing or not a number.
std::uint32_t number(const std::map<std::string, std::string>& p, const std::string& key) {
    const auto it = p.find(key);
    if (it == p.end()) {
        return 0;
    }
    std::string_view s = it->second;
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.remove_prefix(2);
        base = 16;
    }
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), value, base);
    return error == std::errc{} ? value : 0;
}

// A resource's own name (its root block's), without the type suffix ("_cres", "_txmt").
std::string resourceName(const std::string& name, const char* suffix) {
    const std::string s = suffix;
    return name.size() > s.size() && lower(name.substr(name.size() - s.size())) == s ? name.substr(0, name.size() - s.size()) : name;
}

// The skeleton the game gives a Sim of this age when its age data names none.
std::string defaultSkeleton(std::uint32_t age) {
    return age == kToddlerAge ? "puskel" : age == kChildAge ? "cuskel" : age == kBabyAge ? "buskel" : "auskel";
}

// How much a Sim's bones are stretched ("stretch", 1 when missing or not a sane number).
float stretchOf(const std::map<std::string, std::string>& p) {
    const auto it = p.find("stretch");
    if (it == p.end()) {
        return 1;
    }
    // Floating-point from_chars is unavailable in libc++ before macOS 26.  The classic locale
    // keeps the game's dot decimal separator independent of the player's regional settings.
    std::istringstream input{it->second};
    input.imbue(std::locale::classic());
    float value = 1;
    input >> std::noskipws >> value;
    return input && input.eof() && std::isfinite(value) ? std::clamp(value, 0.02f, 50.0f) : 1.0f;
}

// The Sim's skeleton model, read as the game does: older files name its key outright (and then
// any index into the reference list is ignored), newer ones index their reference list. Without
// either, the default skeleton for the Sim's age. Empty when it can't be read.
std::string skeletonName(Assets& assets, const std::map<std::string, std::string>& age, const std::vector<sg::Link>& refs,
                         std::uint32_t ageFlag, std::vector<std::string>& problems) {
    std::optional<sg::Link> key;
    bool useRefs = true;
    if (age.count("skeltype")) {
        key = sg::Link{number(age, "skeltype"), 0, 0, 0};
        useRefs = false;
    }
    if (age.count("skelgroup")) {
        key = key.value_or(sg::Link{});
        key->group = number(age, "skelgroup");
    }
    if (age.count("skelinst")) {
        key = key.value_or(sg::Link{});
        key->instance = number(age, "skelinst");
    }
    if (const std::uint32_t s = number(age, "skeletonkeyidx"); useRefs && age.count("skeletonkeyidx") && s < refs.size()) {
        key = refs[s];
    }
    const auto cres = key ? assets.byKey(*key) : assets.byName(kCres, defaultSkeleton(ageFlag) + "_cres");
    if (!cres) {
        problems.push_back("no skeleton");
        return {};
    }
    try {
        const auto tree = sg::parseResourceNode(*cres);
        return tree.nodes.empty() ? std::string() : resourceName(tree.nodes[0].name, "_cres");
    } catch (const std::exception& ex) {
        problems.push_back(ex.what());
        return {};
    }
}

std::string& skinOf(SimLook& look, std::uint32_t part) {
    switch (part) {
    case kHairPart:
        return look.skinHair;
    case kTopPart:
        return look.skinTop;
    case kBodyPart:
        return look.skinBody;
    default:
        return look.skinBottom;
    }
}

} // namespace

bool wearsLastEntryOnly(std::uint32_t part) {
    return part == kHairPart || part == kTopPart || part == kBodyPart || part == kBottomPart;
}

SimLook simLook(Assets& assets, const std::filesystem::path& characterFile) {
    SimLook look;
    assets.useCharacterFile(characterFile);
    const auto aged = assets.byKey({kAgeData, kLocalGroup, 1, 0});
    const auto agedRefs = assets.byKey({reflist::kType, kLocalGroup, 1, 0});
    if (!aged || !agedRefs) {
        look.problems.push_back("no age data");
        return look;
    }
    const auto age = propset::parse(*aged);
    const auto refs = reflist::parse(*agedRefs);
    look.category = number(age, "activecat");
    look.age = number(age, "age");
    look.gender = number(age, "gender");
    if (const auto it = age.find("skincolor"); it != age.end()) {
        look.skinTone = it->second;
    }
    look.stretch = stretchOf(age);
    look.skeleton = skeletonName(assets, age, refs, look.age, look.problems);
    // Counts come from the file. Each list and entry has its own properties, so a set can't have
    // more than it has properties (a broken file could claim billions).
    const auto atMost = [](std::uint32_t n, std::size_t properties) {
        return static_cast<std::uint32_t>(std::min<std::size_t>(n, properties));
    };
    const std::uint32_t lists = atMost(number(age, "listcnt"), age.size());
    for (std::uint32_t i = 0; i < lists; ++i) {
        // Each list holds the parts of one kind for one category: key (category << 16) | part.
        const std::uint32_t key = number(age, "lk" + std::to_string(i));
        // The skin outfit (category 0x80) holds the Sim's bald head and bare top and bottom (a toddler's
        // bare body): the skin under hair and clothes.
        const std::uint32_t kind = key & 0xFFFF;
        const bool skin = (key >> 16) == kSkinCategory &&
                          (kind == kHairPart || kind == kTopPart || kind == kBodyPart || kind == kBottomPart);
        // The game lays a 0x800 list over the skin whenever the file has one: the head's over the bald
        // head, and the body's under tops, bottoms and full outfits alike.
        const bool plant = (key >> 16) == kPlantCategory && (kind == kHairPart || kind == kBodyPart);
        if ((key >> 16) != look.category && !skin && !plant) {
            continue;
        }
        const std::uint32_t entries = atMost(number(age, "ls" + std::to_string(i)), age.size());
        const bool lastOnly = !skin && !plant && wearsLastEntryOnly(kind);
        for (std::uint32_t j = lastOnly && entries > 0 ? entries - 1 : 0; j < entries; ++j) {
            const std::uint32_t index = number(age, "le" + std::to_string(i) + "_" + std::to_string(j));
            if (index >= refs.size() || refs[index].type != kPartProperties) {
                look.problems.push_back("list " + std::to_string(i) + ": no part property set");
                continue;
            }
            const auto& set = refs[index];
            const auto bytes = assets.skinResource(set);
            const auto setRefs = assets.skinResource({reflist::kType, set.group, set.instance, set.instanceHigh});
            if (!bytes || !setRefs) {
                look.problems.push_back("part property set not found");
                continue;
            }
            const auto props = propset::parse(*bytes);
            const auto links = reflist::parse(*setRefs);
            const std::uint32_t resource = number(props, "resourcekeyidx");
            if (resource >= links.size()) {
                continue;
            }
            const auto cres = assets.byKey(links[resource]);
            if (!cres) {
                look.problems.push_back("model of " + (props.count("name") ? props.at("name") : std::string("a part")) + " not found");
                continue;
            }
            SimPart part;
            part.part = key & 0xFFFF;
            try {
                const auto tree = sg::parseResourceNode(*cres);
                part.model = tree.nodes.empty() ? std::string() : resourceName(tree.nodes[0].name, "_cres");
            } catch (const std::exception& ex) {
                look.problems.push_back(ex.what());
                continue;
            }
            const std::uint32_t overrides = atMost(number(props, "numoverrides"), props.size());
            for (std::uint32_t k = 0; k < overrides; ++k) {
                const std::string n = "override" + std::to_string(k);
                const std::uint32_t m = number(props, n + "resourcekeyidx");
                const auto txmt = m < links.size() ? assets.byKey(links[m]) : std::nullopt;
                if (!txmt) {
                    continue;
                }
                try {
                    part.overrides.push_back({resourceName(sg::parseMaterial(*txmt).name, "_txmt"), props.at(n + "subset")});
                } catch (const std::exception& ex) {
                    look.problems.push_back(ex.what());
                }
            }
            if (skin) {
                if (!part.overrides.empty()) {
                    skinOf(look, part.part) = part.overrides[0].material;
                }
                continue;
            }
            if (plant) {
                if (!part.overrides.empty()) {
                    (part.part == kHairPart ? look.plantHead : look.plantBody) = part.overrides[0].material;
                }
                continue;
            }
            look.parts.push_back(std::move(part));
        }
    }
    return look;
}

std::string skinUnderClothes(const SimLook& look, bool separates) {
    if (separates) {
        return look.skinBottom;
    }
    return look.skinBody.empty() ? look.skinTop : look.skinBody;
}

namespace {

// A game material's base texture. Empty when the material is missing or has none.
std::string baseTexture(Assets& assets, const std::string& material) {
    const auto data = assets.byName(kTxmt, material + "_txmt");
    if (!data) {
        return {};
    }
    try {
        const auto m = sg::parseMaterial(*data);
        if (const auto it = m.properties.find("stdMatBaseTextureName"); it != m.properties.end()) {
            return it->second;
        }
    } catch (const std::exception& ex) {
        diag::warnOnce(std::string("Sims: a material that can't be read gives no texture: ") + ex.what());
    }
    return {};
}

} // namespace

namespace {

// The skin's texture, then a PlantSim's plant skin over it when it has one. `name` gets the layers'
// materials as the game names its composites ("_<skin>_<plant>").
std::vector<std::string> skinLayers(Assets& assets, const std::string& skin, const std::string& plant, std::string& name) {
    const std::string texture = skin.empty() ? std::string() : baseTexture(assets, skin);
    if (texture.empty()) {
        return {};
    }
    std::vector<std::string> layers{texture};
    name = "_" + skin;
    if (const std::string over = plant.empty() ? std::string() : baseTexture(assets, plant); !over.empty()) {
        layers.push_back(over);
        name += "_" + plant;
    }
    return layers;
}

// `material` drawn over the skin, named "_<skin>[_<plant>]_<material>" as the game names it.
std::string overSkin(SceneBuilder& builder, Assets& assets, const std::string& skin, const std::string& plant,
                     const std::string& material) {
    std::string name;
    const auto layers = skinLayers(assets, skin, plant, name);
    if (layers.empty()) {
        return material;
    }
    name += "_" + material;
    return builder.layeredMaterial(name, material, layers) >= 0 ? name : material;
}

} // namespace

std::string clothesOverSkin(SceneBuilder& builder, Assets& assets, const SimLook& look, const std::string& material) {
    return overSkin(builder, assets, skinUnderClothes(look), look.plantBody, material);
}

std::string hairOverScalp(SceneBuilder& builder, Assets& assets, const SimLook& look, const std::string& material) {
    return overSkin(builder, assets, look.skinHair, look.plantHead, material);
}

std::string separatesOverSkin(SceneBuilder& builder, Assets& assets, const SimLook& look, const std::string& top,
                              const std::string& bottom) {
    std::string name;
    auto layers = skinLayers(assets, skinUnderClothes(look, true), look.plantBody, name);
    const std::string topTexture = baseTexture(assets, top);
    if (layers.empty() || topTexture.empty()) {
        return {};
    }
    layers.push_back(topTexture);
    name += "_" + top + "_" + bottom;
    return builder.layeredMaterial(name, bottom, layers) >= 0 ? name : std::string();
}

std::string simSlotBone(const std::filesystem::path& characterFile, std::size_t slot) {
    const auto bones = simSlotBones(characterFile);
    return slot < bones.size() ? bones[slot] : std::string();
}

std::vector<std::string> simSlotBones(const std::filesystem::path& characterFile) {
    try {
        dbpf::Package package{characterFile};
        const dbpf::IndexEntry* definition = nullptr;
        for (const auto& e : package.entries()) {
            if (e.key.type == types::kObjectDefinition) {
                definition = &e;
            }
        }
        if (definition == nullptr) {
            return {};
        }
        const std::uint32_t slotsId = objd::parse(package.read(*definition)).field(objd::field::kSlotsId);
        const dbpf::IndexEntry *slots = nullptr, *names = nullptr;
        for (const auto& e : package.entries()) {
            if (e.key.type == types::kSlots && e.key.instance == slotsId) {
                slots = &e;
            } else if (e.key.type == types::kStringTable && e.key.instance == 0x90) {
                names = &e;
            }
        }
        if (slots == nullptr || names == nullptr) {
            return {};
        }
        const auto parsed = slot::parse(package.read(*slots));
        const auto table = str::parse(package.read(*names));
        const auto entries = table.forLanguage(1);
        std::vector<std::string> bones(parsed.containerCount());
        for (std::size_t slot = 0; slot < bones.size(); ++slot) {
            const std::int32_t b = parsed.containerBone(slot);
            if (b >= 0 && static_cast<std::size_t>(b) < entries.size()) {
                bones[slot] = lower(entries[static_cast<std::size_t>(b)]->value);
            }
        }
        return bones;
    } catch (const std::exception&) {
        return {};
    }
}

std::string standingIdle(Assets& assets, std::uint32_t age) {
    const std::uint8_t ageList = content::ageAnimationList(content::animation_list::kAdult, content::ageValue(age));
    for (const std::uint8_t list : {ageList, content::animation_list::kAdult}) {
        const auto entry = assets.objectString(content::kIdleSystemGroup, list, content::kStandingIdleEntry);
        const std::string name = entry ? content::animationName(*entry) : std::string();
        if (!name.empty() && assets.byName(anim::kType, name + "_anim")) {
            return name;
        }
    }
    return {};
}

std::string standingPose(Assets& assets, std::uint32_t age) {
    const char* own = age == kToddlerAge     ? "p-sitPose-slotRigged"
                      : age == kChildAge     ? "c-pose-neutral-stand"
                      : age == kTeenAge      ? "t-pose-neutral-stand"
                      : age == kElderAge     ? "e-pose-neutral-stand"
                      : age == kBabyAge      ? ""
                                             : "a-pose-neutral-stand";
    for (const std::string name : {std::string(own), std::string("a-pose-neutral-stand")}) {
        if (age != kBabyAge && !name.empty() && assets.byName(anim::kType, name + "_anim")) {
            return name;
        }
    }
    return {};
}

std::optional<anim::Animation> loadAnimation(Assets& assets, const std::string& name) {
    const auto data = assets.byName(anim::kType, name + "_anim");
    if (!data) {
        return std::nullopt;
    }
    try {
        return anim::parse(*data);
    } catch (const std::exception& ex) {
        assets.warnings.push_back("animation " + name + ": " + ex.what());
        return std::nullopt;
    }
}

Pose posedSkeleton(Assets& assets, const std::string& skeleton, const anim::Animation* animation, float ms) {
    Pose pose;
    const Rig rig = loadRig(assets, skeleton);
    // Not owned: the animation outlives the binding.
    const RigAnimation bound = bindAnimation(rig, std::shared_ptr<const anim::Animation>(std::shared_ptr<void>(), animation));
    std::vector<Transform> world;
    poseRig(rig, animation ? &bound : nullptr, ms, world);
    for (std::size_t i = 0; i < rig.nodes.size(); ++i) {
        if (!rig.nodes[i].name.empty()) {
            pose.try_emplace(rig.nodes[i].name, world[i]);
        }
    }
    return pose;
}

} // namespace ts2::scene
