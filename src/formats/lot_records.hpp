#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Small records the game rewrites on every save, as it writes them, so they can be written back.
// Each lot record starts with 64 zero bytes. What most of their fields hold is not known yet, so
// they are kept as numbers (floats as their bits). Each reader is empty for another type or
// version, a name that is not all zeros, a flag other than 0 or 1, a length written longer than
// needed, or bytes cut short or left over.
namespace ts2::lotrecords {

using Word = std::uint32_t;

// The lot's house record (HOUS 0x484F5553, version 0x55): a number, then text.
constexpr std::uint32_t kHouseType = 0x484F5553;
struct House {
    Word value = 0;
    std::string text;
};
std::optional<House> readHouse(std::span<const std::uint8_t> data);
std::vector<std::uint8_t> writeHouse(const House& house);

// Pair tables (0xACE46235, version 1): a number, then pairs of numbers or, in other instances, pairs
// of texts, in the order of the game's own map.
constexpr std::uint32_t kPairTableType = 0xACE46235;
struct PairTable {
    Word value = 0;
    bool texts = false;
    std::vector<std::pair<Word, Word>> numbers;
    std::vector<std::pair<std::string, std::string>> textPairs;
};
// `texts` says which kind the instance holds.
std::optional<PairTable> readPairTable(std::span<const std::uint8_t> data, bool texts);
std::vector<std::uint8_t> writePairTable(const PairTable& table);

// Tables of fixed-size entries (version 1): 0x0C900FDB with nine numbers per entry, 0x916DA14D with
// four.
constexpr std::uint32_t kNineWordTableType = 0x0C900FDB;
constexpr std::uint32_t kFourWordTableType = 0x916DA14D;
struct WordTable {
    std::uint32_t type = kFourWordTableType;
    std::vector<Word> words;  // entry after entry
};
std::size_t entryWords(std::uint32_t type);  // 0 for another type
std::optional<WordTable> readWordTable(std::span<const std::uint8_t> data);
// Throws std::runtime_error for another type or words that do not make whole entries.
std::vector<std::uint8_t> writeWordTable(const WordTable& table);

// The vertex layer (cVertexLayer 0xCB4387A1, version 1): points like the fence posts' with a level,
// a place and a number.
constexpr std::uint32_t kVertexLayerType = 0xCB4387A1;
struct Vertex {
    std::int32_t level = 0;
    Word x = 0;
    Word y = 0;
    Word value = 0;
};
std::optional<std::vector<Vertex>> readVertexLayer(std::span<const std::uint8_t> data);
std::vector<std::uint8_t> writeVertexLayer(const std::vector<Vertex>& vertices);

// 0x8B0C79D6 (version 3): a flag and two numbers.
constexpr std::uint32_t kFlagRecordType = 0x8B0C79D6;
struct FlagRecord {
    bool flag = false;
    std::int32_t first = 0;
    std::int32_t second = 0;
};
std::optional<FlagRecord> readFlagRecord(std::span<const std::uint8_t> data);
std::vector<std::uint8_t> writeFlagRecord(const FlagRecord& record);

// An object's routing aggregate (cTSRoutingObjectAggregate 0xBC66BAEC, version 6, instance = object
// id): a packed number, a float, four 16-bit values when a flag is set, a list of 16-bit values, then
// the objects in it. The game writes each object through the object's own writer, whose layout is
// not known yet, so they are kept as their bytes. Records of other versions were left by older
// games. A record with more than one object, which none of the test saves has, is not read.
constexpr std::uint32_t kRoutingAggregateType = 0xBC66BAEC;
struct RoutingAggregate {
    Word packed = 0;  // a 16-bit number, with bit 16 set by a flag
    Word value = 0;   // a float
    std::optional<std::array<std::uint16_t, 4>> extra;
    std::vector<std::uint16_t> list;
    std::int32_t objects = 0;              // 0 or 1
    std::vector<std::uint8_t> objectBytes;  // what the game wrote for them
};
std::optional<RoutingAggregate> readRoutingAggregate(std::span<const std::uint8_t> data);
// Throws std::runtime_error for more than one object or object bytes that do not match the count.
std::vector<std::uint8_t> writeRoutingAggregate(const RoutingAggregate& aggregate);

// The lot's SG system record (cTSSGSystem 0xBA353CE1, version 4): three numbers, then pairs of
// numbers, one pair per item of the game's list.
constexpr std::uint32_t kSgSystemType = 0xBA353CE1;
struct SgSystem {
    std::array<Word, 3> values{};
    std::vector<std::pair<Word, Word>> pairs;
};
std::optional<SgSystem> readSgSystem(std::span<const std::uint8_t> data);
std::vector<std::uint8_t> writeSgSystem(const SgSystem& system);

// A character file's archive counter (0x0F9F0C21, instance 0): the version 4, then the highest
// instance its object archive records (0x4F6FD33D) use. Unlike the lot's records it has no zero
// bytes first. The game writes it on every load and save of a Sim with archived inventory.
constexpr std::uint32_t kArchiveCounterType = 0x0F9F0C21;
std::optional<std::uint32_t> readArchiveCounter(std::span<const std::uint8_t> data);
std::vector<std::uint8_t> writeArchiveCounter(std::uint32_t lastArchive);

// The neighborhood's Sim DNA index (0x2C310F46, instance 0): the version 1, then the instance of
// every Sim DNA record (0xEBFEE33F) in the package, in ascending order. It has no zero bytes first.
constexpr std::uint32_t kDnaIndexType = 0x2C310F46;
// Empty for another version, instances not strictly ascending, or bytes cut short or left over.
std::optional<std::vector<std::uint32_t>> readDnaIndex(std::span<const std::uint8_t> data);
// Throws std::runtime_error for instances not strictly ascending or too many to write.
std::vector<std::uint8_t> writeDnaIndex(const std::vector<std::uint32_t>& instances);

// The neighborhood's 0xCC2A6A34 record (version 0xCB): one 16-bit value. Like the counter, it has no
// zero bytes first.
constexpr std::uint32_t kShortRecordType = 0xCC2A6A34;
std::optional<std::uint16_t> readShortRecord(std::span<const std::uint8_t> data);
std::vector<std::uint8_t> writeShortRecord(std::uint16_t value);

// The neighborhood's TATT record (0x54415454, version 0x55): lists of 16-bit values, each under a
// number, in the order of the game's own map.
constexpr std::uint32_t kKeyedListsType = 0x54415454;
struct KeyedLists {
    std::vector<std::pair<Word, std::vector<std::uint16_t>>> lists;
};
std::optional<KeyedLists> readKeyedLists(std::span<const std::uint8_t> data);
// Throws std::runtime_error for a list too long to write.
std::vector<std::uint8_t> writeKeyedLists(const KeyedLists& lists);

// The neighborhood's 0xAC8A7A2E record (version 10, no zero bytes first, texts with 32-bit lengths as
// the neighborhood package writes them): the neighborhood's id ("N001"), a number, another id (empty
// or a linked neighborhood's, "P001"), the installed products mask and numbers not known yet.
constexpr std::uint32_t kNeighborhoodInfoType = 0xAC8A7A2E;
struct NeighborhoodInfo {
    std::string id;
    std::int32_t value1 = 0;
    Word value2 = 0;
    std::string linked;
    Word value3 = 0;
    Word products = 0;  // a bit per installed pack, as the game found them when it saved
    Word value4 = 0;
    Word value5 = 0;
    std::array<std::uint8_t, 4> bytes{};
};
std::optional<NeighborhoodInfo> readNeighborhoodInfo(std::span<const std::uint8_t> data);
std::vector<std::uint8_t> writeNeighborhoodInfo(const NeighborhoodInfo& info);

} // namespace ts2::lotrecords
