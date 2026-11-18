#pragma once

#include "formats/dbpf_writer.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ts2::sim {

// A resource of the template a new character is made from, as the installed game holds it (the
// highest pack's version of each key).
struct TemplateResource {
    dbpf::ResourceKey key;
    std::vector<std::uint8_t> body;
};

struct NewCharacter {
    std::string fileName;    // the package's name without ".package", "<hood>_User<5 digits>"
    std::string firstName;   // not empty
    std::string surname;
    std::uint32_t guid = 0;          // its new object type
    std::uint32_t templateGuid = 0;  // the template it is made from
    // For a Sim Make New Character named, its first name and surname in each language (by language
    // - 1). Empty for a newborn, whose names are written in US English alone.
    std::vector<std::pair<std::string, std::string>> languageNames;
};

// The comment every text of a Sim's name gets when the game names a new Sim. Where the game takes
// it from is not known yet. Every Sim born in a game of this version has it.
inline constexpr const char* kNewNameComment = "Sims 2 EP16: Needs Text";

// The first number from 0 with no "<hood>_User<number>.package" among `existing` (the names of the
// Characters folder's files), as five digits. Empty when all of 0 to 31999 are taken, the game's
// limit.
std::string newCharacterFileName(std::string_view hood, std::span<const std::string> existing);

// The game's languages a made Sim's name is written in.
inline constexpr std::size_t kNameLanguages = 44;

// Row `row` of a name table in each of the game's 44 languages, as Make New Character names a Sim:
// the row in that language, or the US English row when the table lacks the language, is shorter, or
// holds an empty text there. `lists` holds the table's texts by language - 1. Empty when the US
// English list has no such row.
std::vector<std::string> inEveryLanguage(const std::vector<std::vector<std::string>>& lists, std::size_t row);

// A new character's package as the game starts it: every resource of `templ` (the template's
// group) but its name map (NREF), under group 0xFFFFFFFF. The object definition becomes a person
// (type 2) under the new GUID, with the template's GUID in fields 0x46 and 0x47, named
// "<file> - <first name>". It gets the name text the object definition points to (CTSS), in each
// language of `languageNames` when it has them, and `installedPacks`, the installed packs record
// (0xEBFEE342) every file the game writes holds. Its look is not in it. Throws std::runtime_error for a template without an object definition,
// one of a layout not known, or a name too long.
dbpf::PackageImage newCharacterPackage(std::span<const TemplateResource> templ, const NewCharacter& character,
                                       std::span<const std::uint8_t> installedPacks);

} // namespace ts2::sim
