#include "formats/simi.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>
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
    const std::size_t saved = s.version == kShippedVersion ? kShippedGlobalCount : kGlobalCount;
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
