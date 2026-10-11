#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ts2::objf {

constexpr std::uint32_t kType = 0x4F424A66;

// Object functions (OBJf): for each of the game's object functions (init, main, ... prepare food,
// sit, awareness...), the tree that checks whether the object can serve it and the tree that does
// it; 0 for none. Layout: a 64-byte name, two reserved words, "fJBO", a count, then per function the
// two tree ids (16 bits each).
struct Function {
    std::uint16_t check = 0;
    std::uint16_t action = 0;
};

std::vector<Function> parse(std::span<const std::uint8_t> data);

} // namespace ts2::objf
