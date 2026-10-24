#pragma once

#include "scene/light_source.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <set>
#include <vector>

namespace ts2::scene::walllight {

// How lamp, window and floor light lands on a lot's walls, as the game's lighting manager does
// it. Each wall face is a run of one-tile segments facing the same way into the same room. A
// segment holds a small grid of colours: 2 halves along the wall by 6 rows up it (row 0 at the
// top, 5 at the bottom), stored as 8-bit colours the way the game stores them.

struct Pixel {
    std::uint8_t b = 0, g = 0, r = 0, a = 0;
    bool operator==(const Pixel&) const = default;
};
using Row = std::array<Pixel, 6>;
// A column of light-map texels up a wall: the row of the segment above, the segment's six rows,
// then the row of the segment below.
using Column = std::array<Pixel, 8>;

// Where a segment continues on the level above or below (the same wall tile there).
struct Link {
    int run = -1, segment = -1;
};

struct Segment {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // in tiles, the face looks to the right of x0,y0 -> x1,y1
    std::array<Row, 2> halves;
    Link up, down;
    // The segment's light as the game draws it: the previous segment's edge column, the two
    // halves, then the next segment's edge column, each with the rows above and below added.
    std::array<Column, 4> texels{};
};

struct Run {
    std::vector<Segment> segments;
    int level = 0;
    std::int16_t room = 0;     // the room the face looks into, 0 outside
    bool diagonal = false;
    bool lamplit = false;      // a lamp or window lit it this time (it then takes more floor light)
    int previous = -1, next = -1;  // the runs before and after it round the same room
    float angle = 0;           // its direction, radians from +x
};

struct Walls {
    std::vector<Run> runs;
    float falloffScale = 0.4f;               // wallDiffusionThreshold
    std::array<float, 3> insideAmbient{0, 0, 0};   // colour times strength
    std::array<float, 3> outsideAmbient{0, 0, 0};
    int steps = 50;                          // wallPropagationSteps
    float brightFloor = 1;                   // brightWallFloorContribution
    float darkFloor = 0.5f;                  // darkWallFloorContribution
};

// Whether the floor at a point (in tiles) on a level is solid there.
using FloorAt = std::function<bool(float x, float y, int level)>;
// The floor's light at a point on a level.
using FloorLight = std::function<std::array<float, 3>(float x, float y, int level)>;

// The passes, in the game's order. `changed` collects the runs to spread light along. Those
// that can do a lot of work return false when they stopped early at their safety limit
// (never on a real lot, so worth logging).
void resetToAmbient(Walls& walls);
[[nodiscard]] bool lightFromSources(Walls& walls, const std::vector<LightSource>& sources, const FloorAt& floorAt, std::set<int>& changed);
void lightFromFloor(Walls& walls, const FloorLight& floorLight, std::set<int>& changed);
[[nodiscard]] bool spread(Walls& walls, const std::set<int>& changed);
// Fills every segment's texels from its halves and its neighbours' along the run and on the
// levels above and below.
void pad(Walls& walls);

// All of the above. False when a pass stopped early.
[[nodiscard]] bool light(Walls& walls, const std::vector<LightSource>& sources, const FloorAt& floorAt, const FloorLight& floorLight);

// A colour as the game stores it in a wall grid.
Pixel pack(const std::array<float, 3>& color);

} // namespace ts2::scene::walllight
