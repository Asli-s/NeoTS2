#include "scene/light_map_texels.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ts2::scene::lightmaptexels {

namespace {

using walllight::Pixel;
using Color = std::array<float, 3>;

// The game works in double precision and rounds to float where it stores a value.
float r24(double v) {
    return static_cast<float>(v);
}

// Rounds to the nearest whole number, halves to even, whatever the processor's rounding mode.
double roundHalfEven(double v) {
    const double down = std::floor(v);
    const double rest = v - down;
    return rest > 0.5 || (rest == 0.5 && std::fmod(down, 2.0) != 0) ? down + 1 : down;
}

Pixel pack(const Color& c) {
    return walllight::pack(c);
}

bool sameBits(float a, float b) {
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

bool sameBits(const Color& a, const Color& b) {
    return sameBits(a[0], b[0]) && sameBits(a[1], b[1]) && sameBits(a[2], b[2]);
}

// ---- Floors ----

// A tile's 4 x 4 grid of colours (index x + 4y): its four quarter cells in the middle, the cells
// beyond them around the edge.
using Patch = std::array<Color, 16>;

// The direction (0..7, as the floor light's) from a tile's middle to an edge cell of its patch.
int edgeDirection(int gx, int gy) {
    if (gx == 0) return gy == 0 ? 1 : gy == 3 ? 3 : 2;
    if (gx == 3) return gy == 0 ? 7 : gy == 3 ? 5 : 6;
    return gy == 0 ? 0 : 4;
}

Color minus(const Color& a, const Color& b) {
    return {r24(static_cast<double>(a[0]) - b[0]), r24(static_cast<double>(a[1]) - b[1]), r24(static_cast<double>(a[2]) - b[2])};
}

// What a half shows in the light map: its light, the ambient where it has no floor, with the
// outside ambient taken off outdoors.
Color shown(const floorlight::Grid& g, const floorlight::Half& h) {
    const Color v = h.valid ? h.light : (h.outside ? g.outsideAmbient : g.insideAmbient);
    return g.dimOutside && h.outside ? minus(v, g.outsideAmbient) : v;
}

Color linkedLight(const floorlight::Grid& g, const floorlight::Half& h) {
    return g.dimOutside && h.outside ? minus(h.light, g.outsideAmbient) : h.light;
}

// The half linked from a node's half in direction d, when it has floor.
const floorlight::Half* linkedHalf(const floorlight::Grid& g, int x, int y, int d, int half) {
    const auto* from = g.at(x, y);
    if (!from) return nullptr;
    const auto& rec = from->halves[static_cast<std::size_t>(half)];
    if (!(rec.links & (1u << d))) return nullptr;
    const auto* to = g.at(x + floorlight::kDirX[d], y + floorlight::kDirY[d]);
    if (!to) return nullptr;
    const auto& h = to->halves[(rec.toSecond >> d) & 1u];
    return h.valid ? &h : nullptr;
}

Patch patchOf(const floorlight::Grid& g, int tx, int ty, int half) {
    Patch p{};
    const floorlight::Cell* n[4] = {g.at(2 * tx, 2 * ty), g.at(2 * tx + 1, 2 * ty), g.at(2 * tx, 2 * ty + 1), g.at(2 * tx + 1, 2 * ty + 1)};
    const int inner[4] = {5, 6, 9, 10};
    for (int k = 0; k < 4; ++k) {
        if (n[k]) p[static_cast<std::size_t>(inner[k])] = shown(g, n[k]->halves[static_cast<std::size_t>(half)]);
    }
    for (int gx = 0; gx < 4; ++gx) {
        for (int gy = 0; gy < 4; ++gy) {
            if ((gx == 1 || gx == 2) && (gy == 1 || gy == 2)) continue;
            const int qx = gx >= 2, qy = gy >= 2;
            const int x2 = 2 * tx + qx, y2 = 2 * ty + qy, d = edgeDirection(gx, gy);
            const Color own = p[static_cast<std::size_t>((1 + qx) + 4 * (1 + qy))];
            Color& out = p[static_cast<std::size_t>(gx + 4 * gy)];
            if (const auto* nb = linkedHalf(g, x2, y2, d, half)) {
                out = linkedLight(g, *nb);
            } else if (d % 2 == 0) {
                out = own;
            } else {
                const auto* a = linkedHalf(g, x2, y2, d - 1, half);
                const auto* b = linkedHalf(g, x2, y2, (d + 1) & 7, half);
                if (a && b) {
                    const Color ca = linkedLight(g, *a), cb = linkedLight(g, *b);
                    for (std::size_t c = 0; c < 3; ++c) out[c] = r24(static_cast<double>(r24(static_cast<double>(cb[c]) + ca[c])) * 0.5);
                } else {
                    out = a ? linkedLight(g, *a) : b ? linkedLight(g, *b) : own;
                }
            }
        }
    }
    return p;
}

// Writes a tile's texels from its patch, bilinearly. For a split tile only the texels of one half
// (`half`, by the test u * a + v < b), with u starting at 0 even on the ground level as the game does.
void writeTile(FloorImage& img, const Patch& p, int tx, int ty, bool ground, bool split, double a, float b, int half) {
    constexpr int kSide = 8;
    float du, dv, v, u0;
    if (ground) {
        du = r24(1.0 / kSide);
        dv = r24(-1.0 / kSide);
        v = r24(1.0 + 0.5 * (-1.0 / kSide));
        u0 = split ? 0.0f : r24(static_cast<double>(du) * 0.5);
    } else {
        du = r24(1.0 / (kSide - 1));
        dv = r24(-1.0 / (kSide - 1));
        v = 1.0f;
        u0 = 0.0f;
    }
    for (int r = 0; r < kSide; ++r) {
        const float gy = r24(static_cast<double>(v) + v + 0.5);
        const double ly = std::clamp(std::floor(static_cast<double>(gy)), 0.0, 2.0);
        const double fy = gy - ly;
        float u = u0;
        for (int x = 0; x < kSide; ++x) {
            if (split && ((static_cast<double>(u) * a + v < b) == (half != 0))) {
                u = r24(static_cast<double>(u) + du);
                continue;  // the other half's texel
            }
            const double gx = 2.0 * u + 0.5;
            const double lx = std::clamp(std::floor(gx), 0.0, 2.0);
            const float fx = r24(gx - lx);
            const float w00 = r24((1.0 - fx) * (1.0 - fy)), w10 = r24(fx * (1.0 - fy));
            const double w11 = fx * fy, w01 = (1.0 - fx) * fy;
            const auto i = static_cast<std::size_t>(lx + 4 * ly);
            Color o{};
            for (std::size_t c = 0; c < 3; ++c) {
                const float p00 = r24(static_cast<double>(p[i][c]) * w00), p10 = r24(static_cast<double>(p[i + 1][c]) * w10);
                const float p01 = r24(p[i + 4][c] * w01), p11 = r24(p[i + 5][c] * w11);
                o[c] = r24(static_cast<double>(r24(static_cast<double>(r24(static_cast<double>(p10) + p00)) + p01)) + p11);
            }
            const std::size_t at = static_cast<std::size_t>(ty * kSide + kSide - 1 - r) * static_cast<std::size_t>(img.width) +
                                   static_cast<std::size_t>(tx * kSide + x);
            img.texels[at] = pack(o);
            u = r24(static_cast<double>(u) + du);
        }
        v = r24(static_cast<double>(v) + dv);
    }
}

// ---- Walls ----

float decoded(std::uint8_t byte) {
    return r24(byte * static_cast<double>(std::bit_cast<float>(0x3B808081u)));  // 1/255 as the game stores it
}

float lerp(float a, float b, double t) {
    return r24(a + static_cast<double>(r24(static_cast<double>(r24(static_cast<double>(b) - a)) * t)));
}

std::array<float, 3> channels(const Pixel& p) {
    return {decoded(p.r), decoded(p.g), decoded(p.b)};
}

// The rows a rect samples from the padded grid: u across 0.5 .. 2.5, v from 6.5 down.
void sample(std::vector<Pixel>& out, const std::array<walllight::Column, 4>& t, int w, int rows, float vstep, const Color& ambient, float scale) {
    // Every row steps u the same way and reads the same texels: work those out once.
    std::array<std::array<Color, 8>, 4> texel{};
    for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t j = 0; j < 8; ++j) texel[i][j] = channels(t[i][j]);
    }
    struct Across {
        std::size_t lu = 0, hu = 0;
        float fu = 0;
    };
    std::array<Across, 64> across{};  // panelTexels keeps w at most 64
    const float ustep = r24(2.0 / (w - 1));
    float u = 0.5f;
    for (int x = 0; x < w && x < 64; ++x) {
        auto& a = across[static_cast<std::size_t>(x)];
        a.lu = static_cast<std::size_t>(std::clamp(std::floor(u), 0.0f, 3.0f));
        a.hu = static_cast<std::size_t>(std::clamp(std::ceil(u), 0.0f, 3.0f));
        a.fu = r24(static_cast<double>(u) - std::floor(u));
        u = r24(static_cast<double>(u) + ustep);
    }
    float v = 6.5f;
    for (int r = 0; r < rows; ++r) {
        const auto lv = static_cast<std::size_t>(std::clamp(std::floor(v), 0.0f, 7.0f)), hv = static_cast<std::size_t>(std::clamp(std::ceil(v), 0.0f, 7.0f));
        const double fv = static_cast<double>(v) - std::floor(v);
        for (int x = 0; x < w && x < 64; ++x) {
            const auto [lu, hu, fu] = across[static_cast<std::size_t>(x)];
            const auto &a0 = texel[lu][lv], &b0 = texel[hu][lv], &a1 = texel[lu][hv], &b1 = texel[hu][hv];
            Color o{};
            for (std::size_t c = 0; c < 3; ++c) {
                const float p1 = lerp(a1[c], b1[c], fu), p0 = lerp(a0[c], b0[c], fu);
                o[c] = lerp(p0, p1, fv);
            }
            // R and G keep the difference unrounded before scaling. B stores it first.
            const Color scaled{r24((static_cast<double>(o[0]) - ambient[0]) * scale), r24((static_cast<double>(o[1]) - ambient[1]) * scale),
                               r24(static_cast<double>(r24(static_cast<double>(o[2]) - ambient[2])) * scale)};
            out[static_cast<std::size_t>(r * w + x)] = pack(scaled);
        }
        v = r24(static_cast<double>(v) - vstep);
    }
}

} // namespace

