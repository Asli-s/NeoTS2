#include "scene/neighborhood_lighting.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ts2::scene {

namespace {

constexpr float kNoShadow = -1000.0f;  // the shadow heights' starting value

// The shadow heights (the height of the shadow over each vertex) cast by `raised` along the sun,
// swept away from the sun one line of vertices at a time. `columns` and `rows` may be swapped (with
// the sun's x and y) to sweep the other way.
void sweep(std::vector<float>& shadow, const std::vector<float>& raised, std::uint32_t columns, std::uint32_t rows, double across,
           double along, double up, double tile, bool swapped) {
    auto at = [&](std::uint32_t line, std::uint32_t i) {
        return swapped ? static_cast<std::size_t>(line) * columns + i : static_cast<std::size_t>(i) * columns + line;
    };
    const std::uint32_t lines = swapped ? rows : columns, length = swapped ? columns : rows;
    std::vector<float> front(length, kNoShadow);
    auto shift = static_cast<float>((-1.0 / along) * across);
    auto drop = static_cast<float>((tile / along) * up);
    int line = 0, step = 1, end = static_cast<int>(lines);
    if (drop >= 0) {
        drop = -drop;
        shift = -shift;
        line = static_cast<int>(lines) - 1;
        step = -1;
        end = -1;
    }
    for (; line != end; line += step) {
        for (std::uint32_t i = 0; i < length; ++i) {
            const std::size_t k = at(static_cast<std::uint32_t>(line), i);
            front[i] = std::max(front[i], raised[k]);
            shadow[k] = front[i];
            front[i] = static_cast<float>(static_cast<double>(drop) + front[i]);
        }
        // The front moves sideways along with the sun, a fraction of a vertex per line.
        if (shift <= 0) {
            for (std::uint32_t i = length - 1; i > 0; --i) {
                front[i] = static_cast<float>(front[i] - (static_cast<double>(front[i - 1]) - front[i]) * shift);
            }
        } else {
            for (std::uint32_t i = 0; i + 1 < length; ++i) {
                front[i] = static_cast<float>((static_cast<double>(front[i + 1]) - front[i]) * shift + front[i]);
            }
        }
    }
}

std::uint8_t toByte(double v) {
    // The game adds 49152 to a float and keeps the low bits: v * 256, rounded, clamped.
    const auto f = static_cast<float>(v + 49152.0);
    const auto scaled = std::nearbyint((static_cast<double>(f) - 49152.0) * 256.0);
    return scaled > 0.0 ? static_cast<std::uint8_t>(std::min(scaled, 255.0)) : std::uint8_t{0};
}

} // namespace

std::vector<float> sunShadow(const std::vector<float>& raised, std::uint32_t columns, std::uint32_t rows, const std::array<float, 3>& s, double tile) {
    std::vector<float> shadow(raised.size(), kNoShadow);
    if (raised.size() != static_cast<std::size_t>(columns) * rows || columns == 0 || rows == 0) {
        return shadow;
    }
    if (s[2] > 0 && (std::fabs(s[0]) >= 0.001 || std::fabs(s[1]) >= 0.001)) {
        if (std::fabs(s[1]) <= std::fabs(s[0])) {
            sweep(shadow, raised, columns, rows, s[1], s[0], s[2], tile, false);
        } else {
            sweep(shadow, raised, columns, rows, s[0], s[1], s[2], tile, true);
        }
    }
    return shadow;
}

Image neighborhoodLightMap(std::uint32_t columns, std::uint32_t rows, const std::vector<float>& heights, const std::vector<float>& normals,
                           float tileSize, const NeighborhoodLight& light, const std::vector<ShadowBox>& boxes) {
    Image out;
    if (columns < 2 || rows < 2 || heights.size() != static_cast<std::size_t>(columns) * rows || normals.size() != heights.size() * 3 ||
        light.lookup.width == 0 || light.lookup.height == 0) {
        return out;
    }
    // Shadow casters: the terrain, raised where boxes stand.
    std::vector<float> raised = heights;
    for (const auto& b : boxes) {
        const auto x0 = std::clamp(static_cast<int>(std::floor((b.x0 - static_cast<double>(light.boxWidening)) * static_cast<double>(1.0f / tileSize))), 0, static_cast<int>(columns) - 1);
        const auto x1 = std::clamp(static_cast<int>(std::ceil((b.x1 + static_cast<double>(light.boxWidening)) * static_cast<double>(1.0f / tileSize))), 0, static_cast<int>(columns) - 1);
        const auto y0 = std::clamp(static_cast<int>(std::floor((b.y0 - static_cast<double>(light.boxWidening)) * static_cast<double>(1.0f / tileSize))), 0, static_cast<int>(rows) - 1);
        const auto y1 = std::clamp(static_cast<int>(std::ceil((b.y1 + static_cast<double>(light.boxWidening)) * static_cast<double>(1.0f / tileSize))), 0, static_cast<int>(rows) - 1);
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const std::size_t k = static_cast<std::size_t>(y) * columns + static_cast<std::size_t>(x);
                raised[k] = std::max(raised[k], static_cast<float>(static_cast<double>(light.boxHeightScale) * b.height + heights[k]));
            }
        }
    }
    const auto& s = light.sun;
    const auto shadow = sunShadow(raised, columns, rows, s, tileSize);

    // A texel per tile.
    out.width = columns - 1;
    out.height = rows - 1;
    out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4);
    const double away[3] = {-s[0], -s[1], s[2]};
    const auto lw = light.lookup.width, lh = light.lookup.height;
    for (std::uint32_t y = 0; y < out.height; ++y) {
        for (std::uint32_t x = 0; x < out.width; ++x) {
            const std::size_t k = static_cast<std::size_t>(y) * columns + x;
            float n[3] = {normals[k * 3], normals[k * 3 + 1], normals[k * 3 + 2]};
            if (shadow[k] > heights[k]) {
                // In shadow: the normal leans away from the sun by how deep the shadow is.
                const auto t = static_cast<float>((static_cast<double>(shadow[k]) - heights[k]) * light.shadowStrength);
                float v[3];
                for (int i = 0; i < 3; ++i) {
                    v[i] = static_cast<float>(away[i] * t + n[i]);
                }
                const auto len2 = static_cast<float>(static_cast<double>(v[2]) * v[2] + static_cast<double>(v[1]) * v[1] + static_cast<double>(v[0]) * v[0]);
                if (len2 > 0) {
                    const auto inv = static_cast<float>(1.0 / std::sqrt(static_cast<double>(len2)));
                    for (int i = 0; i < 3; ++i) {
                        v[i] *= inv;
                    }
                }
                std::copy(v, v + 3, n);
            }
            const auto u = static_cast<std::uint32_t>(static_cast<std::int64_t>(std::floor((n[0] + 1.0) * lw * 0.5))) & (lw - 1);
            const auto w = static_cast<std::uint32_t>(static_cast<std::int64_t>(std::floor((n[1] + 1.0) * lh * 0.5))) & (lh - 1);
            const std::uint8_t* c = &light.lookup.rgba[(static_cast<std::size_t>(w) * lw + u) * 4];
            std::uint8_t* o = &out.rgba[(static_cast<std::size_t>(y) * out.width + x) * 4];
            for (int i = 0; i < 3; ++i) {
                // The game keeps the lookup image as bytes made the same way, so the rounding
                // happens twice.
                const std::uint8_t stored = toByte(static_cast<double>(c[i] / 255.0f));
                o[i] = toByte(static_cast<double>(static_cast<float>(light.tint[i] * (stored / 255.0f))));
            }
            o[3] = 255;
        }
    }
    return out;
}

} // namespace ts2::scene
