#include "scene/floor_light.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ts2::scene::floorlight {

// The game works these out on the x87 in double precision and rounds to float only where it
// stores a value, so the code below does the same: double in between, float where the game
// keeps a float.

namespace {

constexpr int kMaxTiles = 256;   // larger than any lot
constexpr int kMaxSteps = 1000;
constexpr int kMaxLevelsAbove = 32;
constexpr float kSameFalloff = 0.01f;
// A safety limit on the cells one lighting looks at, far above what any real lot needs, so
// odd settings or thousands of lights cannot stall the game.
constexpr long long kMaxCellVisits = 32'000'000;

// A rectangle of cells, inclusive; empty while maxX < minX.
struct Box {
    int minX = 0, minY = 0, maxX = -1, maxY = -1;

    bool empty() const { return maxX < minX || maxY < minY; }
    void add(int x, int y) {
        if (empty()) {
            minX = maxX = x;
            minY = maxY = y;
            return;
        }
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
    }
};

// The half that a half's link in direction d leads to, if it has one and there is floor there.
Half* neighbour(Grid& grid, int x, int y, int half, int d) {
    Cell* cell = grid.at(x, y);
    if (!cell) {
        return nullptr;
    }
    const Half& from = cell->halves[static_cast<std::size_t>(half)];
    if (!(from.links & (1u << d))) {
        return nullptr;
    }
    Cell* next = grid.at(x + kDirX[d], y + kDirY[d]);
    if (!next) {
        return nullptr;
    }
    Half& to = next->halves[(from.toSecond & (1u << d)) ? 1 : 0];
    return to.valid ? &to : nullptr;
}

// Brightens a half from a neighbour, channel by channel, where the neighbour's light faded by
// one step is brighter; returns how many channels changed.
int pull(Half& half, const Half& from, int d, float falloff) {
    double k = falloff;
    if (d & 1) {
        k *= 1.4;
    }
    const double keep = static_cast<float>(1.0 / (1.0 + k));
    int changed = 0;
    for (std::size_t c = 0; c < 3; ++c) {
        const double node = half.light[c], next = from.light[c];
        if (k * node < next - node) {
            half.light[c] = static_cast<float>(keep * next + static_cast<double>(0.01f));
            ++changed;
        }
    }
    return changed;
}

void resetToAmbient(Grid& grid) {
    for (auto& cell : grid.cells) {
        for (auto& half : cell.halves) {
            if (half.valid) {
                half.light = half.outside ? grid.outsideAmbient : grid.insideAmbient;
                half.active = false;
            }
        }
    }
}

// floor(v) as a cell number, kept to a safe range (and NaN to -1) so it always fits an int.
int toCell(double v) {
    if (!(v > -2.0)) {
        return -1;
    }
    return v < 2.0 * kMaxTiles + 2 ? static_cast<int>(std::floor(v)) : 2 * kMaxTiles + 2;
}

// The cells a source's area covers, clamped to the grid; empty when it misses the grid.
Box cellsOf(const Grid& grid, const LightSource& s) {
    int x0 = toCell(2.0 * s.x0 + 0.5), y0 = toCell(2.0 * s.y0 + 0.5);
    int x1 = toCell(2.0 * s.x1 - 0.5), y1 = toCell(2.0 * s.y1 - 0.5);
    if (x1 < x0) {
        std::swap(x0, x1);
    }
    if (y1 < y0) {
        std::swap(y0, y1);
    }
    Box box;
    box.minX = std::max(x0, 0);
    box.minY = std::max(y0, 0);
    box.maxX = std::min(x1, grid.columns() - 1);
    box.maxY = std::min(y1, grid.rows() - 1);
    return box;
}

bool hasRoom(const Cell& cell, std::int16_t room) {
    for (const auto& half : cell.halves) {
        if (half.valid && half.room == room) {
            return true;
        }
    }
    return false;
}

// Whether light from a source above this level gets down to a cell through the floors between.
bool shinesDown(const Grid& grid, const LightSource& s, int x, int y, const FloorStops& stops, long long& budget) {
    for (int level = s.level; level > grid.level; --level) {
        --budget;
        if (stops && stops(x, y, level)) {
            return false;
        }
    }
    return true;
}

// Whether a source lights anything on this level at all.
bool reaches(Grid& grid, const LightSource& s, const FloorStops& stops, long long& budget) {
    const long long above = static_cast<long long>(s.level) - grid.level;
    if (above < 0 || above > kMaxLevelsAbove || !std::isfinite(s.falloff)) {
        return false;
    }
    const Box box = cellsOf(grid, s);
    for (int y = box.minY; y <= box.maxY && budget > 0; ++y) {
        for (int x = box.minX; x <= box.maxX && budget > 0; ++x) {
            --budget;
            const Cell* cell = grid.at(x, y);
            if (cell && hasRoom(*cell, s.room) && shinesDown(grid, s, x, y, stops, budget)) {
                return true;
            }
        }
    }
    return false;
}

// Fills a source's area in its room with its light; grows `lit` over the cells it brightened.
bool deposit(Grid& grid, const LightSource& s, const FloorStops& stops, Box& lit, long long& budget) {
    bool any = false;
    const Box box = cellsOf(grid, s);
    for (int y = box.minY; y <= box.maxY && budget > 0; ++y) {
        for (int x = box.minX; x <= box.maxX && budget > 0; ++x) {
            --budget;
            Cell* cell = grid.at(x, y);
            if (!cell || !hasRoom(*cell, s.room)) {
                continue;
            }
            float intensity = s.floorIntensity;
            if (s.level > grid.level) {
                if (s.interFloorFalloff >= 1 || !shinesDown(grid, s, x, y, stops, budget)) {
                    continue;
                }
                for (int level = s.level; level > grid.level; --level) {
                    intensity = static_cast<float>((1.0 - s.interFloorFalloff) * intensity);
                }
            }
            bool raised = false;
            for (auto& half : cell->halves) {
                if (!half.valid || half.room != s.room) {
                    continue;
                }
                bool brighter = false;
                for (std::size_t c = 0; c < 3; ++c) {
                    const float v = s.color[c] * intensity;
                    if (v > half.light[c]) {
                        half.light[c] = v;
                        brighter = true;
                    }
                }
                if (brighter) {
                    half.active = true;
                    raised = true;
                }
            }
            if (raised) {
                lit.add(x, y);
                any = true;
            }
        }
    }
    return any;
}

// Pulls light into a cell's halves from the given directions; returns the channels changed.
int pullInto(Grid& grid, int x, int y, const int (&dirs)[3], bool needActive, float falloff) {
    Cell* cell = grid.at(x, y);
    if (!cell) {
        return 0;
    }
    int changed = 0;
    for (int h = 0; h < 2; ++h) {
        Half& half = cell->halves[static_cast<std::size_t>(h)];
        if (!half.valid) {
            continue;
        }
        for (const int d : dirs) {
            const Half* from = neighbour(grid, x, y, h, d);
            if (!from || (needActive && !from->active)) {
                continue;
            }
            const int c = pull(half, *from, d, falloff);
            if (c) {
                changed += c;
                half.changed = half.active = true;
            }
        }
    }
    return changed;
}

// One round of spreading: four sweeps over the box (left to right, right to left, then down and
// up each column), each pulling from the side it comes from. The box grows where light reaches
// its edge. Returns the channels changed and the cells that changed.
int spreadOnce(Grid& grid, Box& box, float falloff, Box& changedCells, long long& budget) {
    static constexpr int kFromLeft[3] = {1, 2, 3};
    static constexpr int kFromRight[3] = {5, 6, 7};
    static constexpr int kFromAbove[3] = {7, 0, 1};
    static constexpr int kFromBelow[3] = {3, 4, 5};
    int total = 0;
    auto visit = [&](int x, int y, const int (&dirs)[3], bool needActive) {
        if (budget <= 0) {
            return false;
        }
        --budget;
        const int c = pullInto(grid, x, y, dirs, needActive, falloff);
        if (c) {
            total += c;
            changedCells.add(x, y);
        }
        return c != 0;
    };
    for (int y = box.minY; y <= box.maxY; ++y) {
        for (int x = box.minX; x <= box.maxX + 1 && x < grid.columns(); ++x) {
            if (visit(x, y, kFromLeft, true) && x > box.maxX) {
                box.maxX = x;
            }
        }
        for (int x = box.maxX; x >= box.minX - 1 && x >= 0; --x) {
            if (visit(x, y, kFromRight, true) && x < box.minX) {
                box.minX = x;
            }
        }
    }
    for (int x = box.minX; x <= box.maxX; ++x) {
        // The game does not check that the neighbour is active in this sweep.
        for (int y = box.minY; y <= box.maxY + 1 && y < grid.rows(); ++y) {
            if (visit(x, y, kFromAbove, false) && y > box.maxY) {
                box.maxY = y;
            }
        }
        for (int y = box.maxY; y >= box.minY - 1 && y >= 0; --y) {
            if (visit(x, y, kFromBelow, true) && y < box.minY) {
                box.minY = y;
            }
        }
    }
    // What changed this round passes light on next round. As in the game, the box's last row and
    // column are left out here.
    for (int y = box.minY; y < box.maxY; ++y) {
        for (int x = box.minX; x < box.maxX; ++x) {
            if (Cell* cell = grid.at(x, y)) {
                for (auto& half : cell->halves) {
                    half.active = half.changed;
                    half.changed = false;
                }
            }
        }
    }
    return total;
}

void spread(Grid& grid, Box box, float falloff, long long& budget) {
    if (budget <= 0) {
        return;
    }
    budget -= static_cast<long long>(grid.cells.size());
    box.minX = std::max(box.minX, 0);
    box.minY = std::max(box.minY, 0);
    box.maxX = std::min(box.maxX, grid.columns() - 1);
    box.maxY = std::min(box.maxY, grid.rows() - 1);
    for (auto& cell : grid.cells) {
        for (auto& half : cell.halves) {
            half.changed = false;
        }
    }
    const int steps = std::clamp(grid.steps, 0, kMaxSteps);
    for (int i = 0; i < steps && !box.empty() && budget > 0; ++i) {
        Box next;
        if (spreadOnce(grid, box, falloff, next, budget) == 0) {
            break;
        }
        box = next;
    }
}

} // namespace

