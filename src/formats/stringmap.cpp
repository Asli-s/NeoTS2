#include "formats/stringmap.hpp"

#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"

#include <algorithm>
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

namespace {

// The game keeps the map by name, comparing names as the C library compares them ignoring case:
// ASCII letters lowered, then byte by byte.
int compareIgnoringCase(const std::string& a, const std::string& b) {
    const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c - 'A' + 'a') : c; };
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        const unsigned char x = lower(static_cast<unsigned char>(a[i]));
        const unsigned char y = lower(static_cast<unsigned char>(b[i]));
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
}

} // namespace

std::vector<std::uint8_t> write(const std::map<std::uint16_t, Entry>& entries) {
    std::vector<std::pair<std::uint16_t, const Entry*>> byName;
    byName.reserve(entries.size());
    for (const auto& [id, e] : entries) {
        byName.emplace_back(id, &e);
    }
    std::sort(byName.begin(), byName.end(),
              [](const auto& a, const auto& b) { return compareIgnoringCase(a.second->name, b.second->name) < 0; });
    for (std::size_t i = 1; i < byName.size(); ++i) {
        if (compareIgnoringCase(byName[i - 1].second->name, byName[i].second->name) == 0) {
            throw std::runtime_error("cStringMap: two entries with one name");
        }
    }
    BinaryWriter w;
    w.recordHeader(kType, 1, "cStringMap");
    w.count(byName.size());
    for (const auto& [id, e] : byName) {
        w.text(e->name);
        w.u16(id);
        w.u32(e->uses);
    }
    return w.take();
}

} // namespace ts2::stringmap
