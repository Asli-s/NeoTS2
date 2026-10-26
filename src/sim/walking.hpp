#pragma once

#include "sim/walk_path.hpp"
#include "sim/walk_plan.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ts2::sim {

// A route for Go To Routing Slot, as the simulation plans it: the points a Sim walks from its spot
// to the goal (route coordinates, which are the position's, in tiles) and the direction it faces
// at the end when the slot says one. With no points, `failure` is the game's route result (person
// data 0x3E): 2 path not found, 6 no valid goals, 7 the destination occupied.
// A stair on the way: the Sim walks to points[at] (its entry), the stair's Portal tree climbs it, and
// the walk goes on from points[at + 1] (its exit). Levels count from 1 (the ground).
struct StairCrossing {
    std::size_t at = 0;
    std::int16_t entryPart = 0, exitPart = 0;
    std::int32_t entrySlot = 0, exitSlot = 0;
    int entryLevel = 1, exitLevel = 1;
    std::optional<std::int16_t> entryFacing;  // the entry slot's facing, which the walk there ends in
};

struct SlotRoute {
    std::vector<std::pair<float, float>> points;
    std::vector<StairCrossing> stairs;  // in the order they are crossed
    std::optional<std::int16_t> facing;
    std::int16_t failure = 0;
    std::int16_t goalObject = 0;  // the seat of a seat goal reached, else 0
    std::int16_t goalToken = -1;  // the joint of the slot whose spot was reached, else -1
};

// Where a walk gets along its path.
struct WalkedTo {
    float x = 0, y = 0;
    std::optional<float> heading;  // the way it faces (see turn.hpp), where the path has length
};

// How far it is from (x, y) along the route's points from point `next` to the end.
inline float distanceLeft(const std::vector<std::pair<float, float>>& points, std::size_t next, float x, float y) {
    float left = 0;
    for (; next < points.size(); ++next) {
        left += std::hypot(points[next].first - x, points[next].second - y);
        x = points[next].first;
        y = points[next].second;
    }
    return left;
}

// What a route keeps about the Sims and objects in its way. It lasts the whole route: a plan again
// after a block keeps it.
struct RouteBlock {
    // The tree or wait a blocked walk is in.
    enum class Step { None, Animation, AskToMove, RouteBlocked, Feedback };
    Step step = Step::None;
    std::optional<bool> returned;  // what the tree pushed for `step` returned, unset while it runs
    std::uint16_t tries = 0;       // blocks in a row by the same blocker
    std::int16_t blocker = 0;      // the last one in the way
    std::optional<std::pair<float, float>> lastBlockAt;  // where the Sim stood at the last block
    // Where each Sim on the lot stood when the route was last planned, to tell whether a blocker
    // has moved since.
    struct SimAt {
        std::int16_t id = 0;
        std::int32_t x = 0, y = 0;
    };
    std::vector<SimAt> simsAtPlan;
    // The Sim asked to move and the interaction it was given, cleared once that Sim's route in
    // the interaction ends.
    std::int16_t waitingOn = 0;
    std::uint32_t waitInteraction = 0;
    std::int16_t stepAsides = 0;   // step-aside routes made from this route
    bool steppingAside = false;    // this route is itself a step-aside
    std::int16_t animations = 0;   // "CT - Route Blocked Animation" plays so far
    bool routeBlockedRun = false;  // "CT - Route Blocked" has run on this route
    // Asking the blocker to move: the tick the ask was made, the waiting animations pushed since and
    // the tick of the last one.
    std::optional<std::uint64_t> askedAt;
    std::int16_t waits = 0;
    std::optional<std::uint64_t> lastWaitAt;
};

