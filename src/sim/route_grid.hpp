#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace ts2::sim {

// A rectangle on one level of a lot, in tiles. Route coordinates: x runs along the wall graph's y
// and y along its x (the game's route grid swaps the two).
struct RouteRect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    bool operator==(const RouteRect&) const = default;

    bool overlaps(const RouteRect& o) const { return std::max(x0, o.x0) < std::min(x1, o.x1) && std::max(y0, o.y0) < std::min(y1, o.y1); }
    bool covers(const RouteRect& o) const { return x0 <= o.x0 && o.x1 <= x1 && y0 <= o.y0 && o.y1 <= y1; }
};

// What put an item on the route grid. The values are the game's own.
enum class RouteItemKind : std::uint16_t {
    Solid = 0,       // a small solid square (door frames)
    Area = 1,        // an area that only counts where its owner says so (around doors)
    Floor = 2,       // walkable floor or terrain
    Object = 101,    // an object's footprint
    Wall = 102,      // a wall or fence piece
    Soft = 103,      // a costly area around some objects
    FloorHole = 104, // part of a tile without floor on an upper level
    Post = 105,      // a fence post
    WallJoint = 106, // a wall end or corner, costly to brush past
};

// Cost of a cell no normal Sim can enter.
constexpr float kBlockedCost = std::numeric_limits<float>::max();

// Types 0 and 1 are walkable. Higher types block, in pairs: an odd type for the thing itself and the
// even type below it for the clearance around it.
constexpr int kNoFloorType = 17;

// A door's two sides for routing: points a and b stand either side of its wall, and the door line is
// halfway between them, square to a-b. Side 1 is b's (a point on the line is on side 0).
struct RouteDoor {
    float ax = 0, ay = 0, bx = 0, by = 0;

    bool operator==(const RouteDoor&) const = default;
    int side(float x, float y) const {
        const float d = (bx - ax) * (x - (ax + bx) * 0.5f) + (by - ay) * (y - (ay + by) * 0.5f);
        return d > 0 ? 1 : 0;
    }
};

// One rectangle the game adds to the route grid.
struct RouteItem {
    RouteRect rect;
    float cost = 1;
    int type = 0;
    RouteItemKind kind = RouteItemKind::Floor;
    bool makesFloor = false;  // a cell with no such item has no floor and blocks
    std::uint32_t owner = 0;  // the id the game files it under (a tile, a wall, an object)
    std::optional<RouteDoor> door;  // a doorway or a door's swing area: the door it belongs to
    bool dynamic = false;  // blocking unless its object's "allow intersection" function lets the Sim through
    bool padding = false;  // the margin round an object's footprint, which only the path search keeps clear of
    // Object data 0x28 bit 0x4000: goal spots are tested against the footprint itself, not 0.3 tiles round it.
    bool exactFootprint = false;

    bool operator==(const RouteItem&) const = default;
};

// Whether the Sim being routed may pass the dynamic items of object `owner`, and at what cost.
// Nothing when it may not.
using RouteIntersects = std::function<std::optional<float>(std::uint32_t owner)>;

// The route grid of one level: a quadtree over the lot whose leaves are as large as they can be
// while each lies wholly inside or outside every item, down to an eighth of a tile.
class RouteGrid {
public:
    struct Cell {
        int level = 0;          // 0 for the root
        int x = 0, y = 0;       // position in cells of this level's size
        float size = 0;
        std::array<int, 4> children{-1, -1, -1, -1};  // low x/y, high x, high y, high x and y
        std::vector<int> items;

        bool isLeaf() const { return children[0] < 0; }
        RouteRect rect() const { return {x * size, y * size, (x + 1) * size, (y + 1) * size}; }
    };

    // The leaf type and cost a Sim sees, from the items on the leaf.
    struct Passage {
        int type = kNoFloorType;
        float cost = kBlockedCost;
        bool walkable() const { return type <= 1; }
    };

    // A grid covering a lot of `width` x `height` tiles (route coordinates).
    RouteGrid(float width, float height);

    // Items with bad data (a rect that isn't finite or is inside out, a NaN cost) are left out.
    static bool accepts(const RouteItem& item);
    void add(const RouteItem& item);
    // Removes the items of `kind` filed under any of `owners` (sorted). Cells that no longer need
    // splitting merge again, as the game's do, so the grid ends up as if built from the items left.
    void remove(RouteItemKind kind, std::span<const std::uint32_t> owners);
    // The items of `kind` filed under `owner`, in no particular order.
    std::vector<RouteItem> itemsOf(RouteItemKind kind, std::uint32_t owner) const;

    const std::vector<Cell>& cells() const { return cells_; }
    float width() const { return width_; }
    float height() const { return height_; }
    const std::vector<RouteItem>& items() const { return items_; }
    // The leaf holding a point, or -1 outside the grid.
    int leafAt(float x, float y) const;
    // With `intersects`, dynamic items it lets the Sim through count as passable at its cost.
    Passage passage(const Cell& leaf, const RouteIntersects* intersects = nullptr) const;

    // Leaves are an eighth of a tile at the finest level.
    static constexpr float kFinestSize = 0.125f;

private:
    bool insert(int item, int cell);
    void split(int cell);
    bool removeFrom(int cell, int item, bool& merged);
    bool mayMerge(int cell, int removing);
    void rename(int cell, int from, int to);
    void dropUnusedCells();

    std::vector<Cell> cells_;
    std::vector<RouteItem> items_;
    float width_ = 0, height_ = 0;
    int finestLevel_ = 0;
};

} // namespace ts2::sim
