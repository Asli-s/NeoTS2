#include "sim/ground_click.hpp"
#include "sim/idle_spot.hpp"
#include "sim/motives.hpp"
#include "sim/route_destinations.hpp"
#include "sim/vm_execution.hpp"

namespace ts2::sim::vm_detail {

namespace {

// The route results a blocked route fails with (person data 0x3E).
constexpr std::int16_t kUnknownReason = 1, kBlockedByPerson = 14, kBlockedByObject = 15;
// The global trees a blocked or failed route runs on the Sim.
constexpr std::uint16_t kFailureFeedbackTree = 0x018E, kBlockedAnimationTree = 0x01BB, kAskToMoveAnimationTree = 0x01BC,
                        kWaitingAnimationTree = 0x01BD;
constexpr const char* kRouteBlockedTree = "CT - Route Blocked";
// Bit 0x2 of person data 0x9F lets any Sim ask it to move (the bit's name is not known yet).
constexpr std::size_t kAskablePersonData = 0x9F;
constexpr std::int16_t kAlwaysAskable = 0x02;
// A blocker with object data 8 bit 0x40 is waited for longer (the bit's meaning is not known yet).
constexpr std::size_t kFlagsField = 8;
constexpr std::int16_t kLongWaitFlag = 0x40;
constexpr std::uint64_t kWaitTicks = 126, kLongWaitTicks = 420;
// In tiles: the farthest a Sim is asked to move from, the farthest it is waited for from, and how
// far the walker must have moved since its last block for the count of blocks to start again.
constexpr float kAskDistance = 2.0f, kWaitDistance = 1.25f, kMovedDistance = 0.5f;
// The waiting animation's second value is the distance left to 1.25 tiles, in 24ths.
constexpr float kWaitSteps = 24.0f;
// A Sim asked to move is sent 1 to 3 tiles away when the asker's goal object is within this many
// tiles of it (the square root of 1.5), else it sidesteps.
constexpr float kNearGoal = 1.2247449f;
// The destination's interaction a Sim asked to move runs: number 3, its "Move" line (Interaction -
// Move Out Of Way).
constexpr std::uint32_t kMoveLine = 3;
constexpr std::size_t kSleepStateMotive = 11;
// Slack added to how far another Sim can come in a sweep, so none is missed for rounding.
constexpr float kSweepReachMargin = 0.1f;
// The most other Sims one sweep looks at.
constexpr std::size_t kMaxSweptSims = 32;
// The most Sims whose places a route remembers from its plan.
constexpr std::size_t kMaxRememberedSims = 64;
// The rules only compare the count of blocks with 1 to 4. The cap keeps it from wrapping.
constexpr std::uint16_t kMaxTries = 1000;
// A decision that leads straight back to another (a missing tree, a wait that runs out) is made
// again at once, at most this deep in one go. The count of blocks rises each time, so the rules end
// it well before.
constexpr int kMaxDecisions = 8;

float tiles(std::int32_t fixed) { return static_cast<float>(fixed) / 65536.0f; }

float distanceBetween(const SimObject& a, const SimObject& b) {
    return std::hypot(tiles(a.x) - tiles(b.x), tiles(a.y) - tiles(b.y));
}

int askPriority(const SimObject& sim) { return std::max(sim.personData[kPriorityPersonData] - 1, 1); }

// Whether `sim` is already moving out of someone's way: its running interaction, or one in its
// queue, is the "Move" of a Go Here or Swim Here destination.
bool makingWay(const World& world, const SimObject& sim) {
    const auto isMove = [&](const QueuedInteraction& i) {
        const SimObject* o = world.find(i.object);
        if (o == nullptr || i.index != kMoveLine) {
            return false;
        }
        const std::uint32_t guid = o->definition.guid();
        return guid == destination::kGoHere || guid == destination::kSwimHere;
    };
    return (sim.running && isMove(*sim.running)) || std::ranges::any_of(sim.queue, isMove);
}

// Whether `blocker` takes being asked to move by `sim`: it stands within 2 tiles, isn't making way
// already, and `sim`'s priority less 1 (at least 1) is above its own, unless its person data 0x9F
// has bit 0x2. A Sim's priority (person data 0x21) is its running interaction's, 0 when none runs.
bool takesTheAsk(const World& world, const SimObject& sim, const SimObject& blocker) {
    if (distanceBetween(sim, blocker) > kAskDistance || makingWay(world, blocker)) {
        return false;
    }
    const int mine = askPriority(sim);
    return (blocker.personData[kAskablePersonData] & kAlwaysAskable) != 0 || mine > blocker.personData[kPriorityPersonData];
}

} // namespace

// A step of my walk is blocked by `blocker`: what the router does next, by these rules. Each block
// counts. A new blocker, or one met after I have moved half a tile since the last block, starts
// the count at 1 again. "Moved" means the blocker stands elsewhere than when my route was planned.
// An object fails the route (result 15). For another Sim partway through a route, when we block each
// other and this is my only route: up to 2 blocks I step aside, once per route. When it is stepping
// aside itself, the block doesn't count and the blocked animation plays. Up to 4 blocks I plan again
// if it has moved, else play "CT - Route Blocked Animation" and plan again. Otherwise the first
// block plays the animation, a second plans again if it has moved, and later ones are the last
// resort. For a Sim that stands: one whose Sleep state is below 0 has the first block plan again and
// later ones fail. Otherwise the first block plans again if it has moved, and up to 3 I ask it to
// move and wait, when I'm not waiting on a Sim already. When it refuses, I step aside, once per
// route. After that comes the last resort.
Execution::BlockedStep Execution::routeBlocked(SimObject& sim, Walking& walk, std::int16_t blocker, int depth) {
    RouteBlock& b = walk.block;
    b.step = RouteBlock::Step::None;
    b.askedAt.reset();
    if (depth > kMaxDecisions) {
        diag::warnOnce("routing: a blocked route deciding again too often in one go fails, blocked by a person");
        return routeFailed(sim, walk, kBlockedByPerson, blocker);
    }
    b.tries = static_cast<std::uint16_t>(std::min<int>(b.tries + 1, kMaxTries));
    // Nothing in the way, or a blocker that is gone, fails the route for an unknown reason (1): a
    // walking route has no reason of its own yet.
    if (blocker == 0) {
        return routeFailed(sim, walk, kUnknownReason, 0);
    }
    const float x = tiles(sim.x), y = tiles(sim.y);
    if (blocker != b.blocker) {
        b.blocker = blocker;
        b.tries = 1;
    }
    if (b.lastBlockAt && std::hypot(x - b.lastBlockAt->first, y - b.lastBlockAt->second) >= kMovedDistance) {
        b.tries = 1;
    }
    b.lastBlockAt = std::pair{x, y};
    // A wait on a Sim that is gone is over.
    if (b.waitingOn != 0 && world().find(b.waitingOn) == nullptr) {
        b.waitingOn = 0;
        b.waitInteraction = 0;
    }
    SimObject* other = world().find(blocker);
    if (other == nullptr || other->removed) {
        return routeFailed(sim, walk, kUnknownReason, blocker);
    }
    if (!other->isPerson()) {
        return routeFailed(sim, walk, kBlockedByObject, blocker);
    }
    // A route started inside my post-route tree, and a step-aside, are not my only one. The game
    // fails a route at once with more than two, which can't happen here.
    const bool onlyRoute = !walk.nested && walk.aside == 0;
    const auto planned = std::ranges::find(b.simsAtPlan, blocker, &RouteBlock::SimAt::id);
    const bool known = planned != b.simsAtPlan.end();
    const bool moved = known && (other->x != planned->x || other->y != planned->y);
    if (routing(*other)) {
        const RouteBlock& theirs = other->walking->block;
        if (theirs.blocker == sim.id) {
            if (onlyRoute && b.stepAsides == 0 && b.tries <= 2 && stepAside(sim, walk, *other)) {
                return {true, {}};
            }
            if (onlyRoute && theirs.steppingAside) {
                b.tries = static_cast<std::uint16_t>(b.tries - 1);
                return blockedAnimation(sim, walk);
            }
            if (onlyRoute && b.tries <= 4 && known) {
                return moved ? BlockedStep{true, {}} : blockedAnimation(sim, walk);
            }
            return lastResort(sim, walk);
        }
        if (b.tries <= 1) {
            return blockedAnimation(sim, walk);
        }
        if (b.tries == 2 && moved) {
            return {true, {}};
        }
        return lastResort(sim, walk);
    }
    // A standing Sim whose Sleep state (motive 11) is below 0 is never asked: the first block plans
    // again, later ones fail the route.
    if (motiveValue(*other, kSleepStateMotive) < 0.0f) {
        return b.tries <= 1 ? BlockedStep{true, {}} : routeFailed(sim, walk, kBlockedByPerson, blocker);
    }
    if (b.tries <= 1 && moved) {
        return {true, {}};
    }
    if (b.tries > 3) {
        return lastResort(sim, walk);
    }
    if (b.waitingOn == 0 && askedToMove(sim, walk, *other)) {
        b.step = RouteBlock::Step::AskToMove;
        b.waits = 0;
        b.lastWaitAt.reset();
        return askToMove(sim, walk, depth);
    }
    if (b.waitingOn == 0 && b.stepAsides == 0 && stepAside(sim, walk, *other)) {
        return {true, {}};
    }
    return lastResort(sim, walk);
}

// Back at my walk after a tree a block pushed, or a tick later while I wait for the blocker.
Execution::BlockedStep Execution::blockedTreeReturned(SimObject& sim, Walking& walk) {
    RouteBlock& b = walk.block;
    switch (b.step) {
    case RouteBlock::Step::None:
    case RouteBlock::Step::Animation:
        b.step = RouteBlock::Step::None;
        return {true, {}};
    case RouteBlock::Step::AskToMove:
        return askToMove(sim, walk, 0);
    case RouteBlock::Step::RouteBlocked: {
        const bool goOn = b.returned.value_or(false);
        b.step = RouteBlock::Step::None;
        return goOn ? BlockedStep{true, {}} : routeFailed(sim, walk, kBlockedByPerson, b.blocker);
    }
    case RouteBlock::Step::Feedback:
        break;
    }
    dropWalk(sim);
    return {false, WalkStep{WalkStep::Kind::Done, false}};
}

// "CT - Route Blocked Animation" (global 0x01BB), given how many it has played on this route and the
// blocker. The route is planned again from where I stand once it returns.
Execution::BlockedStep Execution::blockedAnimation(SimObject& sim, Walking& walk) {
    RouteBlock& b = walk.block;
    const std::int16_t args[] = {b.animations, b.blocker};
    b.animations = static_cast<std::int16_t>(std::min<int>(b.animations + 1, std::numeric_limits<std::int16_t>::max()));
    if (auto step = blockedTree(walk, RouteBlock::Step::Animation, kGlobalGroup, kBlockedAnimationTree, sim.id, args)) {
        return {false, std::move(*step)};
    }
    return {true, {}};
}

// Waiting for the Sim I asked to move. The ask is "CT - Route Ask To Move Animation" (global 0x01BC)
// with that Sim as stack object. Then, while fewer than 126 ticks have passed (420 when its object
// data 8 has bit 0x40), "CT - Route Waiting Animation" (global 0x01BD) runs, at most once a tick,
// given how many have run and the distance left to 1.25 tiles in 24ths (at least 1). The route is
// planned again once that Sim's route in the interaction it was given ends, when either of us leaves
// the lot, or when we are more than 1.25 tiles apart. When the time is up the router decides again.
Execution::BlockedStep Execution::askToMove(SimObject& sim, Walking& walk, int depth) {
    RouteBlock& b = walk.block;
    const SimObject* other = world().find(b.waitingOn);
    const float distance = other != nullptr ? distanceBetween(sim, *other) : 0.0f;
    if (other == nullptr || other->x == kOffLot || sim.x == kOffLot || distance > kWaitDistance) {
        if (other == nullptr) {
            b.waitingOn = 0;
            b.waitInteraction = 0;
        }
        b.step = RouteBlock::Step::None;
        b.askedAt.reset();
        return {true, {}};
    }
    const std::uint64_t now = world().ticks();
    if (!b.askedAt) {
        b.askedAt = now;
        if (auto step = blockedTree(walk, RouteBlock::Step::AskToMove, kGlobalGroup, kAskToMoveAnimationTree, other->id, {})) {
            return {false, std::move(*step)};
        }
        return routeBlocked(sim, walk, b.blocker, depth + 1);
    }
    const std::uint64_t limit = (other->data[kFlagsField] & kLongWaitFlag) != 0 ? kLongWaitTicks : kWaitTicks;
    if (now - *b.askedAt < limit) {
        if (b.lastWaitAt == now) {
            return {false, WalkStep{WalkStep::Kind::Wait}};
        }
        b.lastWaitAt = now;
        const int left = std::max(1, static_cast<int>((kWaitDistance - distance) * kWaitSteps));
        const std::int16_t args[] = {b.waits, static_cast<std::int16_t>(left)};
        if (auto step = blockedTree(walk, RouteBlock::Step::AskToMove, kGlobalGroup, kWaitingAnimationTree, sim.id, args)) {
            b.waits = static_cast<std::int16_t>(std::min<int>(b.waits + 1, std::numeric_limits<std::int16_t>::max()));
            return {false, std::move(*step)};
        }
    }
    return routeBlocked(sim, walk, b.blocker, depth + 1);
}

// When nothing else helps: on my only route, "CT - Route Blocked" runs once per route. It is looked
// up by name, as Run Tree by Name finds a tree on me, and for Sims it is a person tree that returns
// true. It runs with the blocker as stack object, given the route's slot owner, the slot as named
// (1000 and up for a global slot), the goal's joint and 0. True plans again, false fails. Otherwise
// the route fails, blocked by a person (14).
Execution::BlockedStep Execution::lastResort(SimObject& sim, Walking& walk) {
    constexpr std::int16_t kFirstGlobalSlot = 1000;
    RouteBlock& b = walk.block;
    if (!walk.nested && walk.aside == 0 && !b.routeBlockedRun) {
        for (const std::uint32_t group : {sim.group, code_.semiGlobalGroup(sim.group), kGlobalGroup}) {
            if (const auto tree = code_.behaviorByName(group, kRouteBlockedTree)) {
                const auto named = static_cast<std::int16_t>(walk.slotGlobal ? walk.slotNumber + kFirstGlobalSlot : walk.slotNumber);
                const std::int16_t args[] = {walk.slotOwner, named, walk.route.goalToken, 0};
                if (auto step = blockedTree(walk, RouteBlock::Step::RouteBlocked, group, *tree, b.blocker, args)) {
                    b.routeBlockedRun = true;
                    return {false, std::move(*step)};
                }
                break;
            }
        }
    }
    return routeFailed(sim, walk, kBlockedByPerson, b.blocker);
}

void Execution::releaseWait(RouteBlock& block) {
    if (block.waitingOn != 0 && block.waitInteraction != 0) {
        if (SimObject* other = world().find(block.waitingOn)) {
            cancelInteraction(*other, block.waitInteraction);
        }
    }
    block.waitingOn = 0;
    block.waitInteraction = 0;
}

void Execution::dropWalk(SimObject& sim) {
    if (sim.walking) {
        releaseWait(sim.walking->block);
        if (sim.walking->outerBlock) {
            releaseWait(*sim.walking->outerBlock);
        }
    }
    sim.walking.reset();
}

void Execution::endAside(Walking& walk) {
    releaseWait(walk.block);
    endStepAside(walk);
}

// My route fails with `reason` (person data 0x3E). On my only route "CT - Route Failure Feedback"
// (global 0x018E) runs first, given the reason, the blocker or object and 1, 1, unless the route's
// flags turn it off. A nested route's failure shows none. The walk is over either way: a route the
// feedback tree starts (it may walk me to a door that blocked me) is nested in this one.
Execution::BlockedStep Execution::routeFailed(SimObject& sim, Walking& walk, std::int16_t reason, std::int16_t object) {
    // A step-aside is quiet: when it fails it shows no feedback and leaves my route result as it is,
    // and the route it stepped aside from is planned again from here, its count of blocks kept.
    if (walk.aside != 0) {
        endAside(walk);
        return {true, {}};
    }
    sim.personData[kRouteResultPersonData] = reason;
    if (!walk.nested && walk.feedback) {
        const std::int16_t args[] = {reason, object, 1, 1};
        if (auto step = blockedTree(walk, RouteBlock::Step::Feedback, kGlobalGroup, kFailureFeedbackTree, sim.id, args)) {
            sim.postRoute = PostRouteWait{walk.frame, walk.group, walk.tree, walk.pc, std::nullopt, true};
            dropWalk(sim);
            return {false, std::move(*step)};
        }
    }
    dropWalk(sim);
    return {false, WalkStep{WalkStep::Kind::Done, false}};
}

// The ask itself, when `other` takes it (see takesTheAsk) and its own route waits on no one: a
// destination is made where it stands, for it to move 1 to 3 tiles away when my route's goal object
// is within 1.22 tiles of it, else to sidestep (see makeWaySlot), and that destination's "Move"
// interaction (number 3 of its table) is queued on it with my priority less 1 (at least 1).
// A full queue refuses the ask, and the destination is removed again. My route then waits on it.
bool Execution::askedToMove(SimObject& sim, Walking& walk, SimObject& other) {
    if (!takesTheAsk(world(), sim, other) || (other.walking && other.walking->block.waitingOn != 0)) {
        return false;
    }
    const SimObject* goal = world().find(walk.aside != 0 ? walk.aside : walk.slotOwner);
    const bool near = goal != nullptr && distanceBetween(*goal, other) < kNearGoal;
    SimObject* made = makeDestination(other, sim, near ? 1 : 0);
    if (made == nullptr) {
        return false;
    }
    const auto table = interactionsOf(code_, *made).table;
    const ttab::Interaction* move = table != nullptr ? ttab::find(*table, kMoveLine) : nullptr;
    if (move == nullptr || other.queue.size() >= kMaxQueuedInteractions) {
        std::erase(other.destinations, made->id);
        world().remove(*made);
        return false;
    }
    QueuedInteraction queued;
    queued.object = made->id;
    queued.group = made->group;
    queued.index = static_cast<std::uint16_t>(kMoveLine);
    queued.action = move->action;
    queued.priority = static_cast<std::int16_t>(askPriority(sim));
    queued.nested = (move->flags & kNestedInteractionFlag) != 0;
    queueInteraction(other, queued);
    walk.block.waitingOn = other.id;
    walk.block.waitInteraction = other.queue.back().id;
    return true;
}

// My own step-aside: a destination where I stand with the sidestep slot (see makeWaySlot) by
// `other`, walked to as a route of its own before my route goes on. False when none can be made.
bool Execution::stepAside(SimObject& sim, Walking& walk, const SimObject& other) {
    const SimObject* made = makeDestination(sim, other, 0);
    if (made == nullptr) {
        return false;
    }
    ++walk.block.stepAsides;
    RouteBlock aside;
    aside.blocker = walk.block.blocker;
    aside.steppingAside = true;
    walk.outerBlock = std::move(walk.block);
    walk.block = std::move(aside);
    walk.aside = made->id;
    return true;
}

// A "Destination - For Go Here & Shoo" at `mover`'s place and level, given the routing slot for
// making way for `other` in `mode` as its slot 0, and kept on `mover`'s list until nothing uses it.
// None when no slot can be made, the type is missing, or `mover` keeps too many already.
SimObject* Execution::makeDestination(SimObject& mover, const SimObject& other, int mode) {
    if (mover.destinations.size() >= kMaxDestinations) {
        diag::warnOnce("routing: a Sim keeping too many destinations to make way makes no more");
        return nullptr;
    }
    auto slot = makeWaySlot(mode, mover, other);
    const ObjectType* type = code_.objectType(destination::kGoHere);
    if (!slot || type == nullptr) {
        return nullptr;
    }
    SimObject& made = world().create(type->group, type->definition, type->containerSlots);
    made.x = mover.x;
    made.y = mover.y;
    made.level = mover.level;
    made.tile = mover.tile;
    made.ownRoutingSlot = std::move(*slot);
    world().updateRoom(made);
    if (startCreated) {
        startCreated(made, *type);
    }
    if (made.removed) {
        return nullptr;
    }
    mover.destinations.push_back(made.id);
    return &made;
}

// The way to my step-aside's spot from where I stand, none when there is none.
std::optional<SlotRoute> Execution::stepAsideRoute(SimObject& sim, const Walking& walk) {
    const SimObject* spot = world().find(walk.aside);
    if (spot == nullptr || !spot->ownRoutingSlot || !world().slotRouter()) {
        return std::nullopt;
    }
    SlotRoute route = world().slotRouter()(sim, *spot, *spot->ownRoutingSlot, {});
    if (route.points.empty()) {
        return std::nullopt;
    }
    return route;
}

// Where every other Sim on the lot stands as my route is planned.
void Execution::rememberSims(Walking& walk) {
    auto& at = walk.block.simsAtPlan;
    at.clear();
    const std::int16_t self = me().id;
    world().nextAfter(std::numeric_limits<std::int16_t>::min(), [&](const SimObject& o) {
        if (o.isPerson() && !o.removed && o.id != self && o.x != kOffLot) {
            at.push_back(RouteBlock::SimAt{o.id, o.x, o.y});
        }
        return at.size() >= kMaxRememberedSims;
    });
}

// A route of `sim` has ended: the Sims waiting for it to make way in the interaction it was given
// stop waiting.
void Execution::routeEnded(const SimObject& sim) {
    std::uint32_t running[2] = {0, 0};
    if (sim.running) {
        running[0] = sim.running->id;
    }
    if (sim.runningSub) {
        running[1] = sim.runningSub->id;
    }
    if (running[0] == 0 && running[1] == 0) {
        return;
    }
    std::vector<std::int16_t> waiting;
    world().nextAfter(std::numeric_limits<std::int16_t>::min(), [&](const SimObject& o) {
        if (o.walking && (o.walking->block.waitingOn == sim.id || (o.walking->outerBlock && o.walking->outerBlock->waitingOn == sim.id))) {
            waiting.push_back(o.id);
        }
        return false;
    });
    for (const std::int16_t id : waiting) {
        SimObject* o = world().find(id);
        for (RouteBlock* b : {o && o->walking ? &o->walking->block : nullptr,
                              o && o->walking && o->walking->outerBlock ? &*o->walking->outerBlock : nullptr}) {
            if (b != nullptr && b->waitingOn == sim.id && b->waitInteraction != 0 &&
                (b->waitInteraction == running[0] || b->waitInteraction == running[1])) {
                b->waitingOn = 0;
                b->waitInteraction = 0;
            }
        }
    }
}

// Where `who` is at `ms` on the world's clock as a sweep sees it: along its walk while it walks
// (not while it turns before or after it or is held up), else where it stands.
SweepPlace Execution::sweepPlace(const SimObject& who, double ms) {
    SweepPlace place{tiles(who.x), tiles(who.y), 0, 0};
    if (!routing(who)) {
        return place;
    }
    const Walking& walk = *who.walking;
    if (!alongPlan(walk) || walk.climbing || walk.block.step != RouteBlock::Step::None) {
        return place;
    }
    const double into = std::clamp(ms - walk.startMs, 0.0, static_cast<double>(walkMs(walk.plan)));
    const WalkedTo at = walkPoint(walk, walkedAt(walk.plan, static_cast<float>(into)));
    place.x = at.x;
    place.y = at.y;
    if (at.heading) {
        place.dx = std::cos(*at.heading);
        place.dy = -std::sin(*at.heading);
    }
    return place;
}

// The look-ahead for other Sims along my walk, run each tick before my step: from the last check to
// ten sweep steps past now (see sweepStepMs), cut at the walk's end, each other Sim on my level that
// can reach me in that time moves along its own walk, or stands. The first one I touch (see
// sweepContact) blocks me. One I already touch at the start is left out while we move apart.
// A Sim in a container (object data 2 not 0) or with object data 8 bit 0x04 or 0x10 set, on either
// side, doesn't count. The game also leaves out Sims with a person flag not known yet, and its sizes
// for pets are not known yet. 0 when the way is clear.
std::int16_t Execution::sweepBlocker(SimObject& sim, Walking& walk, double now) {
    constexpr std::size_t kContainerField = 2;
    constexpr std::int16_t kPassFlags = 0x14;
    const auto passes = [](const SimObject& o) { return o.data[kContainerField] != 0 || (o.data[8] & kPassFlags) != 0; };
    if (!walk.started || walk.arrived || walk.turn || walk.climbing || passes(sim)) {
        return 0;
    }
    const float speed = walkTopSpeed(walk.plan);
    const int step = sweepStepMs(speed);
    const double from = std::max(walk.sweptTo.value_or(walk.startMs), walk.startMs);
    const double end = std::min(std::floor(now + kSweepSteps * step), walk.startMs + static_cast<double>(walkMs(walk.plan)));
    walk.sweptTo = now;
    if (!(end > from)) {
        return 0;
    }
    // The other Sims that can come within touching distance in the window.
    const double windowS = (end - from) / 1000.0;
    std::vector<const SimObject*> near;
    world().nextAfter(std::numeric_limits<std::int16_t>::min(), [&](const SimObject& o) {
        if (!o.isPerson() || o.removed || o.id == sim.id || o.level != sim.level || o.x == kOffLot || passes(o)) {
            return false;
        }
        const float theirs = routing(o) ? walkTopSpeed(o.walking->plan) : 0.0f;
        const float reach = static_cast<float>(windowS) * (speed + theirs) + kSweepFront + kSweepCentres + kSweepReachMargin;
        if (std::hypot(tiles(o.x) - tiles(sim.x), tiles(o.y) - tiles(sim.y)) <= reach) {
            near.push_back(&o);
        }
        return near.size() >= kMaxSweptSims;
    });
    std::vector<std::int16_t> apart;
    double a = from;
    for (int i = 0; i < kMaxSweepSteps && a < end && !near.empty(); ++i) {
        const double b = std::min(a + step, end);
        const SweepPlace mineA = sweepPlace(sim, a), mineB = sweepPlace(sim, b);
        std::optional<float> first;
        const SimObject* hit = nullptr;
        for (const SimObject* o : near) {
            if (std::ranges::find(apart, o->id) != apart.end()) {
                continue;
            }
            const SweepPlace theirA = sweepPlace(*o, a), theirB = sweepPlace(*o, b);
            const auto f = sweepContact(mineA, mineB, theirA, theirB);
            if (!f) {
                continue;
            }
            // Touching already at the start, and moving apart: left out for the rest of the sweep.
            if (a == from && *f * (b - a) < 1.0 &&
                std::hypot(mineA.x - theirA.x, mineA.y - theirA.y) < std::hypot(mineB.x - theirB.x, mineB.y - theirB.y)) {
                apart.push_back(o->id);
                continue;
            }
            if (!first || *f < *first) {
                first = f;
                hit = o;
            }
        }
        if (hit != nullptr) {
            return hit->id;
        }
        a = b;
    }
    return 0;
}

// One of the trees above, run on my stack with `stackObject`, none when the tree is missing.
std::optional<Execution::WalkStep> Execution::blockedTree(Walking& walk, RouteBlock::Step kind, std::uint32_t group, std::uint16_t tree,
                                                          std::int16_t stackObject, std::span<const std::int16_t> args) {
    auto code = code_.behavior(group, tree);
    if (!code) {
        return std::nullopt;
    }
    WalkStep step{WalkStep::Kind::RouteTree};
    step.group = group;
    step.instance = tree;
    step.code = std::move(code);
    step.codeOwner = me().group;
    step.stackObject = stackObject;
    std::copy_n(args.begin(), std::min(args.size(), step.args.size()), step.args.begin());
    walk.block.step = kind;
    walk.block.returned.reset();
    return step;
}

} // namespace ts2::sim::vm_detail
