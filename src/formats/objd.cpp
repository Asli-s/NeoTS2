#include "formats/objd.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>

namespace ts2::objd {

ObjectDefinition parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    ObjectDefinition d;

    const auto nameBytes = r.bytes(64);
    const auto nameEnd = std::find(nameBytes.begin(), nameBytes.end(), std::uint8_t{0});
    d.name.assign(nameBytes.begin(), nameEnd);

    const std::size_t count = std::min(kMaxFields, r.remaining() / 2);
    d.fields.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        d.fields.push_back(r.u16());
    }
    return d;
}

} // namespace ts2::objd
