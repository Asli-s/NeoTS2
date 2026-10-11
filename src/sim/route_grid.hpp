#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace ts2::sim {

// A rectangle on one level of a lot, in tiles. Route coordinates: x runs along the wall graph's y
// and y along its x (the game's route grid swaps the two).
struct RouteRect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;

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

// Types 0 and 1 are walkable; higher types block, in pairs: an odd type for the thing itself and the
// even type below it for the clearance around it.
constexpr int kNoFloorType = 17;

// One rectangle the game adds to the route grid.
struct RouteItem {
    RouteRect rect;
    float cost = 1;
    int type = 0;
    RouteItemKind kind = RouteItemKind::Floor;
    bool makesFloor = false;  // a cell with no such item has no floor and blocks
    std::uint32_t owner = 0;  // the id the game files it under (a tile, a wall, an object)
};

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

    void add(const RouteItem& item);

    const std::vector<Cell>& cells() const { return cells_; }
    const std::vector<RouteItem>& items() const { return items_; }
    // The leaf holding a point, or -1 outside the grid.
    int leafAt(float x, float y) const;
    Passage passage(const Cell& leaf) const;

    // Leaves are an eighth of a tile at the finest level.
    static constexpr float kFinestSize = 0.125f;

private:
    bool insert(int item, int cell);
    void split(int cell);

    std::vector<Cell> cells_;
    std::vector<RouteItem> items_;
    float width_ = 0, height_ = 0;
    int finestLevel_ = 0;
};

} // namespace ts2::sim
