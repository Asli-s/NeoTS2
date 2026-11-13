#include "formats/fami.hpp"

#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"

#include <stdexcept>

namespace ts2::fami {

namespace {
constexpr std::uint32_t kMagic = 0x46414D49;  // "IMAF" as stored
constexpr std::uint32_t kMaxMembers = 0x8000;  // neighbor ids are 16-bit
}

Family parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    Family f;
    f.magic = r.u32();
    if (f.magic != kMagic) {
        throw std::runtime_error("fami: bad magic");
    }
    f.version = r.u32();
    // Versions below 7 store a few flags as bytes and older ones another member. They aren't supported.
    if (f.version < 7) {
        throw std::runtime_error("fami: version too old");
    }
    f.unknown0 = r.u32();
    f.lot = r.i32();
    f.currentLot = f.version >= 0x50 ? r.i32() : f.lot;
    f.vacationLot = f.version >= 0x55 ? r.i32() : 0;
    f.value4 = r.i32();
    f.funds = r.i32();
    if (f.version < 0x4D) {
        r.i32();
    }
    f.value6 = r.i32();
    f.flags = r.u32();
    const std::uint32_t count = r.u32();
    if (count > kMaxMembers) {
        throw std::runtime_error("fami: too many members");
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        f.members.push_back(r.u32());
    }
    if (f.version > 0x4A) {
        f.id7 = r.u32();
    }
    if (f.version > 0x4E) {
        f.value8 = r.u32();
    }
    f.funds2 = f.version >= 0x51 ? r.i32() : f.funds;
    f.extraBytes = r.remaining();
    return f;
}

std::vector<std::uint8_t> write(const Family& family) {
    BinaryWriter w;
    w.u32(kMagic);
    w.u32(kWrittenVersion);
    w.u32(family.unknown0);
    w.i32(family.lot);
    w.i32(family.currentLot);
    w.i32(family.vacationLot);
    w.i32(family.value4);
    w.i32(family.funds);
    w.i32(family.value6);
    w.u32(family.flags);
    w.count(family.members.size());
    for (const std::uint32_t guid : family.members) {
        w.u32(guid);
    }
    w.u32(family.id7);
    w.u32(family.value8);
    w.i32(family.funds2);
    return w.take();
}

namespace {
constexpr std::uint32_t kHistoryVersion = 0x55;
constexpr std::size_t kHistoryZeroShorts = 5;
} // namespace

std::optional<History> readHistory(std::span<const std::uint8_t> data) {
    try {
        BinaryReader r(data);
        if (r.u32() != kHistoryType || r.u32() != kHistoryVersion || r.u32() != 0 || r.u8() != 0) {
            return std::nullopt;
        }
        History h;
        // Each item takes at least 83 bytes.
        h.items.resize(r.u32Count(83));
        for (HistoryItem& item : h.items) {
            for (std::uint16_t& v : item.shorts1) {
                v = r.u16();
            }
            for (std::int32_t& v : item.values1) {
                v = r.i32();
            }
            for (std::uint16_t& v : item.shorts2) {
                v = r.u16();
            }
            for (std::size_t i = 0; i < kHistoryZeroShorts; ++i) {
                if (r.u16() != 0) {
                    return std::nullopt;
                }
            }
            for (std::int32_t& v : item.values2) {
                v = r.i32();
            }
            for (std::uint16_t& v : item.shorts3) {
                v = r.u16();
            }
            for (bool& flag : item.flags) {
                const std::uint8_t b = r.u8();
                if (b > 1) {
                    return std::nullopt;
                }
                flag = b == 1;
            }
            item.texts.resize(r.u32Count(4));
            for (std::string& text : item.texts) {
                const auto bytes = r.bytes(r.u32());
                text.assign(bytes.begin(), bytes.end());
            }
        }
        if (r.remaining() != 0) {
            return std::nullopt;
        }
        return h;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> writeHistory(const History& history) {
    BinaryWriter w;
    w.u32(kHistoryType);
    w.u32(kHistoryVersion);
    w.u32(0);
    w.u8(0);
    w.count(history.items.size());
    for (const HistoryItem& item : history.items) {
        for (const std::uint16_t v : item.shorts1) {
            w.u16(v);
        }
        for (const std::int32_t v : item.values1) {
            w.i32(v);
        }
        for (const std::uint16_t v : item.shorts2) {
            w.u16(v);
        }
        for (std::size_t i = 0; i < kHistoryZeroShorts; ++i) {
            w.u16(0);
        }
        for (const std::int32_t v : item.values2) {
            w.i32(v);
        }
        for (const std::uint16_t v : item.shorts3) {
            w.u16(v);
        }
        for (const bool flag : item.flags) {
            w.u8(flag ? 1 : 0);
        }
        w.count(item.texts.size());
        for (const std::string& text : item.texts) {
            w.fullText(text);
        }
    }
    return w.take();
}

} // namespace ts2::fami
