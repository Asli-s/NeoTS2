// Build mode's floor tool: laying and taking up floor tiles on a lot's floor grid.
#include "test_support.hpp"

#include "content/floor_edit.hpp"

namespace {

using ts2::content::TileRect;

ts2::array3d::Array3D grid(std::uint32_t width, std::uint32_t height, std::uint32_t levels, std::size_t elementSize = 8) {
    ts2::array3d::Array3D g;
    g.width = width;
    g.height = height;
    g.levels = levels;
    g.elementSize = elementSize;
    g.elements.assign(static_cast<std::size_t>(width) * height * levels * elementSize, 0);
    return g;
}

std::uint16_t quarter(const ts2::array3d::Array3D& g, int level, int x, int y, std::size_t q) {
    return static_cast<std::uint16_t>(g.i16(level, x, y, q));
}

// A catalog floor goes into the lot's pattern list by its GUID in hex, once, at the lowest free id.
void floorsJoinThePatternList() {
    std::map<std::uint16_t, ts2::stringmap::Entry> patterns{{1, {"floor_wood", 4}}, {3, {"4CC2A1B7", 8}}};
    CHECK(ts2::content::floorPatternName(0x4cc2a1b7) == "4cc2a1b7");
    CHECK(ts2::content::floorPatternName(0x1a) == "0000001a");
    CHECK(ts2::content::patternId(patterns, "4cc2a1b7") == 3);  // either case
    CHECK(ts2::content::patternId(patterns, "0000001a") == 2);
    CHECK(patterns.size() == 3 && patterns[2].name == "0000001a" && patterns[2].uses == 0);
}

// On the ground storey a floor covers every quarter of the rectangle, dragged either way, but where
// there is no surface (a pool). Above it only quarters already floored change.
void layingFloorCoversTheRectangle() {
    auto floors = grid(4, 4, 2);
    auto surface = grid(4, 4, 2, 4);
    std::fill(surface.elements.begin(), surface.elements.end(), std::uint8_t{1});
    surface.elements[(static_cast<std::size_t>(0) * 4 + 2) * 4 * 4 + 2 * 4 + 1] = 0;  // level 0, tile (2, 2), quarter 1
    std::map<std::uint16_t, ts2::stringmap::Entry> patterns;
    auto change = ts2::content::layFloor(floors, patterns, 0, TileRect{2, 2, 1, 1}, "0000001a", true, &surface);
    CHECK(change.quarters == 15 && change.replaced.empty());
    CHECK(quarter(floors, 0, 1, 1, 0) == 1 && quarter(floors, 0, 2, 2, 3) == 1);
    CHECK(quarter(floors, 0, 2, 2, 1) == 0);  // no surface
    CHECK(quarter(floors, 0, 0, 0, 0) == 0 && quarter(floors, 0, 3, 3, 0) == 0);
    CHECK(patterns[1].uses == 15);
    // Again in another floor, over half of it: the quarters it covered are given back by name.
    change = ts2::content::layFloor(floors, patterns, 0, TileRect{1, 1, 1, 2}, "0000002b", true, &surface);
    CHECK(change.quarters == 8 && change.replaced.size() == 1 && change.replaced["0000001a"] == 8);
    CHECK(patterns[1].uses == 7 && patterns[2].uses == 8);
    // The same floor again changes nothing.
    CHECK(ts2::content::layFloor(floors, patterns, 0, TileRect{1, 1, 1, 2}, "0000002b", true, &surface).quarters == 0);
    // Upstairs, one quarter has floor: only it is covered.
    floors.elements[((static_cast<std::size_t>(1) * 4 + 3) * 4 + 0) * 8 + 2 * 2] = 1;  // level 1, tile (3, 0), quarter 2
    change = ts2::content::layFloor(floors, patterns, 1, TileRect{0, 0, 3, 3}, "0000002b", false);
    CHECK(change.quarters == 1 && quarter(floors, 1, 3, 0, 2) == 2 && quarter(floors, 1, 3, 0, 0) == 0);
    // A rectangle off the lot's edge keeps to the grid.
    CHECK(ts2::content::layFloor(floors, patterns, 0, TileRect{3, 3, 9, 9}, "0000001a", true).quarters == 4);
}

// Taking floor up leaves bare quarters and says what was there.
void removingFloorLeavesItBare() {
    auto floors = grid(3, 3, 1);
    std::map<std::uint16_t, ts2::stringmap::Entry> patterns;
    ts2::content::layFloor(floors, patterns, 0, TileRect{0, 0, 2, 0}, "0000001a", true);
    const auto change = ts2::content::removeFloor(floors, patterns, 0, TileRect{1, 0, 2, 2});
    CHECK(change.quarters == 8 && change.replaced.at("0000001a") == 8);
    CHECK(quarter(floors, 0, 1, 0, 0) == 0 && quarter(floors, 0, 0, 0, 0) == 1);
    CHECK(patterns[1].uses == 4);
}

// A floor's price is per tile; part tiles round up.
void floorsArePricedByTile() {
    CHECK(ts2::content::floorPrice(9, 8) == 18);
    CHECK(ts2::content::floorPrice(9, 15) == 34);
    CHECK(ts2::content::floorPrice(9, 1) == 3);
    CHECK(ts2::content::floorPrice(0, 12) == 0);
}

} // namespace

std::vector<Test> floorEditTests() {
    return {
        {"floors join the lot's pattern list", floorsJoinThePatternList},
        {"laying floor covers the rectangle", layingFloorCoversTheRectangle},
        {"removing floor leaves it bare", removingFloorLeavesItBare},
        {"floors are priced by tile", floorsArePricedByTile},
    };
}
