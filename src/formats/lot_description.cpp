#include "formats/lot_description.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>

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
    // The lot's own description, with its own version. Later versions add fields at the end.
    const std::uint16_t own = r.u16();
    out.width = r.u32();
    out.depth = r.u32();
    out.type = r.u8();
    out.roadSides = r.u8();
    out.ownFacing = r.u8();
    if (own >= 5) {
        out.flags = r.u32();
    }
    if (own >= 6) {
        out.name = text(r);
        out.description = text(r);
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
    if (out.version > 0x0C) {
        r.u8();
    }
    if (out.version > 0x0D) {
        out.owner = r.i32();
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

namespace {

void putU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void putVarLength(std::vector<std::uint8_t>& out, std::size_t length) {
    do {
        const auto low = static_cast<std::uint8_t>(length & 0x7F);
        length >>= 7;
        out.push_back(static_cast<std::uint8_t>(low | (length != 0 ? 0x80 : 0)));
    } while (length != 0);
}

void putVarString(std::vector<std::uint8_t>& out, const std::string& text) {
    putVarLength(out, text.size());
    out.insert(out.end(), text.begin(), text.end());
}

// Reads a text and checks its length was written the shortest way, so it writes back the same.
bool readVarString(BinaryReader& r, std::string& text) {
    const std::size_t before = r.remaining();
    text = r.varString();
    std::vector<std::uint8_t> length;
    putVarLength(length, text.size());
    return before - r.remaining() == length.size() + text.size();
}

// The lot package writes text lengths in 7-bit groups, the neighborhood package as 32 bits.
enum class Lengths { Short, Full };

bool readText(BinaryReader& r, std::string& text, Lengths lengths) {
    if (lengths == Lengths::Short) {
        return readVarString(r, text);
    }
    const auto bytes = r.bytes(r.u32());
    text.assign(bytes.begin(), bytes.end());
    return true;
}

void putText(std::vector<std::uint8_t>& out, const std::string& text, Lengths lengths) {
    if (lengths == Lengths::Short) {
        putVarString(out, text);
        return;
    }
    if (text.size() > 0xFFFFFFFFu) {
        throw std::runtime_error("lot description: a text is too long to write");
    }
    putU32(out, static_cast<std::uint32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

void putU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void putCount(std::vector<std::uint8_t>& out, std::size_t n) {
    if (n > 0x7FFFFFFF) {
        throw std::runtime_error("lot description: a list is too long to write");
    }
    putU32(out, static_cast<std::uint32_t>(n));
}

// The lot info block from its version on, the same in both packages but for the text lengths.
bool readInfoBlock(BinaryReader& r, LotInfoRecord& out, Lengths lengths) {
    if (r.u16() != kLotInfoWrittenVersion) {
        return false;
    }
    out.width = r.u32();
    out.depth = r.u32();
    out.type = r.u8();
    out.roadSides = r.u8();
    out.ownFacing = r.u8();
    out.flags = r.u32();
    if (!readText(r, out.name, lengths) || !readText(r, out.description, lengths)) {
        return false;
    }
    out.values.resize(r.u32Count(4));
    for (std::uint32_t& v : out.values) {
        v = r.u32();
    }
    const auto rest = r.bytes(out.rest.size());
    std::copy(rest.begin(), rest.end(), out.rest.begin());
    return true;
}

void putInfoBlock(std::vector<std::uint8_t>& out, const LotInfoRecord& record, Lengths lengths) {
    putU16(out, kLotInfoWrittenVersion);
    putU32(out, record.width);
    putU32(out, record.depth);
    out.push_back(record.type);
    out.push_back(record.roadSides);
    out.push_back(record.ownFacing);
    putU32(out, record.flags);
    putText(out, record.name, lengths);
    putText(out, record.description, lengths);
    putCount(out, record.values.size());
    for (const std::uint32_t v : record.values) {
        putU32(out, v);
    }
    out.insert(out.end(), record.rest.begin(), record.rest.end());
}

} // namespace

std::optional<LotInfoRecord> readLotInfo(std::span<const std::uint8_t> data) {
    try {
        BinaryReader r(data);
        LotInfoRecord out;
        const auto header = r.bytes(out.header.size());
        std::copy(header.begin(), header.end(), out.header.begin());
        if (!readInfoBlock(r, out, Lengths::Short) || r.remaining() != 0) {
            return std::nullopt;
        }
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> writeLotInfo(const LotInfoRecord& record) {
    std::vector<std::uint8_t> out(record.header.begin(), record.header.end());
    putInfoBlock(out, record, Lengths::Short);
    return out;
}

std::optional<LotDescriptionRecord> readLotDescriptionRecord(std::span<const std::uint8_t> data) {
    try {
        BinaryReader r(data);
        LotDescriptionRecord out;
        if (r.u16() != kLotDescriptionWrittenVersion || !readInfoBlock(r, out.own, Lengths::Full)) {
            return std::nullopt;
        }
        out.x = r.u32();
        out.y = r.u32();
        out.elevation = r.u32();
        out.id = r.i32();
        out.facing = r.u8();
        if (!readText(r, out.texture, Lengths::Full) || r.u8() != 0) {
            return std::nullopt;
        }
        out.owner = r.i32();
        out.value1 = r.i32();
        out.flag = r.u8();
        out.value2 = r.i32();
        out.value3 = r.i32();
        out.entries.resize(r.i32Count(16));
        for (auto& entry : out.entries) {
            for (std::uint32_t& v : entry) {
                v = r.u32();
            }
        }
        for (std::size_t n = r.i32Count(4); n > 0; --n) {
            const std::uint16_t room = r.u16();
            out.roomLights.emplace_back(room, r.u16());
        }
        if (r.remaining() != 0) {
            return std::nullopt;
        }
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> writeLotDescriptionRecord(const LotDescriptionRecord& record) {
    std::vector<std::uint8_t> out;
    putU16(out, kLotDescriptionWrittenVersion);
    putInfoBlock(out, record.own, Lengths::Full);
    putU32(out, record.x);
    putU32(out, record.y);
    putU32(out, record.elevation);
    putU32(out, static_cast<std::uint32_t>(record.id));
    out.push_back(record.facing);
    putText(out, record.texture, Lengths::Full);
    out.push_back(0);
    putU32(out, static_cast<std::uint32_t>(record.owner));
    putU32(out, static_cast<std::uint32_t>(record.value1));
    out.push_back(record.flag);
    putU32(out, static_cast<std::uint32_t>(record.value2));
    putU32(out, static_cast<std::uint32_t>(record.value3));
    putCount(out, record.entries.size());
    for (const auto& entry : record.entries) {
        for (const std::uint32_t v : entry) {
            putU32(out, v);
        }
    }
    putCount(out, record.roomLights.size());
    for (const auto& [room, percent] : record.roomLights) {
        putU16(out, room);
        putU16(out, percent);
    }
    return out;
}

} // namespace ts2::nhood
