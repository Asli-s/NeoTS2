#include "scene/lot_parts.hpp"

#include <algorithm>
#include <cmath>

// Roofs as the game builds them: heights on a grid of points half a tile apart, covering the roof's
// rectangle and its overhang; three layers of that grid make a slab (the shingled top, the edge
// board's two bands and the underside). Each grid cell is split into two triangles, its diagonal
// alternating like a checkerboard, and each top triangle shows one cell of the roof texture's
// 5 x 5 grid of pieces, chosen from what its corners touch: the ridge (the roof's highest points),
// the hips (the diagonals rising from a hipped roof's corners) and other roofs meeting it at the
// same height. Gables, hips and hipped sheds differ only in their heights and where the hips run.

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

// Triangles with corners on a hip have their own pieces. These are the game's rules as they stand,
// per kind; flags as above (kHip marks a corner on a hip), the rest as for handle().
constexpr int kHip = 2;

void oneOnHip(int kind, const std::array<float, 2>& g, Piece& p, int ridges, int joins, bool pair, int other,
              const std::array<int, 3>& flags) {
    auto j = [&](int k) { return (flags[static_cast<std::size_t>(k)] & kJunction) != 0; };
    auto d = [&](int k) { return (flags[static_cast<std::size_t>(k)] & kHip) != 0; };
    const bool xd = g[0] < -kSlope, xu = g[0] > kSlope, yd = g[1] < -kSlope, yu = g[1] > kSlope;
    const int a = ridges ? 0x56 : 0x18, b = ridges ? 0x40 : 0x3e, c = ridges ? 0x2a : 0x28;
    p.cell = ridges ? 0x42 : 2;
    auto set = [&](int cell, int mirror) {
        if (cell >= 0) {
            p.cell = cell;
        }
        if (mirror >= 0) {
            p.mirror = mirror != 0;
        }
    };
    switch (kind) {
    case 0:
        if (yd) {
            if (!d(1)) return set(-1, 0);
            if (pair) return set(c, 1);
            if (joins == 1 && other == 2 && j(0)) return set(a, 1);
            if (joins == 1 && other == 1 && j(0)) return set(b, 1);
            return set(-1, 1);
        }
        if (xd) {
            if (joins == 2) return set(j(0) && j(2) ? a : -1, 0);
            if (joins != 1 || other != 1) return set(-1, 0);
            return set(j(0) ? b : -1, 0);
        }
        if (xu) {
            if (!d(0)) return set(-1, 1);
            if (pair) return set(c, 0);
            if (joins != 1) return set(-1, 0);
            if (j(1) && other == 2) return set(a, 0);
            if (other != 1) return set(-1, 0);
            return set(j(1) ? b : -1, 0);
        }
        if (yu) {
            if (joins == 2 && j(1) && j(2)) return set(a, 1);
            if (joins == 1 && other == 1 && j(1)) set(b, -1);
            return set(-1, 1);
        }
        return;
    case 1:
        if (yd) {
            if (joins == 2 && j(0) && j(1)) return set(a, 1);
            if (joins == 1 && other == 1 && j(0)) set(b, -1);
            return set(-1, 1);
        }
        if (xd) {
            if (pair) return set(c, 0);
            if (joins != 1) return set(-1, 0);
            if (j(0) && other == 2) return set(a, 0);
            if (other == 1 && j(0)) return set(b, 0);
            return set(-1, 0);
        }
        if (xu) {
            if (joins != 2) return set(joins == 1 && other == 1 && j(2) ? b : -1, 0);
            return set(j(1) && j(2) ? a : -1, 0);
        }
        if (!yu) return;
        if (d(0)) {
            if (pair) return set(c, 1);
            if (joins == 1 && other == 2 && j(2)) return set(a, 1);
            if (joins == 1 && other == 1 && j(2)) set(b, -1);
            return set(-1, 1);
        }
        return set(-1, 0);
    case 2:
        if (yd) {
            if (joins == 2) return set(j(0) && j(1) ? a : -1, 0);
            if (joins != 1 || other != 1) return set(-1, 0);
            return set(j(1) ? b : -1, 0);
        }
        if (xd) {
            if (joins == 2 && j(0) && j(2)) return set(a, 1);
            if (joins == 1 && other == 1 && j(2)) set(b, -1);
            return set(-1, 1);
        }
        if (xu) {
            if (!d(2)) {
                if (joins == 1 && other == 1 && (j(1) || j(2))) return set(b, -1);  // keeps its mirroring
                return set(-1, 0);
            }
            if (pair) return set(c, 1);
            if (joins == 1 && other == 2 && j(1)) return set(a, 1);
            if (joins == 1 && other == 1 && j(1)) return set(b, 1);
            return set(-1, 1);
        }
        if (!yu) return;
        if (!d(1)) return set(-1, 1);
        if (pair) return set(c, 0);
        if (joins != 1) return set(-1, 0);
        if (j(2) && other == 2) return set(a, 0);
        if (other != 1) return set(-1, 0);
        return set(j(2) ? b : -1, 0);
    default:
        if (yd) {
            if (pair) return set(c, 0);
            if (joins == 1 && j(0) && other == 2) return set(a, 0);
            if (joins == 1 && other == 1 && j(0)) return set(b, 0);
            return set(-1, 0);
        }
        if (xd) {
            if (!d(0)) return set(-1, 0);
            if (pair) return set(c, 1);
            if (joins == 1 && other == 2 && j(2)) return set(a, 1);
            if (joins == 1 && other == 1 && j(2)) set(b, -1);
            return set(-1, 1);
        }
        if (xu) {
            if (joins == 2 && j(0) && j(1)) return set(a, 1);
            if (joins == 1 && other == 1 && j(0)) set(b, -1);
            return set(-1, 1);
        }
        if (!yu) return;
        if (joins == 2 && j(1) && j(2)) return set(a, 0);
        if (joins == 1 && other == 1 && j(2)) set(b, -1);
        return set(-1, 0);
    }
}

