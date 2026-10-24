#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

namespace ts2::content {

// How the game's live panels write numbers (US English).

// Money: the simoleon sign then the amount with a comma every three digits ("§1,234"). A minus
// sign comes first for less than nothing. Whether the game ever shows one is not known.
std::string moneyText(std::int64_t amount);

// A whole number with a comma every three digits ("1,234", "-5,000").
std::string groupedNumber(std::int64_t value);

// An hour of the day, wrapping past 23: "12 am", "9 am", "12 pm", "1 pm".
std::string hourText(int hour);

// A game string with each "%s" (and "%d") replaced in turn by the given texts. A mark with no text
// left is dropped.
std::string formatted(std::string_view pattern, std::initializer_list<std::string_view> values);

} // namespace ts2::content
