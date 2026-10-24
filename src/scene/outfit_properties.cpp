#include "scene/outfit_properties.hpp"

#include "diag/log.hpp"
#include "formats/propset.hpp"
#include "formats/reference_list.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <optional>
#include <string_view>

namespace ts2::scene {

namespace sg = scenegraph;

namespace {

constexpr std::uint32_t kAgeData = 0xAC598EAC;
constexpr std::uint32_t kPartProperties = 0xEBCF3E27;
constexpr std::uint32_t kShape = 0xFC6EB1F7;
constexpr std::uint32_t kTxmt = 0x49596978;
constexpr std::uint32_t kSkinTone = 0x2C1FD8A1, kMakeup = 0x0C1FE246;
constexpr std::uint32_t kLocalGroup = 0xFFFFFFFF;
constexpr std::uint32_t kFacePart = 2, kHairPart = 1, kEverydayCategory = 1;
// Properties the face's overlays answer (3-6, 9-11) and the hair one (7).
constexpr std::array<std::uint16_t, 7> kFaceProperties{3, 4, 5, 6, 9, 10, 11};
constexpr std::uint16_t kHairFlagProperty = 7, kAfterMakeupProperty = 9, kOtherSpeciesProperty = 10;
// A face material names at most this many overlays (the game reads the count from the material).
constexpr std::uint32_t kMaxOverlays = 64;
constexpr std::uint32_t kMaxListEntries = 256;

using Properties = std::map<std::string, std::string>;

std::uint32_t number(const Properties& p, const std::string& key) {
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
    std::uint32_t v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v, base);
    return v;
}

std::string lowerCase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// The Sim's age data and its reference list.
struct AgeData {
    Properties properties;
    std::vector<sg::Link> refs;
};

// The entries (reference list indices) of the last outfit list with `key`: (category << 16) | part
// for the lists whose key names a part in its low byte, else (category << 16) | the key's second
// byte for the lists that only name one there (`byte2`).
std::vector<std::uint32_t> listEntries(const AgeData& age, std::uint32_t key, bool byte2) {
    std::vector<std::uint32_t> entries;
    const std::uint32_t lists = std::min<std::uint32_t>(number(age.properties, "listcnt"), static_cast<std::uint32_t>(age.properties.size()));
    for (std::uint32_t i = 0; i < lists; ++i) {
        const std::uint32_t lk = number(age.properties, "lk" + std::to_string(i));
        const bool partInLowByte = (lk & 0xFF) != 0;
        const std::uint32_t listKey = partInLowByte ? lk : (lk & 0xFFFF0000) | ((lk >> 8) & 0xFF);
        if (partInLowByte == byte2 || (!partInLowByte && (lk & 0xFF00) == 0) || listKey != key) {
            continue;
        }
        entries.clear();
        const std::uint32_t n = std::min(number(age.properties, "ls" + std::to_string(i)), kMaxListEntries);
        for (std::uint32_t j = 0; j < n; ++j) {
            entries.push_back(number(age.properties, "le" + std::to_string(i) + "_" + std::to_string(j)));
        }
    }
    return entries;
}

// An outfit part's property set (GZPS) and its reference list, from list entry `index`.
std::optional<std::pair<Properties, std::vector<sg::Link>>> part(Assets& assets, const AgeData& age, std::uint32_t index) {
    if (index >= age.refs.size() || age.refs[index].type != kPartProperties) {
        return std::nullopt;
    }
    const auto& set = age.refs[index];
    const auto bytes = assets.skinResource(set);
    const auto links = assets.skinResource({reflist::kType, set.group, set.instance, set.instanceHigh});
    if (!bytes || !links) {
        return std::nullopt;
    }
    try {
        return std::pair{propset::parse(*bytes), reflist::parse(*links)};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// The material of the "face" mesh group of a part's model. Empty when it has none.
std::string faceMaterial(Assets& assets, const Properties& props, const std::vector<sg::Link>& links) {
    const std::uint32_t resource = number(props, "resourcekeyidx");
    const auto cres = resource < links.size() ? assets.byKey(links[resource]) : std::nullopt;
    if (!cres) {
        return {};
    }
    try {
        for (const auto& link : sg::parseResourceNode(*cres).container.links) {
            const auto shape = link.type == kShape ? assets.byKey(link) : std::nullopt;
            if (!shape) {
                continue;
            }
            for (const auto& p : sg::parseShape(*shape).parts) {
                if (p.group == "face" && !p.material.empty()) {
                    return p.material;
                }
            }
        }
    } catch (const std::exception& ex) {
        diag::warnOnce(std::string("outfits: a face shape that can't be read gives no face material: ") + ex.what());
    }
    return {};
}

// The overlay families (GUIDs) a face material lays over the skin, in order.
std::vector<std::string> overlayFamilies(Assets& assets, const std::string& material) {
    std::vector<std::string> families;
    auto data = assets.byName(kTxmt, material + "_txmt");
    if (!data) {
        data = assets.byName(kTxmt, material);
    }
    if (!data) {
        return families;
    }
    try {
        const auto m = sg::parseMaterial(*data);
        const auto count = std::min(number(m.properties, "cafNumOverlays"), kMaxOverlays);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto it = m.properties.find("cafOverlay" + std::to_string(i));
            if (it != m.properties.end()) {
                families.push_back(lowerCase(it->second.substr(0, it->second.find(','))));
            }
        }
    } catch (const std::exception& ex) {
        diag::warnOnce(std::string("outfits: a material that can't be read gives no overlays: ") + ex.what());
    }
    return families;
}

} // namespace

const std::vector<OutfitProperties::Overlay>* OutfitProperties::overlays(const std::string& family) {
    if (!catalogRead_) {
        catalogRead_ = true;
        for (const std::uint32_t type : {kSkinTone, kMakeup}) {
            assets_.forEachSkinResource(type, [&](const std::vector<std::uint8_t>& bytes) {
                try {
                    const auto p = propset::parse(bytes);
                    if (const auto it = p.find("family"); it != p.end()) {
                        overlaysByFamily_[lowerCase(it->second)].push_back({number(p, "subtype"), number(p, "bin"), number(p, "category")});
                    }
                } catch (const std::exception& ex) {
                    diag::warnOnce(std::string("outfits: an overlay description that can't be read is left out: ") + ex.what());
                }
            });
        }
    }
    const auto it = overlaysByFamily_.find(family);
    return it != overlaysByFamily_.end() ? &it->second : nullptr;
}

std::int16_t OutfitProperties::get(const std::filesystem::path& characterFile, std::uint16_t category, std::uint16_t property) {
    const Assets::CharacterInUse inUse(assets_, characterFile);
    return answerFor(characterFile.string(), category, property);
}

std::int16_t OutfitProperties::get(std::uint32_t objectGroup, std::uint16_t category, std::uint16_t property) {
    const Assets::CharacterInUse inUse(assets_, objectGroup);
    return answerFor("group " + std::to_string(objectGroup), category, property);
}

// The answer for the character in use (see Assets::useCharacterFile).
std::int16_t OutfitProperties::answerFor(const std::string& sim, std::uint16_t category, std::uint16_t property) {
    auto& answers = answers_[sim];
    const std::uint32_t key = static_cast<std::uint32_t>(category) << 16 | property;
    if (const auto it = answers.find(key); it != answers.end()) {
        return it->second;
    }
    const bool asked = std::find(kFaceProperties.begin(), kFaceProperties.end(), property) != kFaceProperties.end() ||
                       property == kHairFlagProperty;
    if (!asked) {
        return 0;
    }
    AgeData age;
    const auto agedBytes = assets_.byKey({kAgeData, kLocalGroup, 1, 0});
    const auto refBytes = assets_.byKey({reflist::kType, kLocalGroup, 1, 0});
    try {
        if (agedBytes && refBytes) {
            age.properties = propset::parse(*agedBytes);
            age.refs = reflist::parse(*refBytes);
        }
    } catch (const std::exception& ex) {
        diag::warnOnce(std::string("outfits: a Sim's age data that can't be read is left empty: ") + ex.what());
    }
    const std::uint32_t species = number(age.properties, "species");
    const bool human = species == 0 || species == 1;
    std::int16_t answer = 0;
    if (property == kHairFlagProperty) {
        // The last hair part of the category: its part flags' bit 1.
        auto entries = listEntries(age, static_cast<std::uint32_t>(category) << 16 | kHairPart, true);
        if (entries.empty()) {
            entries = listEntries(age, static_cast<std::uint32_t>(category) << 16 | kHairPart, false);
        }
        if (const auto p = entries.empty() ? std::nullopt : part(assets_, age, entries.back())) {
            answer = (number(p->first, "flags") & 0x02) != 0 ? 1 : 0;
        }
    } else if (property == kOtherSpeciesProperty) {
        // A pet's body options (inferred: the age data's petbodyoption entries).
        const auto first = age.properties.lower_bound("petbodyoption");
        const bool hasOptions = first != age.properties.end() && first->first.rfind("petbodyoption", 0) == 0;
        answer = !human && hasOptions ? 1 : 0;
    } else if (property != kAfterMakeupProperty && human) {
        // Every face property of the category starts at 0. The everyday face's overlays then set
        // theirs, until the one asked for is set.
        for (const std::uint16_t p : kFaceProperties) {
            answers[static_cast<std::uint32_t>(category) << 16 | p] = 0;
        }
        std::string material;
        for (const std::uint32_t entry : listEntries(age, kEverydayCategory << 16 | kFacePart, false)) {
            if (const auto p = part(assets_, age, entry)) {
                if (auto m = faceMaterial(assets_, p->first, p->second); !m.empty()) {
                    material = std::move(m);
                }
            }
        }
        bool found = false;
        auto set = [&](std::uint16_t p) {
            answers[static_cast<std::uint32_t>(category) << 16 | p] = 1;
            found = found || p == property;
        };
        for (const auto& family : material.empty() ? std::vector<std::string>{} : overlayFamilies(assets_, material)) {
            const auto* items = overlays(family);
            for (std::size_t i = 0; items != nullptr && i < items->size() && !found; ++i) {
                const Overlay& o = (*items)[i];
                const bool inCategory = o.category == 0 || (o.category & category) != 0;
                switch (o.subtype) {
                case 0: set(3); break;
                case 2: case 6: case 7: set(5); break;
                case 4: set(6); break;
                case 5:
                    if (inCategory) {
                        if (o.bin == 0x0F) set(4);
                        if (o.bin == 0x10) set(9);
                        set(11);
                    }
                    break;
                case 8:
                    if (inCategory) set(11);
                    break;
                default: break;
                }
            }
            if (found) {
                break;
            }
        }
        answer = answers[key];
    }
    answers[key] = answer;
    return answer;
}

} // namespace ts2::scene
