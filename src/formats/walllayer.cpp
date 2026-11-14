#include "formats/walllayer.hpp"

#include "formats/binary_reader.hpp"

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

} // namespace ts2::walllayer

namespace ts2::walllayer {

std::vector<std::uint8_t> write(std::span<const std::uint8_t> saved, const std::vector<Entry>& entries) {
    BinaryReader r(saved);
    r.skip(72);       // the name, type id and version
    r.skip(r.u8());   // "cWallLayer"
    std::vector<std::uint8_t> out(saved.begin(), saved.begin() + static_cast<std::ptrdiff_t>(r.position()));
    auto put = [&](std::uint32_t v, int bytes) {
        for (int s = 0; s < bytes * 8; s += 8) {
            out.push_back(static_cast<std::uint8_t>(v >> s));
        }
    };
    put(static_cast<std::uint32_t>(entries.size()), 4);
    for (const auto& e : entries) {
        put(e.wall, 4);
        put(e.type, 4);
        put(e.left, 2);
        put(e.right, 2);
    }
    return out;
}

} // namespace ts2::walllayer
