#include "sim/character_look.hpp"

#include "formats/hash.hpp"
#include "formats/propset.hpp"
#include "formats/reference_list.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ts2::sim {

namespace {

constexpr std::uint32_t kAgeDataType = 0xAC598EAC;
constexpr std::uint32_t kPartType = 0xEBCF3E27;
constexpr std::uint32_t kResourceListType = 0xAC506764;
constexpr std::uint32_t kModelType = 0xE519C933;
constexpr std::uint32_t kFaceShapeType = 0xCCCEF852;
constexpr std::uint32_t kOwnGroup = 0xFFFFFFFF;
constexpr std::uint32_t kResourceListMagic = 0xDEADBEEF;
constexpr std::uint32_t kResourceListVersion = 2;  // keys of four numbers
// The skeleton every baby's age data names.
constexpr dbpf::ResourceKey kBabySkeleton{kModelType, 0x1C0532FA, 0xFFF2F1EC, 0x32F5382F};

// The ages a new Sim's look is made for, in the order the game makes them: the age number in the
// model's name and the age flag.
struct Age {
    int number;
    std::uint32_t flag;
};
constexpr std::array<Age, 7> kAges{{{5, 0x20}, {0, 0x01}, {1, 0x02}, {2, 0x04}, {7, 0x40}, {3, 0x08}, {4, 0x10}}};

constexpr std::uint32_t kEveryday = 0x1, kSkin = 0x80;
constexpr std::uint32_t kHairPart = 0x1, kFacePart = 0x2, kBodyPart = 0x8;

propset::Property number(const char* name, std::uint32_t value) {
    return {propset::kUintType, name, value, {}, {}};
}
propset::Property signedNumber(const char* name, std::int32_t value) {
    return {propset::kIntType, name, static_cast<std::uint32_t>(value), {}, {}};
}
propset::Property text(const std::string& name, const std::string& value) {
    return {propset::kStringType, name, 0, value, {}};
}
propset::Property real(const char* name, float value) {
    return {propset::kFloatType, name, std::bit_cast<std::uint32_t>(value), {}, {}};
}

const std::string kNullGuid = "00000000-0000-0000-0000-000000000000";
// An elder's hair tone, whatever its hair gene, as the game greys it.
const std::string kGreyHair = "00000005-0000-0000-0000-000000000000";
constexpr std::uint32_t kElderFlag = 0x10;

std::vector<std::uint8_t> resourceList(const std::vector<dbpf::ResourceKey>& keys) {
    std::vector<std::uint8_t> out;
    const auto put = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    };
    put(kResourceListMagic);
    put(kResourceListVersion);
    put(static_cast<std::uint32_t>(keys.size()));
    for (const dbpf::ResourceKey& key : keys) {
        put(key.type);
        put(key.group);
        put(key.instance);
        put(key.instanceHigh);
    }
    return out;
}

void add(dbpf::PackageImage& package, const dbpf::ResourceKey& key, std::span<const std::uint8_t> body) {
    package.resources.push_back(dbpf::storeResource(key, body));
    package.dataOrder.push_back(package.resources.size() - 1);
}

} // namespace

void addFaceShapeRecord(dbpf::PackageImage& package, std::span<const std::uint8_t> record) {
    add(package, {kFaceShapeType, kOwnGroup, 1, 0}, record);
}

namespace {

bool isListProperty(const std::string& name) {
    return name.size() > 2 && (name.starts_with("ls") || name.starts_with("lk") || name.starts_with("le")) &&
           std::isdigit(static_cast<unsigned char>(name[2]));
}

// std::find over 16-byte keys stops newer Visual Studio libraries from compiling, so keys are
// compared one by one.
std::vector<dbpf::ResourceKey>::iterator findKey(std::vector<dbpf::ResourceKey>& keys, const dbpf::ResourceKey& key) {
    return std::find_if(keys.begin(), keys.end(), [&](const dbpf::ResourceKey& k) { return k == key; });
}

} // namespace

