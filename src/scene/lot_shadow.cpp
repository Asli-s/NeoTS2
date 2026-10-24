#include "scene/lot_shadow.hpp"

#include "diag/log.hpp"
#include "scene/neighborhood_lighting.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ts2::scene::lotshadow {

namespace {

constexpr float kNone = -1000.0f;
constexpr float kStorey = 3.0f;
constexpr int kMaxTiles = 256;

} // namespace

float Grid::shadowAt(float x, float y) const {
    if (!std::isfinite(x) || !std::isfinite(y)) return kNone;
    const auto ix = static_cast<long long>(std::floor((static_cast<double>(x) + 0.001) * kSamplesPerTile));
    const auto iy = static_cast<long long>(std::floor((static_cast<double>(y) + 0.001) * kSamplesPerTile));
    if (ix < 0 || iy < 0 || ix >= width || iy >= height) return kNone;
    return shadow[static_cast<std::size_t>(iy * width + ix)];
}

float Grid::groundAt(int x, int y) const {
    if (x < 0 || y < 0 || x >= width || y >= height) return kNone;
    return ground[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
}

float elevation(const Inputs& in, float x, float y, int level) {
    if (!in.corner || in.levels <= 0 || !(std::abs(x) < 1e6f && std::abs(y) < 1e6f)) return 0;
    if (level < 0) return elevation(in, x, y, 0) + kStorey * static_cast<float>(level);
    if (level >= in.levels) return elevation(in, x, y, in.levels - 1) + kStorey * static_cast<float>(level - in.levels + 1);
    return cornersOf(in, static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), level).at(x, y);
}

Corners cornersOf(const Inputs& in, int x, int y, int level) {
    if (!in.corner) return {};
    return {in.corner(x, y, level), in.corner(x + 1, y, level), in.corner(x, y + 1, level), in.corner(x + 1, y + 1, level)};
}

float Corners::at(float x, float y) const {
    return between(x - std::floor(x), y - std::floor(y));
}

float Corners::between(float fx, float fy) const {
    const float a = x0y0 * (1 - fx) + x1y0 * fx;
    const float b = x0y1 * (1 - fx) + x1y1 * fx;
    return a * (1 - fy) + b * fy;
}

float sunVisible(float depth, float zone) {
    if (!(depth > 0)) return 1;
    return std::max(1.0f - depth * zone, 0.0f);
}

