#include "formats/swaf.hpp"

#include "formats/binary_reader.hpp"

#include <iterator>
#include <stdexcept>
#include <utility>

namespace ts2::swaf {

namespace {

constexpr std::uint32_t kMaxVersion = 6;
constexpr std::uint32_t kMaxWantVersion = 10;
constexpr std::uint32_t kMaxHistoryVersion = 2;
constexpr std::uint8_t kMaxType = 6;
constexpr std::uint32_t kMaxCount = 0x10000;

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

} // namespace ts2::swaf
