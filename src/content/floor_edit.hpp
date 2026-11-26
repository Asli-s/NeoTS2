#pragma once

#include "content/lot.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ts2::content {

// One quarter (0 to 3) of a floor tile on a grid level. `pattern` is a name in the lot's pattern map,
// empty for no floor, and `turn` the saved texture turn nibble (see floorTurnSteps), 1 for none.
struct FloorQuarter {
    std::int32_t level = 0, x = 0, y = 0;
    int quarter = 0;
    std::string pattern;
    int turn = 1;
    bool operator==(const FloorQuarter&) const = default;
};

// The floor pattern a quarter has, empty for none or off the grid.
std::string floorPatternAt(const Lot& lot, std::int32_t level, std::int32_t x, std::int32_t y, int quarter);

// Gives each quarter its pattern and turn, adding new names to the pattern map. Quarters off the grid
// are left out. Returns the quarters as they were, for undo, in the same order.
std::vector<FloorQuarter> setFloors(Lot& lot, const std::vector<FloorQuarter>& quarters);

// A corner of the lot's height grid (instance 1) on a grid level.
struct CornerHeight {
    std::int32_t level = 0, x = 0, y = 0;
    float z = 0;
    bool operator==(const CornerHeight&) const = default;
};

// Levels round floors laid above grid level `ground`, as the game's floor tool does: the corners of the
// 3x3 tiles round a laid tile take its height, and the storeys above move with them. Quarters taken up
// change nothing. Returns the corners as they were, for undo.
std::vector<CornerHeight> levelAroundFloors(Lot& lot, const std::vector<FloorQuarter>& laid, std::int32_t ground);

// Corners off the grid are left out.
void setCornerHeights(Lot& lot, const std::vector<CornerHeight>& corners);

} // namespace ts2::content