const walllight::Pixel* FloorImage::at(int x, int y) const {
    if (x < 0 || y < 0 || x >= width || y >= height) return nullptr;
    return &texels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
}

FloorImage floorTexels(const floorlight::Grid& grid, const walllight::Pixel& fill) {
    FloorImage img;
    if (!grid.fits()) {
        return img;
    }
    img.width = 8 * grid.width;
    img.height = 8 * grid.height;
    img.texels.assign(static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height), fill);
    const bool ground = grid.level == 0;
    for (int ty = 0; ty < grid.height; ++ty) {
        for (int tx = 0; tx < grid.width; ++tx) {
            const auto& tile = grid.tiles[static_cast<std::size_t>(ty) * static_cast<std::size_t>(grid.width) + static_cast<std::size_t>(tx)];
            if (!tile.floor) continue;
            bool any[2] = {false, false};
            for (int k = 0; k < 4; ++k) {
                if (const auto* c = grid.at(2 * tx + (k & 1), 2 * ty + (k >> 1))) {
                    any[0] = any[0] || c->halves[0].valid;
                    any[1] = any[1] || c->halves[1].valid;
                }
            }
            if (tile.diagonal == 0) {
                if (any[0]) writeTile(img, patchOf(grid, tx, ty, 0), tx, ty, ground, false, 0, 0, 0);
                continue;
            }
            const double a = tile.diagonal == 1 ? 1.0 : -1.0;
            const float b = tile.diagonal == 1 ? std::bit_cast<float>(0x3F7D70A4u) : std::bit_cast<float>(0xBC23D70Au);  // 0.99, -0.01
            for (int half = 0; half < 2; ++half) {
                if (any[half]) writeTile(img, patchOf(grid, tx, ty, half), tx, ty, ground, true, a, b, half);
            }
        }
    }
    return img;
}

