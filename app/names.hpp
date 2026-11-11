#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace app {

// Case-blind comparison of two names, below 0 when `a` sorts first.
inline int compareNames(const std::string& a, const std::string& b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const int ca = std::tolower(static_cast<unsigned char>(a[i])), cb = std::tolower(static_cast<unsigned char>(b[i]));
        if (ca != cb) {
            return ca - cb;
        }
    }
    return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
}

// A Sim's first name and surname with a space between, or whichever of the two it has.
inline std::string fullName(const std::string& first, const std::string& last) {
    return first.empty() ? last : last.empty() ? first : first + " " + last;
}

} // namespace app
