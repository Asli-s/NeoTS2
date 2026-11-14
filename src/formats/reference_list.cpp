#include "formats/reference_list.hpp"

#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"

namespace ts2::reflist {

std::vector<scenegraph::Link> parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    r.u32();  // 0xDEADBEEF
    const std::uint32_t version = r.u32();
    const std::uint32_t count = r.u32();
    std::vector<scenegraph::Link> links;
    for (std::uint32_t i = 0; i < count && r.remaining() >= (version >= 2 ? 16u : 12u); ++i) {
        scenegraph::Link l;
        l.type = r.u32();
        l.group = r.u32();
        l.instance = r.u32();
        if (version >= 2) {
            l.instanceHigh = r.u32();
        }
        links.push_back(l);
    }
    return links;
}

std::vector<std::uint8_t> encode(const std::vector<scenegraph::Link>& links) {
    BinaryWriter w;
    w.u32(0xDEADBEEF);
    w.u32(2);
    w.u32(static_cast<std::uint32_t>(links.size()));
    for (const auto& l : links) {
        w.u32(l.type);
        w.u32(l.group);
        w.u32(l.instance);
        w.u32(l.instanceHigh);
    }
    return w.take();
}

} // namespace ts2::reflist
