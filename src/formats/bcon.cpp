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

    const std::uint8_t count = r.u8();
    c.flags = r.u8();
    c.values.reserve(count);
    for (std::uint8_t i = 0; i < count; ++i) {
        c.values.push_back(static_cast<std::int16_t>(r.u16()));
    }
    return c;
}

} // namespace ts2::bcon
