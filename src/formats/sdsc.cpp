#include "formats/sdsc.hpp"

#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"

#include <array>
#include <stdexcept>
#include <utility>

namespace ts2::sdsc {

namespace {

constexpr std::uint32_t kFirstWithPersonData = 0x1E;

// Person-data version -> number of values, as the game's reader maps it: each entry applies from
// its version up to the next one.
constexpr std::array<std::pair<std::int32_t, std::int32_t>, 50> kCounts = {{
    {0, 64},     {3, 80},     {6, 81},     {7, 83},     {8, 88},     {9, 89},     {10, 91},
    {0x0B, 92},  {0x0C, 102}, {0x0D, 104}, {0x0E, 107}, {0x0F, 111}, {0x10, 112}, {0x11, 118},
    {0x12, 119}, {0x13, 121}, {0x14, 122}, {0x15, 123}, {0x16, 124}, {0x17, 144}, {0x18, 157},
    {0x19, 158}, {0x1A, 161}, {0x1B, 162}, {0x1C, 163}, {0x1D, 165}, {0x1E, 166}, {0x1F, 167},
    {0x20, 170}, {0x21, 178}, {0x22, 179}, {0x23, 180}, {0x24, 186}, {0x25, 189}, {0x26, 192},
    {0x27, 193}, {0x28, 194}, {0x29, 195}, {0x2A, 199}, {0x2B, 200}, {0x2C, 200}, {0x2D, 201},
    {0x2E, 204}, {0x30, 216}, {0x31, 219}, {0x32, 226}, {0x33, 228}, {0x34, 229}, {0x35, 230},
    {0x36, 231},
}};

} // namespace

std::size_t personDataCount(std::int32_t personDataVersion) {
    std::size_t count = 0;
    for (const auto& [from, n] : kCounts) {
        if (personDataVersion >= from) {
            count = static_cast<std::size_t>(n);
        }
    }
    return count;
}

SimDescription parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    SimDescription s;
    s.version = r.u32();
    if (s.version < kFirstWithPersonData) {
        throw std::runtime_error("sdsc: version too old");
    }
    s.personDataVersion = r.i32();
    if (s.personDataVersion > 0) {
        s.personDataFlag = r.u32();
        if (s.personDataFlag != 0) {
            const std::size_t count = personDataCount(s.personDataVersion);
            s.personData.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                s.personData.push_back(r.i16());
            }
        }
    }
    s.neighborId = r.i16();
    s.guid = r.u32();
    const auto rest = r.bytes(r.remaining());
    s.rest.assign(rest.begin(), rest.end());
    return s;
}

namespace {
// The version of the relationship list block, written by the same code as a saved object's.
constexpr std::int32_t kRelationshipListVersion = 3;
} // namespace

std::optional<Record> readRecord(std::span<const std::uint8_t> data) {
    try {
        BinaryReader r(data);
        if (r.u32() != kWrittenVersion || r.i32() != kWrittenPersonDataVersion || r.i32() != kWrittenPersonDataVersion) {
            return std::nullopt;
        }
        Record record;
        for (std::int16_t& v : record.personData) {
            v = r.i16();
        }
        record.neighborId = r.i16();
        record.guid = r.u32();
        if (r.i32() != kRelationshipListVersion) {
            return std::nullopt;
        }
        record.relationships.resize(r.u32Count(4));
        for (std::uint32_t& id : record.relationships) {
            id = r.u32();
        }
        if (r.u8() != 0) {
            return std::nullopt;
        }
        record.value1 = r.u32();
        record.value2 = r.u32();
        if (r.remaining() != 0) {
            return std::nullopt;
        }
        return record;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> writeRecord(const Record& record) {
    BinaryWriter w;
    w.u32(kWrittenVersion);
    w.i32(kWrittenPersonDataVersion);
    w.i32(kWrittenPersonDataVersion);
    for (const std::int16_t v : record.personData) {
        w.u16(static_cast<std::uint16_t>(v));
    }
    w.u16(static_cast<std::uint16_t>(record.neighborId));
    w.u32(record.guid);
    w.i32(kRelationshipListVersion);
    w.count(record.relationships.size());
    for (const std::uint32_t id : record.relationships) {
        w.u32(id);
    }
    w.u8(0);
    w.u32(record.value1);
    w.u32(record.value2);
    return w.take();
}

} // namespace ts2::sdsc
