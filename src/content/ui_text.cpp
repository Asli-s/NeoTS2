#include "content/ui_text.hpp"

namespace ts2::content {

namespace {

std::string groupedDigits(std::uint64_t magnitude) {
    std::string digits;
    int count = 0;
    do {
        if (count > 0 && count % 3 == 0) {
            digits.insert(digits.begin(), ',');
        }
        digits.insert(digits.begin(), static_cast<char>('0' + magnitude % 10));
        magnitude /= 10;
        ++count;
    } while (magnitude > 0);
    return digits;
}

// Through unsigned, so the most negative value has a magnitude too.
std::uint64_t magnitude(std::int64_t value) { return value < 0 ? 0 - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value); }

} // namespace

std::string groupedNumber(std::int64_t value) { return std::string(value < 0 ? "-" : "") + groupedDigits(magnitude(value)); }

std::string moneyText(std::int64_t amount) { return std::string(amount < 0 ? "-" : "") + "\xC2\xA7" + groupedDigits(magnitude(amount)); }

std::string hourText(int hour) {
    hour = ((hour % 24) + 24) % 24;
    const int twelve = hour % 12 == 0 ? 12 : hour % 12;
    return std::to_string(twelve) + (hour < 12 ? " am" : " pm");
}

std::string formatted(std::string_view pattern, std::initializer_list<std::string_view> values) {
    std::string out;
    auto next = values.begin();
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '%' && i + 1 < pattern.size() && (pattern[i + 1] == 's' || pattern[i + 1] == 'd')) {
            if (next != values.end()) {
                out += *next++;
            }
            ++i;
        } else {
            out += pattern[i];
        }
    }
    return out;
}

} // namespace ts2::content
