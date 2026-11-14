#include "formats/swaf.hpp"

#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace ts2::swaf {

namespace {

constexpr std::uint32_t kMaxVersion = 6;
constexpr std::uint32_t kMaxWantVersion = 10;
constexpr std::uint32_t kMaxHistoryVersion = 2;
constexpr std::uint32_t kMaxCount = 0x10000;
// More slots than any panel layout has. The game sets no limit.
constexpr std::uint32_t kMaxSlots = 64;

Want readWant(BinaryReader& r) {
    Want w;
    w.version = r.u32();
    if (w.version > kMaxWantVersion) {
        throw std::runtime_error("swaf: unknown want version");
    }
    const std::uint32_t v = w.version;
    w.id = r.u16();
    w.guid = r.u32();
    w.type = r.u8();
    switch (w.type) {
    case 0:
        if (v >= 7) w.value = r.i16();
        break;
    case 1:
        w.argument = r.i16();
        if (v >= 8) w.value = r.i16();
        break;
    case 2:
        w.argument = r.i32();
        if (v >= 3) w.value = r.i16();
        break;
    case 3:
        if (v >= 2) w.argument = r.i32();
        if (v >= 3) w.value = r.i16();
        break;
    case 4:
        if (v >= 2) {
            w.argument = r.i16();
            w.value = r.i16();
        }
        break;
    case 5:
        if (v >= 2) {
            w.argument = r.i32();
            w.value = r.i16();
        }
        break;
    case 6:
        // Before version 10 the argument is a 16-bit code the game maps to a GUID.
        if (v >= 10) {
            w.argument = r.i32();
            w.value = r.i16();
        } else if (v >= 2) {
            w.argument = r.u16();
            w.value = r.i16();
        }
        break;
    default:
        throw std::runtime_error("swaf: unknown argument type");
    }
    w.tail.push_back(r.i32());
    if (v >= 6) w.tail.push_back(r.i32());
    if (v >= 9) w.tail.push_back(r.i32());
    if (v >= 5) w.flags = r.u8();
    return w;
}

std::vector<Want> readWants(BinaryReader& r) {
    const std::uint32_t count = r.u32();
    if (count > kMaxCount) {
        throw std::runtime_error("swaf: too many wants");
    }
    std::vector<Want> wants;
    for (std::uint32_t i = 0; i < count; ++i) {
        wants.push_back(readWant(r));
    }
    return wants;
}

// A want as readWant reads it, its values written by its version and argument type.
void writeWant(BinaryWriter& w, const Want& want) {
    const std::uint32_t v = want.version;
    w.u32(v);
    w.u16(want.id);
    w.u32(want.guid);
    w.u8(want.type);
    switch (want.type) {
    case 0:
        if (v >= 7) w.i16(want.value);
        break;
    case 1:
        w.i16(static_cast<std::int16_t>(want.argument));
        if (v >= 8) w.i16(want.value);
        break;
    case 2:
        w.i32(want.argument);
        if (v >= 3) w.i16(want.value);
        break;
    case 3:
        if (v >= 2) w.i32(want.argument);
        if (v >= 3) w.i16(want.value);
        break;
    case 4:
        if (v >= 2) {
            w.i16(static_cast<std::int16_t>(want.argument));
            w.i16(want.value);
        }
        break;
    case 5:
        if (v >= 2) {
            w.i32(want.argument);
            w.i16(want.value);
        }
        break;
    case 6:
        if (v >= 10) {
            w.i32(want.argument);
            w.i16(want.value);
        } else if (v >= 2) {
            w.u16(static_cast<std::uint16_t>(want.argument));
            w.i16(want.value);
        }
        break;
    default:
        throw std::runtime_error("swaf: unknown argument type");
    }
    // As many values after the argument as the version has, 0 for any missing.
    const std::size_t values = v >= 9 ? 3 : v >= 6 ? 2 : 1;
    for (std::size_t i = 0; i < values; ++i) {
        w.i32(i < want.tail.size() ? want.tail[i] : 0);
    }
    if (v >= 5) w.u8(want.flags);
}

void writeWants(BinaryWriter& w, const std::vector<Want>& wants) {
    w.u32(static_cast<std::uint32_t>(wants.size()));
    for (const Want& want : wants) {
        writeWant(w, want);
    }
}

} // namespace

Slots slots(const WantsAndFears& saved) {
    Slots out;
    if (saved.version >= 2) {
        out.wants = std::min<std::uint32_t>(saved.maxWants, kMaxSlots);
        out.fears = std::min<std::uint32_t>(saved.maxFears, kMaxSlots);
    }
    // The game keeps the extra holds as a short and compares counts unsigned.
    const std::int32_t extra = saved.version >= 4 ? static_cast<std::int16_t>(saved.lastValue) : 0;
    out.holds = static_cast<std::uint32_t>(extra + 1);
    return out;
}

WantsAndFears parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    WantsAndFears s;
    s.version = r.u32();
    if (s.version > kMaxVersion) {
        throw std::runtime_error("swaf: unknown version");
    }
    if (s.version > 2) {
        s.extraFlag = r.u32();
        if (s.extraFlag != 0) {
            s.extra = readWant(r);
        }
    }
    if (s.version > 1) s.maxWants = r.u32();
    s.wants = readWants(r);
    if (s.version > 1) s.maxFears = r.u32();
    s.fears = readWants(r);
    if (s.version >= 4) s.lastValue = r.u32();

    s.historyVersion = r.u32();
    if (s.historyVersion > kMaxHistoryVersion) {
        throw std::runtime_error("swaf: unknown history version");
    }
    if (s.historyVersion > 1) s.historyValue = r.i32();
    const std::uint32_t groups = r.u32();
    if (groups > kMaxCount) {
        throw std::runtime_error("swaf: too many history groups");
    }
    for (std::uint32_t i = 0; i < groups; ++i) {
        const std::uint32_t guid = r.u32();
        auto wants = readWants(r);
        s.historyGroups.emplace_back(guid, wants);
        auto& group = s.history[guid];
        group.insert(group.end(), std::make_move_iterator(wants.begin()), std::make_move_iterator(wants.end()));
    }
    s.extraBytes = r.remaining();
    const auto tail = r.bytes(r.remaining());
    s.tail.assign(tail.begin(), tail.end());
    return s;
}

std::vector<std::uint8_t> encode(const WantsAndFears& s) {
    BinaryWriter w;
    w.u32(s.version);
    if (s.version > 2) {
        // A want saved needs a word that isn't 0 before it.
        w.u32(s.extra ? (s.extraFlag != 0 ? s.extraFlag : 1) : 0);
        if (s.extra) {
            writeWant(w, *s.extra);
        }
    }
    if (s.version > 1) {
        w.u32(s.maxWants);
    }
    writeWants(w, s.wants);
    if (s.version > 1) {
        w.u32(s.maxFears);
    }
    writeWants(w, s.fears);
    if (s.version >= 4) {
        w.u32(s.lastValue);
    }
    w.u32(s.historyVersion);
    if (s.historyVersion > 1) {
        w.i32(s.historyValue);
    }
    w.u32(static_cast<std::uint32_t>(s.historyGroups.size()));
    for (const auto& [guid, wants] : s.historyGroups) {
        w.u32(guid);
        writeWants(w, wants);
    }
    w.bytes(s.tail);
    return w.take();
}

} // namespace ts2::swaf
