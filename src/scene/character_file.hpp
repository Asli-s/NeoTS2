#pragma once

#include "formats/dbpf.hpp"
#include "scene/age_data.hpp"
#include "scene/assets.hpp"
#include "scene/outfit_catalog.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ts2::scene {

// A Sim made in Create-a-Sim, as its character file (Neighborhoods/<hood>/Characters/
// <hood>_User<NNNNN>.package) will hold it: the file it goes to (its name sets the group the file's
// own resources are named in), the Sim's GUID and its names.
struct CharacterFileSim {
    std::filesystem::path file;
    std::uint32_t guid = 0;
    std::string firstName, lastName, bio;
};

struct CharacterResource {
    dbpf::ResourceKey key;
    std::vector<std::uint8_t> data;
};

// The resources of a made Sim's character file, as the game makes one for a Sim it creates:
// - TemplatePerson's resources (its object definition, functions, slots, labels...), the object
//   definition named for the file and the Sim, with the Sim's GUID and TemplatePerson's as its
//   template;
// - the version stamp and the Sim's names (first name, biography, last name);
// - its age data and resource list, the face in every outfit category being its own;
// - its own face for its age and each later one: the base face of its skin (a copy of its resource
//   node, shape, geometry nodes and meshes, named in the file's group) with its face template's
//   change made to the meshes, a material drawing its eyes and eyebrows on it, and the face's part
//   property set.
// Not yet: its face shape (LxNR), portraits, and mesh overlays (facial hair, glasses).
// Throws when the game's files lack what it needs (TemplatePerson, the base face). Problems that
// leave a part out are added to `problems`.
std::vector<CharacterResource> characterResources(Assets& assets, const OutfitCatalog& catalog, const MadeLook& made,
                                                  const CharacterFileSim& sim, std::vector<std::string>& problems);

// Writes them as a package at `file`, uncompressed, through a file beside it that replaces it only
// once written whole.
void writeCharacterFile(const std::filesystem::path& file, const std::vector<CharacterResource>& resources);

} // namespace ts2::scene
