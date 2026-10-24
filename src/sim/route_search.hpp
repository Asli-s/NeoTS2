#pragma once

#include "sim/route_grid.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The game's path search over the lot's route grids: A* from the start points to the goals, then
// shortened, giving the waypoints a Sim walks before the path is smoothed. One search runs over every
// level at once, the levels joined where stairs lead from one to another. See
// docs/open-questions.md (Routing) for what is and isn't known of it.
namespace ts2::sim {

// The game's random numbers for routing: a multiplicative generator giving values in [0, 1). The
// game seeds its generators from the clock. Ours take a seed so runs can be repeated.
class RouteRandom {
public:
    explicit RouteRandom(std::uint32_t seed) : state_(seed != 0 ? seed : 1) {}
    double next();
    double uniform(double lo, double hi) { return next() * (hi - lo) + lo; }

private:
    std::uint32_t state_;
};

// Which leaves of a route grid touch along an edge, in the order the game links them (its search
// tries neighbours in that order). Built once per grid state. Rebuild it after the grid changes.
struct RouteLinks {
    std::vector<std::vector<int>> of;  // by cell index, empty for cells that aren't leaves
    static RouteLinks build(const RouteGrid& grid);
};

// Levels count from 0 (the ground) here, as the route grids do.
struct RouteStart {
    float x = 0, y = 0;
    int level = 0;
};
struct RouteGoal {
    float x = 0, y = 0;
    float score = 0;  // the highest-scoring goal reached wins
    // A seat goal's seat (0 for a spot to stand on), the joint of its slot it is at, and the facing
    // the Sim takes there.
    std::int16_t object = 0;
    std::int16_t token = -1;
    std::optional<int> facing;
    int level = 0;
};

// A door as one route finds it, by its lead's id: what crossing it costs on top of the floor, which
// sides count its swing areas (the door's object data 0x16 bits 0 and 1 in the game, and a side the
// search walks into is added), and which ways this Sim may not go through it (from side 0, from 1).
struct RouteDoorState {
    std::uint32_t owner = 0;
    float extra = 1;
    bool active[2] = {false, false};
    bool refused[2] = {false, false};
};

// LevelEnd is the last point before a change of level (a stair's entry) and LevelStart the first
// after it (its exit).
enum class WaypointKind : std::uint8_t { Plain = 0, LevelEnd = 1, LevelStart = 2, DoorEntry = 3, DoorExit = 4, Start = 5, Goal = 6 };

// A waypoint's facing when it has none.
constexpr float kNoFacing = -999.0f;

struct RouteWaypoint {
    float x = 0, y = 0;
    float cost = 0;  // cost of the path up to here
    WaypointKind kind = WaypointKind::Plain;
    // The way a Sim faces at a start, goal or level change that sets one, in radians.
    float facing = kNoFacing;
    // For a start with a facing: how strongly the path leaving it keeps to that facing, 0 to 1.
    float facingHold = 0;
    // How strongly the path here keeps to the way it leaves the start (or a level change) and the way
    // it reaches the goal, 0 to 1. The last rounding passes move a point less the more it is held.
    float startHold = 0, endHold = 0;
    int level = 0;
    int portal = -1;  // at a stair's entry or exit: which of the search's portals
};

struct RoutePlan {
    int goal = -1;  // index into the goals
    std::vector<RouteWaypoint> waypoints;
    // The waypoints after the game's passes over the path (path_passes.hpp): the ones a Sim walks.
    std::vector<RouteWaypoint> walk;
};

// One level of the search: its route grid and how its leaves link.
struct RouteLevel {
    const RouteGrid* grid = nullptr;
    const RouteLinks* links = nullptr;
};

// A way between levels (a stair): its two ends, crossed from end 0 to end 1 (and back when two-way)
// at a flat cost, whatever the distance. Each end links with the leaves under its point and 0.0625
// tiles around it. A refused one can't be crossed.
struct RoutePortal {
    float x[2] = {0, 0}, y[2] = {0, 0};
    int level[2] = {0, 0};
    bool twoWay = false;
    float cost = 1;
    bool refused = false;
};

// More starts or goals than this are a sign of broken data. Such a request finds no path.
constexpr std::size_t kMaxRouteStarts = 64;
constexpr std::size_t kMaxRouteGoals = 256;

// Searches `grid` (one level) from any start to the best goal it can reach: the highest score,
// then the cheapest. The search aims at `aim` (the game passes one point for all goals). `jitter`
// adds the game's small random cost to each first step into a cell. `heuristic` drives the game's
// random estimate. Doors not in `doors` cost 1 to cross and count no swing areas. Nothing when no
// goal can be reached.
std::optional<RoutePlan> planRoute(const RouteGrid& grid, const RouteLinks& links, std::span<const RouteStart> starts,
                                   std::span<const RouteGoal> goals, float aimX, float aimY, std::span<const RouteDoorState> doors,
                                   RouteRandom& jitter, RouteRandom& heuristic, const RouteIntersects& intersects = {});

// The same over several levels (`levels` by level, a missing grid for a level without one) joined by
// `portals`. Starts and goals name their level.
std::optional<RoutePlan> planRoute(std::span<const RouteLevel> levels, std::span<const RoutePortal> portals,
                                   std::span<const RouteStart> starts, std::span<const RouteGoal> goals, float aimX, float aimY,
                                   std::span<const RouteDoorState> doors, RouteRandom& jitter, RouteRandom& heuristic,
                                   const RouteIntersects& intersects = {});

} // namespace ts2::sim