void twoOnHip(int kind, const std::array<float, 2>& g, Piece& p, int joins, const std::array<int, 3>& flags) {
    auto j = [&](int k) { return (flags[static_cast<std::size_t>(k)] & kJunction) != 0; };
    const bool xd = g[0] < -kSlope, xu = g[0] > kSlope, yd = g[1] < -kSlope, yu = g[1] > kSlope;
    p.cell = 0;
    auto set = [&](int cell, bool mirror) {
        p.cell = cell;
        p.mirror = mirror;
    };
    switch (kind) {
    case 0:
        if (xd) {
            if (joins == 1 && j(0)) return set(8, false);
            if (joins == 2 && j(0) && j(2)) return set(0x2c, true);
            return set(0x52, false);
        } else if (yu) {
            if (joins == 2 && j(1) && j(2)) set(0x2c, false);
            else if (joins == 1 && j(1)) set(8, true);
        }
        return;
    case 1:
        if (yd) {
            if (joins == 2 && j(0) && j(1)) set(0x2c, false);
            else if (joins == 1 && j(0)) set(8, true);
        } else if (xu) {
            if (joins == 1 && j(2)) return set(8, false);
            if (joins == 2 && j(1) && j(2)) return set(0x2c, true);
            set(0x52, false);
        }
        return;
    case 2:
        if (yd) {
            if (joins == 1 && j(1)) return set(8, false);
            if (joins == 2 && j(0) && j(1)) return set(0x2c, true);
            return set(0x52, false);
        }
        if (xd) {
            if (joins == 2 && j(0) && j(2)) set(0x2c, false);
            else if (joins == 1 && j(2)) set(8, true);
        }
        return;
    default:
        if (xu) {
            if (joins == 2 && j(0) && j(1)) set(0x2c, false);
            else if (joins == 1 && j(0)) set(8, true);
        } else if (yu) {
            if (joins == 1 && j(2)) return set(8, false);
            if (joins == 2 && j(1) && j(2)) return set(0x2c, true);
            set(0x52, false);
        }
        return;
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
    RoofGrid g;
    switch (roof.type) {
    case 2: g.kind = Kind::shed; break;
    case 4: g.kind = Kind::gable; break;
    case 5: g.kind = Kind::hip; break;
    default: return std::nullopt;  // not built this way yet
    }
    g.type = static_cast<int>(roof.type);
    g.level = roof.level0;
    const float lx = std::min(roof.x0, roof.x1), hx = std::max(roof.x0, roof.x1);
    const float ly = std::min(roof.y0, roof.y1), hy = std::max(roof.y0, roof.y1);
    const float dx = std::abs(roof.x1 - roof.x0), dy = std::abs(roof.y1 - roof.y0);
    g.ridgeAlongX = dy <= dx;
    g.lengthwise = g.kind == Kind::shed ? dx < dy : g.ridgeAlongX;
    if (g.kind == Kind::shed) {
        // High along the second corner's side, across the roof's length.
        g.high = g.lengthwise ? (roof.x1 >= roof.x0 ? 1 : 0) : (roof.y1 >= roof.y0 ? 3 : 2);
    }
    // The grid covers the rectangle and its overhang, except on a shed's high side.
    const float x0 = lx - (g.high == 0 ? 0 : kOverhang), x1 = hx + (g.high == 1 ? 0 : kOverhang);
    const float y0 = ly - (g.high == 2 ? 0 : kOverhang), y1 = hy + (g.high == 3 ? 0 : kOverhang);
    g.minX = x0;
    g.minY = y0;
    g.nx = static_cast<int>(std::lround(2 * (x1 - x0))) + 1;
    g.ny = static_cast<int>(std::lround(2 * (y1 - y0))) + 1;
    // The saved slope is half the tangent of the roof's angle: the rise per grid step.
    g.scale = roof.slope > 0 ? roof.slope : 0.5f;
    g.eave = eave;
    g.steps.resize(static_cast<std::size_t>(g.nx * g.ny));
    for (int r = 0; r < g.ny; ++r) {
        for (int c = 0; c < g.nx; ++c) {
            // Steps up from the sloped edges: the nearer of the two along the slope for a gable,
            // the nearest of all four for a hip, of all but the high side for a shed.
            const int fromX = std::min(g.high == 0 ? g.nx : c, g.high == 1 ? g.nx : g.nx - 1 - c);
            const int fromY = std::min(g.high == 2 ? g.ny : r, g.high == 3 ? g.ny : g.ny - 1 - r);
            const int s = g.kind == Kind::gable ? (g.ridgeAlongX ? fromY : fromX) : std::min(fromX, fromY);
            g.steps[static_cast<std::size_t>(r * g.nx + c)] = static_cast<float>(s);
            g.peak = std::max(g.peak, static_cast<float>(s));
        }
    }
    return g;
}

int RoofGrid::parity(int row, int col) const {
    return (row + col + static_cast<int>(std::lround(2 * minX)) + static_cast<int>(std::lround(2 * minY))) & 1;
}

int RoofGrid::ridgeMask(int row, int col) const {
    const float s = steps[static_cast<std::size_t>(row * nx + col)];
    if (!(s > 0 && s == peak)) {
        return 0;
    }
    return lengthwise ? 12 : 3;
}

namespace {

// Whether a point of a hip roof's grid (w x h points) lies on a hip, as the four faces it parts:
// from the corner at the low x, low y end 10, high x 9, high y 6, both high 5.
int hipFrom(int i, int j, int w, int h) {
    const int m = std::min(w >> 1, h >> 1);
    if (m < i) {
        if (j <= m) {
            return j == w - i - 1 ? 9 : 0;
        }
        return w - i - 1 == h - j - 1 ? 5 : 0;
    }
    if (m < j) {
        return i == h - j - 1 ? 6 : 0;
    }
    return i == j ? 10 : 0;
}

}  // namespace

int RoofGrid::hipMask(int row, int col) const {
    switch (kind) {
    case Kind::gable: return 0;
    case Kind::hip: return hipFrom(col, row, nx, ny);
    case Kind::shed:
        // A shed is the half of a hip twice as deep that lies on its high side's side.
        if (high <= 1) {
            return hipFrom(col + (high == 0 ? nx - 1 : 0), row, 2 * nx - 1, ny);
        }
        return hipFrom(col, row + (high == 2 ? ny - 1 : 0), nx, 2 * ny - 1);
    }
    return 0;
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
    if (parity(ri, ci) == 0) {  // split along (0,0)-(1,1)
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

std::optional<float> RoofGrid::coveringHeight(float x, float y, int fromLevel) const {
    // Over a roof on a lower level its overhang does not count; over one on its own level it does.
    const float lo = fromLevel < level ? kOverhang : 0.0f;
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
            if (const auto h = o->coveringHeight(x, y, roof.level); h && *h > own + 1e-4f) {
                return true;
            }
        }
        return false;
    };
    // A triangle's corner flags: on the ridge, on a hip, and joining another roof. Another roof joins
    // at a corner where it is at the same height there, unless that point is on its ridge or a hip
    // bordering the way the triangle faces; such a roof joins none of the triangle's corners. When
    // all three corners meet other roofs, those meeting all three do not join.
    auto cornerFlags = [&](const std::array<std::array<int, 2>, 3>& pts) {
        std::array<std::array<float, 3>, 3> w{};
        for (std::size_t i = 0; i < 3; ++i) {
            const auto p = at(pts[i][0], pts[i][1]);
            w[i] = {p[0], p[1], roof.z(pts[i][0], pts[i][1], 0)};
        }
        const float ax = w[1][0] - w[0][0], ay = w[1][1] - w[0][1], az = w[1][2] - w[0][2];
        const float bx = w[2][0] - w[0][0], by = w[2][1] - w[0][1], bz = w[2][2] - w[0][2];
        const float det = ax * by - bx * ay;
        const float gx = (az * by - bz * ay) / det, gy = (ax * bz - bx * az) / det;  // uphill
        const int face = std::abs(gx) >= std::abs(gy) ? (gx > 0 ? 2 : 1) : (gy > 0 ? 8 : 4);
        struct Touch {
            const RoofGrid* roof;
            int masks;  // its ridge and hip masks at the point
        };
        std::array<std::vector<Touch>, 3> touches;
        std::array<int, 3> flags{};
        int touched = 0;
        for (std::size_t i = 0; i < 3; ++i) {
            const int r = pts[i][0], c = pts[i][1];
            flags[i] = (roof.ridgeMask(r, c) ? kRidge : 0) | (roof.hipMask(r, c) ? kHip : 0);
            for (const auto* o : others) {
                const auto h = o->pointHeight(w[i][0], w[i][1]);
                if (h && std::abs(*h - w[i][2]) < 1e-4f) {
                    const int oc = static_cast<int>(std::lround((w[i][0] - o->minX) / kStep));
                    const int orow = static_cast<int>(std::lround((w[i][1] - o->minY) / kStep));
                    touches[i].push_back({o, o->ridgeMask(orow, oc) | o->hipMask(orow, oc)});
                }
            }
            touched += touches[i].empty() ? 0 : 1;
        }
        auto touchesAll = [&](const RoofGrid* o) {
            return std::all_of(touches.begin(), touches.end(), [&](const auto& t) {
                return std::any_of(t.begin(), t.end(), [&](const Touch& x) { return x.roof == o; });
            });
        };
        auto excluded = [&](const RoofGrid* o) {
            if (touched == 3) {
                return touchesAll(o);
            }
            return std::any_of(touches.begin(), touches.end(), [&](const auto& t) {
                return std::any_of(t.begin(), t.end(), [&](const Touch& x) { return x.roof == o && (x.masks & face); });
            });
        };
        for (std::size_t i = 0; i < 3; ++i) {
            for (const Touch& t : touches[i]) {
                if (!(t.masks & face) && !excluded(t.roof)) {
                    flags[i] |= kJunction;
                }
            }
        }
        return flags;
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
            const int first = roof.parity(r, c) == 0 ? 0 : 2;
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
                std::array<std::array<int, 2>, 3> pts{};
                float cx = 0, cy = 0;
                for (std::size_t i = 0; i < 3; ++i) {
                    pts[i] = {r + k.corners[i][0], c + k.corners[i][1]};
                    const auto p = at(pts[i][0], pts[i][1]);
                    cx += p[0] / 3;
                    cy += p[1] / 3;
                }
                tri.flags = cornerFlags(pts);
                for (const int f : tri.flags) {
                    tri.joins += (f & kJunction) ? 1 : 0;
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
                int ridges = 0, hips = 0;
                for (const int f : tri.flags) {
                    ridges += (f & kRidge) ? 1 : 0;
                    hips += (f & kHip) ? 1 : 0;
                }
                const bool pair = (tri.flags[static_cast<std::size_t>(k.pair[0])] & kJunction) &&
                                  (tri.flags[static_cast<std::size_t>(k.pair[1])] & kJunction);
                const int other = tris[static_cast<std::size_t>(1 - t)].joins;
                Piece piece;
                const int face = facing(g);
                if (hips == 2) {
                    twoOnHip(tri.kind, g, piece, tri.joins, tri.flags);
                } else if (hips == 1) {
                    oneOnHip(tri.kind, g, piece, ridges, tri.joins, pair, other, tri.flags);
                } else {
                    piece.cell = ridges ? 4 : 60;
                    if (tri.joins >= 1 && tri.joins <= 2 && face >= 0) {
                        handle(tri.kind, face, piece, ridges, tri.joins, pair, other, tri.flags);
                    }
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