std::vector<std::uint8_t> rewriteOutfitLook(std::span<const std::uint8_t> characterFile, const OutfitLook& look) {
    constexpr dbpf::ResourceKey kAgeData{kAgeDataType, kOwnGroup, 1, 0}, kAgeDataList{kResourceListType, kOwnGroup, 1, 0};
    dbpf::PackageImage package = dbpf::readImage(characterFile);
    dbpf::Package reader(characterFile);
    std::optional<std::vector<propset::Property>> properties;
    std::optional<std::vector<scenegraph::Link>> referenceList;
    for (const auto& entry : reader.entries()) {
        if (entry.key == kAgeData) {
            properties = propset::readBinary(reader.read(entry));
        } else if (entry.key == kAgeDataList) {
            referenceList = reflist::parse(reader.read(entry));
        }
    }
    if (!properties) {
        throw std::runtime_error("the character file has no age data that can be read");
    }
    // The age data's list indices point into this list, which is written with it.
    if (!referenceList) {
        throw std::runtime_error("the character file has no resource list for its age data");
    }
    const std::vector<scenegraph::Link>& references = *referenceList;
    // The entries no outfit list uses come first and stay.
    std::size_t kept = references.size();
    for (const auto& p : *properties) {
        if (p.name.starts_with("le") && isListProperty(p.name) && p.type == propset::kUintType) {
            kept = std::min<std::size_t>(kept, p.word);
        }
    }
    std::vector<dbpf::ResourceKey> keys;
    for (std::size_t i = 0; i < kept; ++i) {
        keys.push_back({references[i].type, references[i].group, references[i].instance, references[i].instanceHigh});
    }
    // A file can name its skeleton twice, with and without the key's high instance (as after a
    // pregnancy), the age data pointing past the kept entries at the second. The game's rewrite keeps
    // one: the kept entry of that skeleton takes the key the age data names.
    for (const auto& p : *properties) {
        if (p.name != "skeletonkeyidx" || p.type != propset::kUintType || p.word < kept || p.word >= references.size()) {
            continue;
        }
        const auto& named = references[p.word];
        const dbpf::ResourceKey skeleton{named.type, named.group, named.instance, named.instanceHigh};
        if (findKey(keys, skeleton) != keys.end()) {
            continue;
        }
        for (auto& key : keys) {
            if (key.type == skeleton.type && key.group == skeleton.group && key.instance == skeleton.instance) {
                key = skeleton;
                break;
            }
        }
    }
    std::vector<propset::Property> out;
    for (propset::Property p : *properties) {
        if (isListProperty(p.name)) {
            continue;
        }
        if (p.name == "listcnt") {
            p.word = static_cast<std::uint32_t>(look.lists.size());
        } else if (const auto it = look.properties.find(p.name); it != look.properties.end()) {
            if (p.type == propset::kStringType) {
                p.text = it->second;
            } else if (p.type == propset::kUintType) {
                p.word = static_cast<std::uint32_t>(std::stoul(it->second));
            }
        }
        out.push_back(std::move(p));
    }
    std::size_t n = 0;
    for (const auto& [key, parts] : look.lists) {
        const std::string i = std::to_string(n++);
        out.push_back(text("ls" + i, std::to_string(parts.size())));
        out.push_back(text("lk" + i, std::to_string(key)));
        for (std::size_t j = 0; j < parts.size(); ++j) {
            auto at = findKey(keys, parts[j]);
            if (at == keys.end()) {
                keys.push_back(parts[j]);
                at = keys.end() - 1;
            }
            out.push_back({propset::kUintType, "le" + i + "_" + std::to_string(j), static_cast<std::uint32_t>(at - keys.begin()), {}, {}});
        }
    }
    // The skeleton is named by its place in the list, which the new list may have moved. It goes on
    // naming the same skeleton. A list that holds it nowhere (a file without a kept entry for it) is
    // refused, as how the game places it then is not known yet.
    for (auto& p : out) {
        if (p.name != "skeletonkeyidx" || p.type != propset::kUintType || p.word < kept || p.word >= references.size()) {
            continue;
        }
        const auto& old = references[p.word];
        const dbpf::ResourceKey skeleton{old.type, old.group, old.instance, old.instanceHigh};
        const auto at = findKey(keys, skeleton);
        if (at == keys.end()) {
            throw std::runtime_error("the character file's skeleton has no place in its new resource list");
        }
        p.word = static_cast<std::uint32_t>(at - keys.begin());
    }
    for (auto& resource : package.resources) {
        if (resource.key == kAgeData) {
            resource = dbpf::storeResource(kAgeData, propset::writeBinary(out));
        } else if (resource.key == kAgeDataList) {
            resource = dbpf::storeResource(kAgeDataList, resourceList(keys));
        }
    }
    // The portrait of the Sim's age and its shadow show the old look. The game draws missing ones
    // anew, as its own save draws these.
    constexpr std::uint32_t kPortraitType = 0x856DDBAC;
    std::uint32_t age = 0;
    for (const auto& p : *properties) {
        if (p.name == "age" && p.type == propset::kUintType) {
            age = p.word;
        }
    }
    const auto stale = [&](const dbpf::ResourceKey& key) {
        return age != 0 && key.type == kPortraitType && key.group == kOwnGroup && key.instanceHigh == 0 && (key.instance == age || key.instance == age << 8);
    };
    // The compression directory keeps its place among the entries and the data that stay.
    std::vector<std::size_t> renumber(package.resources.size());
    std::vector<dbpf::StoredResource> remaining;
    std::size_t indexPosition = package.directoryIndexPosition;
    for (std::size_t i = 0; i < package.resources.size(); ++i) {
        renumber[i] = stale(package.resources[i].key) ? SIZE_MAX : remaining.size();
        if (!stale(package.resources[i].key)) {
            remaining.push_back(std::move(package.resources[i]));
        } else if (i < package.directoryIndexPosition) {
            --indexPosition;
        }
    }
    package.resources = std::move(remaining);
    package.directoryIndexPosition = indexPosition;
    std::vector<std::size_t> order;
    std::size_t dataPosition = package.directoryDataPosition;
    for (std::size_t at = 0; at < package.dataOrder.size(); ++at) {
        const std::size_t i = package.dataOrder[at];
        if (i < renumber.size() && renumber[i] != SIZE_MAX) {
            order.push_back(renumber[i]);
        } else if (at < package.directoryDataPosition) {
            --dataPosition;
        }
    }
    package.dataOrder = std::move(order);
    package.directoryDataPosition = dataPosition;
    std::erase_if(package.directoryOrder, stale);
    return dbpf::writeImage(package);
}

