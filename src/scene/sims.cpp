#include "scene/sims.hpp"

#include "formats/dbpf.hpp"
#include "formats/objd.hpp"
#include "formats/propset.hpp"
#include "formats/reference_list.hpp"
#include "formats/resource_types.hpp"
#include "formats/slot.hpp"
#include "formats/str.hpp"

#include <cmath>
#include <map>

namespace sg = ts2::scenegraph;

namespace ts2::scene {

namespace {

constexpr std::uint32_t kAgeData = 0xAC598EAC;
constexpr std::uint32_t kPartProperties = 0xEBCF3E27;
constexpr std::uint32_t kLocalGroup = 0xFFFFFFFF;
constexpr std::uint32_t kCres = 0xE519C933;
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
    const std::uint32_t lists = number(age, "listcnt");
    for (std::uint32_t i = 0; i < lists; ++i) {
        // Each list holds the parts of one kind for one category: key (category << 16) | part.
        const std::uint32_t key = number(age, "lk" + std::to_string(i));
        if ((key >> 16) != look.category) {
            continue;
        }
        const std::uint32_t entries = number(age, "ls" + std::to_string(i));
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
            const std::uint32_t overrides = number(props, "numoverrides");
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

std::string skinUnderClothes(const SimLook& look) {
    const char* age = look.age == 8 ? "a" : look.age == 4 ? "t" : nullptr;
    const char* gender = look.gender == 1 ? "f" : look.gender == 2 ? "m" : nullptr;
    // The game's own skin tones are 00000001-... to 00000005-...
    const std::string& t = look.skinTone;
    if (!age || !gender || t.size() != 36 || t.compare(0, 7, "0000000") != 0 || t[7] < '1' || t[7] > '5' ||
        t.compare(8, 28, "-0000-0000-0000-000000000000") != 0) {
        return {};
    }
    return std::string(age) + gender + "topnaked_nude_s" + t[7];
}

std::string clothesOverSkin(SceneBuilder& builder, Assets& assets, const SimLook& look, const std::string& material) {
    const std::string skin = skinUnderClothes(look);
    if (skin.empty()) {
        return material;
    }
    const auto data = assets.byName(kTxmt, skin + "_txmt");
    if (!data) {
        return material;
    }
    std::string texture;
    try {
        const auto m = sg::parseMaterial(*data);
        if (const auto it = m.properties.find("stdMatBaseTextureName"); it != m.properties.end()) {
            texture = it->second;
        }
    } catch (const std::exception&) {
    }
    if (texture.empty()) {
        return material;
    }
    const std::string name = "_" + skin + "_" + material;
    return builder.layeredMaterial(name, material, {texture}) >= 0 ? name : material;
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
    const auto cres = assets.byName(kCres, skeleton + "_cres");
    if (!cres) {
        return pose;
    }
    sg::ResourceNode tree;
    try {
        tree = sg::parseResourceNode(*cres);
    } catch (const std::exception&) {
        return pose;
    }
    std::map<std::string, std::array<float, 4>> rotations;
    std::map<std::string, std::array<float, 3>> translations;
    if (animation != nullptr) {
        constexpr float kRadians = 3.14159265358979f / 180.0f;
        for (const auto& c : animation->channels) {
            if (c.components.size() != 3) {
                continue;
            }
            const float x = c.components[0].sample(ms), y = c.components[1].sample(ms), z = c.components[2].sample(ms);
            if (c.kind() == anim::ChannelKind::Translation) {
                translations[lower(c.name)] = {x, y, z};
            } else if (c.kind() == anim::ChannelKind::Rotation) {
                // x first, then y, then z: q = qz * qy * qx.
                const float cx = std::cos(x * kRadians / 2), sx = std::sin(x * kRadians / 2);
                const float cy = std::cos(y * kRadians / 2), sy = std::sin(y * kRadians / 2);
                const float cz = std::cos(z * kRadians / 2), sz = std::sin(z * kRadians / 2);
                rotations[lower(c.name)] = {sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz,
                                            cx * cy * sz - sx * sy * cz, cx * cy * cz + sx * sy * sz};
            }
        }
    }
    std::vector<bool> seen(tree.nodes.size(), false);
    auto walk = [&](auto&& self, std::uint32_t index, const Transform& parent, int depth) -> void {
        if (index >= tree.nodes.size() || seen[index] || depth > 64) {
            return;
        }
        seen[index] = true;
        const auto& node = tree.nodes[index];
        const std::string name = lower(node.name);
        auto t = node.translation;
        auto q = node.rotation;
        if (const auto it = translations.find(name); it != translations.end()) {
            t = it->second;
        }
        if (const auto it = rotations.find(name); it != rotations.end()) {
            q = it->second;
        }
        const Transform world = parent * Transform::of(t, q);
        if (node.kind == sg::SceneNode::Kind::Transform && !name.empty()) {
            pose.try_emplace(name, world);
        }
        for (const auto c : node.children) {
            self(self, c, world, depth + 1);
        }
    };
    if (!tree.nodes.empty()) {
        walk(walk, 0, Transform{}, 0);
    }
    return pose;
}

} // namespace ts2::scene
