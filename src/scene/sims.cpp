#include "scene/sims.hpp"

#include "formats/dbpf.hpp"
#include "formats/objd.hpp"
#include "formats/propset.hpp"
#include "formats/reference_list.hpp"
#include "formats/resource_types.hpp"
#include "formats/slot.hpp"
#include "formats/str.hpp"
#include "scene/rig.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace sg = ts2::scenegraph;

namespace ts2::scene {

namespace {

constexpr std::uint32_t kAgeData = 0xAC598EAC;
constexpr std::uint32_t kPartProperties = 0xEBCF3E27;
constexpr std::uint32_t kLocalGroup = 0xFFFFFFFF;
constexpr std::uint32_t kTxmt = 0x49596978;

std::uint32_t number(const std::map<std::string, std::string>& p, const std::string& key) {
    const auto it = p.find(key);
    return it == p.end() ? 0 : static_cast<std::uint32_t>(std::stoul(it->second, nullptr, 0));
}

// A resource's own name (its root block's), without the type suffix ("_cres", "_txmt").
std::string resourceName(const std::string& name, const char* suffix) {
    const std::string s = suffix;
    return name.size() > s.size() && lower(name.substr(name.size() - s.size())) == s ? name.substr(0, name.size() - s.size()) : name;
}

} // namespace

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
    if (const std::uint32_t s = number(age, "skeletonkeyidx"); s < refs.size()) {
        if (const auto cres = assets.byKey(refs[s])) {
            try {
                const auto tree = sg::parseResourceNode(*cres);
                look.skeleton = tree.nodes.empty() ? std::string() : resourceName(tree.nodes[0].name, "_cres");
            } catch (const std::exception& ex) {
                look.problems.push_back(ex.what());
            }
        }
    }
    // Counts come from the file; each list and entry has its own properties, so a set can't have
    // more than it has properties (a broken file could claim billions).
    const auto atMost = [](std::uint32_t n, std::size_t properties) {
        return static_cast<std::uint32_t>(std::min<std::size_t>(n, properties));
    };
    const std::uint32_t lists = atMost(number(age, "listcnt"), age.size());
    for (std::uint32_t i = 0; i < lists; ++i) {
        // Each list holds the parts of one kind for one category: key (category << 16) | part.
        const std::uint32_t key = number(age, "lk" + std::to_string(i));
        if ((key >> 16) != look.category) {
            continue;
        }
        const std::uint32_t entries = atMost(number(age, "ls" + std::to_string(i)), age.size());
        for (std::uint32_t j = 0; j < entries; ++j) {
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
            look.parts.push_back(std::move(part));
        }
    }
    return look;
}

std::string skinUnderClothes(const SimLook& look, bool separates) {
    const char* age = look.age == 8 ? "a" : look.age == 4 ? "t" : nullptr;
    const char* gender = look.gender == 1 ? "f" : look.gender == 2 ? "m" : nullptr;
    // The game's own skin tones are 00000001-... to 00000005-...
    const std::string& t = look.skinTone;
    if (!age || !gender || t.size() != 36 || t.compare(0, 7, "0000000") != 0 || t[7] < '1' || t[7] > '5' ||
        t.compare(8, 28, "-0000-0000-0000-000000000000") != 0) {
        return {};
    }
    return std::string(age) + gender + (separates ? "bottomnaked_nude_s" : "topnaked_nude_s") + t[7];
}

namespace {

// A game material's base texture; empty when the material is missing or has none.
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
    } catch (const std::exception&) {
    }
    return {};
}

} // namespace

std::string clothesOverSkin(SceneBuilder& builder, Assets& assets, const SimLook& look, const std::string& material) {
    const std::string skin = skinUnderClothes(look);
    const std::string texture = skin.empty() ? std::string() : baseTexture(assets, skin);
    if (texture.empty()) {
        return material;
    }
    const std::string name = "_" + skin + "_" + material;
    return builder.layeredMaterial(name, material, {texture}) >= 0 ? name : material;
}

std::string separatesOverSkin(SceneBuilder& builder, Assets& assets, const SimLook& look, const std::string& top,
                              const std::string& bottom) {
    const std::string skin = skinUnderClothes(look, true);
    const std::string skinTexture = skin.empty() ? std::string() : baseTexture(assets, skin);
    const std::string topTexture = baseTexture(assets, top);
    if (skinTexture.empty() || topTexture.empty()) {
        return {};
    }
    const std::string name = "_" + skin + "_" + top + "_" + bottom;
    return builder.layeredMaterial(name, bottom, {skinTexture, topTexture}) >= 0 ? name : std::string();
}

std::string simSlotBone(const std::filesystem::path& characterFile, std::size_t slot) {
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
        const std::int32_t b = slot::parse(package.read(*slots)).containerBone(slot);
        const auto table = str::parse(package.read(*names));
        const auto entries = table.forLanguage(1);
        return b >= 0 && static_cast<std::size_t>(b) < entries.size() ? lower(entries[static_cast<std::size_t>(b)]->value)
                                                                        : std::string();
    } catch (const std::exception&) {
        return {};
    }
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