dbpf::ResourceKey newFaceModelKey(int age, int part) {
    const hash::ScenegraphKey key = hash::scenegraphKey("age" + std::to_string(age) + "_" + std::to_string(part) + "_cres");
    return {kModelType, kOwnGroup, key.instance, key.instanceHigh};
}

namespace {

// A skin part (GZPS) of the Sim's own face, as the game writes one. A mesh overlay worn on the face
// is written the same way with the overlay's outfit categories.
std::vector<std::uint8_t> ownFacePart(std::uint32_t ageFlag, std::uint32_t gender, const std::string& skinTone, const std::string& hairTone,
                                      std::uint32_t category = 0) {
    const std::vector<propset::Property> part{number("version", 6),
                                              number("product", 0xFFFF),
                                              number("age", ageFlag),
                                              number("gender", gender),
                                              number("species", 1),
                                              number("parts", kFacePart),
                                              number("outfit", kFacePart),
                                              number("flags", 1),
                                              text("name", ""),
                                              text("creator", kNullGuid),
                                              text("family", kNullGuid),
                                              real("genetic", 0.0f),
                                              signedNumber("priority", 0),
                                              text("type", "skin"),
                                              text("skintone", skinTone),
                                              text("hairtone", ageFlag == kElderFlag ? kGreyHair : hairTone),
                                              number("category", category),
                                              number("shoe", 0),
                                              number("fitness", 0),
                                              number("resourcekeyidx", 0),
                                              number("shapekeyidx", 1),
                                              number("numoverrides", 0)};
    return propset::writeBinary(part);
}

// The age data's fields before its lists. A newborn's and a made Sim's differ only in the product.
std::vector<propset::Property> ageHead(std::uint32_t product, std::uint32_t ageFlag, std::uint32_t gender, float stretch, const std::string& skin,
                                       const std::string& hair, const std::string& eyes, std::size_t lists) {
    return {number("product", product),
            number("age", ageFlag),
            number("gender", gender),
            number("species", 1),
            number("allowedparts", 0x3F),
            real("stretch", stretch),
            text("skincolor", skin),
            text("haircolor", hair),
            text("eyecolor", eyes),
            number("skeletonkeyidx", 2),
            number("fitness", 0),
            number("validparts", 0xFFFFFFFF),
            number("activecat", kEveryday),
            text("creatorid", kNullGuid),
            number("listcnt", static_cast<std::uint32_t>(lists))};
}

} // namespace

