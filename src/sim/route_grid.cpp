#include "sim/route_grid.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::sim {

namespace {

// Lots are at most a few dozen tiles across. A bigger (or not finite) size from bad data is cut to
// this, which keeps the grid's depth and cell count small.
constexpr float kMaxLotSize = 256;

float lotSize(float v) {
    return std::isfinite(v) ? std::clamp(v, 1.0f, kMaxLotSize) : 1.0f;
}

bool reachesAllQuarters(const RouteRect& r, const RouteRect& area) {
    const float midX = (area.x0 + area.x1) / 2, midY = (area.y0 + area.y1) / 2;
    const RouteRect quarters[4] = {{area.x0, area.y0, midX, midY}, {midX, area.y0, area.x1, midY},
                                   {area.x0, midY, midX, area.y1}, {midX, midY, area.x1, area.y1}};
    return std::all_of(std::begin(quarters), std::end(quarters), [&](const RouteRect& q) { return r.overlaps(q); });
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

bool RouteGrid::accepts(const RouteItem& item) {
    const RouteRect& r = item.rect;
    return std::isfinite(r.x0) && std::isfinite(r.y0) && std::isfinite(r.x1) && std::isfinite(r.y1) && r.x0 <= r.x1 &&
           r.y0 <= r.y1 && !std::isnan(item.cost);
}

void RouteGrid::add(const RouteItem& item) {
    if (!accepts(item)) {
        return;
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
    if (r.covers(area) || level == finestLevel_ || (level + 1 == finestLevel_ && reachesAllQuarters(r, area))) {
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

void RouteGrid::remove(RouteItemKind kind, std::span<const std::uint32_t> owners) {
    std::vector<int> removed;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].kind == kind && std::binary_search(owners.begin(), owners.end(), items_[i].owner)) {
            removed.push_back(static_cast<int>(i));
        }
    }
    // Highest first: each removed item's slot takes the last item, so only the cells under those two
    // items change.
    bool merged = false;
    for (auto it = removed.rbegin(); it != removed.rend(); ++it) {
        const int item = *it;
        removeFrom(0, item, merged);
        const int last = static_cast<int>(items_.size()) - 1;
        if (item != last) {
            rename(0, last, item);
            items_[static_cast<std::size_t>(item)] = items_.back();
        }
        items_.pop_back();
    }
    if (merged) {
        dropUnusedCells();
    }
}

std::vector<RouteItem> RouteGrid::itemsOf(RouteItemKind kind, std::uint32_t owner) const {
    std::vector<RouteItem> found;
    for (const RouteItem& item : items_) {
        if (item.kind == kind && item.owner == owner) {
            found.push_back(item);
        }
    }
    return found;
}

// Takes `item` out of the leaves under `cell` and merges four leaves back into their parent where
// nothing left needs them apart. True when a leaf lost the item.
bool RouteGrid::removeFrom(int cell, int item, bool& merged) {
    Cell& c = cells_[static_cast<std::size_t>(cell)];
    if (!items_[static_cast<std::size_t>(item)].rect.overlaps(c.rect())) {
        return false;
    }
    if (c.isLeaf()) {
        return std::erase(c.items, item) > 0;
    }
    bool lost = false;
    for (const int child : c.children) {
        lost |= removeFrom(child, item, merged);
    }
    if (lost && mayMerge(cell, item)) {
        merged = true;
    }
    return lost;
}

// Merges the four children of `cell` into it when they are leaves and every item they hold, other
// than `removing`, would let `cell` be one leaf: the opposite of what makes insert split a leaf.
bool RouteGrid::mayMerge(int cell, int removing) {
    std::vector<int> items;
    for (const int child : cells_[static_cast<std::size_t>(cell)].children) {
        const Cell& k = cells_[static_cast<std::size_t>(child)];
        if (!k.isLeaf()) {
            return false;
        }
        items.insert(items.end(), k.items.begin(), k.items.end());
    }
    std::sort(items.begin(), items.end());
    items.erase(std::unique(items.begin(), items.end()), items.end());
    std::erase(items, removing);
    Cell& c = cells_[static_cast<std::size_t>(cell)];
    const RouteRect area = c.rect();
    const bool whole = std::all_of(items.begin(), items.end(), [&](int i) {
        const RouteRect& r = items_[static_cast<std::size_t>(i)].rect;
        return r.covers(area) || (c.level + 1 == finestLevel_ && reachesAllQuarters(r, area));
    });
    if (whole) {
        c.items = std::move(items);
        c.children = {-1, -1, -1, -1};
    }
    return whole;
}

// Files item `from` under index `to` in the leaves under `cell`.
void RouteGrid::rename(int cell, int from, int to) {
    Cell& c = cells_[static_cast<std::size_t>(cell)];
    if (!items_[static_cast<std::size_t>(from)].rect.overlaps(c.rect())) {
        return;
    }
    if (c.isLeaf()) {
        std::replace(c.items.begin(), c.items.end(), from, to);
        return;
    }
    for (const int child : c.children) {
        rename(child, from, to);
    }
}

// Frees the cells merging left behind, keeping the others in their order.
void RouteGrid::dropUnusedCells() {
    std::vector<int> newIndex(cells_.size(), -1);
    std::vector<int> pending{0};
    while (!pending.empty()) {
        const int cell = pending.back();
        pending.pop_back();
        newIndex[static_cast<std::size_t>(cell)] = 0;
        for (const int child : cells_[static_cast<std::size_t>(cell)].children) {
            if (child >= 0) {
                pending.push_back(child);
            }
        }
    }
    std::size_t kept = 0;
    for (std::size_t i = 0; i < cells_.size(); ++i) {
        if (newIndex[i] < 0) {
            continue;
        }
        newIndex[i] = static_cast<int>(kept);
        if (kept != i) {
            cells_[kept] = std::move(cells_[i]);
        }
        ++kept;
    }
    // Shrinking keeps the vector's room for the cells later items split off.
    cells_.erase(cells_.begin() + static_cast<std::ptrdiff_t>(kept), cells_.end());
    for (Cell& c : cells_) {
        for (int& child : c.children) {
            if (child >= 0) {
                child = newIndex[static_cast<std::size_t>(child)];
            }
        }
    }
}

int RouteGrid::leafAt(float x, float y) const {
    const float size = cells_[0].size;
    if (!(x >= 0 && y >= 0 && x < size && y < size)) {  // NaN fails every comparison
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
// (a doorway's square overrides the wall it sits in). Without floor a leaf blocks, and so does any
// type above 1. Areas (kind 1) are left out: they count only on the sides of their door that the
// path search makes active.
RouteGrid::Passage RouteGrid::passage(const Cell& leaf, const RouteIntersects* intersects) const {
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
        if (item.kind == RouteItemKind::Area) {
            continue;
        }
        if (item.dynamic && intersects != nullptr && *intersects) {
            if (const auto through = (*intersects)(item.owner)) {
                cost = std::max(cost, *through);
                continue;
            }
        }
        if (solid && item.type >= 2) {
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
