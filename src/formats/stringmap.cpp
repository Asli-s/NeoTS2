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

namespace ts2::stringmap {

std::vector<std::uint8_t> write(std::span<const std::uint8_t> saved, const std::map<std::uint16_t, Entry>& entries) {
    BinaryReader r(saved);
    r.skip(72);    // the name, type id and version
    r.varString(); // "cStringMap"
    std::vector<std::uint8_t> out(saved.begin(), saved.begin() + static_cast<std::ptrdiff_t>(r.position()));
    auto put = [&](std::uint32_t v, int bytes) {
        for (int s = 0; s < bytes * 8; s += 8) {
            out.push_back(static_cast<std::uint8_t>(v >> s));
        }
    };
    put(static_cast<std::uint32_t>(entries.size()), 4);
    for (const auto& [id, e] : entries) {
        // The name's length in 7-bit groups, low first.
        std::size_t length = e.name.size();
        do {
            const auto low = static_cast<std::uint8_t>(length & 0x7F);
            length >>= 7;
            out.push_back(length != 0 ? static_cast<std::uint8_t>(low | 0x80) : low);
        } while (length != 0);
        out.insert(out.end(), e.name.begin(), e.name.end());
        put(id, 2);
        put(e.uses, 4);
    }
    return out;
}

} // namespace ts2::stringmap
