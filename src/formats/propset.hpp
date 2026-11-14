#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace ts2::propset {

// A text property set (an XML <cGZPropertySetString>): each <AnyType key="name">value</AnyType>
// element becomes one entry, keyed by name. Values are kept as text, with surrounding whitespace
// removed and the basic XML entities decoded. The binary form (magic 0xCBE750E0) is read too, its
// values written as the text form would show them.
std::map<std::string, std::string> parse(std::span<const std::uint8_t> data);

// A text property set as saved, for changing it and writing it back: each property in order, with
// its element (AnyString, AnyUint32...), key, type and value (entities decoded, whitespace kept).
// An empty element (<AnyString key="k" type="t"/>) has no value.
struct TextProperty {
    std::string element, key, type, value;
    bool empty = false;
};
std::vector<TextProperty> readText(std::span<const std::uint8_t> data);
// The set as the game writes it (as the neighborhood's Sim DNA is): the XML declaration, each
// property on its own line indented two spaces, LF line ends and none after the last tag.
std::vector<std::uint8_t> encodeText(const std::vector<TextProperty>& properties);

// A property as a number (decimal, or hex with 0x), 0 when missing or not a number.
std::uint32_t number(const std::map<std::string, std::string>& properties, const std::string& key);

} // namespace ts2::propset
