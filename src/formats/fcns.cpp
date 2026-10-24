#include "formats/fcns.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>
#include <stdexcept>

namespace ts2::fcns {

namespace {

constexpr std::uint32_t kSignature = 0x46434E53;  // "SNCF" in the file

} // namespace

std::optional<float> Tunings::find(std::string_view valueName) const {
    for (const auto& v : values) {
        if (v.name == valueName) {
            return v.value;
        }
    }
    return std::nullopt;
}

Tunings parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    Tunings t;
    const auto nameBytes = r.bytes(64);
    const auto nameEnd = std::find(nameBytes.begin(), nameBytes.end(), std::uint8_t{0});
    t.name.assign(nameBytes.begin(), nameEnd);
    if (r.u32() != kSignature) {
        throw std::runtime_error("FCNS: bad signature");
    }
    t.version = r.u32();
    r.u32();
    // Each value takes at least a length byte, a float and its trailing byte.
    const std::size_t count = r.u32Count(6);
    t.values.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        Tunings::Value v;
        const auto text = r.bytes(r.u8());
        v.name.assign(text.begin(), text.end());
        v.value = r.f32();
        r.u8();  // 0 in the game's files, meaning not known
        t.values.push_back(std::move(v));
    }
    return t;
}

} // namespace ts2::fcns
