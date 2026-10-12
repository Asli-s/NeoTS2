#include "formats/lot_description.hpp"

#include "formats/binary_reader.hpp"

namespace ts2::nhood {

namespace {

std::string text(BinaryReader& r) {
    const auto bytes = r.bytes(r.u32());
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace

namespace {

constexpr std::size_t kMaxRoomPercents = 4096;

// The room light block that ends the description: the smallest count whose pairs fill the rest
// of the data exactly, with rooms rising as the game saves them. Empty when none fits.
std::map<std::int16_t, std::int16_t> roomLightPercents(std::span<const std::uint8_t> d) {
    auto u16 = [&](std::size_t at) { return static_cast<std::uint16_t>(d[at] | d[at + 1] << 8); };
    for (std::size_t n = 0; n <= kMaxRoomPercents && 4 + 4 * n <= d.size(); ++n) {
        const std::size_t at = d.size() - 4 - 4 * n;
        const std::uint32_t count = static_cast<std::uint32_t>(u16(at)) | static_cast<std::uint32_t>(u16(at + 2)) << 16;
        if (count != n) {
            continue;
        }
        std::map<std::int16_t, std::int16_t> out;
        bool rising = true;
        int last = -1;
        for (std::size_t i = 0; i < n; ++i) {
            const int room = u16(at + 4 + 4 * i);
            rising = rising && room > last;
            last = room;
            out[static_cast<std::int16_t>(room)] = static_cast<std::int16_t>(u16(at + 6 + 4 * i));
        }
        if (rising) {
            return out;
        }
    }
    return {};
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
    if (out.version >= 0x12) {
        out.roomLightPercents = roomLightPercents(data);
    }
    return out;
}

std::int16_t LotDescription::roomLightPercent(std::int16_t room) const {
    if (room == 0) {
        return 100;
    }
    const auto it = roomLightPercents.find(room);
    return it == roomLightPercents.end() ? std::int16_t{100} : it->second;
}

} // namespace ts2::nhood
