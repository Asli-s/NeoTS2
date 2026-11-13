#pragma once

#include "formats/cobject_record.hpp"
#include "formats/xobj.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ts2::archive {

// An object archive (0x4F6FD33D): objects a Sim carries in their inventory, saved in their
// character file with one instance per archive. Texts in it have 32-bit lengths.
constexpr std::uint32_t kType = 0x4F6FD33D;
constexpr std::uint32_t kWrittenVersion = 4;

struct Key {
    std::uint32_t type = 0;
    std::uint32_t instance = 0;
    std::uint32_t instanceHigh = 0;
};

// A typed value: 0 a float, 1 a signed number, 2 an unsigned number, 3 true or false, 4 a text,
// 5 a list of pairs of values.
struct Value {
    std::uint8_t type = 1;
    std::uint32_t word = 0;    // the number, a float's bits, or 0 or 1
    std::string text;
    std::vector<Value> pairs;  // for type 5: keys and values in turn
};

// A named list of pairs of values that an object keeps, such as "Sales Info" for an object a Sim
// made and sells.
struct NamedValues {
    std::int32_t key = 0;
    std::string name;
    std::vector<Value> pairs;  // keys and values in turn
};

// The saved state of one part of an object, as in its object record but without the name, then
// its named values.
struct Part {
    xobj::SavedObject state;
    std::vector<NamedValues> values;
};

struct Object {
    std::uint32_t guid = 0;
    std::uint32_t value = 0;   // a number the game keeps with each archived object, its meaning not known yet
    std::vector<Part> parts;   // a multi-tile object has several
    cobject::Record scene;     // the lead part's scene state
};

// Every byte of an object archive as the game writes it now (version 4, objects of the version the
// game writes them with), to be written back. The game writes every object's GUID twice: in a list
// first, then before its parts.
struct Record {
    std::vector<Key> keys;
    std::vector<Object> objects;
};

// Empty for another version of the archive or of its objects, a GUID list that does not match the
// objects, values nested deeper than 16 lists, or bytes cut short or left over.
std::optional<Record> readRecord(std::span<const std::uint8_t> data);

// Throws std::runtime_error for an object without parts, a part or scene state the writers refuse,
// a value type the game does not write, values nested deeper than 16 lists, a pair list with a key
// and no value, or a list too long to write.
std::vector<std::uint8_t> writeRecord(const Record& record);

} // namespace ts2::archive
