#include "formats/stringmap.hpp"

#include "formats/binary_reader.hpp"

#include <stdexcept>

namespace ts2::stringmap {

std::map<std::uint16_t, Entry> parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    r.skip(64);
    r.u32();  // type id
    r.u32();  // version
    if (r.varString() != "cStringMap") {
        throw std::runtime_error("cStringMap: unexpected class name");
    }
    const std::uint32_t count = r.u32();
    // Each entry takes at least 7 bytes.
    if (count > r.remaining() / 7) {
        throw std::runtime_error("cStringMap: too many entries");
    }
    std::map<std::uint16_t, Entry> entries;
    for (std::uint32_t i = 0; i < count; ++i) {
        Entry e{r.varString(), 0};
        const std::uint16_t id = r.u16();
        e.uses = r.u32();
        entries.insert_or_assign(id, std::move(e));
    }
    return entries;
}

} // namespace ts2::stringmap
