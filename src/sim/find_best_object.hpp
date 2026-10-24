#pragma once

#include "sim/code_source.hpp"
#include "sim/world.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace ts2::sim {

// Find Best Object for Function (0x0E): the object on the lot that best serves one of the game's
// object functions (prepare food, sit, awareness...) for a Sim. Operands:
//   bytes 0-1  the function
//   byte 2     flags: 0x01 outside only, 0x02 inside only, 0x04 in the room only, 0x08 relative to an
//              object (bytes 3, 4-5: owner, data), 0x10 in line of sight, 0x20 not in the room only,
//              0x40 only the room in temp 0, 0x80 ignoring lockout
//   byte 6     bit 0: only rooms in an array (object in temp 0, array in temp 1)
//
// Each object with the function's entry in its OBJf is a candidate. Its rating is the object data
// field the function names (or 50 when it names none), written to the Sim's object data 0x42
// ("function score"). A candidate with no rating, or whose check tree fails, is skipped. Its score
// is the rating halved every 5 tiles away: rating / (distance / 5 + 1). The best becomes the
// stack object.
//
// Not yet: whether the Sim can route to the object's room, line of sight, lockouts and objects in
// use, and two per-Sim factors the game multiplies the score by.
struct FunctionSearch {
    World& world;
    CodeSource& code;
    SimObject& me;
    std::span<const std::uint8_t> operands;
    std::span<const std::int16_t> temps;
    // The object distances and "the room" are measured from: me, or the object of flag 0x08.
    SimObject& origin;
    // Runs a candidate's check tree as me with the candidate as stack object. True when it passes.
    std::function<bool(SimObject& candidate, std::uint16_t tree)> check;
};

// The id of the best object, nullopt when no object serves the function.
[[nodiscard]] std::optional<std::int16_t> findBestObjectForFunction(const FunctionSearch& search);

} // namespace ts2::sim