void addMadeLookRecords(dbpf::PackageImage& package, const MadeAgeLook& look) {
    const dbpf::ResourceKey ownFace{kPartType, kOwnGroup, 1, 0};
    // The parts of the mesh overlays the face wears, after the face's own.
    std::vector<dbpf::ResourceKey> ownOverlays;
    for (std::size_t k = 0; k < look.overlayCategories.size(); ++k) {
        ownOverlays.push_back({kPartType, kOwnGroup, static_cast<std::uint32_t>(k + 2), 0});
    }
    std::vector<dbpf::ResourceKey> used{newFaceModelKey(look.ageNumber), {}, look.skeleton};
    const auto indexOf = [&](const dbpf::ResourceKey& key) {
        const auto it = std::find_if(used.begin() + 3, used.end(), [&](const dbpf::ResourceKey& k) { return k == key; });
        if (it != used.end()) {
            return static_cast<std::uint32_t>(it - used.begin());
        }
        used.push_back(key);
        return static_cast<std::uint32_t>(used.size() - 1);
    };
    std::vector<std::pair<std::uint32_t, std::vector<std::uint32_t>>> lists;
    for (const auto& [key, parts] : look.lists) {
        if (parts.empty()) {
            continue;
        }
        std::vector<std::uint32_t> entries;
        for (const auto& part : parts) {
            entries.push_back(indexOf((key & 0xFFFF) == kFacePart ? ownFace : part));
        }
        if ((key & 0xFFFF) == kFacePart) {
            for (const auto& overlay : ownOverlays) {
                entries.push_back(indexOf(overlay));
            }
        }
        lists.emplace_back(key, std::move(entries));
    }
    std::vector<propset::Property> age =
        ageHead(0xFFFF, look.ageFlag, look.gender, look.stretch, look.skinTone, look.hairTone, look.eyeColour, lists.size());
    for (std::size_t i = 0; i < lists.size(); ++i) {
        const std::string n = std::to_string(i);
        age.push_back(text("ls" + n, std::to_string(lists[i].second.size())));
        age.push_back(text("lk" + n, std::to_string(lists[i].first)));
        for (std::size_t j = 0; j < lists[i].second.size(); ++j) {
            age.push_back({propset::kUintType, "le" + n + "_" + std::to_string(j), lists[i].second[j], {}, {}});
        }
    }
    add(package, {kAgeDataType, kOwnGroup, 1, 0}, propset::writeBinary(age));
    add(package, {kResourceListType, kOwnGroup, 1, 0}, resourceList(used));
    add(package, ownFace, ownFacePart(look.ageFlag, look.gender, look.skinTone, look.hairTone));
    for (std::size_t k = 0; k < ownOverlays.size(); ++k) {
        add(package, ownOverlays[k], ownFacePart(look.ageFlag, look.gender, look.skinTone, look.hairTone, look.overlayCategories[k]));
        add(package, {kResourceListType, kOwnGroup, ownOverlays[k].instance, 0},
            resourceList({newFaceModelKey(look.ageNumber, static_cast<int>(k + 1)), {}}));
    }
}

void addBabyLookRecords(dbpf::PackageImage& package, const BabyLook& look) {
    // The age data's resource list: the indices its outfit lists point at.
    constexpr std::uint32_t kOwnFace = 3, kEverydayBody = 4, kSkinHair = 5, kSkinBody = 6, kSkinFace = 7;
    const dbpf::ResourceKey ownFace{kPartType, kOwnGroup, 1, 0};
    const std::vector<dbpf::ResourceKey> used{newFaceModelKey(kAges[0].number), {}, kBabySkeleton, ownFace,
                                              look.everydayBody, look.skinHair, look.skinBody, look.skinFace};
    // Each outfit list, by its key (outfit category << 16 | part), in ascending order.
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> lists{
        {kEveryday << 16 | kFacePart, kOwnFace},  {kEveryday << 16 | kBodyPart, kEverydayBody},
        {0x08u << 16 | kFacePart, kOwnFace},      {0x10u << 16 | kFacePart, kOwnFace},
        {0x20u << 16 | kFacePart, kOwnFace},      {0x40u << 16 | kFacePart, kOwnFace},
        {kSkin << 16 | kHairPart, kSkinHair},     {kSkin << 16 | kFacePart, kSkinFace},
        {kSkin << 16 | kBodyPart, kSkinBody},     {0x100u << 16 | kFacePart, kOwnFace},
        {0x200u << 16 | kFacePart, kOwnFace},     {0x1000u << 16 | kFacePart, kOwnFace}};
    std::vector<propset::Property> age = ageHead(0, kAges[0].flag, look.gender, 1.0f, look.skinTone, look.hairTone, look.eyeColour, lists.size());
    for (std::size_t i = 0; i < lists.size(); ++i) {
        const std::string n = std::to_string(i);
        age.push_back(text("ls" + n, "1"));
        age.push_back(text("lk" + n, std::to_string(lists[i].first)));
        age.push_back({propset::kUintType, "le" + n + "_0", lists[i].second, {}, {}});
    }
    add(package, {kAgeDataType, kOwnGroup, 1, 0}, propset::writeBinary(age));
    add(package, {kResourceListType, kOwnGroup, 1, 0}, resourceList(used));
    for (std::size_t i = 0; i < kAges.size(); ++i) {
        const auto instance = static_cast<std::uint32_t>(i + 1);
        add(package, {kPartType, kOwnGroup, instance, 0}, ownFacePart(kAges[i].flag, look.gender, look.skinTone, look.hairTone));
        if (i > 0) {
            add(package, {kResourceListType, kOwnGroup, instance, 0}, resourceList({newFaceModelKey(kAges[i].number), {}}));
        }
    }
}

} // namespace ts2::sim
