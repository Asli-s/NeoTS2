#include "formats/fami.hpp"

#include "formats/binary_reader.hpp"

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
    // Versions below 7 store a few flags as bytes and older ones another member; they aren't supported.
    if (f.version < 7) {
        throw std::runtime_error("fami: version too old");
    }
    f.unknown0 = r.u32();
    f.lot = r.i32();
    f.lot2 = f.version >= 0x50 ? r.i32() : f.lot;
    f.value3 = f.version >= 0x55 ? r.i32() : 0;
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

} // namespace ts2::fami
