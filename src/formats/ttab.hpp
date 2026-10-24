#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ts2::ttab {

// One motive an interaction advertises: three 16-bit values per entry.
struct MotiveEffect {
    std::int16_t min = 0;
    std::int16_t delta = 0;
    std::int16_t type = 0;
};

// One interaction of an object's interaction table (TTAB): the tree it runs and the tree that
// checks whether it may be offered, plus its settings. Values whose meaning isn't known yet keep
// neutral names.
struct Interaction {
    std::uint16_t action = 0;  // tree instance
    std::uint16_t test = 0;    // tree instance, 0 for none
    std::uint32_t flags = 0;
    std::uint32_t stringIndex = 0;  // the pie-menu text (in the TTAs string table)
    std::uint32_t attenuationCode = 0;
    float attenuation = 0;
    std::uint32_t autonomy = 0;
    std::int32_t joiningIndex = -1;
    std::uint16_t value1 = 0;
    std::uint32_t value2 = 3;
    float value3 = 0.5f;
    // The object type GUID and thumbnail index of the icon the queue shows for it, when flag
    // 0x400000 is set.
    std::uint32_t iconGuid = 0;
    std::uint32_t iconIndex = 0;
    std::vector<std::vector<MotiveEffect>> motives;
    std::vector<std::vector<std::vector<MotiveEffect>>> moreMotives;  // newer tables only
};

struct InteractionTable {
    std::string name;  // the resource's 64-byte name
    std::uint32_t version = 0;
    std::vector<Interaction> interactions;
    std::string tableName;  // saved at the end by newer tables
    std::size_t extraBytes = 0;
};

InteractionTable parse(std::span<const std::uint8_t> data);

// The interaction scripts and the queue call `id`: the first line whose string index is `id`.
// Null when no line has it.
const Interaction* find(const InteractionTable& table, std::uint32_t id);

} // namespace ts2::ttab
