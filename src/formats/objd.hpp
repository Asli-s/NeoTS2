#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ts2::objd {

// An object definition (OBJD): an object type's numbers, read by field index (its GUIDs, price,
// and the ids of its interaction table, slots and strings, among others).
namespace field {
constexpr std::size_t kInteractionTableId = 0x07;
constexpr std::size_t kInteractionGroup = 0x08;  // signed, pairs multi-tile parts (see World::interactionPart)
constexpr std::size_t kType = 0x09;
constexpr std::size_t kMultiTileMasterId = 0x0A;
constexpr std::size_t kMultiTileSubIndex = 0x0B;  // 0xFFFF for the master, (x << 8) | y for parts
constexpr std::size_t kGuid1 = 0x0E;
constexpr std::size_t kGuid2 = 0x0F;
constexpr std::size_t kPrice = 0x12;
constexpr std::size_t kSlotsId = 0x14;
constexpr std::size_t kHauntable = 0x1C;  // nonzero marks a hauntable object (Set to Next 0x21)
constexpr std::size_t kCatalogStringsId = 0x29;
constexpr std::size_t kNumAttributes = 0x3A;
constexpr std::size_t kObjectArrayCount = 0x3B;
constexpr std::size_t kDefaultModel = 0x48;  // the row of its model names (string table 0x85) a new object shows
} // namespace field

constexpr std::size_t kMaxFields = 108;

struct ObjectDefinition {
    std::string name;
    std::vector<std::uint16_t> fields;

    std::uint16_t field(std::size_t index) const {
        return index < fields.size() ? fields[index] : 0;
    }
    // A field as scripts number them (GetObjectDefinitionField): 0 to 107 a field itself, signed, and
    // 201 to 210 a 32-bit value of two fields, low first, starting at fields 0x00, 0x0E (the GUID),
    // 0x46 (an NPC's template GUID), 0x48, 0x5C, 0x15, 0x17, 0x1D, 0x34 and 0x40. None for other numbers.
    std::optional<std::int32_t> scriptField(std::size_t index) const {
        constexpr std::size_t kFirstPair = 201;
        constexpr std::array<std::size_t, 10> kPairs{0x00, 0x0E, 0x46, 0x48, 0x5C, 0x15, 0x17, 0x1D, 0x34, 0x40};
        if (index < kMaxFields) {
            return static_cast<std::int16_t>(field(index));
        }
        if (index >= kFirstPair && index - kFirstPair < kPairs.size()) {
            const std::size_t low = kPairs[index - kFirstPair];
            return static_cast<std::int32_t>(static_cast<std::uint32_t>(field(low)) | static_cast<std::uint32_t>(field(low + 1)) << 16);
        }
        return std::nullopt;
    }
    bool isMultiTileMaster() const {
        return field(field::kMultiTileMasterId) != 0 && field(field::kMultiTileSubIndex) == 0xFFFF;
    }
    std::uint32_t guid() const {
        return static_cast<std::uint32_t>(field(field::kGuid1)) |
               static_cast<std::uint32_t>(field(field::kGuid2)) << 16;
    }
};

ObjectDefinition parse(std::span<const std::uint8_t> data);

} // namespace ts2::objd