bool Grid::fits() const {
    return width > 0 && height > 0 && width <= kMaxTiles && height <= kMaxTiles &&
           cells.size() == static_cast<std::size_t>(columns()) * static_cast<std::size_t>(rows()) &&
           tiles.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
}

const Cell* Grid::at(int x, int y) const {
    if (width <= 0 || height <= 0 || width > kMaxTiles || height > kMaxTiles || x < 0 || y < 0 || x >= columns() ||
        y >= rows()) {
        return nullptr;
    }
    const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(columns()) + static_cast<std::size_t>(x);
    return i < cells.size() ? &cells[i] : nullptr;
}

Cell* Grid::at(int x, int y) {
    return const_cast<Cell*>(static_cast<const Grid&>(*this).at(x, y));
}

bool light(Grid& grid, const std::vector<LightSource>& sources, const FloorStops& stops) {
    if (!grid.fits()) {
        return true;  // nothing to light
    }
    resetToAmbient(grid);

    long long budget = kMaxCellVisits;
    std::vector<const LightSource*> pending;
    for (const auto& s : sources) {
        if (budget > 0 && reaches(grid, s, stops, budget)) {
            pending.push_back(&s);
        }
    }
    // Slowest fading first in the list; taken from the back, so the fastest fading go first.
    std::stable_sort(pending.begin(), pending.end(), [](const LightSource* a, const LightSource* b) { return a->falloff > b->falloff; });

    while (!pending.empty()) {
        const float first = pending.back()->falloff;
        Box lit;
        bool any = deposit(grid, *pending.back(), stops, lit, budget);
        pending.pop_back();
        while (!pending.empty() &&
               std::fabs(static_cast<double>(pending.back()->falloff) - first) < static_cast<double>(kSameFalloff)) {
            any |= deposit(grid, *pending.back(), stops, lit, budget);
            pending.pop_back();
        }
        if (any) {
            spread(grid, lit, first * grid.falloffScale, budget);
        }
    }
    return budget > 0;
}

