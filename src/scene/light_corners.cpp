#include "scene/light_corners.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>

namespace ts2::scene::lightcorners {

namespace {

using walllight::Pixel;

// The game's constants, as it stores them.
const float kHalfPi = std::bit_cast<float>(0x3FC90FDBu);
const float kPi = std::bit_cast<float>(0x40490FDBu);
const float kRightAngleSlack = std::bit_cast<float>(0x3DC104FBu);  // 0.03 pi

// Whether run b turns a right angle (clockwise seen from above) from run a.
bool turnsRight(float a, float b) {
    return std::fabs(static_cast<double>(b) - a + kHalfPi) < kRightAngleSlack;
}

// The direction (0..7) a corner points to, from the halfway angle between its runs.
std::uint8_t octantOf(double angle) {
    const double pi8 = std::bit_cast<float>(0x3EC90FDBu), fourOverPi = std::bit_cast<float>(0x3FA2F983u);
    const double twoPi = static_cast<double>(std::bit_cast<float>(0x40C90FDBu));
    const double v = std::fmod(pi8 - angle, twoPi) * fourOverPi;
    return static_cast<std::uint8_t>((static_cast<long long>(std::floor(v)) - 2) & 7);
}

// Pulls texel column `index` along a run (four per segment) toward `ambient` by `f`, in the
// game's 16-bit steps: packed(ambient * f) - texel * 2 * k / 65536, k = (f - 1) * 32768.
void blendColumn(walllight::Run& run, std::size_t index, const std::array<float, 3>& ambient, float f) {
    const std::size_t segment = index >> 2;
    if (segment >= run.segments.size()) {
        return;
    }
    const Pixel target = walllight::pack({ambient[0] * f, ambient[1] * f, ambient[2] * f});
    // (f - 1) * 32768 rounded to the nearest whole number, halves to even, as the game's float trick does.
    const double v = (static_cast<double>(f) - 1.0) * 32768.0, down = std::floor(v);
    const int k = static_cast<int>(v - down > 0.5 || (v - down == 0.5 && std::fmod(down, 2.0) != 0) ? down + 1 : down);
    auto channel = [k](std::uint8_t to, std::uint8_t t) {
        return static_cast<std::uint8_t>(std::clamp(to - ((2 * t * k) >> 16), 0, 255));
    };
    for (Pixel& t : run.segments[segment].texels[index & 3]) {
        t = {channel(target.b, t.b), channel(target.g, t.g), channel(target.r, t.r), channel(target.a, t.a)};
    }
}

const std::array<float, 3>& ambientOf(const walllight::Walls& walls, const walllight::Run& run) {
    return run.room == 0 ? walls.outsideAmbient : walls.insideAmbient;
}

const walllight::Run* runAt(const walllight::Walls& walls, int index) {
    return index >= 0 && static_cast<std::size_t>(index) < walls.runs.size() ? &walls.runs[static_cast<std::size_t>(index)] : nullptr;
}

// One floor half pulled toward its ambient by w, each step rounded to float as the game stores it
// (1 - w kept unrounded, as on the game's floating point stack).
void blendHalf(floorlight::Half& h, const floorlight::Grid& grid, float w) {
    const auto& ambient = h.outside ? grid.outsideAmbient : grid.insideAmbient;
    for (std::size_t c = 0; c < 3; ++c) {
        const float toward = ambient[c] * w;
        const auto kept = static_cast<float>(static_cast<double>(h.light[c]) * (1.0 - static_cast<double>(w)));
        h.light[c] = toward + kept;
    }
}

} // namespace

void blendWalls(walllight::Walls& walls) {
    const float strong = std::bit_cast<float>(0x3F4CCCCDu), weak = std::bit_cast<float>(0x3ECCCCCDu);  // 0.8, 0.4
    for (auto& w : walls.runs) {
        if (!w.lamplit || w.segments.empty()) {
            continue;
        }
        const auto ambient = ambientOf(walls, w);
        const std::size_t n = 4 * w.segments.size();
        const bool several = w.segments.size() > 1;
        if (const auto* p = runAt(walls, w.previous); p && p->lamplit != w.lamplit && turnsRight(p->angle, w.angle)) {
            blendColumn(w, 0, ambient, strong);
            blendColumn(w, 1, ambient, strong);
            blendColumn(w, 2, ambient, weak);
            blendColumn(w, several ? 4 : 3, ambient, weak);
        }
        if (const auto* q = runAt(walls, w.next); q && q->lamplit != w.lamplit && turnsRight(w.angle, q->angle)) {
            blendColumn(w, n - 1, ambient, strong);
            blendColumn(w, n - 2, ambient, strong);
            blendColumn(w, n - 3, ambient, weak);
            blendColumn(w, several ? n - 5 : n - 4, ambient, weak);
        }
    }
}

std::vector<Corner> corners(const walllight::Walls& walls, const std::vector<std::vector<Tile>>& tiles) {
    std::vector<Corner> out;
    std::vector<bool> first(walls.runs.size()), second(walls.runs.size());
    auto add = [&](std::size_t a, std::size_t b) {
        const auto& ra = walls.runs[a];
        const auto& rb = walls.runs[b];
        if (a >= tiles.size() || tiles[a].empty() || ra.segments.empty() || rb.segments.empty()) {
            return;
        }
        first[a] = true;
        second[b] = true;
        const Tile& t = tiles[a].back();
        const double halfway = static_cast<float>((static_cast<double>(ra.angle) - kPi + rb.angle) * 0.5);
        out.push_back({t.x, t.y, t.level, octantOf(halfway), ra.segments.size() == 1 || rb.segments.size() == 1});
    };
    for (std::size_t i = 0; i < walls.runs.size(); ++i) {
        const auto& w = walls.runs[i];
        if (const auto* q = runAt(walls, w.next); q && !first[i] && !(w.lamplit && q->lamplit) && turnsRight(w.angle, q->angle)) {
            add(i, static_cast<std::size_t>(w.next));
        }
        if (const auto* p = runAt(walls, w.previous); p && !second[i] && !(p->lamplit && w.lamplit) && turnsRight(p->angle, w.angle)) {
            add(static_cast<std::size_t>(w.previous), i);
        }
    }
    return out;
}

void blendFloor(floorlight::Grid& grid, const std::vector<Corner>& corners) {
    if (!grid.fits()) {
        return;
    }
    // Weights by how far a cell is from the corner: 4 x 4 cells (index i + 4j), or 2 x 2 when a
    // run is one segment long.
    static const std::array<std::uint32_t, 16> kWide{0x3F68E8E9, 0x3F3DBDBE, 0x3F038384, 0x3E8C8C8D, 0x3F3DBDBE, 0x3F038384,
                                                     0x3E8C8C8D, 0x3DA0A0A1, 0x3F038384, 0x3E8C8C8D, 0x3E088889, 0x3CC0C0C1,
                                                     0x3E8C8C8D, 0x3DA0A0A1, 0x3CC0C0C1, 0x00000000};
    static const std::array<std::uint32_t, 4> kNarrow{0x3F39B9BA, 0x3E909091, 0x3E909091, 0x3D40C0C1};
    for (const auto& c : corners) {
        constexpr int kFar = 1 << 20;  // beyond any lot; keeps the cell arithmetic in range
        if (c.level != grid.level || !(c.octant & 1) || std::abs(c.x) > kFar || std::abs(c.y) > kFar) {
            continue;
        }
        const int o = c.octant;
        const int sx = 2 * c.x + (o == 1 || o == 3), sy = 2 * c.y + (o == 1 || o == 7);
        const int ax = floorlight::kDirX[(o - 1) & 7], ay = floorlight::kDirY[(o - 1) & 7];
        const int bx = floorlight::kDirX[(o + 1) & 7], by = floorlight::kDirY[(o + 1) & 7];
        auto cell = [&](int i, int j) { return grid.at(sx + i * bx + j * ax, sy + i * by + j * ay); };
        if (c.single) {
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    if (auto* n = cell(i, j)) {
                        const float w = std::bit_cast<float>(kNarrow[static_cast<std::size_t>(i + 2 * j)]);
                        for (auto& h : n->halves) {
                            if (h.valid) blendHalf(h, grid, w);
                        }
                    }
                }
            }
            continue;
        }
        // The game leaves out the nearest seven cells when they have no floor, but blends the
        // farther ones whether or not they do.
        for (int j = 0; j < 4; ++j) {
            for (int i = 0; i < 4; ++i) {
                const int k = i + 4 * j;
                auto* n = cell(i, j);
                if (!n || (k <= 6 && !n->halves[0].valid)) {
                    continue;
                }
                blendHalf(n->halves[0], grid, std::bit_cast<float>(kWide[static_cast<std::size_t>(k)]));
            }
        }
    }
}

} // namespace ts2::scene::lightcorners
