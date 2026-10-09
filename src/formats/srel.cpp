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

} // namespace ts2::srel
