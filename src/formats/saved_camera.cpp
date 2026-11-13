#include "formats/saved_camera.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>
#include <exception>

namespace ts2::savedcamera {

namespace {

constexpr std::size_t kNameSize = 64;

void putU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void putI32(std::vector<std::uint8_t>& out, std::int32_t value) {
    putU32(out, static_cast<std::uint32_t>(value));
}

template <std::size_t N>
void readAll(BinaryReader& r, std::array<std::uint32_t, N>& values) {
    for (std::uint32_t& v : values) {
        v = r.u32();
    }
}

template <std::size_t N>
void putAll(std::vector<std::uint8_t>& out, const std::array<std::uint32_t, N>& values) {
    for (const std::uint32_t v : values) {
        putU32(out, v);
    }
}

} // namespace

std::optional<Record> readRecord(std::span<const std::uint8_t> data) {
    try {
        BinaryReader r(data);
        const auto name = r.bytes(kNameSize);
        if (std::any_of(name.begin(), name.end(), [](std::uint8_t b) { return b != 0; })) {
            return std::nullopt;
        }
        Record record;
        record.kind = r.u8();
        if ((record.kind != kOrbitKind && record.kind != kFreeKind) || r.u8() != kSlots) {
            return std::nullopt;
        }
        for (std::size_t slot = 0; slot < kSlots; ++slot) {
            const std::uint8_t used = r.u8();
            if (used > 1) {
                return std::nullopt;
            }
            if (used == 0) {
                continue;
            }
            if (record.kind == kOrbitKind) {
                OrbitView v;
                v.zoomStep = r.i32();
                v.byte1 = r.u8();
                v.distance = r.u32();
                v.orientation = r.i32();
                v.byte2 = r.u8();
                v.yaw = r.u32();
                v.angle = r.u32();
                readAll(r, v.target);
                v.value1 = r.i32();
                v.value2 = r.i32();
                record.orbit[slot] = v;
            } else {
                FreeView v;
                readAll(r, v.position);
                readAll(r, v.rotation);
                v.value = r.u32();
                v.value1 = r.i32();
                v.value2 = r.i32();
                record.free[slot] = v;
            }
        }
        if (r.remaining() != 0) {
            return std::nullopt;
        }
        return record;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> writeRecord(const Record& record) {
    std::vector<std::uint8_t> out(kNameSize, 0);
    out.push_back(record.kind);
    out.push_back(static_cast<std::uint8_t>(kSlots));
    for (std::size_t slot = 0; slot < kSlots; ++slot) {
        if (record.kind == kOrbitKind) {
            const auto& view = record.orbit[slot];
            out.push_back(view ? 1 : 0);
            if (!view) {
                continue;
            }
            putI32(out, view->zoomStep);
            out.push_back(view->byte1);
            putU32(out, view->distance);
            putI32(out, view->orientation);
            out.push_back(view->byte2);
            putU32(out, view->yaw);
            putU32(out, view->angle);
            putAll(out, view->target);
            putI32(out, view->value1);
            putI32(out, view->value2);
        } else {
            const auto& view = record.free[slot];
            out.push_back(view ? 1 : 0);
            if (!view) {
                continue;
            }
            putAll(out, view->position);
            putAll(out, view->rotation);
            putU32(out, view->value);
            putI32(out, view->value1);
            putI32(out, view->value2);
        }
    }
    return out;
}

} // namespace ts2::savedcamera
