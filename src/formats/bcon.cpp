#include "formats/bcon.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>

namespace ts2::bcon {

Constants parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    Constants c;

    const auto nameBytes = r.bytes(64);
    const auto nameEnd = std::find(nameBytes.begin(), nameBytes.end(), std::uint8_t{0});
    c.name.assign(nameBytes.begin(), nameEnd);

    // A 15-bit count. The top bit is a flag (what it means is not known).
    const std::uint16_t header = r.u16();
    const std::uint16_t count = header & 0x7FFF;
    c.flags = (header & 0x8000) ? 0x80 : 0;
    c.values.reserve(std::min<std::size_t>(count, r.remaining() / 2));
    for (std::uint16_t i = 0; i < count; ++i) {
        c.values.push_back(static_cast<std::int16_t>(r.u16()));
    }
    return c;
}

} // namespace ts2::bcon
