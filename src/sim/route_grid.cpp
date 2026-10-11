#include "sim/route_grid.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::sim {

namespace {

// Lots are at most a few dozen tiles across; a bigger (or not finite) size from bad data is cut to
// this, which keeps the grid's depth and cell count small.
constexpr float kMaxLotSize = 256;

float lotSize(float v) {
    return std::isfinite(v) ? std::clamp(v, 1.0f, kMaxLotSize) : 1.0f;
}

} // namespace

RouteGrid::RouteGrid(float width, float height) : width_(lotSize(width)), height_(lotSize(height)) {
    // The root is the smallest power-of-two square holding the lot (64 tiles for a 30 x 40 lot).
    float size = 1;
    while (size < std::max(width_, height_)) {
        size *= 2;
    }
    finestLevel_ = static_cast<int>(std::lround(std::log2(size / kFinestSize)));
    cells_.push_back({0, 0, 0, size, {-1, -1, -1, -1}, {}});
}

void RouteGrid::add(const RouteItem& item) {
    const RouteRect& r = item.rect;
    if (!std::isfinite(r.x0) || !std::isfinite(r.y0) || !std::isfinite(r.x1) || !std::isfinite(r.y1) || r.x0 > r.x1 ||
        r.y0 > r.y1 || std::isnan(item.cost)) {
        return;  // bad data: routing ignores it
    }
    items_.push_back(item);
    insert(static_cast<int>(items_.size()) - 1, 0);
}

// Files an item in every leaf it touches, splitting leaves its edge passes through until they are
// either wholly inside it or as small as leaves get. One level above the finest, a leaf the item
// reaches into in all four quarters takes it whole instead of splitting.
bool RouteGrid::insert(int item, int cell) {
    const RouteRect& r = items_[static_cast<std::size_t>(item)].rect;
    const RouteRect area = cells_[static_cast<std::size_t>(cell)].rect();
    if (!r.overlaps(area)) {
        return false;
    }
    if (!cells_[static_cast<std::size_t>(cell)].isLeaf()) {
        bool added = false;
        for (int i = 0; i < 4; ++i) {
            added |= insert(item, cells_[static_cast<std::size_t>(cell)].children[static_cast<std::size_t>(i)]);
        }
        return added;
    }
    const int level = cells_[static_cast<std::size_t>(cell)].level;
    auto reachesAllQuarters = [&] {
        const float midX = (area.x0 + area.x1) / 2, midY = (area.y0 + area.y1) / 2;
        const RouteRect quarters[4] = {{area.x0, area.y0, midX, midY}, {midX, area.y0, area.x1, midY},
                                       {area.x0, midY, midX, area.y1}, {midX, midY, area.x1, area.y1}};
        return std::all_of(std::begin(quarters), std::end(quarters), [&](const RouteRect& q) { return r.overlaps(q); });
    };
    if (r.covers(area) || level == finestLevel_ || (level + 1 == finestLevel_ && reachesAllQuarters())) {
        cells_[static_cast<std::size_t>(cell)].items.push_back(item);
        return true;
    }
    split(cell);
    bool added = false;
    for (int i = 0; i < 4; ++i) {
        added |= insert(item, cells_[static_cast<std::size_t>(cell)].children[static_cast<std::size_t>(i)]);
    }
    return added;
}

// Gives a leaf four children, each holding the leaf's items.
void RouteGrid::split(int cell) {
    const Cell parent = cells_[static_cast<std::size_t>(cell)];
    for (int i = 0; i < 4; ++i) {
        Cell child;
        child.level = parent.level + 1;
        child.x = parent.x * 2 + (i & 1);
        child.y = parent.y * 2 + (i >> 1 & 1);
        child.size = parent.size / 2;
        child.items = parent.items;
        cells_[static_cast<std::size_t>(cell)].children[static_cast<std::size_t>(i)] = static_cast<int>(cells_.size());
        cells_.push_back(std::move(child));
    }
    cells_[static_cast<std::size_t>(cell)].items.clear();
}

int RouteGrid::leafAt(float x, float y) const {
    const float size = cells_[0].size;
    if (x < 0 || y < 0 || x >= size || y >= size) {
        return -1;
    }
    int cell = 0;
    while (!cells_[static_cast<std::size_t>(cell)].isLeaf()) {
        const RouteRect r = cells_[static_cast<std::size_t>(cell)].rect();
        const int i = (x >= (r.x0 + r.x1) / 2 ? 1 : 0) + (y >= (r.y0 + r.y1) / 2 ? 2 : 0);
        cell = cells_[static_cast<std::size_t>(cell)].children[static_cast<std::size_t>(i)];
    }
    return cell;
}

// The highest type and cost among the leaf's items. A solid square overrides anything that blocks
// (a doorway's square overrides the wall it sits in). Without floor a leaf blocks; so does any
// type above 1. Areas (kind 1) count only where their owner says so, which isn't handled yet.
RouteGrid::Passage RouteGrid::passage(const Cell& leaf) const {
    bool floor = false, solid = false;
    for (const int i : leaf.items) {
        const RouteItem& item = items_[static_cast<std::size_t>(i)];
        floor |= item.makesFloor;
        solid |= item.kind == RouteItemKind::Solid;
    }
    int type = 0;
    float cost = 1;
    for (const int i : leaf.items) {
        const RouteItem& item = items_[static_cast<std::size_t>(i)];
        if (item.kind == RouteItemKind::Area || (solid && item.type >= 2)) {
            continue;
        }
        type = std::max(type, item.type);
        cost = std::max(cost, item.cost);
    }
    if (!floor) {
        type = kNoFloorType;
    }
    return {type, type <= 1 ? cost : kBlockedCost};
}

} // namespace ts2::sim
