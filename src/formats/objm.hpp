#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ts2::objm {

struct ObjectMap {
    std::uint32_t unknown = 0;
    std::uint32_t version = 0;
    // Object id -> index of its entry in the lot's object type table (objt).
    std::unordered_map<std::uint32_t, std::uint32_t> typeIndexByObject;
    // The ticks each object id still waits before its thread runs again (index id - 1), as a Sleep
    // left it when the lot was saved. Negative or 0 when it doesn't wait. Empty when the map has none.
    std::vector<std::int16_t> waitTicks;
};

// Reads the object list at the start of an ObjM resource and the wait table after it. Each wait
// entry's second value is not known yet.
ObjectMap parse(std::span<const std::uint8_t> data);

// ObjM instance 2: lists of object ids, each under a number. What the lists are is not known yet.
struct ObjectGroups {
    std::uint32_t version = 0;
    std::map<std::uint32_t, std::vector<std::uint16_t>> objectsByKey;
};

ObjectGroups parseGroups(std::span<const std::uint8_t> data);

// The version of instance 1 the game writes. It reads older ones and saves them as this.
constexpr std::uint32_t kWrittenVersion = 0xD8;
constexpr std::uint32_t kGroupsWrittenVersion = 2;

// A script waiting for a message: the object running it and the tree's name.
struct Subscriber {
    std::int32_t object = 0;
    std::string tree;
};

struct Subscription {
    std::uint32_t message = 0;
    std::vector<Subscriber> subscribers;
};

// Every field of instance 1 as the game writes it, to be written back.
struct MapRecord {
    std::array<std::uint8_t, 64> header{};
    // Each object in the lot's list order: its id and its entry in the object type table.
    std::vector<std::pair<std::int32_t, std::int32_t>> objects;
    // The wait table by object id from 1. An id with no object holds 0.
    std::vector<std::uint32_t> waits;
    std::int32_t waitsWithObject = 0;  // how many of those ids had an object
    std::vector<std::int32_t> buildBuyBlockers;  // objects keeping build and buy mode closed, as listed
    std::string text;  // from the simulator, empty in every save seen
    std::vector<Subscription> subscriptions;
};

// Reads instance 1 of the version the game writes. Empty for another version, a first word that
// is not 0, a field the game writes as 0 that is not, or bytes cut short or left over.
std::optional<MapRecord> readMapRecord(std::span<const std::uint8_t> data);
std::vector<std::uint8_t> writeMapRecord(const MapRecord& record);

// Every field of instance 2 (version 2): each key with its list of object ids, in the order read.
struct GroupsRecord {
    std::array<std::uint8_t, 64> header{};
    std::vector<std::pair<std::int32_t, std::vector<std::uint16_t>>> groups;
};

std::optional<GroupsRecord> readGroupsRecord(std::span<const std::uint8_t> data);
std::vector<std::uint8_t> writeGroupsRecord(const GroupsRecord& record);

} // namespace ts2::objm
