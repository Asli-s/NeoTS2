#include "formats/world_db.hpp"

#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"

#include <algorithm>
#include <stdexcept>

namespace ts2::worlddb {

namespace {

constexpr std::size_t kNameSize = 64;
constexpr const char* kClassName = "cWorldDB";

std::size_t lengthBytes(std::size_t length) {
    std::size_t n = 1;
    while (length >= 0x80) {
        length >>= 7;
        ++n;
    }
    return n;
}

} // namespace

std::optional<Record> readRecord(std::span<const std::uint8_t> data) {
    try {
        BinaryReader r(data);
        const auto name = r.bytes(kNameSize);
        if (std::any_of(name.begin(), name.end(), [](std::uint8_t b) { return b != 0; })) {
            return std::nullopt;
        }
        if (r.u32() != kType || r.u32() != kWrittenVersion || r.varString() != kClassName) {
            return std::nullopt;
        }
        Record record;
        for (std::int32_t& v : record.values) {
            v = r.i32();
        }
        const std::int32_t flag = r.i32();
        if (flag != 0 && flag != 1) {
            return std::nullopt;
        }
        if (flag == 1) {
            const std::size_t start = r.position();
            std::string text = r.varString();
            if (r.position() - start != lengthBytes(text.size()) + text.size()) {
                return std::nullopt;
            }
            record.text = std::move(text);
        }
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
    w.recordHeader(kType, kWrittenVersion, kClassName);
    for (const std::int32_t v : record.values) {
        w.i32(v);
    }
    w.i32(record.text ? 1 : 0);
    if (record.text) {
        w.text(*record.text);
    }
    return w.take();
}

} // namespace ts2::worlddb
