#pragma once

#include <cstddef>
#include <cstdint>

namespace ts2::sim {

// Reach/Put (0x74) in progress: which instruction runs it, what moves where, and when.
struct Reaching {
    enum class Mode : std::uint8_t { Grab = 0, Put = 1, PutOnFloor = 2 };
    std::size_t frame = 0;
    std::uint32_t group = 0;
    std::uint16_t tree = 0;
    std::uint16_t pc = 0;
    Mode mode = Mode::Grab;
    std::int16_t object = 0;     // what moves: grabbed, or held and put down
    std::int16_t container = 0;  // a put's target object
    std::int16_t slot = 0;       // the hand to grab into, or the target's slot
    double transferMs = 0;       // the reach clip's event 0: the object changes hands
    double endMs = 0;            // the reach clip's end
    bool transferred = false;
};

} // namespace ts2::sim
