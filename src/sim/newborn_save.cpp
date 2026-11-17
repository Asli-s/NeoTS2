#include "sim/newborn_save.hpp"

#include "diag/log.hpp"
#include "formats/dbpf.hpp"
#include "formats/face_shape.hpp"
#include "formats/propset.hpp"
#include "scene/age_data.hpp"
#include "scene/assets.hpp"
#include "scene/newborn_face.hpp"
#include "scene/sims.hpp"
#include "sim/lot_simulation.hpp"
#include "sim/newborn_file.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace ts2::sim {

namespace {

constexpr std::uint32_t kMaterialType = 0x49596978, kInstalledPacksType = 0xEBFEE342;
constexpr std::uint32_t kOwnGroup = 0xFFFFFFFF;
constexpr std::size_t kTemplateGuid1 = 0x46, kTemplateGuid2 = 0x47;
// Person data 0xBA, the species: 0 human.
constexpr std::size_t kSpeciesPersonData = 0xBA;

// What the neighborhood's character files hold that a new one needs: their names, the installed
// packs record and the keys of their materials.
struct CharacterFolder {
    std::vector<std::string> names;
    std::vector<std::uint8_t> installedPacks;
    std::set<std::uint64_t> materials;
};

CharacterFolder readCharacterFolder(const std::filesystem::path& folder) {
    CharacterFolder out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::filesystem::path& path = it->path();
        out.names.push_back(path.filename().string());
        if (path.extension() != ".package") {
            continue;
        }
        try {
            dbpf::Package package{path};
            for (const auto& e : package.entries()) {
                if (e.key.type == kMaterialType && e.key.group == kOwnGroup) {
                    out.materials.insert(static_cast<std::uint64_t>(e.key.instanceHigh) << 32 | e.key.instance);
                } else if (e.key.type == kInstalledPacksType && out.installedPacks.empty()) {
                    out.installedPacks = package.read(e);
                }
            }
        } catch (const std::exception& ex) {
            diag::warnOnce(std::string("new character files: a character file that can't be read is passed over: ") + ex.what());
        }
    }
    return out;
}

