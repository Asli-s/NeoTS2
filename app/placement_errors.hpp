#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace app {

// A line of a UI text table (group 0x7FA7DC7C), counting from 1.
using UiLine = std::function<std::string(std::uint32_t table, std::uint32_t line)>;

// The line of the placement error strings (UI text 0x89) a placement error code names, 0 for none.
std::uint32_t placementErrorLine(int code);

// The game's message for a placement error code. A code naming the list separator line is about
// where the object may stand, and the object writes that itself from its object data 4 (the
// surfaces) and 0x2A (the kinds of floor). Empty when there is no message.
std::string placementErrorText(const UiLine& text, int code, std::uint16_t heights, std::uint16_t placement);

} // namespace app
