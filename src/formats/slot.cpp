#include "formats/slot.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>
#include <stdexcept>

namespace ts2::slot {

namespace {
constexpr std::uint32_t kMagic = 0x534C4F54;  // "TOLS" as stored
constexpr std::uint32_t kMaxVersion = 0x47;
constexpr std::uint32_t kMaxCount = 4096;
// Entries of versions 0x43 to 0x47 are 78 bytes, the bone index 8 bytes from the end.
constexpr std::size_t kEntrySize = 78;
}

std::size_t Slots::containerCount() const {
    return static_cast<std::size_t>(std::count(types.begin(), types.end(), kContainerSlot));
}

std::int32_t Slots::containerBone(std::size_t slot) const {
    for (std::size_t i = 0; i < types.size(); ++i) {
        if (types[i] == kContainerSlot && slot-- == 0) {
            return i < bones.size() ? bones[i] : -1;
        }
    }
    return -1;
}

Slots parse(std::span<const std::uint8_t> data) {
    for (const std::size_t nameSize : {std::size_t{64}, std::size_t{72}}) {
        if (data.size() < nameSize + 16) {
            continue;
        }
        BinaryReader r(data);
        r.skip(nameSize);
        if (r.u32() != kMagic) {
            continue;
        }
        Slots s;
        s.version = r.u32();
        if (s.version == 0 || s.version > kMaxVersion) {
            throw std::runtime_error("slot: unsupported version");
        }
        r.u32();
        const std::uint32_t count = r.u32();
        if (count > kMaxCount) {
            throw std::runtime_error("slot: too many entries");
        }
        if (count == 0) {
            return s;
        }
        if (r.remaining() % count != 0 || r.remaining() / count < 2) {
            throw std::runtime_error("slot: entries of unequal size");
        }
        const std::size_t entrySize = r.remaining() / count;
        for (std::uint32_t i = 0; i < count; ++i) {
            s.types.push_back(r.u16());
            if (entrySize == kEntrySize) {
                r.skip(entrySize - 10);
                s.bones.push_back(static_cast<std::int32_t>(r.u32()));
                r.u32();
            } else {
                r.skip(entrySize - 2);
                s.bones.push_back(-1);
            }
        }
        return s;
    }
    throw std::runtime_error("slot: bad magic");
}

} // namespace ts2::slot
