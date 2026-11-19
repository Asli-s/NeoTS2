#include "formats/simi.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <stdexcept>

namespace ts2::simi {

SimulatorState parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    r.bytes(64);
    if (r.u32() != 0x53494D49) {  // "IMIS"
        throw std::runtime_error("SIMI: no magic");
    }
    SimulatorState s;
    s.version = r.u32();
    r.u32();
    s.globals.resize(kGlobalCount);
    const std::size_t saved = savedGlobalCount(s.version);
    for (std::size_t i = 0; i < saved; ++i) {
        s.globals[i] = r.i16();
    }
    // The ticks' random state is taken only from version 0xD4 on. Whether the game takes it from
    // older versions is not known yet.
    constexpr std::uint32_t kWithRandomState = 0xD4;
    constexpr std::size_t kBeforeRandomState = 4 + 2 + 4;
    if (s.version >= kWithRandomState && r.remaining() >= kBeforeRandomState + 4) {
        r.skip(kBeforeRandomState);
        s.randomState = r.u32();
    }
    return s;
}

std::size_t savedGlobalCount(std::uint32_t version) {
    // Each entry: the first version that saves more, and how many the versions before it save.
    constexpr std::pair<std::uint32_t, std::size_t> kCounts[] = {
        {0x40, 32},  {0x70, 33},  {0x75, 34},  {0x79, 35},  {0x7C, 36},  {0x7D, 37},  {0x8C, 40},
        {0x8D, 43},  {0x8E, 44},  {0x92, 45},  {0xA1, 46},  {0xA3, 50},  {0xA7, 51},  {0xAA, 52},
        {0xAB, 53},  {0xAF, 54},  {0xC5, 55},  {0xCA, 56},  {0xCB, 57},  {0xD4, 58}};
    if (version < 0x12) {
        return 0;
    }
    for (const auto& [below, count] : kCounts) {
        if (version < below) {
            return count;
        }
    }
    return kGlobalCount;
}

std::optional<LotValue> readLotValue(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    // Before the parts: the name, magic, version and a reserved word, the globals, ten bytes, and from
    // version 0x0E the two random states, then one more value.
    constexpr std::size_t kHeader = 64 + 4 + 4 + 4, kBetween = 10, kRandomStates = 8;
    constexpr std::uint32_t kWithParts = 0x25, kWithThird = 0xAD;
    constexpr std::size_t kGlobal15 = 0x15;
    if (data.size() < kHeader) {
        return std::nullopt;
    }
    r.skip(64);
    const std::uint32_t magic = r.u32(), version = r.u32();
    const std::size_t globals = savedGlobalCount(version);
    const std::size_t parts = version >= kWithThird ? 4 : 3;
    const std::size_t needed = kHeader + 2 * globals + kBetween + kRandomStates + 4 + 4 * parts;
    if (magic != 0x53494D49 || version < kWithParts || data.size() < needed) {
        return std::nullopt;
    }
    r.u32();
    LotValue out;
    for (std::size_t i = 0; i < globals; ++i) {
        const std::int16_t value = r.i16();
        if (i == kGlobal15) {
            out.global15 = value;
        }
    }
    r.skip(kBetween + kRandomStates + 4);
    out.first = r.i32();
    out.second = r.i32();
    if (parts == 4) {
        out.third = r.i32();
    }
    out.fourth = r.i32();
    return out;
}

std::int32_t lotValue(const LotValue& parts, bool leaveOutSecond) {
    constexpr float kFourthShare = 0.8f;
    const auto fourth = static_cast<std::int64_t>(static_cast<float>(parts.fourth) * kFourthShare);
    const std::int64_t sum = std::int64_t{parts.first} + (leaveOutSecond ? 0 : parts.second) + parts.third + fourth;
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(sum, INT32_MIN, INT32_MAX));
}

namespace {

constexpr std::uint32_t kMagic = 0x53494D49;  // "IMIS" in the file

void putU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

} // namespace

std::optional<Record> readRecord(std::span<const std::uint8_t> data) {
    if (data.size() != kRecordSize && data.size() != kShippedRecordSize) {
        return std::nullopt;
    }
    BinaryReader r(data);
    Record record;
    const auto name = r.bytes(record.name.size());
    std::copy(name.begin(), name.end(), record.name.begin());
    if (r.u32() != kMagic) {
        return std::nullopt;
    }
    const std::uint32_t version = r.u32();
    const bool shipped = version == kShippedVersion && data.size() == kShippedRecordSize;
    if ((!shipped && (version != kWrittenVersion || data.size() != kRecordSize)) || r.u32() != 0) {
        return std::nullopt;
    }
    const std::size_t saved = shipped ? kShippedGlobalCount : kGlobalCount;
    for (std::size_t i = 0; i < saved; ++i) {
        record.globals[i] = r.i16();
    }
    const auto before = r.bytes(record.beforeRandomStates.size());
    std::copy(before.begin(), before.end(), record.beforeRandomStates.begin());
    record.tickRandomState = r.u32();
    record.otherRandomState = r.u32();
    if (!shipped) {
        const auto rest = r.bytes(record.rest.size());
        std::copy(rest.begin(), rest.end(), record.rest.begin());
        return record;
    }
    // The shipped version's blocks are version 1: the version and eight values. The game keeps
    // them, writes the block as version 2 and its two newer values as 0.
    constexpr std::size_t kValues = 20, kBlocks = 6, kOldBlock = 36, kNewBlock = 44;
    const auto values = r.bytes(kValues);
    std::copy(values.begin(), values.end(), record.rest.begin());
    for (std::size_t b = 0; b < kBlocks; ++b) {
        const auto block = r.bytes(kOldBlock);
        if (block[0] != 1 || block[1] != 0 || block[2] != 0 || block[3] != 0) {
            return std::nullopt;
        }
        auto* out = record.rest.data() + kValues + b * kNewBlock;
        std::copy(block.begin(), block.end(), out);
        out[0] = 2;
        out[1] = 0;
    }
    return record;
}

std::vector<std::uint8_t> writeRecord(const Record& record) {
    std::vector<std::uint8_t> out(record.name.begin(), record.name.end());
    out.reserve(kRecordSize);
    putU32(out, kMagic);
    putU32(out, kWrittenVersion);
    putU32(out, 0);
    for (const std::int16_t g : record.globals) {
        const auto bits = static_cast<std::uint16_t>(g);
        out.push_back(static_cast<std::uint8_t>(bits));
        out.push_back(static_cast<std::uint8_t>(bits >> 8));
    }
    out.insert(out.end(), record.beforeRandomStates.begin(), record.beforeRandomStates.end());
    putU32(out, record.tickRandomState);
    putU32(out, record.otherRandomState);
    out.insert(out.end(), record.rest.begin(), record.rest.end());
    return out;
}

} // namespace ts2::simi
