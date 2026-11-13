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
    if (s.version > 2 && r.u32() != 0) {
        s.extra = readWant(r);
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
        auto& group = s.history[guid];
        group.insert(group.end(), std::make_move_iterator(wants.begin()), std::make_move_iterator(wants.end()));
    }
    s.extraBytes = r.remaining();
    return s;
}

namespace {

// A want as the game writes it now: the argument and value by the argument's kind.
void writeWant(BinaryWriter& w, const Want& want) {
    w.u32(kWrittenWantVersion);
    w.u16(want.id);
    w.u32(want.guid);
    w.u8(want.type);
    switch (want.type) {
    case 0:
        break;
    case 1:
    case 4:
        w.u16(static_cast<std::uint16_t>(want.argument));
        break;
    case 2:
    case 3:
    case 5:
    case 6:
        w.i32(want.argument);
        break;
    default:
        throw std::runtime_error("swaf: a want type the game does not write");
    }
    w.u16(static_cast<std::uint16_t>(want.value));
    if (want.tail.size() != 3) {
        throw std::runtime_error("swaf: a want without its three numbers");
    }
    for (const std::int32_t v : want.tail) {
        w.i32(v);
    }
    w.u8(want.flags);
}

void writeWantList(BinaryWriter& w, const std::vector<Want>& wants) {
    w.count(wants.size());
    for (const Want& want : wants) {
        writeWant(w, want);
    }
}

// Reads a want written now, empty for another version or an argument outside the bits it is
// written with.
std::optional<Want> readCurrentWant(BinaryReader& r) {
    Want w = readWant(r);
    const bool shortArgument = w.type == 1 || w.type == 4;
    if (w.version != kWrittenWantVersion || (shortArgument && w.argument != static_cast<std::int16_t>(w.argument))) {
        return std::nullopt;
    }
    return w;
}

bool readCurrentWants(BinaryReader& r, std::vector<Want>& out) {
    // A want takes at least 26 bytes.
    for (std::size_t n = r.u32Count(26); n > 0; --n) {
        auto w = readCurrentWant(r);
        if (!w) {
            return false;
        }
        out.push_back(std::move(*w));
    }
    return true;
}

} // namespace

std::optional<Record> readRecord(std::span<const std::uint8_t> data, std::size_t* leftOver) {
    try {
        BinaryReader r(data);
        if (r.u32() != kWrittenVersion) {
            return std::nullopt;
        }
        Record record;
        const std::uint32_t hasExtra = r.u32();
        if (hasExtra > 1) {
            return std::nullopt;
        }
        if (hasExtra == 1) {
            record.extra = readCurrentWant(r);
            if (!record.extra) {
                return std::nullopt;
            }
        }
        record.maxWants = r.u32();
        if (!readCurrentWants(r, record.wants)) {
            return std::nullopt;
        }
        record.maxFears = r.u32();
        if (!readCurrentWants(r, record.fears)) {
            return std::nullopt;
        }
        const std::int32_t last = r.i32();
        if (last != static_cast<std::int16_t>(last) || r.u32() != kWrittenHistoryVersion) {
            return std::nullopt;
        }
        record.lastValue = static_cast<std::int16_t>(last);
        record.historyValue = r.i32();
        for (std::size_t n = r.u32Count(8); n > 0; --n) {
            const std::uint32_t guid = r.u32();
            std::vector<Want> wants;
            if (!readCurrentWants(r, wants)) {
                return std::nullopt;
            }
            record.history.emplace_back(guid, std::move(wants));
        }
        if (leftOver) {
            *leftOver = r.remaining();
        } else if (r.remaining() != 0) {
            return std::nullopt;
        }
        return record;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> writeRecord(const Record& record) {
    BinaryWriter w;
    w.u32(kWrittenVersion);
    w.u32(record.extra ? 1 : 0);
    if (record.extra) {
        writeWant(w, *record.extra);
    }
    w.u32(record.maxWants);
    writeWantList(w, record.wants);
    w.u32(record.maxFears);
    writeWantList(w, record.fears);
    w.i32(record.lastValue);
    w.u32(kWrittenHistoryVersion);
    w.i32(record.historyValue);
    w.count(record.history.size());
    for (const auto& [guid, wants] : record.history) {
        w.u32(guid);
        writeWantList(w, wants);
    }
    return w.take();
}

} // namespace ts2::swaf