namespace {

// Which texels of a split tile its first half wrote (index i + 8 * texel row), by the same
// coordinates and test as writeTile.
std::array<bool, 64> firstHalfTexels(bool ground, int diagonal) {
    constexpr int kSide = 8;
    const double a = diagonal == 1 ? 1.0 : -1.0;
    const float b = diagonal == 1 ? std::bit_cast<float>(0x3F7D70A4u) : std::bit_cast<float>(0xBC23D70Au);
    const float du = r24(ground ? 1.0 / kSide : 1.0 / (kSide - 1)), dv = r24(ground ? -1.0 / kSide : -1.0 / (kSide - 1));
    float v = ground ? r24(1.0 + 0.5 * (-1.0 / kSide)) : 1.0f;
    std::array<bool, 64> first{};
    for (int r = 0; r < kSide; ++r) {
        float u = 0.0f;
        for (int x = 0; x < kSide; ++x) {
            first[static_cast<std::size_t>((kSide - 1 - r) * kSide + x)] = static_cast<double>(u) * a + v < b;
            u = r24(static_cast<double>(u) + du);
        }
        v = r24(static_cast<double>(v) + dv);
    }
    return first;
}

Pixel saturatingAdd(const Pixel& t, const Pixel& add) {
    auto ch = [](std::uint8_t a, std::uint8_t b) { return static_cast<std::uint8_t>(std::min(255, a + b)); };
    return {ch(t.b, add.b), ch(t.g, add.g), ch(t.r, add.r), t.a};
}

} // namespace

