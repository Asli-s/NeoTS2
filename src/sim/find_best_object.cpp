#include "sim/find_best_object.hpp"

#include "sim/object_functions.hpp"

#include <cmath>
#include <limits>

namespace ts2::sim {

namespace {

// The functions the game can search for, each with the object data field rating how well an object
// serves it (-1: none, every object rates 50).
struct SearchableFunction {
    std::uint16_t function;
    std::int16_t rating;
};
constexpr SearchableFunction kFunctions[] = {
    {0x00, 0x1F},  // prepare food: prep value
    {0x01, 0x20},  // cook food: cook value
    {0x02, 0x21},  // flat surface: surface value
    {0x03, 0x24},  // dispose: dispose value
    {0x06, 0x25},  // wash dish: wash dish value
    {0x07, 0x26},  // eating surface: eating surface value
    {0x08, 0x1A},  // sit: sitting value
    {0x0A, 0x3D},  // serving surface: serving surface value
    {0x0B, 0x27},  // clean: dirty level
    {0x0C, 0x40},  // garden: gardening value
    {0x0D, 0x41},  // wash hands: wash hands value
    {0x0E, 0x0F},  // repair: repair level
    {0x0F, -1},    // dry clothes
    {0x17, -1},    // awareness
    {0x19, -1},    // look-at target
    {0x1B, -1},    // global awareness
};

constexpr std::size_t kFunctionScoreField = 0x42;
constexpr std::size_t kRoomField = 0x1D;
constexpr std::size_t kUnavailableField = 0x38;   // bits 0x01 and 0x04 rule an object out
constexpr std::size_t kReservedField = 0x6B;
constexpr std::size_t kAwarenessLockoutField = 0x19;
constexpr std::int16_t kUnratedScore = 50;
constexpr float kHalfScoreDistance = 5.0f;  // tiles
constexpr float kLevelHeight = 10.0f;       // a level counts as this many tiles

constexpr std::uint8_t kOutsideOnly = 0x01;
constexpr std::uint8_t kInsideOnly = 0x02;
constexpr std::uint8_t kInRoomOnly = 0x04;
constexpr std::uint8_t kNotInRoomOnly = 0x20;
constexpr std::uint8_t kRoomInTempOnly = 0x40;
constexpr std::uint8_t kIgnoringLockout = 0x80;

float distance(const SimObject& a, const SimObject& b) {
    const float dx = static_cast<float>(a.x - b.x) / 65536.0f;
    const float dy = static_cast<float>(a.y - b.y) / 65536.0f;
    const float dz = static_cast<float>(b.level - a.level) * kLevelHeight;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool inside(const World& world, const SimObject& object) {
    const RoomInfo* room = world.rooms().room(object.data[kRoomField]);
    return room != nullptr && room->layerRegion[0] > 0;
}

// The room filters of the flags (the first that is set applies).
bool inWantedRoom(const FunctionSearch& s, const SimObject& candidate, std::uint8_t flags, bool roomArray) {
    const std::int16_t room = candidate.data[kRoomField];
    if (flags & kOutsideOnly) {
        return !inside(s.world, candidate);
    }
    if (flags & kInsideOnly) {
        return inside(s.world, candidate);
    }
    if (flags & kInRoomOnly) {
        return room == s.origin.data[kRoomField];
    }
    if (flags & kNotInRoomOnly) {
        return room != s.origin.data[kRoomField];
    }
    if (flags & kRoomInTempOnly) {
        return room == s.temps[0];
    }
    if (roomArray) {
        const SimObject* holder = s.world.find(s.temps[0]);
        const auto index = static_cast<std::size_t>(s.temps[1]);
        if (holder == nullptr || s.temps[1] < 0 || index >= holder->arrays.size()) {
            return false;
        }
        for (const std::int16_t r : holder->arrays[index]) {
            if (r == room) {
                return true;
            }
        }
        return false;
    }
    return true;
}

} // namespace

std::optional<std::int16_t> findBestObjectForFunction(const FunctionSearch& s) {
    const auto& o = s.operands;
    const auto function = static_cast<std::uint16_t>(o[0] | o[1] << 8);
    const SearchableFunction* wanted = nullptr;
    for (const auto& f : kFunctions) {
        if (f.function == function) {
            wanted = &f;
        }
    }
    const auto entryIndex = wanted ? objectFunctionEntry(function) : std::nullopt;
    if (!entryIndex) {
        return std::nullopt;
    }
    const std::uint8_t flags = o[2];
    const bool roomArray = (o[6] & 0x01) != 0;
    const bool awareness = *entryIndex == 0x2A || *entryIndex == 0x2C;

    std::optional<std::int16_t> best;
    float bestScore = -std::numeric_limits<float>::max();
    std::vector<std::int16_t> ids;
    s.world.nextAfter(0, [&](const SimObject& candidate) {
        ids.push_back(candidate.id);
        return false;
    });
    for (const std::int16_t id : ids) {
        SimObject* candidate = s.world.find(id);
        const ObjectType* type = candidate ? s.code.objectType(candidate->definition.guid()) : nullptr;
        const auto* functions = type ? s.code.objectFunctions(*type) : nullptr;
        if (functions == nullptr || *entryIndex >= functions->size()) {
            continue;
        }
        const objf::Function& entry = (*functions)[*entryIndex];
        if (entry.check == 0 && entry.action == 0) {
            continue;
        }
        const auto& data = candidate->data;
        if ((data[kUnavailableField] & 0x05) != 0) {
            continue;
        }
        if (data[kReservedField] != 0 && !(*entryIndex == 0x1F && (data[kReservedField] & 0xFF00) == 0x200)) {
            continue;
        }
        if (awareness && !(flags & kIgnoringLockout) && data[kAwarenessLockoutField] > 0) {
            continue;
        }
        auto& score = s.me.data[kFunctionScoreField];
        score = wanted->rating < 0 ? kUnratedScore : data[static_cast<std::size_t>(wanted->rating)];
        if (score == 0) {
            continue;
        }
        if (entry.check != 0 && (!s.check(*candidate, entry.check) || s.me.data[kFunctionScoreField] == 0)) {
            continue;
        }
        if (!inWantedRoom(s, *candidate, flags, roomArray)) {
            continue;
        }
        const float value = static_cast<float>(s.me.data[kFunctionScoreField]) /
                            (distance(s.origin, *candidate) / kHalfScoreDistance + 1.0f);
        if (value > bestScore) {
            bestScore = value;
            best = candidate->id;
        }
    }
    return best;
}

} // namespace ts2::sim