std::array<float, 3> sample(const Grid& grid, float x, float y) {
    const std::array<float, 3> none{0, 0, 0};
    if (!grid.fits() || !(x >= 0) || !(y >= 0) || x >= static_cast<float>(grid.width) || y >= static_cast<float>(grid.height)) {
        return none;
    }
    const int tx = static_cast<int>(std::floor(x)), ty = static_cast<int>(std::floor(y));
    const Tile& tile = grid.tiles[static_cast<std::size_t>(ty) * static_cast<std::size_t>(grid.width) + static_cast<std::size_t>(tx)];
    if (!tile.floor) {
        return none;
    }
    // Which quarter of the tile, and on a diagonal tile which side of the diagonal.
    const double fx = static_cast<double>(x) - tx - 0.5, fy = static_cast<double>(y) - ty - 0.5;
    const Cell* cell = grid.at(2 * tx + (fx >= 0 ? 1 : 0), 2 * ty + (fy >= 0 ? 1 : 0));
    const bool first = tile.diagonal == 0 || (tile.diagonal == 1 && fy < -fx) || (tile.diagonal == 2 && fy < fx);
    if (!cell) {
        return none;
    }
    const Half& half = cell->halves[first ? 0 : 1];
    if (!half.valid) {
        return none;
    }
    std::array<float, 3> out = half.light;
    for (std::size_t c = 0; c < 3; ++c) {
        if (grid.dimOutside && half.outside) {
            out[c] -= grid.outsideAmbient[c];
        }
        out[c] *= 2;
    }
    return out;
}

} // namespace ts2::scene::floorlight
