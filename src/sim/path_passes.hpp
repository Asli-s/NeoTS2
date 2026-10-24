#pragma once

#include "sim/route_grid.hpp"
#include "sim/route_search.hpp"

#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace ts2::sim {

// How the Sim being routed sees one leaf of the route grid: whether it may walk there, and what
// walking a tile of it costs.
struct LeafPassage {
    bool passable = false;
    double cost = kBlockedCost;
};
using RouteLeafPassage = std::function<LeafPassage(int cell)>;

// The line tests the path passes make over one level's route grid. A line is the segment from
// (x0, y0) to (x1, y1) with a margin of `tolerance` tiles round it.
class RouteLines {
public:
    RouteLines(const RouteGrid& grid, RouteLeafPassage passage) : grid_(grid), passage_(std::move(passage)) {}

    // Whether the line touches a leaf the Sim may not walk on.
    bool blocked(float x0, float y0, float x1, float y1, float tolerance) const;
    // Whether every leaf the line touches is walkable at one cost (within 0.001).
    bool evenCost(float x0, float y0, float x1, float y1, float tolerance) const;
    // The cost of walking the line: each leaf's cost times the length of the line inside it, or
    // kBlockedCost when it crosses a blocked leaf.
    double cost(float x0, float y0, float x1, float y1) const;
    // The point (x, y) pushed out of the blocked leaves within `tolerance` of it, each pushing it
    // along one axis to its nearest side.
    std::pair<float, float> pushClear(float x, float y, float tolerance) const;

private:
    template <class Touches, class Visit> bool walk(Touches&& touches, Visit&& visit) const;

    const RouteGrid& grid_;
    RouteLeafPassage passage_;
};

// Which end of a path a heading is asked for.
enum class HeadingEnd { Start, Goal, LevelEnd, LevelStart };

// The routed Sim's say in how its path leaves a start and reaches a goal that set no facing. When
// `active`, those ends hold the path to the way of its first (last) leg. With `snap` that way turns
// to the nearest eighth of a turn, at a start counted from the Sim's own `facing`. The game sets it
// up inactive for its Sims. What makes it active is not known yet (RT-3 in docs/open-questions.md).
struct RouteHeading {
    bool active = false;
    bool snap = false;
    float facing = 0;  // radians

    // The way (radians) a path takes when it leaves or reaches `end` along `angle`.
    float leaving(float angle, HeadingEnd end) const;
};

// More points than this come only from broken data. The passes stop adding points there.
constexpr std::size_t kMaxPathPoints = 4096;

// The game's passes over a found path, in the order it runs them. Starts, goals and the points either
// side of a change of level never move, and door points are never dropped.

// Lays points every eighth of a tile along each leg that passes within 0.04 tiles of something
// blocked, and pushes them and the existing points 0.0625 tiles clear of it. Then drops points
// closer than 0.1 tiles to both neighbours when the line past them is clear.
void layClearOfObstacles(std::vector<RouteWaypoint>& points, const RouteLines& lines);
// Keeps only the points the path needs: a point is dropped when the straight line from the last kept
// point to the next one costs no more than the path through it, or costs more by less than half a
// tile's walk, or when it is within 0.1 tiles of the last kept point.
void keepTurns(std::vector<RouteWaypoint>& points, const RouteLines& lines);
// Halves every leg longer than half a tile until none is.
void splitLongLegs(std::vector<RouteWaypoint>& points);
// Moves each point but the ends `share` of the way toward the middle of its two neighbours, when the
// legs to and from its new place stay clear (`evenCost`: also walkable at one cost). With `byHold`
// the share shrinks by the point's larger hold (see holdToEnds).
void roundCorners(std::vector<RouteWaypoint>& points, const RouteLines& lines, float share, bool evenCost, bool byHold);
// Holds the path near each end to that end's direction: a start's or goal's facing, a change of
// level's, or with an active `heading` the way of the first or last leg. A hold starts at 1 (a start's
// facingHold for its facing, 0.2 for its first leg) and keeps 0.35 of itself per tile walked away.
// Each other point moves to where the held directions put it, blended by their holds, when the legs
// to and from it stay clear (else a third of the way, if those stay clear), and keeps both holds.
void holdToEnds(std::vector<RouteWaypoint>& points, const RouteLines& lines, const RouteHeading& heading);

// All the passes in the game's order: lay clear, keep turns, split, eight rounding rounds at 0.45
// keeping to one cost, hold to the ends, then two rounds at 0.325 less the hold that only keep clear.
void smoothPath(std::vector<RouteWaypoint>& points, const RouteLines& lines, const RouteHeading& heading = {});

} // namespace ts2::sim
