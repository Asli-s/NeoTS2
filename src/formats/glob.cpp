#include "formats/glob.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>

namespace ts2::glob {

SemiGlobalReference parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    SemiGlobalReference g;

    const auto nameBytes = r.bytes(64);
    const auto nameEnd = std::find(nameBytes.begin(), nameBytes.end(), std::uint8_t{0});
    g.name.assign(nameBytes.begin(), nameEnd);

    const std::size_t afterName = r.position();
    const std::uint8_t length = r.u8();
    if (length <= r.remaining()) {
        const auto semiGlobal = r.bytes(length);
        g.semiGlobalName.assign(semiGlobal.begin(), semiGlobal.end());
        return g;
    }

    const auto rest = data.subspan(afterName);
    const auto end = std::find(rest.begin(), rest.end(), std::uint8_t{0});
    g.semiGlobalName.assign(rest.begin(), end);
    return g;
}

} // namespace ts2::glob
