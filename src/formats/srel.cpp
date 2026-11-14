#include "formats/srel.hpp"

#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"

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
    const auto tail = r.bytes(r.remaining());
    rel.tail.assign(tail.begin(), tail.end());
    return rel;
}

std::vector<std::uint8_t> encode(const Relationship& rel) {
    BinaryWriter w;
    w.i32(rel.version);
    w.u32(static_cast<std::uint32_t>(rel.values.size()));
    for (const std::int32_t v : rel.values) {
        w.i32(v);
    }
    w.bytes(rel.tail);
    return w.take();
}

} // namespace ts2::srel
