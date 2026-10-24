#include "formats/slot.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ts2::slot {

namespace {
constexpr std::uint32_t kMagic = 0x534C4F54;  // "TOLS" as stored
constexpr std::uint32_t kMaxVersion = 0x47;
constexpr std::uint32_t kMaxCount = 4096;
// Entries of versions 0x41 to 0x47 are 78 bytes.
constexpr std::uint32_t kFullEntryVersion = 0x41;
constexpr std::size_t kEntrySize = 78;
// Before version 0x47 some values are stored in sixteenths of a tile.
constexpr float kSixteenth = 0.0625f;

float finite(float v) {
    if (!std::isfinite(v)) {
        throw std::runtime_error("slot: bad number");
    }
    return v;
}

Entry readEntry(BinaryReader& r, std::uint32_t version) {
    Entry e;
    e.type = r.u16();
    const float scale = version < 0x47 ? kSixteenth : 1.0f;
    e.x = finite(r.f32()) * scale;
    e.y = finite(r.f32()) * scale;
    e.z = finite(r.f32());
    for (auto& v : e.routingInts) {
        v = r.i32();
    }
    e.routingFlags = r.u32();
    e.routingInt = r.i32();
    float* distances[] = {&e.minDistance, &e.maxDistance, &e.optimalDistance};
    for (float* d : distances) {
        *d = version < 0x43 ? static_cast<float>(r.i32()) * kSixteenth : finite(r.f32());
    }
    e.containerValue = r.i32();
    e.objectFlags = r.u32();
    e.routingFloat = finite(r.f32());
    e.containerKind = r.i32();
    e.facing = r.i32();
    e.step = version < 0x47 ? static_cast<float>(r.i32()) * kSixteenth : finite(r.f32());
    e.bone = r.i32();
    e.id = r.i32();
    return e;
}
}

std::size_t Slots::containerCount() const {
    return static_cast<std::size_t>(
        std::count_if(entries.begin(), entries.end(), [](const Entry& e) { return e.type == kContainerSlot; }));
}

std::int32_t Slots::containerBone(std::size_t slot) const {
    for (const Entry& e : entries) {
        if (e.type == kContainerSlot && slot-- == 0) {
            return e.bone;
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
        const bool full = s.version >= kFullEntryVersion && entrySize == kEntrySize;
        s.entries.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (full) {
                s.entries.push_back(readEntry(r, s.version));
            } else {
                Entry e;
                e.type = r.u16();
                r.skip(entrySize - 2);
                s.entries.push_back(e);
            }
        }
        return s;
    }
    throw std::runtime_error("slot: bad magic");
}

} // namespace ts2::slot
