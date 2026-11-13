#include "formats/walllayer.hpp"

#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"

#include <stdexcept>
#include <string>

namespace ts2::walllayer {

std::vector<Entry> parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    r.skip(64);
    r.u32();  // type id
    r.u32();  // version
    const auto cls = r.bytes(r.u8());
    if (std::string(cls.begin(), cls.end()) != "cWallLayer") {
        throw std::runtime_error("cWallLayer: unexpected class name");
    }
    const std::uint32_t count = r.u32();
    if (count > r.remaining() / 12) {
        throw std::runtime_error("cWallLayer: too many entries");
    }
    std::vector<Entry> entries(count);
    for (auto& e : entries) {
        e.wall = r.u32();
        e.type = r.u32();
        e.left = r.u16();
        e.right = r.u16();
    }
    return entries;
}

std::vector<std::uint8_t> write(const std::vector<Entry>& entries) {
    BinaryWriter w;
    w.recordHeader(kType, 1, "cWallLayer");
    w.count(entries.size());
    for (const Entry& e : entries) {
        w.u32(e.wall);
        w.u32(e.type);
        w.u16(e.left);
        w.u16(e.right);
    }
    return w.take();
}

} // namespace ts2::walllayer
