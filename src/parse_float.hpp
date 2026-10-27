#pragma once

#include <charconv>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace ts2 {

// macOS libc++ does not provide floating-point from_chars before macOS 26.
// Keep its fallback here so callers can otherwise use the same parser.
inline std::optional<float> parseFloat(std::string_view text) {
#ifdef __APPLE__
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    float value = 0;
    input >> std::noskipws >> value;
    return input && input.eof() ? std::optional<float>(value) : std::nullopt;
#else
    float value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() ? std::optional<float>(value) : std::nullopt;
#endif
}

}  // namespace ts2
