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

// A token as the game writes it now (every field), the same in objects and the neighborhood's
// inventories. Throws std::runtime_error for too many properties.
std::vector<std::uint8_t> writeToken(const Token& token);

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
    std::int32_t stackValue = 0;  // after the frame count, not known yet
    std::int32_t relationshipVersion = 0;
    std::vector<std::uint32_t> relationshipIds;
    Slots slots;
    std::int16_t effectFlag = 0;  // 1 when the object has effects, which follow
    std::uint16_t effectVersion = 0;
    std::vector<Effect> effects;
    std::vector<MaterialOverride> materials;
    std::vector<TimerEntry> timers;
    std::vector<std::uint8_t> materialBlock;
};

// An interaction as saved with a Sim: one waiting in a queue, or one the Sim is running.
// Fields with a version are saved from that lot version on.
struct SavedInteraction {
    std::int32_t value1 = 0;
    // The interaction's own number (0 for a blank interaction). Loading keeps the highest one seen.
    std::int32_t number = 0;
    std::int16_t actor = 0;
    std::int16_t target = 0;
    // The object whose picture the queue shows, by id (-1 for none).
    std::int16_t iconObject = 0;
    std::uint16_t value2 = 0;  // 0xA6
    // The interaction's line in its object's TTAB (-1 for none).
    std::int32_t index = 0;
    std::array<std::uint16_t, 4> values{};
    std::int32_t priority = 0;
    // The tree the interaction runs, by id.
    std::uint16_t tree = 0;
    float value5 = 0;
    std::int32_t flags = 0;
    // Copied from the interaction's TTAB line. lineFlag2 is the line's nested flag (0x8000), which
    // sends the interaction to the sub-queue.
    std::uint16_t lineFlag3 = 0;
    std::uint16_t lineFlag1 = 0;
    std::uint16_t lineFlag2 = 0;
    // The interaction's id from the simulator's id counter, and the id of the interaction it is
    // linked with (a social's other half).
    std::int16_t id = 0;
    std::int16_t linkId = 0;
    std::string name;            // 0x97
    std::int32_t iconGuid = 0;   // 0x97
    std::int32_t iconIndex = 0;  // 0x97
    std::uint16_t value6 = 0;    // 0xBB
};

// A blank interaction is what a Sim with nothing to do keeps as its running one.
bool isBlank(const SavedInteraction& interaction);

// A tree started on a Sim's own stack as a run of its own, saved until it returns.
struct SavedRun {
    std::int16_t object = 0;  // by id, 0 for none
    std::int16_t depth = 0;   // the number of frames on the Sim's stack when it started
    bool flag = false;
};

struct PersonValueEntry {
    std::int16_t key = 0;
    float value1 = 0;
    float value2 = 0;
};

// What follows a Sim's person data: the interactions and their runs. Fields with a version are
// saved from that lot version on.
struct PersonState {
    SavedInteraction running;
    // The main interaction the running one interrupted, or a blank one.
    SavedInteraction interrupted;
    std::vector<SavedInteraction> queue;
    std::vector<SavedRun> runs;
    std::vector<PersonValueEntry> entries;
    // A count kept from the start of the running sub-queue interaction.
    std::uint16_t subStartCount = 0;  // 0x9B
    std::uint16_t value1 = 0;        // 0x9E
    std::optional<SavedInteraction> runningSub;
    std::vector<SavedInteraction> subQueue;
    std::string text;                  // 0x9D
    // The queue picture of the interaction autonomy picked.
    std::int32_t pickedIconIndex = 0;  // 0xA9
    std::int32_t pickedIconGuid = 0;   // 0xA9
    std::int32_t pickedIconObject = 0; // 0xA9
    std::array<std::uint16_t, 8> values{};  // 0xA9
    std::int32_t value2 = 0;                // 0xB4
    std::vector<SavedInteraction> list;     // 0xB5
    std::uint32_t value3 = 0;               // 0xCD
    std::uint32_t value4 = 0;               // 0xCD
};

struct PersonPart {
    std::int32_t version = 0;
    std::array<float, kMotiveCount> motives{};
    std::array<float, kMotiveCount> motives2{};
    std::vector<std::int16_t> personData;
    PersonState state;
};

struct SavedObject {
    std::array<std::uint8_t, 64> name{};
    std::uint32_t version = 0;
    Header header;
    std::vector<std::int16_t> attributes;
    std::vector<std::int16_t> semiAttributes;
    std::array<std::int16_t, kTempCount> temps{};
    // Versions with fewer fields leave the rest at zero, as the game does.
    std::array<std::int16_t, kDataCount> data{};
    // The object's token reference (version 0x66 and later), as SimObject::TokenSource.
    std::array<std::int16_t, 8> tokenSource{};
    std::optional<Tail> tail;
    std::optional<PersonPart> person;
    // Bytes not decoded by parse(): all of the tail before version 0x93, and any after a Sim's part.
    std::vector<std::uint8_t> rest;
};

SavedObject parse(std::span<const std::uint8_t> data, std::uint32_t version,
                  RecordKind kind = RecordKind::Object);

// A saved object inside other data (without the 64-byte name a resource of its own starts with),
// possibly followed by more. `consumed` is set to its length. Throws for records whose length
// isn't known: versions before 0x93 and Sims.
// The version the game writes saved objects with, the lot's version in its object map.
constexpr std::uint32_t kWrittenVersion = 0xD8;

// Writes a saved object as the game writes it, with its 64-byte name. Only version kWrittenVersion
// is written. Throws for another version or a record missing the parts its kind has.
std::vector<std::uint8_t> write(const SavedObject& object, RecordKind kind);

SavedObject parseEmbedded(std::span<const std::uint8_t> data, std::uint32_t version, RecordKind kind,
                          std::size_t& consumed);

// A saved object as an object archive holds it: the version the game writes, without the name, and
// with 32-bit text lengths. `consumed` is set to its length, since more follows it. Throws for Sims.
SavedObject parseArchived(std::span<const std::uint8_t> data, RecordKind kind, std::size_t& consumed);
std::vector<std::uint8_t> writeArchived(const SavedObject& object, RecordKind kind);

} // namespace ts2::xobj
