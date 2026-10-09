#include "formats/namemap.hpp"

#include "formats/binary_reader.hpp"

#include <stdexcept>

namespace ts2::namemap {

std::vector<Entry> parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    const std::uint32_t count = r.u32();
    // Each entry takes at least 12 bytes.
    if (count > r.remaining() / 12) {
        throw std::runtime_error("NmaP: too many entries");
    }
    std::vector<Entry> entries(count);
    for (auto& e : entries) {
        e.group = r.u32();
        e.instance = r.u32();
        const auto name = r.bytes(r.u32());
        e.name.assign(name.begin(), name.end());
    }
    return entries;
}

} // namespace ts2::namemap
