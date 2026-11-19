#include "sim/newborn_file.hpp"

#include "formats/hash.hpp"
#include "formats/scenegraph.hpp"
#include "scene/assets.hpp"
#include "scene/newborn_face_shape.hpp"
#include "scene/overlay_fit.hpp"
#include "sim/character_models.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <stdexcept>

namespace ts2::sim {

namespace {

constexpr std::uint32_t kOwnGroup = 0xFFFFFFFF;
constexpr std::uint32_t kNodeType = 0x7BA3838C, kShapeType = 0xFC6EB1F7, kModelType = 0xE519C933, kMeshType = 0xAC4F8687,
                        kMaterialType = 0x49596978;
constexpr std::uint32_t kFemale = 1, kElder = 4, kYoungAdult = 7, kAdult = 3;
// The group of the catalog's skeleton models.
constexpr std::uint32_t kSkeletonGroup = 0x1C0532FA;
// Every elder's face material takes grey as its hair tone.
constexpr const char* kGreyHair = "00000005-0000-0000-0000-000000000000";
// The game makes the ages' faces in this order, and counts the material names on in it.
constexpr std::array<int, 7> kAgeOrder{5, 0, 1, 2, kYoungAdult, kAdult, kElder};

void add(dbpf::PackageImage& package, std::uint32_t type, const std::string& name, std::span<const std::uint8_t> body) {
    const hash::ScenegraphKey key = hash::scenegraphKey(name);
    package.resources.push_back(dbpf::storeResource({type, kOwnGroup, key.instance, key.instanceHigh}, body));
    package.dataOrder.push_back(package.resources.size() - 1);
}

std::string ageName(int age, bool far, const char* suffix) {
    return "age" + std::to_string(age) + "_0" + (far ? "LOD15" : "") + suffix;
}

[[noreturn]] void missing(const std::string& what, int age) {
    throw std::runtime_error("newborn character file: no " + what + " for age " + std::to_string(age));
}

// The catalog base face mesh a base face node draws.
std::vector<std::uint8_t> nodeMesh(scene::Assets& assets, std::span<const std::uint8_t> node, int age) {
    for (const auto& link : scenegraph::parseContainer(node).links) {
        if (link.type == kMeshType) {
            if (auto mesh = assets.byKey(link)) {
                return std::move(*mesh);
            }
        }
    }
    missing("base face mesh", age);
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// What one age's face records are made from: the Sim's look, and where its face's vertices and
// grips come from (a newborn's blended from its parents, a made Sim's from its face template).
struct AgeFace {
    std::uint32_t gender = 0;
    std::string skinTone, hairTone, eyeColour, browFamily;
    std::vector<std::string> facialHairFamilies;
    std::vector<MadeLookParts::MeshOverlay> meshOverlays;
    bool browFirst = false;
    std::uint32_t group = 0;
    const std::set<std::uint64_t>* takenMaterials = nullptr;
    // By face shape age and gender (0 male, 1 female) and level of detail.
    std::function<std::optional<scene::NewbornFaceVertices>(std::uint8_t, std::uint8_t, bool)> vertices;
    // By age number and skeleton model name.
    std::function<std::vector<scene::FaceGrip>(int, const std::string&)> grips;
};

bool taken(const AgeFace& face, const std::string& materialName) {
    const hash::ScenegraphKey key = hash::scenegraphKey(materialName + "_txmt");
    return face.takenMaterials->contains(static_cast<std::uint64_t>(key.instanceHigh) << 32 | key.instance);
}

void addAge(dbpf::PackageImage& package, scene::Assets& assets, const AgeFace& look, int age, std::uint32_t& counter) {
    const std::uint32_t ageFlag = baseFaceAgeFlag(age);
    // The face shape keeps a young adult's face as an adult's.
    const auto shapeAge = static_cast<std::uint8_t>(age == kYoungAdult ? kAdult : age);
    const auto shapeGender = static_cast<std::uint8_t>(look.gender == kFemale ? 1 : 0);
    // Capped so a set holding every name can't hold the save up.
    constexpr int kMaxSkips = 1 << 20;
    for (int skips = 0; skips < kMaxSkips && taken(look, faceMaterialName(counter, age, shapeGender)); ++skips) {
        ++counter;
    }
    const std::string materialName = faceMaterialName(counter, age, shapeGender);
    std::vector<std::string> morphs;
    // The near base face and the Sim's, which the mesh overlays are fitted to.
    std::vector<float> baseFace, simFace;
    for (const bool far : {false, true}) {
        const auto node = scene::baseFaceNode(assets, ageFlag, look.gender, far, look.skinTone);
        if (!node) {
            missing("base face node", age);
        }
        add(package, kNodeType, ageName(age, far, "_gmnd"), newFaceNode(*node, age, far, look.group));
        const std::vector<std::uint8_t> catalogMesh = nodeMesh(assets, *node, age);
        if (!far) {
            morphs = meshMorphs(catalogMesh);
        }
        const auto vertices = look.vertices(shapeAge, shapeGender, far);
        if (!vertices) {
            missing("new face", age);
        }
        const auto mesh = newFaceMesh(catalogMesh, age, far, look.group, vertices->positions, vertices->normals);
        if (!far && !look.meshOverlays.empty()) {
            baseFace = scene::meshPositions(catalogMesh);
            simFace = scene::meshPositions(mesh);
        }
        add(package, kMeshType, ageName(age, far, "_gmdc"), mesh);
    }
    add(package, kShapeType, "age" + std::to_string(age) + "_00_shpe", newFaceShape(age, look.group, lower(materialName)));

    const std::string skeletonName = skeletonModelName(age);
    const auto skeleton = assets.byName(kModelType, skeletonName);
    if (!skeleton) {
        missing("skeleton model", age);
    }
    std::vector<GripAdjust> grips;
    for (const auto& g : look.grips(age, skeletonName)) {
        grips.push_back({g.bone, g.translation, g.rotation});
    }
    add(package, kModelType, "age" + std::to_string(age) + "_0_cres", newFaceModel(*skeleton, age, look.group, grips, morphs));

    FaceMaterialLook material;
    material.name = materialName;
    material.eyeColour = look.eyeColour;
    material.browFamily = look.browFamily;
    material.hairTone = age == kElder ? kGreyHair : look.hairTone;
    material.skinTone = look.skinTone;
    material.age = age;
    material.paramGender = look.gender == kFemale ? 1 : 0;
    material.browFirst = look.browFirst;
    material.facialHairFamilies = look.facialHairFamilies;
    const auto catalog = scene::newbornFaceMaterialParts(assets, ageFlag, look.gender, look.skinTone, look.eyeColour, material.hairTone,
                                                         look.browFamily, look.facialHairFamilies);
    if (!catalog) {
        missing("base face material", age);
    }
    const auto copy = [](const scene::FaceOverlayTexture& t) { return FaceTexture{t.texture, t.layer}; };
    material.eye = copy(catalog->eye);
    material.brow = copy(catalog->brow);
    for (const auto& t : catalog->others) {
        if (t.texture.empty()) {
            missing("facial hair overlay", age);
        }
        material.facialHair.push_back(copy(t));
    }
    for (const auto& overlay : look.meshOverlays) {
        material.meshOverlayFamilies.push_back(overlay.family);
    }
    add(package, kMaterialType, materialName + "_txmt", newFaceMaterial(catalog->catalogMaterial, look.group, material));

    // Each mesh overlay (a mesh beard or glasses) is fitted to the face and written as the age's next
    // part, its materials named on from the face's.
    for (std::size_t k = 0; k < look.meshOverlays.size(); ++k) {
        const int part = static_cast<int>(k + 1);
        std::string problem;
        const auto pieces = scene::meshOverlayCatalog(assets, look.meshOverlays[k].part, problem);
        if (!pieces) {
            throw std::runtime_error("character file: " + problem);
        }
        const auto fitted = scene::fitMeshOverlay(scene::meshPositions(pieces->mesh), scene::meshPositions(pieces->maskMesh),
                                                  scene::meshColours(pieces->maskMesh), baseFace, simFace);
        if (fitted.empty()) {
            throw std::runtime_error("character file: a mesh overlay that can't be fitted to the face");
        }
        const std::string stem = "age" + std::to_string(age) + "_" + std::to_string(part);
        add(package, kNodeType, stem + "_gmnd", newOverlayNode(pieces->node, age, part, look.group));
        add(package, kMeshType, stem + "_gmdc", newOverlayMesh(pieces->mesh, age, part, look.group, fitted));
        std::vector<OverlaySubset> subsets;
        for (const auto& subset : pieces->subsets) {
            ++counter;
            for (int skips = 0; skips < kMaxSkips && taken(look, faceMaterialName(counter, age, shapeGender)); ++skips) {
                ++counter;
            }
            const std::string name = faceMaterialName(counter, age, shapeGender);
            add(package, kMaterialType, name + "_txmt", newOverlayMaterial(subset.material, look.group, name));
            subsets.push_back({subset.subset, lower(name)});
        }
        add(package, kShapeType, stem + "0_shpe", newOverlayShape(age, part, look.group, subsets));
        add(package, kModelType, stem + "_cres", newFaceModel(*skeleton, age, look.group, {}, meshMorphs(pieces->mesh), part));
    }
}

// A catalog GUID as bare hex digits (as the catalog keeps it) written as the files write GUIDs,
// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx". Anything else is kept as it is.
std::string dashedGuid(const std::string& guid) {
    if (guid.size() != 32 || !std::all_of(guid.begin(), guid.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) {
        return guid;
    }
    return lower(guid.substr(0, 8) + "-" + guid.substr(8, 4) + "-" + guid.substr(12, 4) + "-" + guid.substr(16, 4) + "-" + guid.substr(20));
}

// A made Sim's age number in its face records' names (7 young adult) for an age flag.
int ageNumber(std::uint32_t ageFlag) {
    switch (ageFlag) {
    case 0x01: return 0;
    case 0x02: return 1;
    case 0x04: return 2;
    case 0x08: return kAdult;
    case 0x10: return kElder;
    case 0x40: return kYoungAdult;
    default: throw std::runtime_error("made character file: an age that isn't known: " + std::to_string(ageFlag));
    }
}

} // namespace

dbpf::PackageImage newbornCharacterPackage(scene::Assets& assets, std::span<const TemplateResource> templ, const NewCharacter& character,
                                           std::span<const std::uint8_t> installedPacks, const NewbornLookParts& parts) {
    dbpf::PackageImage package = newCharacterPackage(templ, character, installedPacks);
    addBabyLookRecords(package, parts.look);
    const auto shape = scene::newbornFaceShape(assets, parts.parentA, parts.parentB, parts.picks, parts.look.skinTone);
    if (!shape) {
        throw std::runtime_error("newborn character file: its face shape can't be blended from its parents'");
    }
    addFaceShapeRecord(package, faceshape::write(*shape));
    AgeFace face;
    face.gender = parts.look.gender;
    face.skinTone = parts.look.skinTone;
    face.hairTone = parts.look.hairTone;
    face.eyeColour = parts.look.eyeColour;
    face.browFamily = parts.browFamily;
    face.group = parts.group;
    face.takenMaterials = &parts.takenMaterials;
    face.vertices = [&](std::uint8_t age, std::uint8_t gender, bool far) {
        return scene::newbornFaceVertices(assets, parts.parentA, parts.parentB, parts.picks, parts.look.skinTone, age, gender, far);
    };
    face.grips = [&](int age, const std::string& skeleton) {
        return scene::newbornFaceGrips(assets, parts.parentA, parts.parentB, parts.picks, parts.look.skinTone, age, parts.look.gender, skeleton);
    };
    std::uint32_t counter = parts.materialCounter;
    for (const int age : kAgeOrder) {
        addAge(package, assets, face, age, counter);
        ++counter;
    }
    return package;
}

dbpf::PackageImage madeCharacterPackage(scene::Assets& assets, const std::vector<scene::CatalogPart>& faceTemplates,
                                        std::span<const TemplateResource> templ, const NewCharacter& character,
                                        std::span<const std::uint8_t> installedPacks, const MadeLookParts& parts) {
    const auto& p = parts.data.properties;
    const auto field = [&](const char* name) {
        const auto it = p.find(name);
        return it != p.end() ? it->second : std::string();
    };
    const auto numberOf = [&](const char* name) { return static_cast<std::uint32_t>(std::strtoul(field(name).c_str(), nullptr, 0)); };
    MadeAgeLook look;
    look.ageFlag = numberOf("age");
    look.gender = numberOf("gender");
    look.ageNumber = ageNumber(look.ageFlag);
    look.stretch = p.contains("stretch") ? std::strtof(field("stretch").c_str(), nullptr) : 1.0f;
    look.skinTone = dashedGuid(field("skincolor"));
    look.hairTone = dashedGuid(field("haircolor"));
    look.eyeColour = dashedGuid(field("eyecolor"));
    if (look.gender != kFemale && look.gender != 2) {
        throw std::runtime_error("made character file: a gender that isn't known");
    }
    const std::string skeletonName = skeletonModelName(look.ageNumber);
    const hash::ScenegraphKey skeletonKey = hash::scenegraphKey(skeletonName);
    look.skeleton = {kModelType, kSkeletonGroup, skeletonKey.instance, skeletonKey.instanceHigh};
    for (const auto& [key, list] : parts.data.lists) {
        auto& out = look.lists[key];
        for (const auto& l : list) {
            out.push_back({l.type, l.group, l.instance, l.instanceHigh});
        }
    }

    dbpf::PackageImage package = newCharacterPackage(templ, character, installedPacks);
    for (const auto& overlay : parts.meshOverlays) {
        look.overlayCategories.push_back(overlay.category);
    }
    addMadeLookRecords(package, look);
    const auto shape = scene::madeFaceShape(assets, faceTemplates, parts.faceFamily, look.skinTone);
    if (!shape) {
        throw std::runtime_error("made character file: its face shape can't be made");
    }
    addFaceShapeRecord(package, faceshape::write(*shape));

    AgeFace face;
    face.gender = look.gender;
    face.skinTone = look.skinTone;
    face.hairTone = look.hairTone;
    face.eyeColour = look.eyeColour;
    face.browFamily = parts.browFamily;
    face.facialHairFamilies = parts.facialHairFamilies;
    face.meshOverlays = parts.meshOverlays;
    face.browFirst = true;
    face.group = parts.group;
    face.takenMaterials = &parts.takenMaterials;
    face.vertices = [&](std::uint8_t age, std::uint8_t gender, bool far) {
        return scene::madeFaceVertices(assets, faceTemplates, parts.faceFamily, look.skinTone, age, gender, far);
    };
    face.grips = [&](int age, const std::string& skeleton) {
        return scene::madeFaceGrips(assets, faceTemplates, parts.faceFamily, look.skinTone, age, look.gender, skeleton);
    };
    std::uint32_t counter = parts.materialCounter;
    addAge(package, assets, face, look.ageNumber, counter);
    return package;
}

} // namespace ts2::sim
