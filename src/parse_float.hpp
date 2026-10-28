#pragma once

#include <charconv>
#include <cstddef>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace ts2 {

// macOS libc++ does not provide floating-point from_chars before macOS 26.
// Keep its fallback here so callers can otherwise use the same parser.

// The number at the start of the text, ignoring anything after it. Nothing when the text doesn't
// start with one.
inline std::optional<float> parseLeadingFloat(std::string_view text, std::size_t* length = nullptr) {
#ifdef __APPLE__
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    float value = 0;
    input >> std::noskipws >> value;
    if (input.fail()) {
        return std::nullopt;
    }
    if (length) {
        *length = input.eof() ? text.size() : static_cast<std::size_t>(input.tellg());
    }
    return value;
#else
    float value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{}) {
        return std::nullopt;
    }
    if (length) {
        *length = static_cast<std::size_t>(end - text.data());
    }
    return value;
#endif
}

// The number the whole text spells. Nothing when anything else follows it.
inline std::optional<float> parseFloat(std::string_view text) {
    std::size_t length = 0;
    const auto value = parseLeadingFloat(text, &length);
    return value && length == text.size() ? value : std::nullopt;
}

}  // namespace ts2
