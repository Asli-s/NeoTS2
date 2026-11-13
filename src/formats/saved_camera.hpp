#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ts2::savedcamera {

// A camera's saved views (resource 0xEC44BDDC), one resource per camera, the instance taken from
// the camera's name. A lot keeps three and a neighborhood one. The game writes 64 zero bytes, a
// kind (4 for the orbiting cameras, 1 for the free one), the slot count 7, then each slot: a byte
// saying whether it holds a view, and the view if it does. Floats are kept as their bits so they
// write back exactly.
constexpr std::uint32_t kType = 0xEC44BDDC;
constexpr std::size_t kSlots = 7;
constexpr std::uint8_t kOrbitKind = 4;
constexpr std::uint8_t kFreeKind = 1;

// A view of an orbiting camera, 42 bytes.
struct OrbitView {
    std::int32_t zoomStep = 0;
    std::uint8_t byte1 = 0;  // not known yet
    std::uint32_t distance = 0;
    std::int32_t orientation = 0;  // the quarter turn
    std::uint8_t byte2 = 0;  // not known yet
    std::uint32_t yaw = 0;
    std::uint32_t angle = 0;  // not known yet
    std::array<std::uint32_t, 3> target{};
    std::int32_t value1 = 0;  // not known yet
    std::int32_t value2 = 0;  // not known yet
};

// A view of the free camera, 40 bytes.
struct FreeView {
    std::array<std::uint32_t, 3> position{};
    std::array<std::uint32_t, 4> rotation{};  // a quaternion
    std::uint32_t value = 0;  // not known yet
    std::int32_t value1 = 0;  // not known yet
    std::int32_t value2 = 0;  // not known yet
};

struct Record {
    std::uint8_t kind = kOrbitKind;
    std::array<std::optional<OrbitView>, kSlots> orbit;  // for kOrbitKind
    std::array<std::optional<FreeView>, kSlots> free;    // for kFreeKind
};

// Reads a record as the game writes it. Empty for another kind or slot count, a name that is not
// all zeros, a used byte other than 0 or 1, or bytes cut short or left over.
std::optional<Record> readRecord(std::span<const std::uint8_t> data);

std::vector<std::uint8_t> writeRecord(const Record& record);

} // namespace ts2::savedcamera
