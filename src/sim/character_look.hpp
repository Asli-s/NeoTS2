#pragma once

#include "formats/dbpf.hpp"
#include "formats/dbpf_writer.hpp"

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace ts2::sim {

// What a newborn's character file records of its look: its gender (1 female, 2 male), the
// dominant values of its skin, hair and eye genes, and the four catalog parts it wears as a baby
// (its everyday body, and its skin outfit's bald head, bare body and face, all of its skin tone).
struct BabyLook {
    std::uint32_t gender = 0;
    std::string skinTone, hairTone, eyeColour;
    dbpf::ResourceKey everydayBody, skinHair, skinBody, skinFace;
};

// Adds a newborn's look records to its character file as the game writes them: its age data
// (AGED), and for each of its seven ages a skin part (GZPS) of its own face, with the list of
// resources it uses (3IDR). The age data's list holds its baby face's model, its skeleton, its baby
// face part and the four catalog parts. Every outfit category takes its own face, the everyday
// outfit also the everyday body, and the skin outfit the catalog's bald head, face and body. The
// elder's part has grey hair, as every elder does. The models, meshes and materials these name are
// not written yet.
void addBabyLookRecords(dbpf::PackageImage& package, const BabyLook& look);

// What a made Sim's character file records of its look, for its one age: the age flag (1 toddler,
// 2 child, 4 teen, 8 adult, 0x10 elder, 0x40 young adult) and number in its face model's name
// (newFaceModelKey), gender (1 female, 2 male), stretch, tones as GUIDs, the skeleton model it
// stands on, and its outfit lists (category << 16 | part) of catalog parts. Every face list wears
// the Sim's own face, whatever it names.
struct MadeAgeLook {
    std::uint32_t ageFlag = 0, gender = 0;
    int ageNumber = 0;
    float stretch = 1.0f;
    std::string skinTone, hairTone, eyeColour;
    dbpf::ResourceKey skeleton;
    std::map<std::uint32_t, std::vector<dbpf::ResourceKey>> lists;
    // The outfit categories of each mesh overlay (a mesh beard, glasses) the face wears, in the order worn.
    std::vector<std::uint32_t> overlayCategories;
};

// Adds a made Sim's look records to its character file as Make New Character writes them: its age
// data (AGED), the list of resources the age data points into (3IDR) and the skin part (GZPS) of
// its own face. The list holds its face model, an empty key, its skeleton, then each part in the
// order the outfit lists first name it, walked in ascending key order. Lists with no parts are left
// out, as the game's files hold none. Each mesh overlay gets a skin part of its own after the face's
// (instance 2 on), with its categories and a list naming its model, and every face list wears it
// after the face.
void addMadeLookRecords(dbpf::PackageImage& package, const MadeAgeLook& look);

// Adds a Sim's face shape record (LxNR, as faceshape::write makes it) to its character file, as
// instance 1 of the file's own group.
void addFaceShapeRecord(dbpf::PackageImage& package, std::span<const std::uint8_t> record);

// The model (CRES) of a new Sim's face at age number `age` (0 toddler to 5 baby, 7 young adult),
// "age<number>_0_cres" in the character file's own group, or of its mesh overlay `part` (from 1).
dbpf::ResourceKey newFaceModelKey(int age, int part = 0);

// A Sim's look as its age data keeps it: its text and number properties by name (as read, such as
// "haircolor"), and its outfit lists, keyed (outfit category << 16) | part, of part keys.
struct OutfitLook {
    std::map<std::string, std::string> properties;
    std::map<std::uint32_t, std::vector<dbpf::ResourceKey>> lists;
};

// A character file with its age data (AGED) and that data's resource list (3IDR) rewritten for
// `look`, as the game's save writes them. Every other record and every other property stays as it
// was. The resource list keeps the entries before the first one an outfit list uses (its model and
// skeleton), then lists each part the outfit lists name once, in the order the lists first name
// them, the lists taken in key order. Only properties the age data already has and that hold text,
// a whole number or a float (the stretch) change. The portrait of the age the file held and its
// shadow are left out, for the game to draw anew. A Sim that aged keeps its old age's model entry,
// as the game's saves keep it. Throws std::runtime_error when the file has no age data or it
// can't be read.
std::vector<std::uint8_t> rewriteOutfitLook(std::span<const std::uint8_t> characterFile, const OutfitLook& look);

} // namespace ts2::sim