// Go To Routing Slot in progress: which instruction runs it, the walk as planned (see walk_plan.hpp)
// and where and when it started. The Sim is as far along its route as the plan's speed carries it.
struct Walking {
    enum class Phase { Start, Walk, Stop };
    std::size_t frame = 0;
    std::uint32_t group = 0;
    std::uint16_t tree = 0;
    std::uint16_t pc = 0;
    SlotRoute route;
    float fromX = 0, fromY = 0;  // where it started, route coordinates
    WalkPath path;               // from there along the route's points
    double startMs = 0;          // on the world's clock
    WalkPlan plan;
    Phase phase = Phase::Walk;   // the part of the walk the Sim is in
    float endHeading = 0;        // the way the Sim faces once there
    float startHeading = 0;      // the way it faced as the walk started
    // A turn on the spot before or after the walk (see routeShape): from and to, and whether its
    // clip has started.
    struct Turn {
        float from = 0, to = 0;
        bool started = false;
    };
    std::optional<Turn> turn;
    std::optional<float> turnAfter;  // the facing to turn to once the walk is over
    bool arrived = false;            // the walk is over (a turn after it may still run)
    // A block ended the walk where I stand, as an arrival ends it. It shows no walk until it is
    // planned again.
    bool stopped = false;
    // The route was interrupted, and it ends as interrupted once the walk is over. `stopsSoon`: the
    // walk was cut short to stop soon (see stopSoon) and ends where that stop does.
    bool interrupted = false;
    bool stopsSoon = false;
    // What the Sim's pre-route tree returned (it runs before the walk starts), unset while it runs.
    std::optional<bool> preRoute;
    bool started = false;        // walking (after the pre-route tree)
    bool nested = false;         // started inside a post-route tree: no post-route tree of its own
    bool turnOnly = false;       // already at the goal, only turning to its facing: no route trees
    bool feedback = true;        // a failure runs "CT - Route Failure Feedback" (route flag 0x4000)
    // The slot the first plan chose (owner id, number, global): a plan again after the pre-route
    // tree routes to the same slot, whatever that tree did to the temps.
    std::int16_t slotOwner = 0, slotNumber = 0;
    bool slotGlobal = false;
    // A route over stairs is walked a stretch at a time: `route` holds the stretch to the next stair's
    // entry, `ahead` the route from that stair's exit on (its stairs counted from there), and the
    // route's facing at the goal waits in `goalFacing` until the last stretch.
    std::optional<SlotRoute> ahead;
    std::optional<std::int16_t> goalFacing;
    bool climbing = false;          // the stair's Portal tree runs on the Sim
    std::optional<bool> climbed;    // what it returned
    // The goal check of the object the walking tree runs on: it runs before the Sim sets off and
    // again each tile walked, and false ends the walk. `checkAt` is how far along this stretch the
    // next one runs.
    bool startChecked = false;
    bool checking = false;          // the goal check runs on the Sim
    std::optional<bool> goalCheck;  // what it returned
    float checkAt = 1;
    RouteBlock block;
    std::optional<double> sweptTo;  // the clock time the look-ahead for other Sims last ran
    // A step-aside in progress: the destination it walks to, and the blocked-route state of the
    // route it stepped aside from, which goes on once the step-aside is over.
    std::int16_t aside = 0;
    std::optional<RouteBlock> outerBlock;
};

// Ends a step-aside: the route it stepped aside from takes up its own blocked-route state again.
// Whether the Sim is moving along its walk plan, and is drawn from it: not turning on the spot,
// not arrived and not stopped by a block.
inline bool alongPlan(const Walking& walk) {
    return walk.started && !walk.turn && !walk.arrived && !walk.stopped;
}

inline void endStepAside(Walking& walk) {
    if (walk.outerBlock) {
        walk.block = std::move(*walk.outerBlock);
    }
    walk.outerBlock.reset();
    walk.aside = 0;
}

// Cuts a route at its first stair: `walk.route` becomes the stretch up to the stair's entry, ending
// in the entry slot's facing, and `walk.ahead` the rest from its exit. A route without stairs is
// left whole.
inline void cutAtStair(Walking& walk) {
    SlotRoute& route = walk.route;
    if (route.stairs.empty() || route.stairs.front().at + 1 >= route.points.size()) {
        walk.ahead.reset();
        return;
    }
    const std::size_t cut = route.stairs.front().at + 1;
    SlotRoute rest = route;
    rest.points.erase(rest.points.begin(), rest.points.begin() + static_cast<std::ptrdiff_t>(cut));
    rest.stairs.erase(rest.stairs.begin());
    for (StairCrossing& s : rest.stairs) {
        s.at -= cut;
    }
    route.points.resize(cut);
    route.stairs.resize(1);
    walk.goalFacing = route.facing;
    route.facing = route.stairs.front().entryFacing;
    rest.facing = walk.goalFacing;
    walk.ahead = std::move(rest);
}

// A Go To Routing Slot whose walk is over, waiting for the Sim's post-route tree to return: the
// instruction (its frame, tree and node) and what the tree returned, unset while it runs. With
// `failed`, the route has failed and the tree is "CT - Route Failure Feedback": the route is false
// whatever it returns. A route started inside either tree is nested in this one.
struct PostRouteWait {
    std::size_t frame = 0;
    std::uint32_t group = 0;
    std::uint16_t tree = 0;
    std::uint16_t pc = 0;
    std::optional<bool> result;
    bool failed = false;
};

// Where a walk gets `walked` tiles along its path from where it started.
inline WalkedTo walkPoint(const Walking& walk, float walked) {
    const auto at = walk.path.at(walked);
    return WalkedTo{at.x, at.y, at.heading};
}

} // namespace ts2::sim
