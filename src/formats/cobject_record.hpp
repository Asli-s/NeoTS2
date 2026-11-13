#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// A lot object's saved scene state (resource 0xFA1C39F7, instance = object id) in full, as the game
// writes it, so it can be written back. The game writes 64 zero bytes, then the header of each class
// of the object's chain, outermost first (tag, version, name), then each class's data, innermost
// first. Fields not known yet are kept as their 32-bit words, floats as their bits, so a record
// writes back exactly.
namespace ts2::cobject {

constexpr std::uint32_t kType = 0xFA1C39F7;

using Word = std::uint32_t;
using Place = std::array<Word, 7>;  // a position, then a rotation as a quaternion

// The classes a record can hold, each with the classes inside it.
enum class Chain { Object, Animatable, Person };

// The materials chosen for the subsets of one node of the model.
struct NodeMaterials {
    std::string node;
    Word value = 0;  // not known yet
    std::vector<std::pair<std::string, std::string>> subsets;  // subset, then material
};

struct PosedJoint {
    std::string name;
    Place place;  // relative to its parent
};

struct NamedWeight {
    std::string first;
    std::string second;
    Word weight = 0;
};

struct ObjectPart {
    std::string model;
    std::vector<NodeMaterials> materials;
    Place place{};
    std::vector<PosedJoint> joints;
    std::vector<NamedWeight> blends;
};

// The request an animation was started with.
struct Request {
    Word flags = 0;  // bit 0 played mirrored
    std::string name;
    std::string type;
    std::int32_t value1 = 0;  // not known yet
    std::int32_t category = 0;
    std::int16_t eventObject = 0;  // the event tree's stack object
    std::uint16_t eventTree = 0;
    std::int32_t value2 = 0;  // not known yet
    Word rate = 0;
    std::array<Word, 15> words1{};  // not known yet, a place among them
    Word eventObjectGuid = 0;
    std::array<Word, 2> words2{};  // not known yet
    std::string text;              // not known yet
    std::optional<std::pair<std::int32_t, Word>> part;  // not known yet
    std::array<Word, 2> words3{};  // not known yet
    std::int32_t value3 = 0;       // not known yet
};

// One segment of a blend curve: 0 rising to the clip's weight, 1 holding it, 2 falling from it.
struct CurveSegment {
    std::int32_t kind = 0;
    std::array<Word, 2> values{};  // not known yet
    Word length = 0;               // in milliseconds
};

struct Curve {
    std::int32_t value = 0;  // not known yet
    Word word = 0;           // not known yet
    std::vector<CurveSegment> segments;
};

// How an animation was playing.
struct Instance {
    std::int32_t state = 0;  // 1 playing
    Word word1 = 0;          // not known yet
    std::int32_t startTick = 0;
    Word stopTick = 0;
    std::array<Word, 3> words2{};  // not known yet
    std::int32_t layer = 0;
    Word weight = 0;
    std::optional<Curve> curve;
    Word word3 = 0;  // not known yet
    std::int32_t plays = 1;  // -1: until stopped
    std::array<Word, 26> words4{};  // not known yet, three places among them
    std::int32_t savedTick = 0;
    std::array<Word, 5> words5{};  // not known yet
    Word time = 0;   // time into the clip
    Word word6 = 0;  // not known yet
};

// An animation entry. The game writes an empty one for an animation it does not keep.
struct Animation {
    enum class Kind : std::uint32_t { None = 0, Clip = 1, Attached = 2 };
    Kind kind = Kind::None;
    Request request;
    Instance instance;
    std::string resource;  // a clip's resource name, or what an attached one is attached to
    Word word = 0;         // a clip's, not known yet
    std::array<Word, 12> attached{};  // an attached one's, not known yet
};

struct AnimatablePart {
    Word scale = 0;
    std::optional<std::array<Word, 14>> places;  // two places, not known yet
    std::array<Word, 7> settings{};              // not known yet
    Place place{};
    std::vector<Animation> animations;
};

struct LocomotionSet {
    Word id = 0;
    std::vector<std::pair<std::string, Word>> weights;  // a name and a float
};

struct LocomotablePart {
    Word value = 0;  // not known yet
    std::vector<LocomotionSet> sets;
};

struct Record {
    Chain chain = Chain::Object;
    ObjectPart object;
    AnimatablePart animatable;    // for Animatable and Person
    LocomotablePart locomotable;  // for Person
};

// Reads a record as the game writes it now. Empty for a chain or a part version the game no longer
// writes (it upgrades them when it loads), a name that is not all zeros, a length written longer
// than needed, or bytes cut short or left over.
std::optional<Record> readRecord(std::span<const std::uint8_t> data);

// Throws std::runtime_error for a list too long to write.
std::vector<std::uint8_t> writeRecord(const Record& record);

// The same record as an object archive holds it: without the zero bytes first, and with 32-bit text
// lengths. `consumed` is set to its length, since more follows it.
std::optional<Record> readArchived(std::span<const std::uint8_t> data, std::size_t& consumed);
std::vector<std::uint8_t> writeArchived(const Record& record);

} // namespace ts2::cobject