namespace {

// 0..1, with a value that isn't a number (from broken light data) as 0.
double unitClamp(double v) {
    return v > 0.0 ? std::min(v, 1.0) : 0.0;
}

// A whole number kept well inside int's range (0 for a value that isn't a number).
int boundedInt(double v) {
    return v == v ? static_cast<int>(std::clamp(v, -1.0e6, 1.0e6)) : 0;
}

} // namespace

walllight::Pixel roomAdd(const std::array<float, 3>& unlitColor, float share) {
    // As the game turns a colour into a byte here: n = 256 x the value (0..1) rounded, then n - n / 256.
    auto byte = [share](float c) {
        const double x = unitClamp(static_cast<double>(r24(static_cast<double>(r24(c * 0.5)) * share)));
        const int n = static_cast<int>(roundHalfEven(256.0 * x));
        return static_cast<std::uint8_t>(n - (n >> 8));
    };
    return {byte(unlitColor[2]), byte(unlitColor[1]), byte(unlitColor[0]), 0};
}

void addRoomLight(FloorImage& image, const floorlight::Grid& grid, const std::function<walllight::Pixel(std::int16_t room)>& add) {
    if (!grid.fits() || !add || image.width != 8 * grid.width || image.height != 8 * grid.height) {
        return;
    }
    const bool ground = grid.level == 0;
    const auto split1 = firstHalfTexels(ground, 1), split2 = firstHalfTexels(ground, 2);
    for (int ty = 0; ty < grid.height; ++ty) {
        for (int tx = 0; tx < grid.width; ++tx) {
            const auto& tile = grid.tiles[static_cast<std::size_t>(ty) * static_cast<std::size_t>(grid.width) + static_cast<std::size_t>(tx)];
            if (!tile.floor) continue;
            // Each half's room: the first quarter cell of the tile that has floor in that half.
            std::array<std::int16_t, 2> room{0, 0};
            for (int h = 0; h < 2; ++h) {
                for (int k = 0; k < 4 && room[static_cast<std::size_t>(h)] == 0; ++k) {
                    const auto* c = grid.at(2 * tx + (k & 1), 2 * ty + (k >> 1));
                    if (c && c->halves[static_cast<std::size_t>(h)].valid) room[static_cast<std::size_t>(h)] = c->halves[static_cast<std::size_t>(h)].room;
                }
            }
            const Pixel first = room[0] > 0 ? add(room[0]) : Pixel{};
            const Pixel second = tile.diagonal != 0 && room[1] > 0 ? add(room[1]) : Pixel{};
            const auto& mask = tile.diagonal == 1 ? split1 : split2;
            for (int j = 0; j < 8; ++j) {
                for (int i = 0; i < 8; ++i) {
                    const bool isFirst = tile.diagonal == 0 || mask[static_cast<std::size_t>(j * 8 + i)];
                    auto& t = image.texels[static_cast<std::size_t>(8 * ty + j) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(8 * tx + i)];
                    t = saturatingAdd(t, isFirst ? first : second);
                }
            }
        }
    }
}

