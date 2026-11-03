#pragma once

#include "formats/array3d.hpp"
#include "formats/wallgraph.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace ts2::sim {

struct RoomInfo {
    std::int32_t id = 0;
    std::int32_t level = 0;
    std::int32_t quarterTiles = 0;
    bool touchesLotEdge = false;
    // The room's region in the game's two other room layers (see RoomMap::assignLayer), -1 when
    // unknown. Room field 1 ("outside") is 1 when the layer 0 region is 0, room field 7 the same for layer 1.
    std::array<std::int32_t, 2> layerRegion{-1, -1};

    // Area in whole tiles, rounded toward zero as the game does.
    std::int32_t area() const { return quarterTiles / 4; }
};

// Which room each part of each tile belongs to. Every tile is split into four triangles (top,
// right, bottom, left) so that diagonal walls cut tiles in half. Either read from the room grid
// the game saves with the lot, or rebuilt from the room wall graph: walls block movement
// between triangles and each enclosed region takes the room its walls name on that side.
class RoomMap {
public:
    static constexpr std::int32_t kNoRoom = -1;

    RoomMap() = default;
    explicit RoomMap(const wallgraph::WallGraph& graph);
    // The room the game saved for every quarter tile (c3DArray instance 9). Grid level 0 is the lot's
    // lowest level (the wall graph's firstLevel).
    RoomMap(const array3d::Array3D& savedRooms, std::int32_t firstLevel);

    // x and y in wall graph coordinates (tiles), level as wall graph points use it (0 = ground).
    std::int32_t roomAt(std::int32_t level, float x, float y) const;
    // The same for a position as Sims and objects have them (route coordinates: x along the wall
    // graph's y, y along its x).
    std::int32_t roomAtPosition(std::int32_t level, float x, float y) const { return roomAt(level, y, x); }
    // Room of one triangle of a tile: 0 top, 1 right, 2 bottom, 3 left.
    std::int32_t quarterRoom(std::int32_t level, std::int32_t x, std::int32_t y, int quarter) const;
    const RoomInfo* room(std::int32_t id) const;
    // The game keeps two more region layers, each built from its own wall graph. Gives each room the
    // region that covers most of its quarter tiles. Quarters the layer's walls don't enclose are
    // region 0.
    void assignLayer(int layer, const RoomMap& regions);
    const std::map<std::int32_t, RoomInfo>& rooms() const { return rooms_; }

    std::int32_t width() const { return width_; }
    std::int32_t height() const { return height_; }
    // Tiles along a position's x and y.
    std::int32_t positionWidth() const { return height_; }
    std::int32_t positionHeight() const { return width_; }
    std::int32_t firstLevel() const { return firstLevel_; }
    std::int32_t levelCount() const { return levelCount_; }

private:
    std::size_t index(std::int32_t level, std::int32_t x, std::int32_t y, int quarter) const;

    std::int32_t width_ = 0;
    std::int32_t height_ = 0;
    std::int32_t firstLevel_ = 0;
    std::int32_t levelCount_ = 0;
    std::vector<std::int32_t> quarterRoom_;
    std::map<std::int32_t, RoomInfo> rooms_;
};

// A wall or fence segment and its wall layer type: a wall style, or a fence's GUID.
struct WallPiece {
    wallgraph::Point from, to;
    std::uint32_t type = 0;
};

// The game's layer of the lot's walls and fences together (fence arches and the wall styles 300 and
// 301 are kept out of it), as a planar graph whose faces are the areas they enclose.
class WallOutline {
public:
    WallOutline() = default;
    explicit WallOutline(const std::vector<WallPiece>& pieces);

    // Room field 6: whether more than half of the outline round room `room` is greenhouse walls
    // (fences whose GUID is in `greenhouse`). The outline is the face on the room's side of the
    // segment nearest a point in the room's first tile (lowest level, then x, then y), looked for
    // on that tile's edges and at its nearest corner, a level lower when none is there. Segments
    // the game finds outside this layer are not known yet. Of two corner segments as near, the
    // one the game takes is not known either.
    bool mostlyGreenhouse(const RoomMap& rooms, std::int32_t room, const std::set<std::uint32_t>& greenhouse) const;

private:
    struct Vertex {
        float x = 0, y = 0;
        std::int32_t level = 0;
        std::vector<std::size_t> out;  // half-edges leaving it, by angle
    };
    struct HalfEdge {
        std::size_t from = 0, to = 0;
        std::uint32_t type = 0;
        float angle = 0;
    };
    std::optional<std::size_t> vertex(float x, float y, std::int32_t level) const;
    std::optional<std::size_t> edge(std::size_t from, std::size_t to) const;
    std::optional<std::pair<std::size_t, std::size_t>> nearest(const RoomMap& rooms, float x, float y,
                                                               std::int32_t level) const;
    // The next half-edge round the face on the right of `e`.
    std::size_t next(std::size_t e) const;

    std::vector<Vertex> vertices_;
    std::vector<HalfEdge> edges_;  // the twin of half-edge i is i ^ 1
    std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t>, std::size_t> vertexAt_;
};

// The room on one side of the wall from `from` to `to` (wall graph points): the room of the quarter
// tile beside the wall on the left, walking from `from` to `to` (the wall's left room when the room
// map agrees with the wall graph). Swap the points for the other side. -1 off the room map.
std::int32_t roomOnSideOfWall(const RoomMap& rooms, const wallgraph::Point& from, const wallgraph::Point& to);

} // namespace ts2::sim
