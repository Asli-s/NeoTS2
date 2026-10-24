#include "ui/font_metrics.hpp"

#include <algorithm>
#include <cstdint>

namespace ts2::ui {

namespace {

// The game's font engine works in 16.16 fixed point and rounds every step toward zero, so its
// arithmetic is copied exactly: a different rounding moves text by a pixel.
std::int32_t fixedMul(std::int32_t a, std::int32_t b) {
    const bool negative = (a < 0) != (b < 0);
    const std::uint32_t x = a < 0 ? 0u - static_cast<std::uint32_t>(a) : static_cast<std::uint32_t>(a);
    const std::uint32_t y = b < 0 ? 0u - static_cast<std::uint32_t>(b) : static_cast<std::uint32_t>(b);
    const std::uint32_t xh = x >> 16, xl = x & 0xFFFF, yh = y >> 16, yl = y & 0xFFFF;
    const std::uint32_t m = ((xl * yl) >> 16) + xl * yh + xh * yl + ((xh * yh) << 16);
    return static_cast<std::int32_t>(negative ? 0u - m : m);
}

// The remainder is halved, with the divisor, until it fits 16 bits before its fraction is found.
std::int32_t fixedDiv(std::int32_t a, std::int32_t b) {
    const bool negative = (a < 0) != (b < 0);
    std::uint32_t x = a < 0 ? 0u - static_cast<std::uint32_t>(a) : static_cast<std::uint32_t>(a);
    std::uint32_t y = b < 0 ? 0u - static_cast<std::uint32_t>(b) : static_cast<std::uint32_t>(b);
    if (y == 0) {
        return 0;
    }
    const std::uint32_t whole = (x / y) << 16;
    std::uint32_t rest = x % y;
    while (rest > 0xFFFF) {
        rest >>= 1;
        y >>= 1;
    }
    const std::uint32_t m = whole + (rest << 16) / y;
    return static_cast<std::int32_t>(negative ? 0u - m : m);
}

} // namespace

LineMetrics typeOneLine(int ascender, int descender, int unitsPerEm, int size) {
    // Font units are 16-bit in the game. A broken font can't push the sums past them.
    ascender = std::clamp(ascender, -32767, 32767);
    descender = std::clamp(descender, -32767, 32767);
    unitsPerEm = std::clamp(unitsPerEm, 1, 32767);
    size = std::clamp(size, 1, 2048);
    const std::int32_t lineGap = std::clamp(unitsPerEm - (ascender - descender), 0, 32767);
    constexpr std::int32_t kOne = 65536;
    const std::int32_t scale = fixedDiv(size * kOne, unitsPerEm * kOne);
    const std::int64_t above = fixedMul(ascender * kOne, scale);
    const std::int64_t below = fixedMul(descender * kOne, scale);
    const std::int64_t gap = fixedMul(lineGap * kOne, scale);
    // Pixel measures past 4096 only come from broken fonts.
    LineMetrics out;
    out.ascent = static_cast<int>(std::clamp<std::int64_t>((gap + above) / kOne, -4096, 4096));
    out.height = static_cast<int>(std::clamp<std::int64_t>((gap - below + above) / kOne, 1, 4096));
    // Type 1 lines are one pixel taller than measured, and one more past 28.
    ++out.height;
    if (out.height > 28) {
        ++out.height;
    }
    return out;
}

} // namespace ts2::ui
