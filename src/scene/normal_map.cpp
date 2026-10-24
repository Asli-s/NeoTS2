#include "scene/normal_map.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace ts2::scene {

namespace {

// The game's float-to-integer step: add 1.5 x 2^23 in single precision and keep the low bits
// (rounds to the nearest whole number, halves to even).
std::int32_t roundLikeTheGame(float v) {
    return std::bit_cast<std::int32_t>(v + 12582912.0f) - 0x4B400000;
}

// The game's table by the squared slopes' size (index (x^2 + y^2) >> 6): the gain on
// the slopes and on straight out, kept as 16-bit words. Each entry takes one Newton step toward
// 1 / sqrt(64 i + f^2) from the entry before, with f = 1020 / amplitude.
struct Table {
    int shift = 0;  // the slopes are shifted up by this before the gain
    std::vector<std::int16_t> gain, up;
};

Table makeTable(float amplitude) {
    constexpr int kEntries = 16384;
    Table t;
    const float f = 1.0f / amplitude * 1020.0f;
    float estimate = 1.0f / f;
    for (float g = 8355840.0f * estimate; g > 32767.0f && t.shift < 30; g *= 0.5f) {
        ++t.shift;
    }
    const float slopeScale = 8355840.0f / static_cast<float>(1 << t.shift);
    const float upScale = f * 32640.0f;
    const float ff = f * f;
    t.gain.resize(kEntries);
    t.up.resize(kEntries);
    for (int i = 0; i < kEntries; ++i) {
        estimate = (1.5f - estimate * estimate * (static_cast<float>(i) * 64.0f + ff) * 0.5f) * estimate;
        t.gain[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(roundLikeTheGame(slopeScale * estimate));
        t.up[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(roundLikeTheGame(upScale * estimate));
    }
    return t;
}

// The high word of a 16 x 16-bit product, as the game's pmulhw.
std::int32_t highWord(std::int32_t a, std::int32_t b) {
    return (static_cast<std::int16_t>(a) * static_cast<std::int32_t>(static_cast<std::int16_t>(b))) >> 16;
}

std::uint8_t saturate8(std::int32_t v) {
    return static_cast<std::uint8_t>(std::clamp(v, 0, 255));
}

} // namespace

std::vector<std::uint8_t> bumpNormals(std::span<const std::uint8_t> heights, std::uint32_t width, std::uint32_t height, float amplitude) {
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    if (pixels == 0 || heights.size() != pixels || !(amplitude > 0) || !std::isfinite(amplitude)) {
        return {};
    }
    const Table table = makeTable(amplitude);
    auto at = [&](std::uint32_t x, std::uint32_t y) { return static_cast<std::int32_t>(heights[static_cast<std::size_t>(y) * width + x]); };
    std::vector<std::uint8_t> out(pixels * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint32_t above = y == 0 ? height - 1 : y - 1, below = y + 1 == height ? 0 : y + 1;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t left = x == 0 ? width - 1 : x - 1, right = x + 1 == width ? 0 : x + 1;
            const std::int32_t falling = at(left, above) - at(right, below), rising = at(right, above) - at(left, below);
            const std::int32_t sy = rising + falling, sx = falling - rising;
            const std::size_t i = static_cast<std::size_t>((sx * sx + sy * sy) >> 6);
            std::uint8_t* p = &out[(static_cast<std::size_t>(y) * width + x) * 4];
            p[0] = saturate8(128 + highWord(sx << table.shift, table.gain[i]));
            p[1] = saturate8(128 + highWord(sy << table.shift, table.gain[i]));
            p[2] = saturate8(128 + highWord(0x100, table.up[i]));
            p[3] = 255;
        }
    }
    return out;
}

} // namespace ts2::scene
