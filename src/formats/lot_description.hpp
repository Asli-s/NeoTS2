#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ts2::nhood {

// A lot as the neighborhood places it (resource 0x0BF999E7 in <hood>_Neighborhood.package, one per
// lot, instance = lot id). It starts with the lot's own description, the same block its lot package
// keeps (resource 0x6C589723 there).
struct LotDescription {
    std::uint16_t version = 0;
    std::uint32_t width = 0, depth = 0;  // tiles
    std::uint8_t type = 0;
    std::uint8_t roadSides = 0;  // a bit per side with a road, 1 << ownFacing among them
    std::uint8_t ownFacing = 0;  // the quarter turn the lot was built facing (on a beach lot, its beach's side)
    // Flags (from version 5): 0x80 a beach lot.
    std::uint32_t flags = 0;
    std::string name;
    std::string description;  // the text its card shows (a community lot's)
    std::uint32_t x = 0, y = 0;  // the lot's corner, in neighborhood tiles
    float elevation = 0;
    std::uint32_t id = 0;
    std::uint8_t facing = 0;  // the quarter turn it is placed at in the neighborhood
    std::string texture;
    std::int32_t owner = 0;  // from version 0x0E: the owning neighbor's id
    // How brightly each room's lights are set, in percent, for rooms the lot saved one for (from
    // version 0x12, the last block: a count, then room and percent pairs by ascending room).
    std::map<std::int16_t, std::int16_t> roomLightPercents;

    // A room's light percent, 100 unless saved (room 0, the outdoors, is always 100).
    std::int16_t roomLightPercent(std::int16_t room) const;
};

constexpr std::uint32_t kLotDescriptionType = 0x0BF999E7;

LotDescription parseLotDescription(std::span<const std::uint8_t> data);

// The lot's own description as its lot package keeps it (resource 0x6C589723): a 64-byte name, then
// the block, its texts with 7-bit lengths. The game writes version 11 and reads older ones.
constexpr std::uint32_t kLotInfoType = 0x6C589723;
constexpr std::uint16_t kLotInfoWrittenVersion = 11;

// Every byte of a version 11 lot info record, to be written back. Fields whose meaning is not known
// yet are kept as their bytes.
struct LotInfoRecord {
    std::array<std::uint8_t, 64> header{};
    std::uint32_t width = 0, depth = 0;  // tiles
    std::uint8_t type = 0;
    std::uint8_t roadSides = 0;
    std::uint8_t ownFacing = 0;
    std::uint32_t flags = 0;
    std::string name;
    std::string description;
    std::vector<std::uint32_t> values;  // a counted list of floats, kept as their bits
    std::array<std::uint8_t, 22> rest{};
};

// Reads a version 11 record. Empty for any other version, a record cut short or longer than its
// fields, or a text length not written the shortest way (as the game writes them).
std::optional<LotInfoRecord> readLotInfo(std::span<const std::uint8_t> data);

std::vector<std::uint8_t> writeLotInfo(const LotInfoRecord& record);

// Every byte of a version 0x12 lot description in the neighborhood (0x0BF999E7), to be written back:
// the lot info block (with 32-bit text lengths, as the neighborhood package writes text), where and
// how the lot is placed, then two lists. Fields whose meaning is not known yet are kept as numbers.
constexpr std::uint16_t kLotDescriptionWrittenVersion = 0x12;

struct LotDescriptionRecord {
    LotInfoRecord own;  // its header is not part of this record
    std::uint32_t x = 0, y = 0;
    std::uint32_t elevation = 0;  // a float, kept as its bits
    std::int32_t id = 0;
    std::uint8_t facing = 0;
    std::string texture;
    std::int32_t owner = 0;
    std::int32_t value1 = 0;  // not known yet
    std::uint8_t flag = 0;    // not known yet
    std::int32_t value2 = 0, value3 = 0;  // not known yet
    std::vector<std::array<std::uint32_t, 4>> entries;  // not known yet
    // Room and light percent pairs, in the game's order (by room).
    std::vector<std::pair<std::uint16_t, std::uint16_t>> roomLights;
};

// Empty for any other version, a record cut short or longer than its fields, or a byte the game
// writes as 0 that is not.
std::optional<LotDescriptionRecord> readLotDescriptionRecord(std::span<const std::uint8_t> data);

// Throws std::runtime_error for a list or text too long to write.
std::vector<std::uint8_t> writeLotDescriptionRecord(const LotDescriptionRecord& record);

} // namespace ts2::nhood
