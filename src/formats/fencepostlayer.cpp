#include "formats/fencepostlayer.hpp"

#include "formats/binary_reader.hpp"

#include <bit>
#include <stdexcept>
#include <string>

namespace ts2::fencepostlayer {

std::vector<Post> parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    r.skip(64);
    r.u32();  // type id
    r.u32();  // version
    const auto cls = r.bytes(r.u8());
    if (std::string(cls.begin(), cls.end()) != "cFencePostLayer") {
        throw std::runtime_error("cFencePostLayer: unexpected class name");
    }
    const std::uint32_t count = r.u32();
    if (count > r.remaining() / 16) {
        throw std::runtime_error("cFencePostLayer: too many posts");
    }
    std::vector<Post> posts(count);
    for (auto& p : posts) {
        p.level = static_cast<std::int32_t>(r.u32());
        p.x = std::bit_cast<float>(r.u32());
        p.y = std::bit_cast<float>(r.u32());
        p.fence = r.u32();
    }
    return posts;
}

} // namespace ts2::fencepostlayer
