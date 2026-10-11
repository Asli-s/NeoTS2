#pragma once

#include <cstdint>
#include <optional>

namespace ts2::sim {

// The game's object functions as scripts number them (prepare food 0, cook food 1, flat surface 2,
// dispose 3, eat 4, pick up from slot 5, wash dish 6, eating surface 7, sit 8, stand 9, serving
// surface 10, clean 11, garden 12, wash hands 13, repair 14, dry clothes 15, wash clothes 16,
// message handler 17, pre-route 18, post-route 19, goal check 20, reaction handler 21, along-route
// callback 22, awareness 23, lookat target 25, utility state change 26, global awareness 27), each
// to its entry in an object's OBJf; nullopt for none.
constexpr std::optional<std::uint16_t> objectFunctionEntry(std::uint16_t function) {
    constexpr std::int16_t kEntries[] = {
        0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B,  // 0-9
        0x0E, 0x1C, 0x10, 0x11, 0x1D, 0x1F, 0x20, 0x24, 0x25, 0x26,  // 10-19
        0x27, 0x28, 0x29, 0x2A, -1, 0x2C, 0x2E, 0x35,                // 20-27
    };
    if (function >= sizeof(kEntries) / sizeof(kEntries[0]) || kEntries[function] < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(kEntries[function]);
}

} // namespace ts2::sim
