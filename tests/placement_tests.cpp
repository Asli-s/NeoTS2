// The placement test's rules for Buy and Build mode.
#include "test_support.hpp"

#include "sim/floor_support.hpp"
#include "sim/placement_ground.hpp"
#include "sim/placement_roofs.hpp"
#include "sim/placement_walls.hpp"

#include <cstring>
#include <limits>

namespace {

using ts2::array3d::Array3D;

Array3D makeGrid(std::uint32_t width, std::uint32_t height, std::uint32_t levels, std::size_t elementSize) {
    Array3D g;
    g.width = width;
    g.height = height;
    g.levels = levels;
    g.elementSize = elementSize;
    g.elements.assign(static_cast<std::size_t>(width) * height * levels * elementSize, 0);
    return g;
}

// Grids keep their elements by level, then x, then y.
std::uint8_t* cell(Array3D& g, std::uint32_t level, std::uint32_t x, std::uint32_t y) {
    return g.elements.data() + ((static_cast<std::size_t>(level) * g.width + x) * g.height + y) * g.elementSize;
}

void setHeight(Array3D& heights, std::uint32_t level, std::uint32_t x, std::uint32_t y, float z) {
    std::memcpy(cell(heights, level, x, y), &z, sizeof z);
}

// A 4 by 4 lot with the ground storey and one above, flat at 0 and 3.
ts2::content::Lot flatLot() {
    ts2::content::Lot lot;
    lot.worldBounds = ts2::content::WorldBounds{0, 0, 0, 3, 3, 1};
    lot.grids[ts2::content::kFloorGridInstance] = makeGrid(4, 4, 2, 8);
    lot.grids[ts2::content::kSurfaceGridInstance] = makeGrid(4, 4, 2, 4);
    Array3D heights = makeGrid(5, 5, 2, 4);
    for (std::uint32_t x = 0; x < 5; ++x) {
        for (std::uint32_t y = 0; y < 5; ++y) {
            setHeight(heights, 1, x, y, 3.0f);
        }
    }
    lot.grids[ts2::content::kFloorHeightGridInstance] = heights;
    return lot;
}

void floorTile(ts2::content::Lot& lot, std::uint32_t level, std::uint32_t x, std::uint32_t y) {
    std::uint8_t* f = cell(lot.grids[ts2::content::kFloorGridInstance], level, x, y);
    for (int q = 0; q < 4; ++q) {
        f[q * 2] = 1;
    }
    std::memset(cell(lot.grids[ts2::content::kSurfaceGridInstance], level, x, y), 1, 4);
}

void groundRulesRunInOrder() {
    using ts2::sim::PlacedFlags;
    ts2::content::Lot lot = flatLot();
    floorTile(lot, 1, 1, 1);
    // The storey above the ground tile (1, 1) comes down to 2.5, so a ceiling there is low.
    auto& heights = lot.grids[ts2::content::kFloorHeightGridInstance];
    for (const auto [x, y] : {std::pair{1u, 1u}, {1u, 2u}, {2u, 2u}, {2u, 1u}}) {
        setHeight(heights, 1, x, y, 2.5f);
    }
    // The ground under tile (3, 3) slopes.
    setHeight(heights, 0, 4, 4, 0.5f);
    const ts2::sim::PlacementGround ground(lot);
    const PlacedFlags anywhere{0, 0x3, 0, 0};  // may stand on a floor and on the ground
    const auto code = [&](const PlacedFlags& o, float x, float y, std::int32_t level) { return ground.code(o, x, y, level, {}, std::nullopt); };

    CHECK(code(anywhere, -0.5f, 1.5f, 0) == std::int16_t{2});
    CHECK(code(anywhere, 4.0f, 4.0f, 0) != std::int16_t{2});  // the far edges are on the lot
    CHECK(code(anywhere, 1.5f, 1.5f, 2) == std::int16_t{3});
    CHECK(!code(anywhere, 0.5f, 0.5f, 0));
    CHECK(code(PlacedFlags{0, 0x1, 0, 0}, 0.5f, 0.5f, 0) == std::int16_t{0x8E});  // floor only, on bare ground
    CHECK(code(anywhere, 2.5f, 2.5f, 1) == std::int16_t{0x8F});
    CHECK(!code(anywhere, 1.5f, 1.5f, 1));
    CHECK(code(PlacedFlags{0, 0x2, 0, 0}, 1.5f, 1.5f, 1) == std::int16_t{0x8D});  // ground only, on a floor
    CHECK(code(anywhere, 3.5f, 3.5f, 0) == std::int16_t{0x12});
    CHECK(!code(PlacedFlags{0, 0x103, 0, 0}, 3.5f, 3.5f, 0));  // may stand on a slope
    CHECK(code(anywhere, 1.5f, 1.5f, 0) == std::int16_t{0x2F});
    CHECK(!code(PlacedFlags{0, 0x3, 0x20, 0}, 1.5f, 1.5f, 0));  // may stand under a low ceiling
    // An object that stands in for a floor lets one stand upstairs on a tile without floor.
    const auto standIn = [](std::int32_t level, std::int32_t x, std::int32_t y) { return level == 1 && x == 3 && y == 0; };
    CHECK(!ground.code(anywhere, 3.5f, 0.5f, 1, standIn, std::nullopt));
    CHECK(code(anywhere, 3.5f, 0.5f, 1) == std::int16_t{0x8F});

    // A lot with nothing saved refuses every spot as off the lot.
    const ts2::sim::PlacementGround empty(ts2::content::Lot{});
    CHECK(empty.code(anywhere, 0.5f, 0.5f, 0, {}, std::nullopt) == std::int16_t{2});
}

void wallsThroughFootprints() {
    using ts2::sim::GameRect;
    ts2::content::Lot lot;
    lot.worldBounds = ts2::content::WorldBounds{0, 0, 0, 3, 3, 0};
    ts2::wallgraph::WallGraph graph;
    graph.width = 4;
    graph.height = 4;
    graph.levels = 1;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    // A wall along y = 2 from x = 1 to 3, a rising diagonal through tile (0, 0), and one broken point.
    graph.points = {{1, 1, 2, 0}, {2, 3, 2, 0}, {3, 0, 0, 0}, {4, 1, 1, 0}, {5, nan, 0, 0}};
    graph.walls = {{1, 1, 0, 2, 0}, {2, 3, 0, 4, 0}, {3, 5, 0, 2, 0}};
    lot.roomGraph = graph;
    const ts2::sim::PlacementWalls walls(lot);

    CHECK(walls.crossed(GameRect{1.2f, 1.2f, 1.8f, 2.8f}, 0));
    CHECK(!walls.crossed(GameRect{1.2f, 1.2f, 1.8f, 1.9f}, 0));
    CHECK(!walls.crossed(GameRect{1.2f, 2.1f, 1.8f, 2.9f}, 0));  // the wall only runs along its edge
    CHECK(walls.crossed(GameRect{0.2f, 0.2f, 0.8f, 0.8f}, 0));
    CHECK(!walls.crossed(GameRect{0.6f, 0.05f, 0.95f, 0.4f}, 0));  // the quarter the diagonal misses
    CHECK(!walls.crossed(GameRect{1.2f, 1.2f, 1.8f, 2.8f}, 1));  // no such level
    CHECK(!walls.crossed(GameRect{1.8f, 1.2f, 1.2f, 2.8f}, 0));  // an empty rectangle
    CHECK(!ts2::sim::PlacementWalls(ts2::content::Lot{}).crossed(GameRect{0, 0, 1, 1}, 0));

    // An object's sides: the wall along y = 2 is side 0 of tile (1, 1) facing 0, and also side 0 of
    // tile (1, 2) facing 4. A side that forbids a wall there refuses with code 8 at the side's middle, and
    // a side that needs one is satisfied by a wall but not by a fence or a room divider.
    const auto sides = [&](std::uint32_t style, std::uint16_t bits, std::int32_t x, std::int32_t y, std::int16_t facing) {
        ts2::content::Lot styled = lot;
        if (style != 0) {
            styled.wallCoverings = {{1, style, 0, 0}};
        }
        return ts2::sim::PlacementWalls(styled).sides(bits, x, y, 0, facing, false);
    };
    constexpr std::uint32_t kWall = 1, kFence = 0xAD0B35F2, kDivider = 0;
    const auto forbidden = sides(kWall, 0x0100, 1, 1, 0);
    CHECK(forbidden && forbidden->code == 8 && forbidden->at == (std::array<float, 2>{1.5f, 2.0f}));
    const auto turned = sides(kWall, 0x0100, 1, 2, 4);
    CHECK(turned && turned->code == 8 && turned->at == (std::array<float, 2>{1.5f, 2.0f}));
    CHECK(!sides(kWall, 0x0001, 1, 1, 0));
    CHECK(sides(kFence, 0x0001, 1, 1, 0) && sides(kFence, 0x0001, 1, 1, 0)->code == 7);
    CHECK(sides(kFence, 0x0100, 1, 1, 0) && sides(kFence, 0x0100, 1, 1, 0)->code == 8);  // a fence still blocks
    CHECK(!sides(kDivider, 0x0100, 1, 1, 0));
    CHECK(sides(kDivider, 0x0001, 1, 1, 0) && sides(kDivider, 0x0001, 1, 1, 0)->code == 7);
    CHECK(!sides(kWall, 0x1000 | 0x0100, 1, 1, 0));  // objects on fences aren't tested yet
}

void roofsLeaveRoom() {
    ts2::content::Lot lot = flatLot();
    const ts2::sim::PlacementRoofs none(lot);
    CHECK(!none.tooLow(1.5f, 1.5f, 0));
    // A gable over the ground storey with its eaves at the ground: low everywhere under it.
    ts2::roofs::Roof roof;
    roof.id = 1;
    roof.x0 = 1;
    roof.y0 = 1;
    roof.x1 = 3;
    roof.y1 = 3;
    roof.type = 3;
    roof.slope = 0.5f;
    lot.roofs = {roof};
    const ts2::sim::PlacementRoofs low(lot);
    CHECK(low.tooLow(2.0f, 2.0f, 0));
    CHECK(!low.tooLow(0.2f, 3.8f, 0));  // not under it
    CHECK(!low.tooLow(2.0f, 2.0f, -1));
}

void floorsNeedSupport() {
    ts2::content::Lot lot;
    lot.worldBounds = ts2::content::WorldBounds{0, 0, 0, 5, 5, 1};
    lot.grids[ts2::content::kFloorGridInstance] = makeGrid(6, 6, 2, 8);
    lot.grids[ts2::content::kFloorHeightGridInstance] = makeGrid(7, 7, 2, 4);
    ts2::wallgraph::WallGraph graph;
    graph.width = 6;
    graph.height = 6;
    graph.levels = 2;
    // A load-bearing wall on the ground along y = 3 from x = 2 to 3.
    graph.points = {{1, 2, 3, 0}, {2, 3, 3, 0}};
    graph.walls = {{1, 1, 0, 2, 0}};
    lot.roomGraph = graph;
    lot.wallCoverings = {{1, 7, 0, 0}};
    const auto partitions = [](std::uint32_t type) -> std::optional<ts2::sim::PartitionSupport> {
        if (type == 7) {
            return ts2::sim::PartitionSupport{true, false};
        }
        return std::nullopt;
    };
    const ts2::sim::FloorSupport support(lot, partitions, {});

    CHECK(support.allowed(0, 0, 0));  // the ground storey always
    CHECK(support.allowed(1, 2, 3));  // the wall under its low edge
    CHECK(support.allowed(1, 2, 2));  // the wall under its high edge
    CHECK(!support.allowed(1, 4, 1));
    CHECK(!support.allowed(1, 0, 3));  // the lot's edge
    CHECK(!support.allowed(5, 2, 3));  // no such level
    // Without the load-bearing style nothing holds the floor up.
    const ts2::sim::FloorSupport unknown(lot, {}, {});
    CHECK(!unknown.allowed(1, 2, 3));
}

} // namespace

std::vector<Test> placementTests() {
    return {
        {"placement ground rules run in order", groundRulesRunInOrder},
        {"placement walls through footprints", wallsThroughFootprints},
        {"placement roofs leave room", roofsLeaveRoom},
        {"floors need support from below", floorsNeedSupport},
    };
}
