#include "formats/srel.hpp"

#include "formats/binary_reader.hpp"

#include <stdexcept>
#include <string>

namespace ts2::srel {

namespace {
// The Relationship primitive addresses variables with one byte.
constexpr std::uint32_t kMaxValues = 256;
}

Relationship parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    Relationship rel;
    rel.version = r.i32();
    if (rel.version != 2 && rel.version != 3) {
        throw std::runtime_error("srel: unsupported version " + std::to_string(rel.version));
    }
    const std::uint32_t count = r.u32();
    if (count > kMaxValues) {
        throw std::runtime_error("srel: too many values");
    }
    rel.values.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        rel.values.push_back(r.i32());
    }
    rel.extraBytes = r.remaining();
    return rel;
}

std::optional<Record> readRecord(std::span<const std::uint8_t> data) {
    try {
        BinaryReader r(data);
        if (r.i32() != kWrittenVersion) {
            return std::nullopt;
        }
        const std::uint32_t count = r.u32();
        if (count > kMaxValues || r.remaining() != std::size_t{count} * 4) {
            return std::nullopt;
        }
        Record record;
        record.values.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            record.values.push_back(r.i32());
        }
        return record;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> writeRecord(const Record& record) {
    if (record.values.size() > kMaxValues) {
        throw std::runtime_error("srel: too many values to write");
    }
    std::vector<std::uint8_t> out;
    out.reserve(8 + 4 * record.values.size());
    const auto put = [&out](std::uint32_t v) {
        for (int shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<std::uint8_t>(v >> shift));
        }
    };
    put(kWrittenVersion);
    put(static_cast<std::uint32_t>(record.values.size()));
    for (const std::int32_t v : record.values) {
        put(static_cast<std::uint32_t>(v));
    }
    return out;
}

} // namespace ts2::srel