walllight::Pixel outdoorAdd(const Outdoor& outdoor, const std::array<float, 3>& normal, float sun) {
    double weight = 0;
    std::array<double, 3> c{0, 0, 0};
    for (std::size_t i = 0; i < outdoor.lights.size(); ++i) {
        const auto& l = outdoor.lights[i];
        const double facing = static_cast<double>(normal[0]) * l.direction[0] + static_cast<double>(normal[1]) * l.direction[1] +
                              static_cast<double>(normal[2]) * l.direction[2];
        const double w = l.strength * std::max(facing, 0.0) * (i == 0 ? static_cast<double>(sun) : 1.0);
        weight += w;
        for (std::size_t k = 0; k < 3; ++k) c[k] += l.color[k] * w;
    }
    // As the game turns it into a byte: n = 256 x the value (0..1) rounded, then n - n / 256.
    auto byte = [&](std::size_t i) {
        const double x = unitClamp((c[i] + outdoor.skylight[i] * (1.0 - weight)) * 0.5 * outdoor.strength);
        const int n = static_cast<int>(roundHalfEven(256.0 * x));
        return static_cast<std::uint8_t>(n - (n >> 8));
    };
    return {byte(2), byte(1), byte(0), 0};
}

void addOutdoorLight(FloorImage& image, const floorlight::Grid& grid, const Outdoor& outdoor, const OutdoorAt& at) {
    if (!grid.fits() || image.width != 8 * grid.width || image.height != 8 * grid.height) {
        return;
    }
    const Pixel fullSun = outdoorAdd(outdoor, {0, 0, 1});
    // Neighbouring texels mostly share their normal and sun: the last texel's light is reused for them.
    std::optional<OutdoorTexel> last;
    Pixel lastAdd{};
    const bool ground = grid.level == 0;
    const auto split1 = firstHalfTexels(ground, 1), split2 = firstHalfTexels(ground, 2);
    for (int ty = 0; ty < grid.height; ++ty) {
        for (int tx = 0; tx < grid.width; ++tx) {
            const auto& tile = grid.tiles[static_cast<std::size_t>(ty) * static_cast<std::size_t>(grid.width) + static_cast<std::size_t>(tx)];
            if (!tile.floor) continue;
            // Each half is outdoors when a quarter cell of the tile has floor there that is outside.
            std::array<bool, 2> out{false, false};
            for (int h = 0; h < 2; ++h) {
                for (int k = 0; k < 4; ++k) {
                    const auto* c = grid.at(2 * tx + (k & 1), 2 * ty + (k >> 1));
                    const auto& half = c ? c->halves[static_cast<std::size_t>(h)] : floorlight::Half{};
                    out[static_cast<std::size_t>(h)] = out[static_cast<std::size_t>(h)] || (half.valid && half.outside);
                }
            }
            if (!out[0] && !(tile.diagonal != 0 && out[1])) continue;
            const auto& mask = tile.diagonal == 1 ? split1 : split2;
            for (int j = 0; j < 8; ++j) {
                for (int i = 0; i < 8; ++i) {
                    const bool isFirst = tile.diagonal == 0 || mask[static_cast<std::size_t>(j * 8 + i)];
                    if (!(isFirst ? out[0] : out[1])) continue;
                    auto& t = image.texels[static_cast<std::size_t>(8 * ty + j) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(8 * tx + i)];
                    if (!at) {
                        t = saturatingAdd(t, fullSun);
                        continue;
                    }
                    const auto texel = at(tx, ty, i, j);
                    if (!last || !sameBits(texel.normal, last->normal) || !sameBits(texel.sun, last->sun)) {
                        last = texel;
                        lastAdd = outdoorAdd(outdoor, texel.normal, texel.sun);
                    }
                    t = saturatingAdd(t, lastAdd);
                    t.a = texel.alpha;
                }
            }
        }
    }
}

