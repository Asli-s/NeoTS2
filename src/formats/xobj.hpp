#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ts2 {
class BinaryReader;
}

namespace ts2::xobj {

constexpr std::size_t kTempCount = 8;
constexpr std::size_t kDataCount = 114;
constexpr std::size_t kMotiveCount = 16;
constexpr std::size_t kPersonDataCount = 231;

// Saved objects carry no version of their own: the lot's version (from ObjM) decides the layout.
std::size_t dataCountForVersion(std::uint32_t version);
std::size_t personDataCountForVersion(std::uint32_t version);

// XOBJ, XMTO (adds a block at the end) or PERS (adds the Sim's own part).
enum class RecordKind { Object, ObjectWithMaterial, Person };

struct Header {
    std::array<float, 4> reserved{};
    std::array<float, 2> location{};
    std::int32_t value1 = 0;
    std::int16_t value2 = 0;
    float value3 = 0;
    std::int32_t value4 = -1;
    std::int16_t value5 = 0;
    float value6 = 0;
};

struct Token {
    std::uint32_t guid = 0;
    std::uint16_t flags = 0;
    std::uint16_t value1 = 0;
    std::uint32_t value2 = 0;
    std::uint16_t value3 = 0;
    std::vector<std::int16_t> properties;
};

// Reads a token as saved with this version (objects and the neighborhood's inventories).
Token readToken(BinaryReader& r, std::uint32_t version);

struct StackFrame {
    std::int16_t objectId = 0;
    std::int16_t treeId = 0;
    std::uint16_t node = 0;
    std::int16_t value1 = 0;
    std::int16_t value2 = 0;
    std::vector<std::int16_t> params;
    std::vector<std::int16_t> locals;
    std::int32_t value3 = 0;
    // Index into the lot's object type table (objt): the object whose code the frame runs.
    std::int16_t codeOwner = 0;
};

struct Slots {
    std::int16_t marker = 0;
    std::int16_t extra = 0;
    std::vector<std::int16_t> values;
};

struct Effect {
    std::string name;
    std::string slot;
    std::uint32_t value1 = 0;
    std::uint32_t value2 = 0;
    std::uint8_t value3 = 0;
};

struct MaterialOverride {
    std::string resource;
    std::string subset;
    std::string material;
};

struct TimerEntry {
    std::uint32_t value1 = 0;
    std::uint32_t value2 = 0;
    std::uint16_t value3 = 0;
    std::uint32_t value4 = 0;
    std::int32_t value5 = 0;
    std::int32_t value6 = 0;
    std::array<std::int16_t, 8> values{};
};

// Everything after the object data, for versions 0x93 and later.
struct Tail {
    Token token;
    std::vector<std::vector<std::int16_t>> arrays;
    std::vector<std::int16_t> shorts;
    std::int32_t value1 = 0;
    std::vector<std::string> strings;
    std::uint16_t value2 = 0;
    std::vector<StackFrame> stack;
    std::int32_t relationshipVersion = 0;
    std::vector<std::uint32_t> relationshipIds;
    Slots slots;
    std::uint16_t effectVersion = 0;
    std::vector<Effect> effects;
    std::vector<MaterialOverride> materials;
    std::vector<TimerEntry> timers;
    std::vector<std::uint8_t> materialBlock;
};

struct PersonPart {
    std::int32_t version = 0;
    std::array<float, kMotiveCount> motives{};
    std::array<float, kMotiveCount> motives2{};
    std::vector<std::int16_t> personData;
};

struct SavedObject {
    std::uint32_t version = 0;
    Header header;
    std::vector<std::int16_t> attributes;
    std::vector<std::int16_t> semiAttributes;
    std::array<std::int16_t, kTempCount> temps{};
    // Versions with fewer fields leave the rest at zero, as the game does.
    std::array<std::int16_t, kDataCount> data{};
    std::array<std::int16_t, 8> extra{};
    std::optional<Tail> tail;
    std::optional<PersonPart> person;
    // Bytes not decoded: all of the tail before version 0x93, and the end of a Sim's part.
    std::vector<std::uint8_t> rest;
};

SavedObject parse(std::span<const std::uint8_t> data, std::uint32_t version,
                  RecordKind kind = RecordKind::Object);

} // namespace ts2::xobj