// A parent's face shape: from its character file, or for a Sim without one (an NPC such as the
// alien) from its object's group.
std::optional<faceshape::FaceShape> parentFaceShape(LotSimulation& simulation, std::int16_t parent) {
    if (const auto file = simulation.characterFile(parent)) {
        return scene::faceShapeOf(*file);
    }
    const SavedNeighbor* saved = simulation.world().neighbor(parent);
    const ObjectType* type = saved != nullptr ? simulation.code().objectType(saved->guid) : nullptr;
    const auto bytes = type != nullptr ? simulation.code().characterResource(faceshape::kType, type->group) : std::nullopt;
    if (!bytes) {
        return std::nullopt;
    }
    try {
        return faceshape::parse(*bytes);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string gene(const SavedNeighbor& neighbor, std::uint32_t number) {
    const auto it = neighbor.genes.find(number);
    return it != neighbor.genes.end() ? it->second.dominant : std::string();
}

// A new Sim's file start: its names, GUIDs and template, as for any Sim made during play.
struct FileStart {
    NewCharacter character;
    std::vector<TemplateResource> templ;
};

FileStart fileStart(LotSimulation& simulation, const std::filesystem::path& hoodFolder, const CharacterFolder& folder, std::int16_t id,
                    const SavedNeighbor& neighbor) {
    World& world = simulation.world();
    const auto first = world.neighborFirstName(id);
    const auto surname = world.neighborSurname(id);
    const ObjectType* type = simulation.code().objectType(neighbor.guid);
    if (!first || first->empty() || type == nullptr) {
        throw std::runtime_error("a new Sim has no name or object type yet");
    }
    const auto& fields = type->definition.fields;
    FileStart out;
    NewCharacter& character = out.character;
    character.fileName = newCharacterFileName(hoodFolder.filename().string(), folder.names);
    character.firstName = *first;
    character.surname = surname.value_or("");
    character.guid = neighbor.guid;
    character.templateGuid = fields.size() > kTemplateGuid2 ? static_cast<std::uint32_t>(fields[kTemplateGuid1] | fields[kTemplateGuid2] << 16) : 0;
    const ObjectType* templateType = simulation.code().objectType(character.templateGuid);
    if (character.fileName.empty() || templateType == nullptr || folder.installedPacks.empty()) {
        throw std::runtime_error("a new Sim's character file has no free name, template or installed packs record");
    }
    for (auto& [key, bytes] : simulation.code().everyGroupResource(templateType->group)) {
        out.templ.push_back({key, std::move(bytes)});
    }
    return out;
}

// Keeps the next file from taking this one's name or material names.
NewCharacterFile finish(CharacterFolder& folder, const std::filesystem::path& hoodFolder, std::int16_t id, const NewCharacter& character,
                        const dbpf::PackageImage& package) {
    NewCharacterFile out;
    out.neighbor = id;
    out.path = hoodFolder / "Characters" / (character.fileName + ".package");
    out.bytes = dbpf::writeImage(package);
    folder.names.push_back(character.fileName + ".package");
    for (const auto& r : package.resources) {
        if (r.key.type == kMaterialType) {
            folder.materials.insert(static_cast<std::uint64_t>(r.key.instanceHigh) << 32 | r.key.instance);
        }
    }
    return out;
}

// One newborn's file. Throws std::runtime_error saying why when it can't be made.
NewCharacterFile newbornFile(LotSimulation& simulation, scene::Assets& assets, const scene::BabyCatalog& catalog,
                             const std::filesystem::path& hoodFolder, CharacterFolder& folder, std::int16_t id, std::uint32_t group) {
    const SavedNeighbor* neighbor = simulation.world().neighbor(id);
    if (neighbor == nullptr || neighbor->genes.empty() || neighbor->faceParentA == 0 || neighbor->faceParentB == 0) {
        throw std::runtime_error("a newborn has no genes or parents");
    }
    const FileStart start = fileStart(simulation, hoodFolder, folder, id, *neighbor);
    const NewCharacter& character = start.character;
    const auto& templ = start.templ;
    const auto parentA = parentFaceShape(simulation, neighbor->faceParentA);
    const auto parentB = parentFaceShape(simulation, neighbor->faceParentB);
    if (!parentA || !parentB) {
        throw std::runtime_error("a newborn's parent has no face shape");
    }
    scene::Newborn baby;
    baby.gender = neighbor->personData.size() > kGenderPersonData && neighbor->personData[kGenderPersonData] == 1 ? 1 : 2;
    baby.skinTone = gene(*neighbor, kSkinGene);
    baby.hairTone = gene(*neighbor, kHairGene);
    baby.eyeColour = gene(*neighbor, kEyeGene);
    baby.browDraw = neighbor->browDraw;
    const scene::NewbornOutfit outfit = scene::newbornOutfit(catalog, baby);
    constexpr std::uint32_t kHair = 1, kFace = 2, kBody = 8;
    if (!outfit.body || !outfit.skin.contains(kHair) || !outfit.skin.contains(kFace) || !outfit.skin.contains(kBody)) {
        throw std::runtime_error("the catalog has no baby outfit for a newborn's skin tone");
    }
    const auto key = [](const scenegraph::Link& l) { return dbpf::ResourceKey{l.type, l.group, l.instance, l.instanceHigh}; };
    NewbornLookParts parts;
    parts.look = {baby.gender, baby.skinTone, baby.hairTone, baby.eyeColour, key(*outfit.body), key(outfit.skin.at(kHair)),
                  key(outfit.skin.at(kBody)), key(outfit.skin.at(kFace))};
    parts.parentA = *parentA;
    parts.parentB = *parentB;
    parts.picks = neighbor->facePicks;
    parts.browFamily = scene::newbornBrowFamily(catalog, baby);
    parts.group = group;
    parts.materialCounter = neighbor->guid;
    parts.takenMaterials = folder.materials;
    return finish(folder, hoodFolder, id, character, newbornCharacterPackage(assets, templ, character, folder.installedPacks, parts));
}

// One made Sim's file. Throws std::runtime_error saying why when it can't be made.
NewCharacterFile madeFile(LotSimulation& simulation, scene::Assets& assets, const scene::OutfitCatalog& outfits, const scene::MadeLook& look,
                          const std::filesystem::path& hoodFolder, CharacterFolder& folder, std::int16_t id, std::uint32_t group) {
    constexpr std::uint32_t kEyebrows = 1, kEyes = 3;
    const SavedNeighbor* neighbor = simulation.world().neighbor(id);
    if (neighbor == nullptr || neighbor->personData.size() <= kSpeciesPersonData || neighbor->personData[kSpeciesPersonData] != 0) {
        throw std::runtime_error("a made Sim that isn't human can't be written yet");
    }
    if (!look.problems.empty()) {
        throw std::runtime_error(std::format("a made Sim's look isn't fully made yet (neighbor {}, made in outfit 0x{:08X}): {}", id,
                                             neighbor->madeAs ? neighbor->madeAs->outfit : 0u, look.problems.front()));
    }
    const auto* names = simulation.world().neighborLanguageNames(id);
    if (names == nullptr || names->empty()) {
        throw std::runtime_error("a made Sim's names in every language aren't known");
    }
    // Its overlays are its eyebrows and eyes alone. Facial hair and glasses aren't written yet.
    // The overlays are the catalog's face overlays, not its parts.
    const auto subtypeOf = [&](std::size_t i) {
        if (i >= look.faceOverlays.size()) {
            return 0u;
        }
        const auto& key = look.faceOverlays[i];
        const auto it = std::find_if(outfits.faceOverlays.begin(), outfits.faceOverlays.end(), [&](const scene::CatalogPart& p) {
            return p.key.type == key.type && p.key.group == key.group && p.key.instance == key.instance && p.key.instanceHigh == key.instanceHigh;
        });
        return it != outfits.faceOverlays.end() ? it->subtype : 0u;
    };
    if (look.faceOverlays.size() != 2 || subtypeOf(0) != kEyebrows || subtypeOf(1) != kEyes) {
        throw std::runtime_error("a made Sim with facial hair or glasses can't be written yet");
    }
    const auto brows = assets.skinResource(look.faceOverlays[0]);
    const auto browProperties = brows ? propset::parse(*brows) : std::map<std::string, std::string>{};
    const auto browFamily = browProperties.find("family");
    if (browFamily == browProperties.end()) {
        throw std::runtime_error("a made Sim's eyebrows have no family");
    }
    FileStart start = fileStart(simulation, hoodFolder, folder, id, *neighbor);
    start.character.languageNames = *names;
    MadeLookParts parts;
    parts.data = look.data;
    parts.faceFamily = look.faceFamily;
    parts.browFamily = browFamily->second;
    parts.group = group;
    parts.materialCounter = neighbor->guid;
    parts.takenMaterials = folder.materials;
    return finish(folder, hoodFolder, id, start.character,
                  madeCharacterPackage(assets, outfits.faceTemplates, start.templ, start.character, folder.installedPacks, parts));
}

} // namespace

NewCharacterFiles newCharacterFiles(LotSimulation& simulation, scene::Assets& assets, const scene::BabyCatalog& catalog,
                                    const std::filesystem::path& hoodFolder, const scene::OutfitCatalog* outfits, const MadeLookOf& madeLook) {
    NewCharacterFiles out;
    const std::set<std::int16_t>& made = simulation.world().addedNeighbors();
    if (made.empty()) {
        return out;
    }
    try {
        CharacterFolder folder = readCharacterFolder(hoodFolder / "Characters");
        // The game numbers each package it opens in a run from 0x6F000000.
        std::uint32_t group = 0x6F000000;
        for (const std::int16_t id : made) {
            if (simulation.characterFile(id)) {
                out.neighbors.insert(id);
                continue;
            }
            const SavedNeighbor* neighbor = simulation.world().neighbor(id);
            if (neighbor != nullptr && neighbor->madeAs) {
                const scene::MadeLook* look = madeLook ? madeLook(id) : nullptr;
                if (look == nullptr || outfits == nullptr) {
                    throw std::runtime_error("a made Sim's look isn't known");
                }
                out.files.push_back(madeFile(simulation, assets, *outfits, *look, hoodFolder, folder, id, ++group));
            } else {
                out.files.push_back(newbornFile(simulation, assets, catalog, hoodFolder, folder, id, ++group));
            }
            out.neighbors.insert(id);
        }
    } catch (const std::exception& ex) {
        out.files.clear();
        out.neighbors.clear();
        out.problem = ex.what();
    }
    return out;
}

} // namespace ts2::sim
