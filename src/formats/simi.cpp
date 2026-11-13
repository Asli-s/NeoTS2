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
    for (auto& g : s.globals) {
        g = r.i16();
    }
    // Versions before 0xD4 save fewer globals, so the fields after them sit elsewhere.
    constexpr std::uint32_t kSixtyGlobals = 0xD4;
    constexpr std::size_t kBeforeRandomState = 4 + 2 + 4;
    if (s.version >= kSixtyGlobals && r.remaining() >= kBeforeRandomState + 4) {
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
    if (data.size() != kRecordSize) {
        return std::nullopt;
    }
    BinaryReader r(data);
    Record record;
    const auto name = r.bytes(record.name.size());
    std::copy(name.begin(), name.end(), record.name.begin());
    if (r.u32() != kMagic || r.u32() != kWrittenVersion || r.u32() != 0) {
        return std::nullopt;
    }
    for (std::int16_t& g : record.globals) {
        g = r.i16();
    }
    const auto before = r.bytes(record.beforeRandomStates.size());
    std::copy(before.begin(), before.end(), record.beforeRandomStates.begin());
    record.tickRandomState = r.u32();
    record.otherRandomState = r.u32();
    const auto rest = r.bytes(record.rest.size());
    std::copy(rest.begin(), rest.end(), record.rest.begin());
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
