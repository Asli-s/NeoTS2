#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ts2::propset {

// A text property set (an XML <cGZPropertySetString>): each <AnyType key="name">value</AnyType>
// element becomes one entry, keyed by name. Values are kept as text, with surrounding whitespace
// removed and the basic XML entities decoded. The binary form (magic 0xCBE750E0) is read too, its
// values written as the text form would show them.
std::map<std::string, std::string> parse(std::span<const std::uint8_t> data);

// A property as a number (decimal, or hex with 0x), 0 when missing or not a number.
std::uint32_t number(const std::map<std::string, std::string>& properties, const std::string& key);

// Every byte of a binary property set (magic 0xCBE750E0, version 2) as the game writes it, to be
// written back: each property's type, name and value, in the game's order.
constexpr std::uint32_t kBinaryMagic = 0xCBE750E0;
constexpr std::uint32_t kUintType = 0xEB61E4F7, kStringType = 0x0B8BEA18, kFloatType = 0xABC78708,
                        kBoolType = 0xCBA908E1, kIntType = 0x0C264712, kStringListType = 0xAD9A861E;

struct Property {
    std::uint32_t type = kUintType;
    std::string name;
    std::uint32_t word = 0;  // a number, a float's bits or a bool (0 or 1)
    std::string text;        // for kStringType
    std::vector<std::string> texts;  // for kStringListType
};

// Empty for another magic or version, a type the game does not write, a bool other than 0 or 1, a
// text holding a zero byte (the game writes texts up to their first), or bytes cut short or left
// over.
std::optional<std::vector<Property>> readBinary(std::span<const std::uint8_t> data);

// Throws std::runtime_error for a type the game does not write, a text holding a zero byte, or a
// list too long to write.
std::vector<std::uint8_t> writeBinary(const std::vector<Property>& properties);

// Every byte of a text property set (the XML <cGZPropertySetString> form, which the game saves as
// Sim DNA) as the game writes it, to be written back. Empty when writing the properties back would
// not give the same bytes, for example for other spacing, escapes or number forms.
std::optional<std::vector<Property>> readXml(std::span<const std::uint8_t> data);

// The game's layout: two spaces per level, each element on its own line, no line break at the end.
// Floats are written with their whole integer part and at most 10 more digits, cut off, not
// rounded. Throws std::runtime_error for a type the game does not write, a name or text holding a
// zero byte, or a float that is not finite, is negative zero or is 1e7 or more in size (the game's
// digits for those are not known).
std::vector<std::uint8_t> writeXml(const std::vector<Property>& properties);

} // namespace ts2::propset
