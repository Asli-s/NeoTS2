#pragma once

#include "formats/dbpf_writer.hpp"
#include "formats/face_shape.hpp"
#include "sim/character_file.hpp"
#include "sim/character_look.hpp"
#include "scene/age_data.hpp"
#include "sim/genetics.hpp"

#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace ts2::scene {
class Assets;
}

namespace ts2::sim {

// What a newborn's look in its character file is made from, beyond its template and names.
struct NewbornLookParts {
    BabyLook look;
    // The parents its face is blended from and which gave each region, as the birth drew them.
    faceshape::FaceShape parentA, parentB;
    FacePicks picks;
    // The family of the baby eyebrows the birth drew (scene::newbornBrowFamily).
    std::string browFamily;
    // The number the file's scenegraph names carry ("#0x<group>!"). The game gives the open file
    // 0x6F000000 and a count, and reads names without it, so any such number loads.
    std::uint32_t group = 0x6F000000;
    // The first face material's name counter. The game counts it with time and counts on past a
    // name another loaded file has, one more for each age's material in turn.
    std::uint32_t materialCounter = 0;
    // The keys (instance high << 32 | instance) of the materials other character files have, whose
    // names the counter skips.
    std::set<std::uint64_t> takenMaterials;
};

// A newborn's whole character file as the game writes it at a birth: the template copy
// (newCharacterPackage), its look records (addBabyLookRecords), its face shape blended from its
// parents, and for each of its seven ages (baby, toddler, child, teen, young adult, adult, elder) its
// face's two geometry nodes and meshes (near and far away), shape, model and material. The
// portraits are left out, as the game draws a Sim's missing portraits itself and saves them with
// its file. Resources go in the order they are made, as the game reads them by key.
//
// Throws std::runtime_error when the catalog lacks a base face, skeleton or material the newborn
// needs, or a parent's face doesn't fit its base face.
dbpf::PackageImage newbornCharacterPackage(scene::Assets& assets, std::span<const TemplateResource> templ, const NewCharacter& character,
                                           std::span<const std::uint8_t> installedPacks, const NewbornLookParts& parts);

// What a made Sim's look in its character file is made from: its look as Make New Character drew
// it (scene::MadeLook), and the file's names as for a newborn.
struct MadeLookParts {
    // Its age data: "age", "gender", "stretch" and the tones, and its outfit lists.
    scene::AgeData data;
    // Its face template's family as the catalog keeps it (none for the plain face), and the GUID
    // of its eyebrow overlay's family.
    std::string faceFamily, browFamily;
    std::uint32_t group = 0x6F000000;
    std::uint32_t materialCounter = 0;
    std::set<std::uint64_t> takenMaterials;
};

// A made Sim's whole character file as Make New Character writes it: the template copy, its look
// records (addMadeLookRecords), its face shape (scene::madeFaceShape), and for its own age alone its
// face's geometry nodes, meshes, shape, model and material. The material lists its overlays
// eyebrows first. Portraits are left out, as for a newborn.
//
// Throws std::runtime_error when the catalog lacks a base face, skeleton or material the Sim
// needs, its age or gender isn't known, or a face template can't be read.
dbpf::PackageImage madeCharacterPackage(scene::Assets& assets, const std::vector<scene::CatalogPart>& faceTemplates,
                                        std::span<const TemplateResource> templ, const NewCharacter& character,
                                        std::span<const std::uint8_t> installedPacks, const MadeLookParts& parts);

} // namespace ts2::sim
