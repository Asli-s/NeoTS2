#include "scene/lot_parts.hpp"

#include <algorithm>
#include <cmath>

// Roofs as the game builds them: heights on a grid of points half a tile apart, covering the roof's
// rectangle and its overhang; three layers of that grid make a slab (the shingled top, the edge
// board's two bands and the underside). Each grid cell is split into two triangles, its diagonal
// alternating like a checkerboard, and each top triangle shows one cell of the roof texture's
// 5 x 5 grid of pieces, chosen from what its corners touch: the ridge (the roof's highest points)
// and other roofs meeting it at the same height.

namespace ts2::scene {

namespace {

constexpr float kOverhang = 0.5f;
constexpr float kStep = 0.5f;                                      // grid spacing, in tiles
constexpr std::array<float, 3> kLayerDrop{0.42f, 0.5075f, 0.67f};  // below the eave line, per layer
constexpr float kSlope = 0.1f;                                     // facing threshold on a cell's slope

// The texture is addressed through a table of 10 x 10 points: u and v each run over the cell edges
// 0, .2, .2, .4, ... 1 (two per edge), entry 10 v + u.
std::array<float, 2> texturePoint(int entry) {
    static constexpr std::array<float, 10> edge{0, .2f, .2f, .4f, .4f, .6f, .6f, .8f, .8f, 1};
    return {edge[static_cast<std::size_t>(entry % 10)], edge[static_cast<std::size_t>(entry / 10)]};
}

// One top triangle's corner flags.
constexpr int kRidge = 1, kJunction = 4;

// The four triangle kinds: corners (row, column offsets in the cell) in emitted order, the corner
// pair one test looks at, the texture corners used by facing (x down, x up, y down, y up), and
// whether a flat triangle takes its cell's piece (otherwise the plain one).
struct Kind {
    std::array<std::array<int, 2>, 3> corners;
    std::array<int, 2> pair;
    std::array<std::array<int, 3>, 4> texture;
    bool flatTakesCell;
    std::array<std::array<int, 3>, 1> under;
};

const std::array<Kind, 4> kKinds{{
    // Even cells, split along (0,0)-(1,1).
    {{{{0, 0}, {1, 1}, {1, 0}}}, {0, 1}, {{{11, 0, 10}, {0, 11, 1}, {10, 1, 0}, {1, 10, 11}}}, false, {{{0, 9, 90}}}},
    {{{{0, 0}, {0, 1}, {1, 1}}}, {0, 2}, {{{11, 1, 0}, {0, 10, 11}, {10, 11, 1}, {1, 0, 10}}}, false, {{{9, 99, 90}}}},
    // Odd cells, split along (0,1)-(1,0).
    {{{{0, 0}, {0, 1}, {1, 0}}}, {1, 2}, {{{11, 1, 10}, {0, 10, 1}, {10, 11, 0}, {1, 0, 11}}}, true, {{{0, 99, 90}}}},
    {{{{0, 1}, {1, 1}, {1, 0}}}, {0, 2}, {{{1, 0, 10}, {10, 11, 1}, {11, 1, 0}, {0, 10, 11}}}, true, {{{0, 9, 99}}}},
}};

// What a triangle's facing handler may change: its piece and whether the piece is mirrored.
struct Piece {
    int cell = 60;
    bool mirror = false;
};

// The facing handlers come in two forms. The first looks at one corner (and the junction pair);
// the second at one corner when one corner meets another roof, at a corner pair when two do.
// ridges: corners on the ridge; joins: corners meeting another roof; pair: the kind's pair both
// meet one; other: how many corners of the cell's other triangle meet one.
void pairHandler(Piece& p, int corner, bool singleMirror, bool pairMirror, int ridges, int joins, bool pair, int other,
                 const std::array<int, 3>& flags) {
    const bool joined = (flags[static_cast<std::size_t>(corner)] & kJunction) != 0;
    if (joins == 2 ? !pair : false) {
        return;
    }
    if (joins != 2 && !pair) {
        if (other == 2 && joined) {
            p.mirror = false;
            p.cell = ridges == 2 ? 6 : 0x2e;
            return;
        }
        if (!joined) {
            return;
        }
        p.mirror = singleMirror;
        p.cell = ridges == 2 ? 0x1a : 0x14;
        return;
    }
    p.mirror = pairMirror;
    p.cell = ridges == 2 ? 0x2a : 0x16;
}

void cornerHandler(Piece& p, int single, int first, int second, bool singleMirror, int ridges, int joins, int other,
                   const std::array<int, 3>& flags) {
    auto joined = [&](int k) { return (flags[static_cast<std::size_t>(k)] & kJunction) != 0; };
    if (joins == 1) {
        if (other == 1 && joined(single)) {
            p.cell = ridges == 1 ? 0x1a : 0x14;
            p.mirror = singleMirror;
        }
    } else if (joins == 2 && joined(first) && joined(second)) {
        p.cell = ridges == 1 ? 6 : 0x2e;
        p.mirror = false;
    }
}

// The handlers of each kind, by facing: x down, x up, y down, y up.
void handle(int kind, int facing, Piece& p, int ridges, int joins, bool pair, int other, const std::array<int, 3>& f) {
    // Per kind, per facing: {form, corner args...}; form 0 = pair handler (corner, single mirror,
    // pair mirror), form 1 = corner handler (single, first, second, single mirror).
    struct H {
        int form, a, b, c, d;
    };
    static constexpr H table[4][4] = {
        {{1, 0, 0, 2, 1}, {0, 1, 1, 0, 0}, {0, 0, 0, 1, 0}, {1, 1, 1, 2, 0}},  // e18: ab20, aa90, ac00, ab90
        {{0, 0, 1, 0, 0}, {1, 2, 1, 2, 1}, {1, 0, 0, 1, 0}, {0, 2, 0, 1, 0}},  // e78: b670, aed0, b560, b5d0
        {{1, 2, 0, 2, 0}, {0, 1, 0, 1, 0}, {1, 1, 0, 1, 1}, {0, 2, 1, 0, 0}},  // ed8: b1e0, b140, b2e0, b250
        {{0, 2, 0, 1, 0}, {1, 0, 0, 1, 0}, {0, 0, 1, 0, 0}, {1, 2, 1, 2, 1}},  // f38: b5d0, b560, b670, aed0
    };
    const H& h = table[kind][facing];
    if (h.form == 0) {
        pairHandler(p, h.a, h.b != 0, h.c != 0, ridges, joins, pair, other, f);
    } else {
        cornerHandler(p, h.a, h.b, h.c, h.d != 0, ridges, joins, other, f);
    }
}

// Which way a cell's triangle faces, from its slope (the downhill direction in grid steps):
// 0 x down, 1 x up, 2 y down, 3 y up, -1 flat.
int facing(const std::array<float, 2>& g) {
    if (g[0] < -kSlope) {
        return 0;
    }
    if (g[0] > kSlope) {
        return 1;
    }
    if (g[1] < -kSlope) {
        return 2;
    }
    if (g[1] > kSlope) {
        return 3;
    }
    return -1;
}

}  // namespace

std::optional<RoofGrid> RoofGrid::of(const roofs::Roof& roof, float eave) {
    if (roof.type != 4) {
        return std::nullopt;  // only gables are built this way yet
    }
    RoofGrid g;
    g.type = static_cast<int>(roof.type);
    const float lx = std::min(roof.x0, roof.x1), hx = std::max(roof.x0, roof.x1);
    const float ly = std::min(roof.y0, roof.y1), hy = std::max(roof.y0, roof.y1);
    g.minX = lx - kOverhang;
    g.minY = ly - kOverhang;
    g.nx = static_cast<int>(std::lround(2 * (hx - lx + 2 * kOverhang))) + 1;
    g.ny = static_cast<int>(std::lround(2 * (hy - ly + 2 * kOverhang))) + 1;
    // The saved slope is half the tangent of the roof's angle: the rise per grid step.
    g.scale = roof.slope > 0 ? roof.slope : 0.5f;
    g.eave = eave;
    g.ridgeAlongX = std::abs(roof.y1 - roof.y0) <= std::abs(roof.x1 - roof.x0);
    g.steps.resize(static_cast<std::size_t>(g.nx * g.ny));
    for (int r = 0; r < g.ny; ++r) {
        for (int c = 0; c < g.nx; ++c) {
            const int s = g.ridgeAlongX ? std::min(r, g.ny - 1 - r) : std::min(c, g.nx - 1 - c);
            g.steps[static_cast<std::size_t>(r * g.nx + c)] = static_cast<float>(s);
            g.peak = std::max(g.peak, static_cast<float>(s));
        }
    }
    return g;
}

float RoofGrid::z(int row, int col, int layer) const {
    const float raise = scale < 0.499f ? 0.5f - scale : 0.0f;
    return scale * steps[static_cast<std::size_t>(row * nx + col)] + raise + eave - kLayerDrop[static_cast<std::size_t>(layer)];
}

float RoofGrid::surface(float x, float y) const {
    const float c = (x - minX) / kStep, r = (y - minY) / kStep;
    const int ci = std::clamp(static_cast<int>(std::floor(c)), 0, nx - 2), ri = std::clamp(static_cast<int>(std::floor(r)), 0, ny - 2);
    const float fc = c - static_cast<float>(ci), fr = r - static_cast<float>(ri);
    auto h = [&](int dr, int dc) { return z(ri + dr, ci + dc, 0); };
    if ((ri + ci) % 2 == 0) {  // split along (0,0)-(1,1)
        if (fr >= fc) {
            return h(0, 0) + (h(1, 0) - h(0, 0)) * fr + (h(1, 1) - h(1, 0)) * fc;
        }
        return h(0, 0) + (h(0, 1) - h(0, 0)) * fc + (h(1, 1) - h(0, 1)) * fr;
    }
    if (fr + fc <= 1) {
        return h(0, 0) + (h(0, 1) - h(0, 0)) * fc + (h(1, 0) - h(0, 0)) * fr;
    }
    return h(1, 1) + (h(1, 0) - h(1, 1)) * (1 - fc) + (h(0, 1) - h(1, 1)) * (1 - fr);
}

std::optional<float> RoofGrid::pointHeight(float x, float y) const {
    const float c = (x - minX) / kStep, r = (y - minY) / kStep;
    if (c < 0 || r < 0 || c > static_cast<float>(nx - 1) || r > static_cast<float>(ny - 1)) {
        return std::nullopt;
    }
    const float ci = std::round(c), ri = std::round(r);
    if (std::abs(c - ci) > 1e-3f || std::abs(r - ri) > 1e-3f) {
        return std::nullopt;
    }
    return z(static_cast<int>(ri), static_cast<int>(ci), 0);
}

std::optional<float> RoofGrid::coveringHeight(float x, float y) const {
    const float lo = kOverhang;
    if (!(minX + lo < x && x <= minX + static_cast<float>(nx - 1) * kStep - lo && minY + lo < y &&
          y <= minY + static_cast<float>(ny - 1) * kStep - lo)) {
        return std::nullopt;
    }
    return surface(x, y);
}

void addRoofGrid(Surfaces& surfaces, const RoofGrid& roof, const std::vector<const RoofGrid*>& others, const RoofLook& look) {
    const int nx = roof.nx, ny = roof.ny;
    auto at = [&](int r, int c) { return std::array<float, 2>{roof.minX + static_cast<float>(c) * kStep, roof.minY + static_cast<float>(r) * kStep}; };
    auto position = [&](int r, int c, int layer) {
        const auto p = at(r, c);
        return std::array<float, 3>{p[0], p[1], roof.z(r, c, layer)};
    };
    // Another roof covers a point when its surface is above this one's there.
    auto covered = [&](float x, float y) {
        const float own = roof.surface(x, y);
        for (const auto* o : others) {
            if (const auto h = o->coveringHeight(x, y); h && *h > own + 1e-4f) {
                return true;
            }
        }
        return false;
    };
    auto cornerFlags = [&](int r, int c) {
        int f = 0;
        const float s = roof.steps[static_cast<std::size_t>(r * nx + c)];
        if (s > 0 && s == roof.peak) {
            f |= kRidge;
        }
        const auto p = at(r, c);
        const float own = roof.z(r, c, 0);
        for (const auto* o : others) {
            if (const auto h = o->pointHeight(p[0], p[1]); h && std::abs(*h - own) < 1e-4f) {
                f |= kJunction;
            }
        }
        return f;
    };
    auto triangle = [&](Primitive& prim, const std::array<std::array<float, 3>, 3>& v, const std::array<std::array<float, 2>, 3>& uv) {
        const std::array<float, 3> a{v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2]};
        const std::array<float, 3> b{v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2]};
        std::array<float, 3> n{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 0) {
            n = {n[0] / len, n[1] / len, n[2] / len};
        }
        for (std::size_t k = 0; k < 3; ++k) {
            addVertex(prim, v[k], n, uv[k]);
        }
    };

    for (int r = 0; r + 1 < ny; ++r) {
        for (int c = 0; c + 1 < nx; ++c) {
            const int first = (r + c) % 2 == 0 ? 0 : 2;
            struct Tri {
                int kind;
                std::array<int, 3> flags;
                int joins;
                bool keep;
            };
            std::array<Tri, 2> tris{};
            for (int t = 0; t < 2; ++t) {
                const Kind& k = kKinds[static_cast<std::size_t>(first + t)];
                Tri& tri = tris[static_cast<std::size_t>(t)];
                tri.kind = first + t;
                float cx = 0, cy = 0;
                for (std::size_t i = 0; i < 3; ++i) {
                    const int rr = r + k.corners[i][0], cc = c + k.corners[i][1];
                    tri.flags[i] = cornerFlags(rr, cc);
                    tri.joins += (tri.flags[i] & kJunction) ? 1 : 0;
                    const auto p = at(rr, cc);
                    cx += p[0] / 3;
                    cy += p[1] / 3;
                }
                tri.keep = !covered(cx, cy);
            }
            for (int t = 0; t < 2; ++t) {
                const Tri& tri = tris[static_cast<std::size_t>(t)];
                if (!tri.keep) {
                    continue;
                }
                const Kind& k = kKinds[static_cast<std::size_t>(tri.kind)];
                // The cell's slope (downhill, in grid steps) as the kind measures it.
                auto s = [&](int dr, int dc) { return roof.steps[static_cast<std::size_t>((r + dr) * nx + c + dc)]; };
                std::array<float, 2> g{};
                switch (tri.kind) {
                case 0: g = {(s(1, 0) - s(0, 0)) - (s(1, 1) - s(0, 0)), -(s(1, 0) - s(0, 0))}; break;
                case 1: g = {-(s(0, 1) - s(0, 0)), (s(0, 1) - s(0, 0)) - (s(1, 1) - s(0, 0))}; break;
                case 2: g = {-(s(0, 1) - s(0, 0)), -(s(1, 0) - s(0, 0))}; break;
                default: g = {-(s(1, 1) - s(1, 0)), (s(0, 1) - s(1, 0)) - (s(1, 1) - s(1, 0))}; break;
                }
                int ridges = 0;
                for (const int f : tri.flags) {
                    ridges += (f & kRidge) ? 1 : 0;
                }
                const bool pair = (tri.flags[static_cast<std::size_t>(k.pair[0])] & kJunction) &&
                                  (tri.flags[static_cast<std::size_t>(k.pair[1])] & kJunction);
                Piece piece;
                piece.cell = ridges ? 4 : 60;
                const int face = facing(g);
                if (tri.joins >= 1 && tri.joins <= 2 && face >= 0) {
                    handle(tri.kind, face, piece, ridges, tri.joins, pair, tris[static_cast<std::size_t>(1 - t)].joins, tri.flags);
                }
                std::array<int, 3> entries{};
                if (face < 0) {
                    const int e = k.flatTakesCell ? piece.cell : 60;
                    entries = {e, e, e};
                } else {
                    for (std::size_t i = 0; i < 3; ++i) {
                        int corner = k.texture[static_cast<std::size_t>(face)][i];
                        if (piece.mirror) {
                            corner = corner == 0 ? 1 : corner == 1 ? 0 : corner == 10 ? 11 : 10;
                        }
                        entries[i] = piece.cell + corner;
                    }
                }
                std::array<std::array<float, 3>, 3> top{}, under{};
                std::array<std::array<float, 2>, 3> uv{}, underUv{};
                for (std::size_t i = 0; i < 3; ++i) {
                    top[i] = position(r + k.corners[i][0], c + k.corners[i][1], 0);
                    under[2 - i] = position(r + k.corners[i][0], c + k.corners[i][1], 2);
                    uv[i] = texturePoint(entries[i]);
                    underUv[i] = texturePoint(k.under[0][i]);  // in emitted (reversed) order
                }
                if (look.top >= 0) {
                    triangle(surfaces[look.top], top, uv);
                }
                if (look.under >= 0) {
                    triangle(surfaces[look.under], under, underUv);
                }
            }
        }
    }

    // The edge board: a quad per grid step along the four sides, between layers 0 and 1 (the edges
    // material) and 1 and 2 (the trim), unless another roof covers the step's middle. Its texture is
    // the top row of the material's texture.
    auto strip = [&](int r0, int c0, int r1, int c1, const std::array<float, 3>& out) {
        const auto a = at(r0, c0), b = at(r1, c1);
        if (covered((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)) {
            return;
        }
        for (const auto& [upper, material] : {std::pair{0, look.edges}, std::pair{1, look.trim}}) {
            if (material < 0) {
                continue;
            }
            addQuad(surfaces[material], {{position(r0, c0, upper), position(r1, c1, upper), position(r1, c1, upper + 1), position(r0, c0, upper + 1)}},
                    out, {{texturePoint(0), texturePoint(1), texturePoint(9), texturePoint(8)}});
        }
    };
    for (int r = 0; r + 1 < ny; ++r) {
        strip(r, 0, r + 1, 0, {-1, 0, 0});
        strip(r, nx - 1, r + 1, nx - 1, {1, 0, 0});
    }
    for (int c = 0; c + 1 < nx; ++c) {
        strip(0, c, 0, c + 1, {0, -1, 0});
        strip(ny - 1, c, ny - 1, c + 1, {0, 1, 0});
    }
}

} // namespace ts2::scene
