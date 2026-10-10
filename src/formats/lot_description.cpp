#include "formats/lot_description.hpp"

#include "formats/binary_reader.hpp"

namespace ts2::nhood {

namespace {

std::string text(BinaryReader& r) {
    const auto bytes = r.bytes(r.u32());
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace

LotDescription parseLotDescription(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    LotDescription out;
    out.version = r.u16();
    // The lot's own description, with its own version; later versions add fields at the end.
    const std::uint16_t own = r.u16();
    out.width = r.u32();
    out.depth = r.u32();
    out.type = r.u8();
    out.roadSides = r.u8();
    out.ownFacing = r.u8();
    if (own >= 5) {
        r.u32();
    }
    if (own >= 6) {
        out.name = text(r);
        text(r);  // description
    }
    if (own >= 4) {
        r.skip(std::size_t{r.u32()} * 4);
    }
    if (own >= 7) {
        r.f32();
    }
    if (own >= 8) {
        r.u32();
    }
    if (own >= 9) {
        r.u8();
        r.u32();
        r.u32();
    }
    if (own >= 10) {
        r.u32();
    }
    if (own >= 11) {
        r.u8();
    }
    // Where the neighborhood has it.
    if (out.version < 10) {
        text(r);
    }
    out.x = r.u32();
    out.y = r.u32();
    out.elevation = r.f32();
    if (out.version < 12) {
        r.u32();
    }
    out.id = r.u32();
    out.facing = out.version >= 8 ? r.u8() : out.ownFacing;
    if (out.version >= 11) {
        out.texture = text(r);
    }
    return out;
}

} // namespace ts2::nhood
