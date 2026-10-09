#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>

namespace ts2::propset {

// A text property set (an XML <cGZPropertySetString>): each <AnyType key="name">value</AnyType>
// element becomes one entry, keyed by name. Values are kept as text, with surrounding whitespace
// removed and the basic XML entities decoded.
std::map<std::string, std::string> parse(std::span<const std::uint8_t> data);

} // namespace ts2::propset
