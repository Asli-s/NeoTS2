#include "sim/character_file.hpp"

#include "formats/resource_types.hpp"
#include "formats/str.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <set>
#include <stdexcept>

namespace ts2::sim {

namespace {

constexpr std::uint32_t kNameMapType = 0x4E524546;   // NREF
constexpr std::uint32_t kInstalledPacksType = 0xEBFEE342;
constexpr std::uint32_t kNewGroup = 0xFFFFFFFF;
constexpr std::size_t kNameBytes = 64;
constexpr std::size_t kFieldsStart = kNameBytes;
// The object definition's fields up to its second name: that name's length sits where field
// 0x6C would be, then its bytes.
constexpr std::size_t kSecondNameAt = kFieldsStart + 2 * 0x6C;
constexpr std::size_t kTypeField = 0x09, kGuidField = 0x0E, kOwnGuidField = 0x1D, kTemplateGuidField = 0x46;
constexpr std::size_t kCatalogStringsField = 0x29;
constexpr std::uint8_t kPersonType = 2;
constexpr int kMostCharacterFiles = 32000;

std::uint16_t fieldAt(std::span<const std::uint8_t> objd, std::size_t field) {
    const std::size_t at = kFieldsStart + 2 * field;
    return static_cast<std::uint16_t>(objd[at] | objd[at + 1] << 8);
}

void setField(std::vector<std::uint8_t>& objd, std::size_t field, std::uint16_t value) {
    const std::size_t at = kFieldsStart + 2 * field;
    objd[at] = static_cast<std::uint8_t>(value);
    objd[at + 1] = static_cast<std::uint8_t>(value >> 8);
}

void setGuid(std::vector<std::uint8_t>& objd, std::size_t field, std::uint32_t guid) {
    setField(objd, field, static_cast<std::uint16_t>(guid));
    setField(objd, field + 1, static_cast<std::uint16_t>(guid >> 16));
}

std::vector<std::uint8_t> renamedDefinition(std::span<const std::uint8_t> templ, const NewCharacter& character) {
    if (templ.size() < kSecondNameAt + 4) {
        throw std::runtime_error("character file: template object definition too short");
    }
    const std::uint32_t secondName = static_cast<std::uint32_t>(templ[kSecondNameAt]) | templ[kSecondNameAt + 1] << 8 |
                                     templ[kSecondNameAt + 2] << 16 | static_cast<std::uint32_t>(templ[kSecondNameAt + 3]) << 24;
    if (templ.size() != kSecondNameAt + 4 + secondName) {
        throw std::runtime_error("character file: template object definition of a layout not known");
    }
    const std::string name = character.fileName + " - " + character.firstName;
    if (name.size() >= kNameBytes) {
        throw std::runtime_error("character file: name too long");
    }
    std::vector<std::uint8_t> out(templ.begin(), templ.begin() + kSecondNameAt);
    std::fill(out.begin(), out.begin() + kNameBytes, std::uint8_t{0});
    std::copy(name.begin(), name.end(), out.begin());
    setField(out, kTypeField, kPersonType);
    setGuid(out, kGuidField, character.guid);
    setGuid(out, kOwnGuidField, character.guid);
    setGuid(out, kTemplateGuidField, character.templateGuid);
    const auto length = static_cast<std::uint32_t>(name.size());
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(length >> (8 * i)));
    }
    out.insert(out.end(), name.begin(), name.end());
    return out;
}

} // namespace

std::string newCharacterFileName(std::string_view hood, std::span<const std::string> existing) {
    // Names are compared in any case, as Windows finds the files, so a renamed file is never overwritten.
    const auto lower = [](std::string s) {
        for (char& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    };
    std::set<std::string> taken;
    for (const std::string& file : existing) {
        taken.insert(lower(file));
    }
    for (int n = 0; n < kMostCharacterFiles; ++n) {
        char digits[8];
        std::snprintf(digits, sizeof digits, "%05d", n);
        std::string name = std::string(hood) + "_User" + digits;
        if (!taken.contains(lower(name + ".package"))) {
            return name;
        }
    }
    return {};
}

std::vector<std::string> inEveryLanguage(const std::vector<std::vector<std::string>>& lists, std::size_t row) {
    if (lists.empty() || row >= lists[0].size()) {
        return {};
    }
    std::vector<std::string> out(kNameLanguages);
    for (std::size_t i = 0; i < kNameLanguages; ++i) {
        const auto* list = i < lists.size() ? &lists[i] : nullptr;
        const bool own = list != nullptr && row < list->size() && (i == 0 || !(*list)[row].empty());
        out[i] = own ? (*list)[row] : lists[0][row];
    }
    return out;
}

dbpf::PackageImage newCharacterPackage(std::span<const TemplateResource> templ, const NewCharacter& character,
                                       std::span<const std::uint8_t> installedPacks) {
    if (character.firstName.empty()) {
        throw std::runtime_error("character file: no first name");
    }
    dbpf::PackageImage out;
    std::optional<std::uint16_t> catalogStrings;
    for (const TemplateResource& resource : templ) {
        if (resource.key.type == kNameMapType) {
            continue;
        }
        const dbpf::ResourceKey key{resource.key.type, kNewGroup, resource.key.instance, resource.key.instanceHigh};
        if (resource.key.type == types::kObjectDefinition) {
            const std::vector<std::uint8_t> definition = renamedDefinition(resource.body, character);
            catalogStrings = fieldAt(definition, kCatalogStringsField);
            out.resources.push_back(dbpf::storeResource(key, definition));
        } else {
            out.resources.push_back(dbpf::storeResource(key, resource.body));
        }
    }
    if (!catalogStrings) {
        throw std::runtime_error("character file: template without an object definition");
    }
    str::Record name;
    if (character.languageNames.empty()) {
        name.entries = {{1, character.firstName, kNewNameComment}, {1, "", kNewNameComment}, {1, character.surname, kNewNameComment}};
    }
    // Only the first name and surname lines are given in each language, so the line between them
    // has no comment.
    for (std::size_t i = 0; i < character.languageNames.size(); ++i) {
        const auto language = static_cast<std::uint8_t>(i + 1);
        const auto& [first, surname] = character.languageNames[i];
        name.entries.push_back({language, first, kNewNameComment});
        name.entries.push_back({language, "", ""});
        name.entries.push_back({language, surname, kNewNameComment});
    }
    out.resources.push_back(dbpf::storeResource({types::kCatalogString, kNewGroup, *catalogStrings, 0}, str::writeRecord(name)));
    out.resources.push_back(dbpf::storeResource({kInstalledPacksType, kNewGroup, 1, 0}, installedPacks));
    for (std::size_t i = 0; i < out.resources.size(); ++i) {
        out.dataOrder.push_back(i);
    }
    return out;
}

} // namespace ts2::sim
