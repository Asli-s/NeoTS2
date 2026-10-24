#include "scene/incidence_map.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ts2::scene {

namespace {

// The game's float-to-integer step: add 1.5 x 2^23 in single precision and keep the low bits
// (rounds to the nearest whole number, halves to even).
std::int32_t roundLikeTheGame(float v) {
    const float shifted = v + 12582912.0f;
    return std::bit_cast<std::int32_t>(shifted) - 0x4B400000;
}

// The game's table by the squared gradient's size (index (Lx^2 + Ly^2) >> 17): the gain on the
// gradient and the extra blue, kept as 16-bit words.
struct Table {
    std::int32_t height = 0;  // round((1 - heightClamp) x k x 255)
    std::vector<std::int16_t> gain, blue;
};

Table makeTable(float k, float heightClamp) {
    constexpr int kEntries = 16384;
    Table t;
    t.height = roundLikeTheGame((1.0f - heightClamp) * k * 255.0f);
    const float kk = k * k * 65025.0f;
    t.gain.resize(kEntries);
    t.blue.resize(kEntries);
    for (int i = 0; i < kEntries; ++i) {
        const float f = 0.5f / std::sqrt(static_cast<float>(i) * 8.0f + kk);
        t.gain[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(roundLikeTheGame(130560.0f * f));
        t.blue[static_cast<std::size_t>(i)] =
            static_cast<std::int16_t>(roundLikeTheGame(static_cast<float>(t.height * t.height) * f * 4.0156863f));
    }
    return t;
}

std::int32_t saturate16(std::int32_t v) {
    return std::clamp(v, -32768, 32767);
}

std::uint8_t saturate8(std::int32_t v) {
    return static_cast<std::uint8_t>(std::clamp(v, 0, 255));
}

} // namespace

Image incidenceMap(const Image& lightMap, float scale, float heightClamp) {
    const std::uint32_t w = lightMap.width / 2, h = lightMap.height / 2;
    if (w == 0 || h == 0 || !(scale > 0) || !std::isfinite(scale) || !std::isfinite(heightClamp) ||
        lightMap.rgba.size() != static_cast<std::size_t>(lightMap.width) * lightMap.height * 4) {
        return {};
    }
    const Table table = makeTable(2.0f / scale, heightClamp);
    // Luminance weights for red, green and blue (the game's table head: blue 7, green 38, red 19).
    constexpr std::array<std::int32_t, 3> kWeight{19, 38, 7};
    Image out;
    out.width = w;
    out.height = h;
    out.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    const std::size_t stride = static_cast<std::size_t>(lightMap.width) * 4;
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t* upper = &lightMap.rgba[2 * y * stride];
        const std::uint8_t* lower = upper + stride;
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint8_t* m0 = upper + 8 * static_cast<std::size_t>(x);
            const std::uint8_t* m2 = lower + 8 * static_cast<std::size_t>(x);
            std::int32_t lx = 0, ly = 0;
            for (std::size_t c = 0; c < 3; ++c) {
                const std::int32_t gx = (m0[4 + c] + m2[4 + c]) - (m0[c] + m2[c]);
                const std::int32_t gy = (m2[c] + m2[4 + c]) - (m0[c] + m0[4 + c]);
                lx += gx * kWeight[c];
                ly += gy * kWeight[c];
            }
            lx = saturate16(lx);
            ly = saturate16(ly);
            const std::size_t i = static_cast<std::size_t>((lx * lx + ly * ly) >> 17);
            const std::int32_t gain = table.gain[i];
            std::uint8_t* p = &out.rgba[(static_cast<std::size_t>(y) * w + x) * 4];
            p[0] = saturate8(128 + ((lx * gain) >> 16));
            p[1] = saturate8(128 + ((ly * gain) >> 16));
            p[2] = saturate8(383 - table.height + ((0x4000 * table.blue[i]) >> 16));
            p[3] = 255;
        }
    }
    return out;
}

} // namespace ts2::scene
