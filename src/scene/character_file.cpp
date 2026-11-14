#include "scene/character_file.hpp"

#include "formats/binary_writer.hpp"
#include "formats/dbpf_writer.hpp"
#include "formats/hash.hpp"
#include "formats/propset.hpp"
#include "formats/reference_list.hpp"
#include "formats/scenegraph.hpp"
#include "formats/str.hpp"
#include "scene/sims.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <stdexcept>
#include <utility>

namespace ts2::scene {

namespace {

namespace sg = scenegraph;

constexpr std::uint32_t kLocal = 0xFFFFFFFF;
constexpr std::uint32_t kCres = 0xE519C933, kShpe = 0xFC6EB1F7, kGmnd = 0x7BA3838C, kGmdc = 0xAC4F8687, kTxmt = 0x49596978;
constexpr std::uint32_t kPartProperties = 0xEBCF3E27, kAgeDataType = 0xAC598EAC, kVersionStamp = 0xEBFEE342, kNames = 0x43545353;
constexpr std::uint32_t kObjectDefinition = 0x4F424A44, kNameReference = 0x4E524546;
constexpr std::uint32_t kNamesInstance = 0x7D0;
// TemplatePerson, the object a Sim's character file is made from, and its group.
constexpr std::uint32_t kTemplatePersonGuid = 0x7FD96B54, kTemplatePersonGroup = 0x7FEDFE16;
constexpr std::uint32_t kSkinCategory = 0x80;
// The ages a Sim lives through, youngest first.
constexpr std::array<std::uint32_t, 5> kAges = {kToddlerAge, kChildAge, kTeenAge, kAdultAge, kElderAge};
// The game version the stamp names: the newest full character file the game wrote here has it.
constexpr const char* kGameVersion = "1.0.0.904";

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("character file: " + what); }

// An age flag as the face's names number it: 0 toddler, 1 child, 2 teen, 3 adult (and young
// adult), 4 elder.
int ageIndex(std::uint32_t age) {
    switch (age) {
    case kToddlerAge: return 0;
    case kChildAge: return 1;
    case kTeenAge: return 2;
    case kElderAge: return 4;
    default: return 3;
    }
}

std::string lowered(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// A resource of the file's own, by its name without the file's group: group 0xFFFFFFFF, the
// instance its name hashes to.
sg::Link localKey(std::uint32_t type, const std::string& name) {
    const auto key = hash::scenegraphKey(name);
    return {type, kLocal, key.instance, key.instanceHigh};
}

dbpf::ResourceKey asKey(const sg::Link& l) { return {l.type, l.group, l.instance, l.instanceHigh}; }

std::vector<std::uint8_t> need(std::optional<std::vector<std::uint8_t>> bytes, const std::string& what) {
    if (!bytes) {
        fail(what + " not found");
    }
    return std::move(*bytes);
}

std::optional<std::size_t> linkOfType(const sg::Container& c, std::uint32_t type) {
    for (std::size_t i = 0; i < c.links.size(); ++i) {
        if (c.links[i].type == type) {
            return i;
        }
    }
    return std::nullopt;
}

void put16(std::vector<std::uint8_t>& b, std::size_t at, std::uint16_t v) {
    if (at + 2 > b.size()) {
        fail("the object definition is too short");
    }
    b[at] = static_cast<std::uint8_t>(v);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

void put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    put16(b, at, static_cast<std::uint16_t>(v));
    put16(b, at + 2, static_cast<std::uint16_t>(v >> 16));
}

// TemplatePerson's object definition made the Sim's, as the game makes it: its file name (at the
// start and at the end), the version the game writes Sims' in, the Sim's GUID and TemplatePerson's
// as its template, and the values the game sets for a Sim.
std::vector<std::uint8_t> simObjectDefinition(std::vector<std::uint8_t> b, std::uint32_t guid, const std::string& fileName) {
    constexpr std::size_t kNameField = 64;
    if (b.size() < 0xD0 + 4) {
        fail("TemplatePerson's object definition is too short");
    }
    // The name at the end: its length, then the text, last in the definition.
    const std::string templateName(reinterpret_cast<const char*>(b.data()));
    const std::size_t tail = b.size() - templateName.size() - 4;
    std::fill(b.begin(), b.begin() + kNameField, std::uint8_t{0});
    std::copy_n(fileName.begin(), std::min(fileName.size(), kNameField - 1), b.begin());
    put16(b, 0x40, 0x8B);
    put16(b, 0x52, 0x11);
    put32(b, 0x5C, guid);
    put16(b, 0x98, 0x000A);
    put16(b, 0xA2, 0);
    put16(b, 0xC0, 0);
    put32(b, 0xCC, kTemplatePersonGuid);
    b.resize(tail);
    BinaryWriter w;
    w.u32(static_cast<std::uint32_t>(fileName.size()));
    w.text(fileName);
    b.insert(b.end(), w.data().begin(), w.data().end());
    return b;
}

// The stamp a character file carries: "The Sims 2" and the game's version (a binary property set).
std::vector<std::uint8_t> versionStamp() {
    constexpr std::uint32_t kString = 0x0B8BEA18;
    BinaryWriter w;
    w.u32(0xCBE750E0);
    w.u16(2);
    w.u32(2);
    for (const auto& [key, value] : std::array<std::pair<const char*, const char*>, 2>{{{"0", "The Sims 2"}, {"1", kGameVersion}}}) {
        w.u32(kString);
        w.u32(static_cast<std::uint32_t>(std::string_view(key).size()));
        w.text(key);
        w.u32(static_cast<std::uint32_t>(std::string_view(value).size()));
        w.text(value);
    }
    return w.take();
}

std::vector<std::uint8_t> names(const CharacterFileSim& sim) {
    str::StringTable t;
    t.format = 0xFFFD;
    t.entries = {{1, sim.firstName, ""}, {1, sim.bio, ""}, {1, sim.lastName, ""}};
    return str::encode(t);
}

// A face's part property set, as the game writes a Sim's own face.
std::vector<std::uint8_t> facePart(std::uint32_t age, std::uint32_t gender, const std::string& skinTone, const std::string& hairTone) {
    constexpr const char* kUint = "0xeb61e4f7";
    constexpr const char* kString = "0x0b8bea18";
    constexpr const char* kFloat = "0xabc78708";
    const std::string none = "00000000-0000-0000-0000-000000000000";
    auto u = [&](const char* k, std::string v) { return propset::TextProperty{"AnyUint32", k, kUint, std::move(v)}; };
    auto s = [&](const char* k, std::string v) { return propset::TextProperty{"AnyString", k, kString, std::move(v)}; };
    return propset::encodeText({
        u("age", std::to_string(age)),
        u("category", "0"),
        s("creator", none),
        s("family", none),
        u("fitness", "0"),
        u("flags", "1"),
        u("gender", std::to_string(gender)),
        propset::TextProperty{"AnyFloat32", "genetic", kFloat, "0"},
        s("hairtone", hairTone),
        s("name", ""),
        u("numoverrides", "0"),
        u("outfit", "2"),
        u("resourcekeyidx", "0"),
        u("shapekeyidx", "1"),
        u("shoe", "0"),
        s("skintone", skinTone),
        u("species", "1"),
        s("type", "skin"),
    });
}

std::string property(const std::map<std::string, std::string>& p, const char* key, std::string fallback = {}) {
    const auto it = p.find(key);
    return it != p.end() ? it->second : fallback;
}

// A tone as the catalog keeps it: hex digits only, lowercase.
std::string toneDigits(const std::string& tone) {
    std::string out;
    for (const char c : tone) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return out;
}

// A tone as the game writes it in a Sim's files: a GUID with its dashes (the catalog keeps digits only).
std::string toneGuid(const std::string& tone) {
    const std::string d = toneDigits(tone);
    if (d.size() != 32) {
        return tone;
    }
    return d.substr(0, 8) + "-" + d.substr(8, 4) + "-" + d.substr(12, 4) + "-" + d.substr(16, 4) + "-" + d.substr(20);
}

// The base face of a skin at an age: the skin outfit's face part of that age, gender and tone, of
// the family of the Sim's own (the game's faces for each skin, one family) when there is one.
std::optional<sg::Link> baseFace(const OutfitCatalog& catalog, std::uint32_t age, std::uint32_t gender, const std::string& skinTone,
                                 const std::string& family) {
    std::optional<sg::Link> any;
    for (const auto& p : catalog.parts) {
        if ((p.parts & kFacePart) && (p.category & kSkinCategory) && (p.age & age) && (p.gender & gender) && (p.species & 1) &&
            toneDigits(p.skinTone) == toneDigits(skinTone)) {
            if (toneDigits(p.family) == toneDigits(family)) {
                return p.key;
            }
            any = any ? any : std::optional(p.key);
        }
    }
    return any;
}

// The family of a catalog part, empty when the catalog hasn't it.
std::string familyOf(const OutfitCatalog& catalog, const sg::Link& key) {
    for (const auto& p : catalog.parts) {
        if (p.key.type == key.type && p.key.group == key.group && p.key.instance == key.instance && p.key.instanceHigh == key.instanceHigh) {
            return p.family;
        }
    }
    return {};
}

struct FaceSet {
    sg::Link cres;
    std::vector<CharacterResource> resources;
};

// The Sim's own face at `age`: copies of the base face's resource node, shape, geometry nodes and
// meshes, named as the game names a Sim's (age<N>_...), the face template's change made to the
// meshes, and its material.
FaceSet faceSet(Assets& assets, const OutfitCatalog& catalog, const MadeLook& made, const sg::Link& basePart, std::uint32_t age,
                std::uint32_t materialId, const std::string& prefix, std::vector<std::string>& problems) {
    const auto props = propset::parse(need(assets.skinResource(basePart), "the base face's part"));
    const auto refs = reflist::parse(need(assets.skinResource({0xAC506764, basePart.group, basePart.instance, basePart.instanceHigh}),
                                          "the base face's resource list"));
    const auto at = [&](const char* key) {
        const std::uint32_t i = propset::number(props, key);
        if (!props.contains(key) || i >= refs.size()) {
            fail(std::string("the base face's ") + key + " points nowhere");
        }
        return refs[i];
    };
    const std::string a = std::to_string(ageIndex(age));
    const std::uint32_t gender = propset::number(made.data.properties, "gender");
    FaceSet out;

    // The resource node and the shape it holds.
    auto cres = need(assets.byKey(at("resourcekeyidx")), "the base face's resource node");
    const auto cresLinks = sg::parseContainer(cres);
    const auto shapeLink = linkOfType(cresLinks, kShpe);
    if (!shapeLink) {
        fail("the base face's resource node holds no shape");
    }
    auto shpe = need(assets.byKey(cresLinks.links[*shapeLink]), "the base face's shape");
    const auto shape = sg::parseShape(shpe);

    // Each geometry node of the shape and its mesh, the template's change made to the mesh.
    const MadeFace face = madeFace(assets, catalog, made, age, problems);
    for (std::size_t item = 0; item < shape.geometryNodes.size(); ++item) {
        const std::string level = item < shape.detailLevels.size() && shape.detailLevels[item] != 0
                                      ? "LOD" + std::to_string(shape.detailLevels[item])
                                      : std::string();
        const std::string gmndName = "age" + a + "_0" + level + "_gmnd", gmdcName = "age" + a + "_0" + level + "_gmdc";
        const std::string& named = shape.geometryNodes[item];
        const auto linked = item < shape.geometryLinks.size() ? shape.geometryLinks[item] : std::nullopt;
        auto gmnd = !named.empty() ? need(assets.byName(kGmnd, named), "the base face's geometry node " + named)
                    : linked && *linked < shape.container.links.size()
                        ? need(assets.byKey(shape.container.links[*linked]), "the base face's geometry node")
                        : (fail("a shape item names no geometry node"), std::vector<std::uint8_t>{});
        const auto gmndLinks = sg::parseContainer(gmnd);
        const auto meshLink = linkOfType(gmndLinks, kGmdc);
        if (!meshLink) {
            fail("the base face's geometry node holds no mesh");
        }
        auto gmdc = need(assets.byKey(gmndLinks.links[*meshLink]), "the base face's mesh");
        if (face.edit) {
            // The changed values written over the copy's, in place: the edit changes values only.
            auto mesh = sg::parseMesh(gmdc);
            const auto before = mesh.elements;
            face.edit->apply(mesh);
            for (std::size_t e = 0; e < mesh.elements.size() && e < before.size(); ++e) {
                const auto& now = mesh.elements[e];
                if (now.data != before[e].data && now.data.size() == before[e].data.size() && now.dataOffset + now.data.size() <= gmdc.size()) {
                    std::copy(now.data.begin(), now.data.end(), gmdc.begin() + static_cast<std::ptrdiff_t>(now.dataOffset));
                }
            }
        }
        sg::renameResource(gmdc, prefix + gmdcName);
        const auto gmdcKey = localKey(kGmdc, gmdcName);
        sg::setContainerLink(gmnd, *meshLink, gmdcKey);
        sg::renameResource(gmnd, prefix + gmndName);
        const auto gmndKey = localKey(kGmnd, gmndName);
        if (!named.empty()) {
            sg::replaceName(shpe, named, prefix + gmndName);
        } else {
            sg::setContainerLink(shpe, *linked, gmndKey);
        }
        out.resources.push_back({asKey(gmdcKey), std::move(gmdc)});
        out.resources.push_back({asKey(gmndKey), std::move(gmnd)});
    }

    // The material: the base face's, drawing the skin with the eyes and eyebrows over it, as the
    // game composites a Sim's face.
    sg::Link baseMaterial = at("resourcekeyidx");
    bool haveMaterial = false;
    for (std::uint32_t i = 0; i < propset::number(props, "numoverrides") && i < 16; ++i) {
        const std::string o = "override" + std::to_string(i);
        if (property(props, (o + "subset").c_str()) == "face") {
            baseMaterial = at((o + "resourcekeyidx").c_str());
            haveMaterial = true;
        }
    }
    if (!haveMaterial) {
        fail("the base face names no material for its face");
    }
    auto material = sg::parseMaterial(need(assets.byKey(baseMaterial), "the base face's material"));
    char id[16];
    std::snprintf(id, sizeof id, "0x%07x", materialId);
    const std::string g = gender == 2 ? "0" : "1";
    const std::string description = "ss_customface_" + std::string(id) + "_" + a + "_" + g + "_cmpm";
    const std::string materialName = "ss_customFace_" + std::string(id) + "_" + a + "_" + g + "_cmpm_txmt";
    const std::string skin = property(material.properties, "stdMatBaseTextureName");
    std::vector<std::string> layers{skin};
    // Written new, as the game writes a Sim's face material: no links, version 11.
    material.container.clear();
    material.tail.clear();
    material.order.clear();
    material.version = 11;
    material.type = "SimSkin";
    material.name = prefix + materialName;
    material.description = description;
    material.properties["baseTexture0"] = skin;
    for (std::size_t i = 0; i < face.overlays.size(); ++i) {
        material.properties["baseTexture" + std::to_string(i + 1)] = face.overlays[i].texture;
        material.properties["cafOverlay" + std::to_string(i)] = face.overlays[i].family + ",";
        layers.push_back(face.overlays[i].texture);
    }
    std::string composite;
    for (const auto& layer : layers) {
        std::string l = lowered(layer);
        std::replace(l.begin(), l.end(), '-', '_');
        composite += (composite.empty() ? "" : "_") + l;
    }
    composite += "-diffuse";
    material.properties["cafNumOverlays"] = std::to_string(face.overlays.size());
    material.properties["cafSkinTone"] = toneGuid(property(made.data.properties, "skincolor"));
    material.properties["cafHairTone"] = toneGuid(property(made.data.properties, "haircolor"));
    material.properties["compositeBaseTextureName"] = composite;
    material.properties["eyeGizmoEnable"] = "true";
    material.properties["numTexturesToComposite"] = std::to_string(layers.size());
    material.properties["paramAge"] = a;
    material.properties["paramGender"] = g;
    material.textures = {skin, composite};
    for (const auto& part : shape.parts) {
        sg::replaceName(shpe, part.material, description);
    }
    const std::string shpeName = "age" + a + "_00_shpe", cresName = "age" + a + "_0_cres";
    sg::renameResource(shpe, prefix + shpeName);
    const auto shpeKey = localKey(kShpe, shpeName);
    sg::setContainerLink(cres, *shapeLink, shpeKey);
    sg::renameResource(cres, prefix + cresName);
    out.cres = localKey(kCres, cresName);
    out.resources.push_back({asKey(shpeKey), std::move(shpe)});
    out.resources.push_back({asKey(out.cres), std::move(cres)});
    out.resources.push_back({asKey(localKey(kTxmt, materialName)), sg::encodeMaterial(material)});
    return out;
}

} // namespace

std::vector<CharacterResource> characterResources(Assets& assets, const OutfitCatalog& catalog, const MadeLook& made,
                                                  const CharacterFileSim& sim, std::vector<std::string>& problems) {
    const std::string stem = sim.file.stem().string();
    char prefix[32];
    std::snprintf(prefix, sizeof prefix, "#0x%08x!", hash::groupId(lowered(stem)));
    const auto& props = made.data.properties;
    const std::uint32_t age = propset::number(props, "age"), gender = propset::number(props, "gender");
    const std::string skinTone = toneGuid(property(props, "skincolor")), hairTone = toneGuid(property(props, "haircolor"));
    std::vector<CharacterResource> out;

    // TemplatePerson's resources, its object definition made the Sim's. Its name reference names the
    // template only, so the game leaves it out.
    bool definition = false;
    for (auto& [link, bytes] : assets.objectGroupResources(kTemplatePersonGroup)) {
        if (link.type == kNameReference) {
            continue;
        }
        if (link.type == kObjectDefinition) {
            bytes = simObjectDefinition(std::move(bytes), sim.guid, stem + " - " + sim.firstName);
            definition = true;
        }
        out.push_back({{link.type, kLocal, link.instance, link.instanceHigh}, std::move(bytes)});
    }
    if (!definition) {
        fail("TemplatePerson isn't in the game's objects");
    }
    out.push_back({{kVersionStamp, kLocal, 1, 0}, versionStamp()});
    out.push_back({{kNames, kLocal, kNamesInstance, 0}, names(sim)});

    // Its own face for its age and each later one: part property set 1 is its age's.
    const auto ownFace = made.data.lists.find(kSkinCategory << 16 | kFacePart);
    const bool hasOwnFace = ownFace != made.data.lists.end() && !ownFace->second.empty();
    const std::string faceFamily = hasOwnFace ? familyOf(catalog, ownFace->second.back()) : std::string();
    std::uint32_t partInstance = 1;
    const std::uint32_t materialBase = (sim.guid >> 4) & 0x0FFFFFF0;
    sg::Link currentCres{};
    for (const std::uint32_t later : kAges) {
        if (later < (age == kYoungAdultAge ? kAdultAge : age)) {
            continue;
        }
        const auto base = later == age && hasOwnFace ? std::optional(ownFace->second.back()) : baseFace(catalog, later, gender, skinTone, faceFamily);
        if (!base) {
            problems.push_back("no base face for age " + std::to_string(later));
            continue;
        }
        auto set = faceSet(assets, catalog, made, *base, later, materialBase + static_cast<std::uint32_t>(ageIndex(later)), prefix, problems);
        out.push_back({{kPartProperties, kLocal, partInstance, 0}, facePart(later, gender, skinTone, hairTone)});
        if (partInstance == 1) {
            currentCres = set.cres;
        } else {
            out.push_back({{0xAC506764, kLocal, partInstance, 0}, reflist::encode({set.cres, sg::Link{}})});
        }
        for (auto& r : set.resources) {
            out.push_back(std::move(r));
        }
        ++partInstance;
    }
    if (partInstance == 1) {
        fail("no face could be made");
    }

    // The age data: its own face in every outfit category (none in the skin outfit), and the age's
    // skeleton.
    AgeData data = made.data;
    for (const char* tone : {"skincolor", "haircolor", "eyecolor"}) {
        if (data.properties.contains(tone)) {
            data.properties[tone] = toneGuid(data.properties[tone]);
        }
    }
    data.lists.erase(kSkinCategory << 16 | kFacePart);
    std::vector<std::uint32_t> categories;
    for (const auto& [key, list] : data.lists) {
        const std::uint32_t category = key >> 16;
        if (category != kSkinCategory && std::find(categories.begin(), categories.end(), category) == categories.end()) {
            categories.push_back(category);
        }
    }
    for (const std::uint32_t category : categories) {
        data.lists[category << 16 | kFacePart] = {sg::Link{kPartProperties, kLocal, 1, 0}};
    }
    if (!data.skeleton) {
        data.skeleton = defaultSkeletonKey(age == kYoungAdultAge ? kAdultAge : age);
    }
    const auto written = writeAgeData(data, {currentCres, sg::Link{}});
    out.push_back({{kAgeDataType, kLocal, 1, 0}, written.properties});
    out.push_back({{0xAC506764, kLocal, 1, 0}, written.references});
    return out;
}

void writeCharacterFile(const std::filesystem::path& file, const std::vector<CharacterResource>& resources) {
    auto temporary = file;
    temporary += ".neots2new";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            fail("can't write " + temporary.string());
        }
        dbpf::PackageWriter writer(out);
        writer.keepVersions(1, 2);
        for (const auto& r : resources) {
            writer.add(r.key, r.data, false, static_cast<std::uint32_t>(r.data.size()));
        }
        writer.finish(0, true);
        if (!out) {
            fail("couldn't write " + temporary.string());
        }
    }
    std::filesystem::rename(temporary, file);
}

} // namespace ts2::scene
