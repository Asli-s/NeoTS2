#pragma once

#include <charconv>
#include <cstddef>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace ts2 {

// macOS libc++ does not provide floating-point from_chars before macOS 26.
// Keep its fallback here so callers can otherwise use the same parser.
#ifdef __APPLE__
namespace detail {

inline bool startsWithWord(std::string_view text, std::string_view word) {
    if (text.size() < word.size()) {
        return false;
    }
    for (std::size_t i = 0; i < word.size(); ++i) {
        if ((text[i] | 0x20) != word[i]) {
            return false;
        }
    }
    return true;
}

inline bool isDigit(char c) { return c >= '0' && c <= '9'; }

// How much of the text from_chars would read as a number: an optional minus, digits with at most
// one point, and an exponent only when digits follow it. libc++'s streams refuse a number with
// letters after it, so they are only handed this part.
inline std::size_t leadingFloatLength(std::string_view text) {
    std::size_t i = !text.empty() && text[0] == '-' ? 1 : 0;
    std::size_t digits = 0;
    while (i < text.size() && isDigit(text[i])) {
        ++i;
        ++digits;
    }
    if (i < text.size() && text[i] == '.') {
        ++i;
        while (i < text.size() && isDigit(text[i])) {
            ++i;
            ++digits;
        }
    }
    if (digits == 0) {
        return 0;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        std::size_t j = i + 1;
        if (j < text.size() && (text[j] == '+' || text[j] == '-')) {
            ++j;
        }
        if (j < text.size() && isDigit(text[j])) {
            while (j < text.size() && isDigit(text[j])) {
                ++j;
            }
            i = j;
        }
    }
    return i;
}

} // namespace detail
#endif

// The number at the start of the text, ignoring anything after it. Nothing when the text doesn't
// start with one.
inline std::optional<float> parseLeadingFloat(std::string_view text, std::size_t* length = nullptr) {
#ifdef __APPLE__
    // Infinity and not-a-number, which from_chars reads too.
    const bool negative = !text.empty() && text[0] == '-';
    const std::string_view word = text.substr(negative ? 1 : 0);
    for (const std::string_view name : {"infinity", "inf", "nan"}) {
        if (detail::startsWithWord(word, name)) {
            if (length) {
                *length = name.size() + (negative ? 1 : 0);
            }
            const float value = name == "nan" ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
            return negative ? -value : value;
        }
    }
    const std::size_t n = detail::leadingFloatLength(text);
    if (n == 0) {
        return std::nullopt;
    }
    std::istringstream input{std::string(text.substr(0, n))};
    input.imbue(std::locale::classic());
    float value = 0;
    input >> std::noskipws >> value;
    if (input.fail()) {
        return std::nullopt;
    }
    if (length) {
        *length = n;
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