void addRoomLight(std::vector<walllight::Pixel>& panel, const walllight::Pixel& add) {
    for (auto& t : panel) {
        t = saturatingAdd(t, add);
    }
}

Size panelSize(bool inside, int heightUnits) {
    const int n = std::clamp(heightUnits, 1, 4);
    return inside && n == 1 ? Size{4, 8} : Size{8, 8 * n};
}

std::vector<walllight::Pixel> panelTexels(const std::array<walllight::Column, 4>& t, Size size, const std::array<float, 3>& ambient, float scale,
                                          bool attic, float fade) {
    const int w = size.width, h = size.height;
    if (w <= 1 || h <= 0 || w > 64 || h > 256) {
        return {};
    }
    std::vector<Pixel> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    if (w == 4 && h == 8) {
        // A 1:1 copy in the game's 16-bit steps: (texel - ambient) * scale on the packed bytes.
        const Pixel a = pack(ambient);
        const auto k = boundedInt(roundHalfEven(static_cast<double>(scale) * 4096.0));
        auto ch = [k](std::uint8_t c, std::uint8_t amb) { return static_cast<std::uint8_t>(std::clamp(((c - amb) * 16 * k) >> 16, 0, 255)); };
        for (int r = 0; r < 8; ++r) {
            for (int x = 0; x < 4; ++x) {
                const Pixel& c = t[static_cast<std::size_t>(x)][static_cast<std::size_t>(7 - r)];
                out[static_cast<std::size_t>(r * 4 + x)] = {ch(c.b, a.b), ch(c.g, a.g), ch(c.r, a.r), 255};
            }
        }
    } else if (3 * w == h) {
        sample(out, t, w, h, r24(6.0 / (h - 1)), ambient, scale);
    } else if (h % w == 0) {
        // Shorter rects take the bottom of the 3:1 sampling. Taller ones fade upward from it.
        const int rows = std::min(h, 3 * w);
        sample(out, t, w, rows, r24(6.0 / (3 * w - 1)), ambient, scale);
        const auto kF = boundedInt(roundHalfEven(65536.0 * r24(0.0625 / (static_cast<double>(fade) * 0.4 + 1.0))));
        auto ch = [kF](std::uint8_t c) { return static_cast<std::uint8_t>(std::clamp((c * 16 * kF) >> 16, 0, 255)); };
        for (int r = rows; r < h; ++r) {
            for (int x = 0; x < w; ++x) {
                const Pixel& prev = out[static_cast<std::size_t>((r - 1) * w + x)];
                out[static_cast<std::size_t>(r * w + x)] = attic ? Pixel{0, 0, 0, 0} : Pixel{ch(prev.b), ch(prev.g), ch(prev.r), 255};
            }
        }
    }
    return out;
}

} // namespace ts2::scene::lightmaptexels