Grid build(const Inputs& in, const std::array<float, 3>& sun) {
    Grid g;
    if (in.width <= 0 || in.height <= 0 || in.width > kMaxTiles || in.height > kMaxTiles || in.levels <= 0 || in.levels > 16 || !in.corner) {
        if (in.width > kMaxTiles || in.height > kMaxTiles || in.levels > 16) {
            diag::warnOnce("lot shadow: a lot larger than 256 tiles or 16 levels casts no shadows");
        }
        return g;
    }
    g.width = in.width * kSamplesPerTile;
    g.height = in.height * kSamplesPerTile;
    const std::size_t n = static_cast<std::size_t>(g.width) * static_cast<std::size_t>(g.height);
    g.heights.assign(n, kNone);
    g.ground.assign(n, kNone);
    auto at = [&](int c, int r) { return static_cast<std::size_t>(r) * static_cast<std::size_t>(g.width) + static_cast<std::size_t>(c); };

    // Floors: each quarter tile at its topmost floor, the ground below it kept as well.
    for (int ty = 0; ty < in.height; ++ty) {
        for (int tx = 0; tx < in.width; ++tx) {
            const Corners groundCorners = cornersOf(in, tx, ty, 0);
            for (int q = 0; q < 4; ++q) {
                const int triangle = q ^ (q >> 1);
                for (int level = in.levels - 1; level >= 0; --level) {
                    if (level != 0 && !(in.floorAt && in.floorAt(tx, ty, level, q))) continue;
                    const Corners floorCorners = level == 0 ? groundCorners : cornersOf(in, tx, ty, level);
                    for (int r = 0; r < kSamplesPerTile; ++r) {
                        for (int c = 0; c < kSamplesPerTile; ++c) {
                            if (((c + r >= 7 ? 1 : 0) | (c >= r ? 2 : 0)) != triangle) continue;
                            const float px = (static_cast<float>(kSamplesPerTile * tx + c) + 0.5f) * kCell;
                            const float py = (static_cast<float>(kSamplesPerTile * ty + r) + 0.5f) * kCell;
                            const std::size_t s = at(kSamplesPerTile * tx + c, kSamplesPerTile * ty + r);
                            g.heights[s] = floorCorners.at(px, py);
                            g.ground[s] = groundCorners.at(px, py);
                        }
                    }
                    break;
                }
            }
        }
    }

    // Walls: the top of each, written into the line of samples on its side away from the sun.
    const int W = g.width, H = g.height;
    auto put = [&](int c, int r, float v) {
        if (c >= 0 && r >= 0 && c < W && r < H) g.heights[at(c, r)] = v;
    };
    for (const auto& w : in.walls) {
        if (w.level < 0) continue;
        if (std::max({std::abs(w.x0), std::abs(w.y0), std::abs(w.x1), std::abs(w.y1)}) > kMaxTiles) {
            diag::warnOnce("lot shadow: a wall more than 256 tiles out casts no shadow");
            continue;
        }
        float h0, h1;
        if (w.fence) {
            h0 = in.corner(w.x0, w.y0, w.level) + w.fenceHeight;
            h1 = in.corner(w.x1, w.y1, w.level) + w.fenceHeight;
        } else if (w.level >= in.levels - 1) {
            h0 = in.corner(w.x0, w.y0, w.level) + kStorey;
            h1 = in.corner(w.x1, w.y1, w.level) + kStorey;
        } else {
            h0 = in.corner(w.x0, w.y0, w.level + 1);
            h1 = in.corner(w.x1, w.y1, w.level + 1);
        }
        auto value = [&](double t) { return static_cast<float>((static_cast<double>(h1) - h0) * t + h0); };
        int x0 = w.x0 * kSamplesPerTile, y0 = w.y0 * kSamplesPerTile, x1 = w.x1 * kSamplesPerTile, y1 = w.y1 * kSamplesPerTile;
        if (x0 == x1 && y0 == y1) continue;
        if (x0 == x1) {  // along y
            const double step0 = 1.0 / (y1 - y0);
            double t0 = 0.5 * step0, step = step0;
            if (y1 < y0) {
                t0 = 1.0 - t0;
                std::swap(y0, y1);
            }
            int r0 = y0, r1 = y1;
            if (r0 > 0 && sun[1] >= 0) --r0;
            if (r1 >= H || sun[1] > 0) --r1;
            if (x0 < W && sun[0] <= 0) for (int r = r0; r <= r1; ++r) put(x0, r, value(t0 + (r - r0) * step));
            if (x0 > 0 && sun[0] >= 0) for (int r = r0; r <= r1; ++r) put(x0 - 1, r, value(t0 + (r - r0) * step));
        } else if (y0 == y1) {  // along x
            const double step0 = 1.0 / (x1 - x0);
            double t0 = 0.5 * step0, step = step0;
            if (x1 < x0) {
                t0 = 1.0 - t0;
                std::swap(x0, x1);
            }
            int c0 = x0, c1 = x1;
            if (c0 > 0 && sun[0] >= 0) --c0;
            if (c1 >= W || sun[0] > 0) --c1;
            if (y0 < H && sun[1] <= 0) for (int c = c0; c <= c1; ++c) put(c, y0, value(t0 + (c - c0) * step));
            if (y0 > 0 && sun[1] >= 0) for (int c = c0; c <= c1; ++c) put(c, y0 - 1, value(t0 + (c - c0) * step));
        } else {  // a diagonal
            const double step0 = 1.0 / (x1 - x0);
            double t0 = 0.5 * step0, step = step0;
            if (x1 < x0) {
                t0 = 1.0 - t0;
                std::swap(x0, x1);
                std::swap(y0, y1);
            }
            const int d = y1 > y0 ? 1 : -1;
            const int r0 = y1 > y0 ? y0 : y0 - 1;
            const int count = x1 - x0;
            for (int k = 0; k < count; ++k) put(x0 + k, r0 + k * d, value(t0 + k * step));
            const bool a = d * sun[1] - sun[0] <= 0, b = d * sun[1] - sun[0] >= 0;
            if (b) for (int k = 0; k + 1 < count; ++k) put(x0 + 1 + k, r0 + k * d, value(t0 + k * step));
            if (a) for (int k = 0; k + 1 < count; ++k) put(x0 + k, r0 + (k + 1) * d, value(t0 + k * step));
            if (a && x0 > 0) put(x0 - 1, r0, value(t0));
            if (b && x1 < W) put(x1, r0 + (count - 1) * d, value(1 - t0));
            if (const int r = r0 + count * d; a && r >= 0 && r < H) put(x1 - 1, r, value(1 - t0));
            if (const int r = r0 - d; b && r >= 0 && r < H) put(x0, r, value(t0));
        }
    }

    g.shadow = sunShadow(g.heights, static_cast<std::uint32_t>(g.width), static_cast<std::uint32_t>(g.height), sun, kCell);
    return g;
}

} // namespace ts2::scene::lotshadow
