#include "scene/light_grid.hpp"

#include "content/lot.hpp"
#include "diag/log.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>

namespace ts2::scene::lightgrid {

namespace {

using floorlight::Cell;
using floorlight::Grid;
using floorlight::Half;
using floorlight::kDirX;
using floorlight::kDirY;

constexpr int kMaxTiles = 256;
constexpr int kMaxLevels = 64;

std::uint8_t bit(int d) {
    return static_cast<std::uint8_t>(1u << d);
}

// The four cells of a tile, as the game names them: 0 its (-x, -y) quarter, 1 (+x, -y),
// 2 (-x, +y), 3 (+x, +y).
std::array<Cell*, 4> quarters(Grid& g, int x, int y) {
    return {g.at(2 * x, 2 * y), g.at(2 * x + 1, 2 * y), g.at(2 * x, 2 * y + 1), g.at(2 * x + 1, 2 * y + 1)};
}

// A wall along an edge closes the links of the two cells beside it that lead through it.
void closeEdge(Cell& a, Cell& b, std::uint8_t keep) {
    a.halves[0].links &= keep;
    b.halves[0].links &= keep;
}

// A diagonal wall splits each quarter into two halves: the second starts as a copy of the first,
// then the links crossing the wall are closed on each side.
void splitDiagonal(const std::array<Cell*, 4>& q, std::uint8_t side) {
    for (Cell* c : q) {
        c->halves[1] = c->halves[0];
    }
    if (side == kDiagonalXEqualsY) {
        q[0]->halves[0].links &= 0xF3;
        q[2]->halves[0].links &= 0xC1;
        q[3]->halves[0].links &= 0xE7;
        q[0]->halves[1].links &= 0x7E;
        q[1]->halves[1].links &= 0x1C;
        q[3]->halves[1].links &= 0x3F;
    } else {
        q[2]->halves[0].links &= 0xCF;
        q[3]->halves[0].links &= 0x07;
        q[1]->halves[0].links &= 0x9F;
        q[2]->halves[1].links &= 0xF9;
        q[0]->halves[1].links &= 0x70;
        q[1]->halves[1].links &= 0xFC;
    }
}

void setRoom(const std::array<Cell*, 4>& q, int half, std::int16_t room) {
    for (Cell* c : q) {
        c->halves[static_cast<std::size_t>(half)].room = room;
        c->halves[static_cast<std::size_t>(half)].outside = room == 0;
    }
}

// Walls, diagonals and rooms of one floored tile.
void buildTile(Grid& g, const LevelInput& in, int x, int y, std::uint8_t walls) {
    const auto q = quarters(g, x, y);
    for (Cell* c : q) {
        if (!c) {
            return;
        }
    }
    std::uint8_t diagonal = 0;
    for (std::uint8_t side = kMinusY; side <= kDiagonalXEqualsY; side = static_cast<std::uint8_t>(side << 1)) {
        if (!(walls & side)) {
            continue;
        }
        switch (side) {
        case kMinusX: closeEdge(*q[0], *q[2], 0xF1); break;
        case kPlusY: closeEdge(*q[2], *q[3], 0xC7); break;
        case kMinusY: closeEdge(*q[0], *q[1], 0x7C); break;
        case kPlusX: closeEdge(*q[1], *q[3], 0x1F); break;
        case kDiagonalXEqualsY:
            splitDiagonal(q, side);
            diagonal = 2;
            break;
        default:
            splitDiagonal(q, side);
            diagonal = 1;
            break;
        }
    }
    g.tiles[static_cast<std::size_t>(y) * static_cast<std::size_t>(g.width) + static_cast<std::size_t>(x)].diagonal = diagonal;
    const float fx = static_cast<float>(x), fy = static_cast<float>(y);
    auto roomAt = [&](float px, float py) { return in.roomAt ? in.roomAt(px, py) : std::int16_t{0}; };
    if (diagonal == 0) {
        setRoom(q, 0, roomAt(fx + 0.5f, fy + 0.5f));
    } else if (diagonal == 1) {
        setRoom(q, 0, roomAt(fx + 0.25f, fy + 0.25f));
        setRoom(q, 1, roomAt(fx + 0.75f, fy + 0.75f));
    } else {
        setRoom(q, 0, roomAt(fx + 0.75f, fy + 0.25f));
        setRoom(q, 1, roomAt(fx + 0.25f, fy + 0.75f));
    }
}

// Agrees one link between a cell and its neighbour in direction d: which halves it joins, or
// none. Only this cell's links and both cells' target bits change. The neighbour settles its own
// links when its turn comes.
void reconcile(Cell& a, Cell& b, int d) {
    const int o = (d + 4) & 7;
    const std::uint8_t mine = bit(d), theirs = bit(o);
    Half& aA = a.halves[0];
    Half& aB = a.halves[1];
    Half& bA = b.halves[0];
    Half& bB = b.halves[1];
    const bool fromA = aA.valid && (aA.links & mine), fromB = aB.valid && (aB.links & mine);
    const bool toA = bA.valid && (bA.links & theirs), toB = bB.valid && (bB.links & theirs);
    auto none = [&] {
        aA.links &= static_cast<std::uint8_t>(~mine);
        aB.links &= static_cast<std::uint8_t>(~mine);
    };
    if (fromA && fromB) {
        if (toA && toB) {
            aA.toSecond &= static_cast<std::uint8_t>(~mine);
            bA.toSecond &= static_cast<std::uint8_t>(~theirs);
            aB.toSecond |= mine;
            bB.toSecond |= theirs;
        } else if (toA) {
            aA.toSecond &= static_cast<std::uint8_t>(~mine);
            bA.toSecond &= static_cast<std::uint8_t>(~theirs);
            aB.links &= static_cast<std::uint8_t>(~mine);
        } else if (toB) {
            aA.links &= static_cast<std::uint8_t>(~mine);
            aB.toSecond |= mine;
            bB.toSecond |= theirs;
        } else {
            none();
        }
    } else if (fromA) {
        if (toA) {
            aA.toSecond &= static_cast<std::uint8_t>(~mine);
            bA.toSecond &= static_cast<std::uint8_t>(~theirs);
            aB.links &= static_cast<std::uint8_t>(~mine);
        } else if (toB) {
            aA.toSecond |= mine;
            bB.toSecond &= static_cast<std::uint8_t>(~theirs);
            aB.links &= static_cast<std::uint8_t>(~mine);
        } else {
            none();
        }
    } else if (fromB) {
        if (toA) {
            aA.links &= static_cast<std::uint8_t>(~mine);
            aB.toSecond &= static_cast<std::uint8_t>(~mine);
            bA.toSecond |= theirs;
        } else if (toB) {
            aA.links &= static_cast<std::uint8_t>(~mine);
            aB.toSecond |= mine;
            bB.toSecond |= theirs;
        } else {
            none();
        }
    } else {
        none();
    }
}

// The half a half's link in direction s leads to, if the neighbour exists.
const Half* across(Grid& g, int x, int y, const Half& h, int s) {
    const Cell* c = g.at(x + kDirX[s], y + kDirY[s]);
    if (!c) {
        return nullptr;
    }
    return &c->halves[(h.toSecond & bit(s)) ? 1 : 0];
}

// Light may not cut a corner diagonally unless it could also go round it through one of the two
// side neighbours to the same half.
void blockCornerCutting(Grid& g, int x, int y, int d) {
    Cell* cell = g.at(x, y);
    if (!cell) {
        return;
    }
    const int before = d - 1, after = (d + 1) & 7;
    for (int h = 0; h < 2; ++h) {
        Half& half = cell->halves[static_cast<std::size_t>(h)];
        if (!half.valid || !(half.links & bit(d))) {
            continue;
        }
        const bool second = (half.toSecond & bit(d)) != 0;
        auto around = [&](int s, int t) {
            if (!(half.links & bit(s))) {
                return false;
            }
            const Half* m = across(g, x, y, half, s);
            return m && m->valid && (m->links & bit(t)) && (((m->toSecond & bit(t)) != 0) == second);
        };
        if (around(before, after) || around(after, before)) {
            continue;
        }
        half.links &= static_cast<std::uint8_t>(~bit(d));
        if (Cell* far = g.at(x + kDirX[d], y + kDirY[d])) {
            far->halves[second ? 1 : 0].links &= static_cast<std::uint8_t>(~bit((d + 4) & 7));
        }
    }
}

// Whether a tile has floor in any quarter on a grid level (counted from the lot's lowest).
bool hasFloor(const array3d::Array3D& floors, int level, int x, int y) {
    if (!floors.contains(level, x, y)) {
        return false;
    }
    for (std::size_t q = 0; q < 4; ++q) {
        if (floors.i16(level, x, y, q) != 0) {
            return true;
        }
    }
    return false;
}

// Marks the tile sides a wall stands on, one tile step at a time. Walls along the grid are on a
// side of the tiles both sides of them. Diagonal walls cross the tiles they pass through.
void markWall(LevelInput& in, const wallruns::Wall& w) {
    const int ax = w.x0, ay = w.y0, bx = w.x1, by = w.y1;
    auto mark = [&](int x, int y, std::uint8_t side) {
        if (x >= 0 && y >= 0 && x < in.width && y < in.height) {
            in.walls[static_cast<std::size_t>(y) * static_cast<std::size_t>(in.width) + static_cast<std::size_t>(x)] |= side;
        }
    };
    const int dx = bx - ax, dy = by - ay;
    const int steps = std::max(std::abs(dx), std::abs(dy));
    if (steps == 0 || (dx != 0 && dy != 0 && std::abs(dx) != std::abs(dy))) {
        return;
    }
    const int sx = dx > 0 ? 1 : (dx < 0 ? -1 : 0), sy = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
    for (int i = 0; i < steps; ++i) {
        const int x0 = ax + i * sx, y0 = ay + i * sy;
        const int x = std::min(x0, x0 + sx), y = std::min(y0, y0 + sy);
        if (sy == 0) {
            mark(x, y - 1, kPlusY);
            mark(x, y, kMinusY);
        } else if (sx == 0) {
            mark(x - 1, y, kPlusX);
            mark(x, y, kMinusX);
        } else {
            mark(x, y, sx == sy ? kDiagonalXEqualsY : kDiagonalXPlusY);
        }
    }
}

} // namespace

LevelInput fromLot(const content::Lot& lot, int level, const std::vector<wallruns::Wall>& walls) {
    LevelInput in;
    const auto floors = lot.grids.find(content::kFloorGridInstance);
    const int ground = content::groundGridLevel(lot);
    if (level < -ground || level >= kMaxLevels || floors == lot.grids.end() || floors->second.width == 0 || floors->second.height == 0 ||
        floors->second.width > static_cast<std::uint32_t>(kMaxTiles) || floors->second.height > static_cast<std::uint32_t>(kMaxTiles)) {
        if (level >= kMaxLevels || (floors != lot.grids.end() && (floors->second.width > static_cast<std::uint32_t>(kMaxTiles) ||
                                                                  floors->second.height > static_cast<std::uint32_t>(kMaxTiles)))) {
            diag::warnOnce("light grid: a level past 64 or a lot larger than 256 tiles is left unlit");
        }
        return in;
    }
    in.level = level;
    in.width = static_cast<int>(floors->second.width);
    in.height = static_cast<int>(floors->second.height);
    const std::size_t tiles = static_cast<std::size_t>(in.width) * static_cast<std::size_t>(in.height);
    in.floor.assign(tiles, 0);
    in.walls.assign(tiles, 0);
    for (int y = 0; y < in.height; ++y) {
        for (int x = 0; x < in.width; ++x) {
            // The ground storey counts every tile, a storey below it only its own floor or the
            // ground storey's floor above.
            const bool floor = level == 0 || hasFloor(floors->second, level + ground, x, y) || hasFloor(floors->second, level + ground + 1, x, y);
            in.floor[static_cast<std::size_t>(y) * static_cast<std::size_t>(in.width) + static_cast<std::size_t>(x)] = floor ? 1 : 0;
        }
    }
    for (const auto& wall : walls) {
        if (wall.level == level) {
            markWall(in, wall);
        }
    }
    return in;
}

floorlight::Grid build(const LevelInput& in) {
    Grid g;
    const std::size_t tiles = static_cast<std::size_t>(in.width) * static_cast<std::size_t>(in.height);
    if (in.width <= 0 || in.height <= 0 || in.width > kMaxTiles || in.height > kMaxTiles || in.floor.size() != tiles ||
        in.walls.size() != tiles) {
        if (in.width > kMaxTiles || in.height > kMaxTiles) {
            diag::warnOnce("light grid: a level larger than 256 tiles is left unlit");
        }
        return g;
    }
    g.level = in.level;
    g.width = in.width;
    g.height = in.height;
    Half blank;
    blank.links = 0xFF;
    blank.outside = true;
    Cell empty;
    empty.halves = {blank, blank};
    g.cells.assign(tiles * 4, empty);
    g.tiles.assign(tiles, floorlight::Tile{});

    // Floor: the tile's four cells take part (their first halves).
    for (int y = 0; y < in.height; ++y) {
        for (int x = 0; x < in.width; ++x) {
            const std::size_t t = static_cast<std::size_t>(y) * static_cast<std::size_t>(in.width) + static_cast<std::size_t>(x);
            if (!in.floor[t]) {
                continue;
            }
            g.tiles[t].floor = true;
            for (Cell* c : quarters(g, x, y)) {
                if (c) {
                    c->halves[0].valid = true;
                }
            }
        }
    }
    // Walls, diagonals and rooms.
    for (int y = 0; y < in.height; ++y) {
        for (int x = 0; x < in.width; ++x) {
            const std::size_t t = static_cast<std::size_t>(y) * static_cast<std::size_t>(in.width) + static_cast<std::size_t>(x);
            if (g.tiles[t].floor) {
                buildTile(g, in, x, y, in.walls[t]);
            }
        }
    }
    // Links: closed at the grid's edge, agreed with each neighbour, then no corner cutting.
    for (int y = 0; y < g.rows(); ++y) {
        for (int x = 0; x < g.columns(); ++x) {
            Cell& c = *g.at(x, y);
            for (int d = 0; d < 8; ++d) {
                if (Cell* next = g.at(x + kDirX[d], y + kDirY[d])) {
                    reconcile(c, *next, d);
                } else {
                    c.halves[0].links &= static_cast<std::uint8_t>(~bit(d));
                    c.halves[1].links &= static_cast<std::uint8_t>(~bit(d));
                }
            }
        }
    }
    for (int y = 0; y < g.rows(); ++y) {
        for (int x = 0; x < g.columns(); ++x) {
            for (int d = 1; d < 8; d += 2) {
                blockCornerCutting(g, x, y, d);
            }
        }
    }
    return g;
}

} // namespace ts2::scene::lightgrid
