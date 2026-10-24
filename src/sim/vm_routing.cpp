#include "sim/vm_execution.hpp"

namespace ts2::sim::vm_detail {

namespace {

float tiles(std::int32_t fixed) { return static_cast<float>(fixed) / 65536.0f; }

} // namespace

// Operands:
//   byte 0         0 to the target, 1 straight ahead from the source, others fail
//   byte 1, 2-3    the source object
//   byte 4, 5      where on it: 0 three quarters up its middle, 1 targeting, 2 routing, 3 containment
//                  slot (byte 5 the slot)
//   byte 8, 9-10   the target object (mode 0), bytes 11, 12 where on it
//   byte 15        bit 0: windows don't block
// True when nothing is in the way (temp 0 = 0) or the target is reached (temp 0 = its id). False
// when an object is (its id) or a wall or floor (-1). A multi-tile part traces from its lead.
bool Execution::rayTrace(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    const std::uint8_t mode = o[0];
    if (mode > 1) {
        return false;
    }
    auto object = [&](std::uint8_t scope, std::uint8_t low, std::uint8_t high) {
        SimObject* found = world().find(read(scope, static_cast<std::uint16_t>(low | high << 8), f));
        if (found != nullptr && found->data[kLeadTileField] != 0) {
            found = world().find(found->data[kLeadTileField]);
        }
        if (found == nullptr) {
            throw Stop{Outcome::Error, "could not find object reference", "RayTrace"};
        }
        return found;
    };
    auto point = [&](const SimObject& on, std::uint8_t kind, std::uint8_t slot) {
        if (kind == 1 || kind == 2) {
            throw Stop{Outcome::Unsupported, "RayTrace from a targeting or routing slot not implemented", ""};
        }
        return RayPoint{on.id, static_cast<std::uint8_t>(kind == 3 ? 3 : 0), slot};
    };
    const SimObject* source = object(o[1], o[2], o[3]);
    RayRequest request;
    request.from = point(*source, o[4], o[5]);
    const SimObject* target = nullptr;
    if (mode == 0) {
        target = object(o[8], o[9], o[10]);
        request.to = point(*target, o[11], o[12]);
    }
    // Without a scene (tools running the simulation alone) there is nothing to trace in. The game
    // always has one, so its scripts' "blocked" paths would be taken wrongly.
    const auto& query = world().rayQuery();
    if (!query) {
        throw Stop{Outcome::Unsupported, "RayTrace needs the lot's scene", ""};
    }
    const auto answer = query(request);
    if (!answer) {
        return false;
    }
    if (answer->objectMissing) {
        throw Stop{Outcome::Error, "could not find object reference", "RayTrace: not drawn"};
    }
    if (answer->slotMissing) {
        throw Stop{Outcome::Error, "Slot number out of range", "RayTrace"};
    }
    RayRules rules;
    rules.source = source->id;
    rules.target = target ? target->id : 0;
    rules.sourceIsSlot = request.from.kind != 0;
    rules.targetIsSlot = request.to && request.to->kind != 0;
    rules.windowsIgnored = (o[15] & 0x01) != 0;
    rules.containerOf = [this](std::int16_t id) {
        const SimObject* held = world().find(id);
        return held ? held->data[kContainerData] : std::int16_t{0};
    };
    rules.isWindow = [this](std::int16_t id) {
        const SimObject* hit = world().find(id);
        return hit && hit->definition.field(objd::field::kType) == kWindowObjectType;
    };
    const RayResult result = judgeRay(answer->hits, rules);
    thread_.temps[0] = result.hit;
    return result.clear;
}

// Operands:
//   bytes 0-1  the slot number, or the param or local holding it
//   bytes 2-3  where it comes from: 0 a param, 1 the number (temp 0 with byte 4 bit 1), 2 a global
//              slot, 3 a local. From a param, the number or a local, 1000 and up is global slot n - 1000
//   byte 4     bit 0: no failure feedback tree. Bit 3: the slot may be on another level. These
//              replace the slot's own flags 0x4000 and 0x8000 for this route.
// Walks me to a routing slot of the stack object and is true, or false when there is no route, my
// person data 0x3E then saying why. Waits a tick at a time while I walk. My pre-route tree runs
// before the walk and my post-route tree after it (it sits me in a seat the route reached). A step
// blocked by another Sim or an object stops me where I am, and the router decides what next (see
// routeBlocked). The trees along the route are not run yet.
// With `relative`, the slot is that one on the stack object instead of one my operands name (Go
// To Relative Position).
Execution::WalkStep Execution::goToRoutingSlot(const bhav::Instruction& in, Frame& f, const slot::Entry* relative) {
    constexpr std::int16_t kGoalNotAvailable = 13;
    constexpr std::uint8_t kNoFeedback = 0x01, kSlotFromTemp0 = 0x02, kAnyLevel = 0x08;
    constexpr std::uint32_t kFailureTrees = 0x4000, kOtherLevels = 0x8000;
    constexpr std::int16_t kFirstGlobal = 1000;
    SimObject& sim = me();
    if (!sim.isPerson()) {
        throw Stop{Outcome::Error, "Go To Routing Slot on an object that is not a person", ""};
    }
    const std::size_t frame = thread_.frames.size() - 1;
    // Waiting for my post-route or failure feedback tree here: it has returned, and the route is
    // over (false when the tree said so, or after the feedback). A wait at this depth or deeper for
    // another instruction is left over from a tree left some other way.
    if (sim.postRoute && sim.postRoute->frame >= frame) {
        const bool here = sim.postRoute->frame == frame && sim.postRoute->group == f.group && sim.postRoute->tree == f.instance &&
                          sim.postRoute->pc == f.pc;
        if (!here) {
            sim.postRoute.reset();
        }
    }
    if (sim.postRoute && sim.postRoute->frame == frame) {
        const bool done = !sim.postRoute->failed && sim.postRoute->result.value_or(true);
        if (!done && !sim.postRoute->failed) {
            sim.personData[kRouteResultPersonData] = kPostRouteFailed;
        }
        sim.postRoute.reset();
        return WalkStep{WalkStep::Kind::Done, done};
    }
    // A route started inside my post-route or failure feedback tree is nested in that route.
    const bool nested = sim.postRoute && !sim.postRoute->result && frame > sim.postRoute->frame;
    if (sim.walking && !(sim.walking->frame == frame && sim.walking->group == f.group && sim.walking->tree == f.instance &&
                         sim.walking->pc == f.pc)) {
        dropWalk(sim);
    }
    // The route to the slot my operands name, planned from where I stand, none when there is none
    // (my person data 0x3E then says why).
    struct Planned {
        SlotRoute route;
        std::int16_t owner = 0, named = 0, value = 0;
        bool global = false;
        bool feedback = true;
    };
    // Set when the last plan found no route: the route's failure feedback flag.
    std::optional<bool> noRoute;
    // With `again`, the slot is the one the walk's first plan chose. Otherwise my operands name it.
    const auto plan = [&](const Walking* again) -> std::optional<Planned> {
        noRoute.reset();
        const auto& o = in.operands;
        const SimObject* owner = world().find(again ? again->slotOwner : stackObject());
        if (owner == nullptr || owner->x == kOffLot || owner->y == kOffLot) {
            return std::nullopt;
        }
        if (relative != nullptr) {
            if (!world().slotRouter()) {
                throw Stop{Outcome::Unsupported, "routing needs the lot's route grid", ""};
            }
            Planned planned{world().slotRouter()(sim, *owner, *relative, {})};
            planned.feedback = (relative->routingFlags & kFailureTrees) != 0;
            sim.personData[kRouteResultPersonData] = planned.route.points.empty() ? planned.route.failure : 0;
            if (planned.route.points.empty()) {
                noRoute = planned.feedback;
                return std::nullopt;
            }
            planned.owner = owner->id;
            planned.named = -1;
            planned.value = -1;
            return planned;
        }
        std::int16_t value = 0;
        bool global = false;
        if (again) {
            value = again->slotNumber;
            global = again->slotGlobal;
        } else {
            const auto number = static_cast<std::int16_t>(o[0] | o[1] << 8);
            const auto source = static_cast<std::int16_t>(o[2] | o[3] << 8);
            global = source == 2;
            switch (source) {
            case 0: value = slotAt(f.params, number, "param"); break;
            case 1: value = (o[4] & kSlotFromTemp0) ? thread_.temps[0] : number; break;
            case 2: value = number; break;
            case 3: value = slotAt(f.locals, number, "local"); break;
            default: throw Stop{Outcome::Error, "invalid routing slot specified", std::to_string(source)};
            }
            if (!global && value >= kFirstGlobal) {
                global = true;
                value = static_cast<std::int16_t>(value - kFirstGlobal);
            }
        }
        const ObjectType* type = global ? nullptr : code_.objectType(owner->definition.guid());
        const auto& slots = global ? code_.globalSlots() : (type ? type->routingSlots : code_.globalSlots());
        // An object given a routing slot of its own uses it as its slot 0.
        const bool own = !global && value == 0 && owner->ownRoutingSlot;
        if ((!global && type == nullptr) || value < 0 || (!own && static_cast<std::size_t>(value) >= slots.size())) {
            throw Stop{Outcome::Error, "invalid routing slot specified", std::to_string(value)};
        }
        slot::Entry entry = own ? *owner->ownRoutingSlot : slots[static_cast<std::size_t>(value)];
        entry.routingFlags = (entry.routingFlags & ~(kFailureTrees | kOtherLevels)) | ((o[4] & kNoFeedback) ? 0 : kFailureTrees) |
                             ((o[4] & kAnyLevel) ? kOtherLevels : 0);
        if (!world().slotRouter()) {
            throw Stop{Outcome::Unsupported, "routing needs the lot's route grid", ""};
        }
        const auto joints = slotJoints(*owner, entry, global ? kGlobalGroup : type->group);
        Planned planned{world().slotRouter()(sim, *owner, entry, joints)};
        planned.feedback = (entry.routingFlags & kFailureTrees) != 0;
        SlotRoute& route = planned.route;
        sim.personData[kRouteResultPersonData] = route.points.empty() ? route.failure : 0;
        if (route.points.empty()) {
            noRoute = planned.feedback;
            return std::nullopt;
        }
        planned.named = static_cast<std::int16_t>(global ? value + kFirstGlobal : value);
        planned.owner = owner->id;
        planned.global = global;
        planned.value = value;
        sim.personData[kRouteOwnerPersonData] = owner->id;
        sim.personData[kRouteSlotPersonData] = planned.named;
        sim.personData[kRouteJointPersonData] = route.goalToken;
        return planned;
    };
    // A plan that found no route fails the route as a block does (see routeFailed), with the reason
    // the planner gave (1, unknown, when it gave none) and no blocker. A missing or off-lot slot
    // owner just makes the primitive false.
    const auto unplanned = [&](Walking& walk) -> WalkStep {
        constexpr std::int16_t kUnknownReason = 1;
        if (!noRoute) {
            dropWalk(sim);
            return WalkStep{WalkStep::Kind::Done, false};
        }
        walk.feedback = *noRoute;
        const std::int16_t reason = sim.personData[kRouteResultPersonData];
        return routeFailed(sim, walk, reason != 0 ? reason : kUnknownReason, 0).step;
    };
    // While I should be interrupted (see interruptPending) no route is planned: it ends interrupted.
    if (!sim.walking) {
        if (interruptPending(sim)) {
            return routeInterrupted(sim);
        }
        auto planned = plan(nullptr);
        if (!planned) {
            Walking walk{frame, f.group, f.instance, f.pc, {}};
            walk.nested = nested;
            return unplanned(walk);
        }
        const SimObject* owner = world().find(planned->owner);
        const bool global = planned->global;
        const std::int16_t value = planned->value, named = planned->named;
        Walking walk{frame, f.group, f.instance, f.pc, std::move(planned->route)};
        walk.nested = nested;
        walk.feedback = planned->feedback;
        walk.slotOwner = planned->owner;
        walk.slotNumber = value;
        walk.slotGlobal = global;
        walk.fromX = static_cast<float>(sim.x) / 65536.0f;
        walk.fromY = static_cast<float>(sim.y) / 65536.0f;
        cutAtStair(walk);
        const float length = distanceLeft(walk.route.points, 1, walk.fromX, walk.fromY);
        // Already there: the route ends at once, with no pre- or post-route tree and no walk.
        if (!(length > 0) && !walk.ahead) {
            return WalkStep{WalkStep::Kind::Done, true};
        }
        rememberSims(walk);
        sim.walking = std::move(walk);
        // My pre-route tree runs first (it stops my idles and decides whether I run), given the
        // object, the slot as named and its number (-1 for a global slot), and the route's length
        // rounded to a tile. What the game passes as the third value (0 or 1) and why the fourth
        // is sometimes another object isn't known yet. 0 and the object are passed.
        if (auto pre = routeTree(sim, kPreRouteEntry)) {
            WalkStep step{WalkStep::Kind::PreRoute};
            step.group = pre->group;
            step.instance = pre->instance;
            step.code = std::move(pre->code);
            const auto rounded = static_cast<std::int16_t>(std::clamp(std::lround(length), 0L, 32767L));
            step.args = {owner->id, named, 0, owner->id, static_cast<std::int16_t>(global ? -1 : value), rounded, 0};
            return step;
        }
        sim.walking->preRoute = true;
    }
    // Once my pre-route tree has returned: false gives up the walk, as it gives up a turn.
    if (!sim.walking->started) {
        Walking& walk = *sim.walking;
        if (!walk.preRoute || !*walk.preRoute) {
            dropWalk(sim);
            return WalkStep{WalkStep::Kind::Done, false};
        }
        // When my pre-route tree has moved me (a seated Sim stands up), the route is planned again
        // from where I stand, as the game's router does after a move of more than 0.01 tiles. It
        // doesn't run my pre-route tree again.
        const float x = static_cast<float>(sim.x) / 65536.0f, y = static_cast<float>(sim.y) / 65536.0f;
        constexpr float kMovedSquared = 1e-4f;
        if ((x - walk.fromX) * (x - walk.fromX) + (y - walk.fromY) * (y - walk.fromY) > kMovedSquared) {
            auto again = plan(&walk);
            if (!again) {
                return unplanned(walk);
            }
            walk.route = std::move(again->route);
            rememberSims(walk);
            cutAtStair(walk);
            walk.fromX = x;
            walk.fromY = y;
            if (!(distanceLeft(walk.route.points, 1, x, y) > 0) && !walk.ahead) {
                dropWalk(sim);
                return WalkStep{WalkStep::Kind::Done, true};
            }
        }
        // The goal check runs before I set off. False fails the route, the goal not available.
        if (!walk.startChecked) {
            walk.startChecked = true;
            if (auto check = goalCheck(sim, f, distanceLeft(walk.route.points, 1, x, y))) {
                return *check;
            }
        }
        if (walk.checking) {
            walk.checking = false;
            if (!walk.goalCheck.value_or(true)) {
                return routeFailed(sim, walk, kGoalNotAvailable, 0).step;
            }
        }
        // The walk is planned as the game plans it (see walk_plan.hpp): start, walk loop and
        // stop clips laid along the route, and the speed that carries me along it.
        const double now = world().clockMs();
        walk.startMs = now;
        walk.started = true;
        planWalkFor(sim, walk);
        // A turn too small for a clip is made at once.
        if (walk.turn && turnOnTheSpot(sim, walk, now)) {
            walk.turn.reset();
        }
        if (!walk.turn) {
            followWalk(sim, walk, 0, now);
        }
        return WalkStep{WalkStep::Kind::Wait};
    }
    Walking& walk = *sim.walking;
    const double now = world().clockMs();
    // After a block, the route is planned again from where I stand: during a step-aside to its spot,
    // else to the slot the first plan chose. A step-aside with no way to its spot, or already there,
    // is over, and the route it stepped aside from is planned again.
    const auto planAgain = [&]() -> WalkStep {
        if (interruptPending(sim)) {
            return routeInterrupted(sim);
        }
        std::optional<SlotRoute> route = walk.aside != 0 ? stepAsideRoute(sim, walk) : std::nullopt;
        if (walk.aside != 0 && (!route || !(distanceLeft(route->points, 1, tiles(sim.x), tiles(sim.y)) > 0))) {
            endAside(walk);
            route.reset();
        }
        if (walk.aside == 0) {
            auto again = plan(&walk);
            if (!again) {
                return unplanned(walk);
            }
            route = std::move(again->route);
        }
        walk.route = std::move(*route);
        rememberSims(walk);
        walk.ahead.reset();
        cutAtStair(walk);
        walk.fromX = tiles(sim.x);
        walk.fromY = tiles(sim.y);
        walk.arrived = false;
        walk.turn.reset();
        walk.turnAfter.reset();
        if (!(distanceLeft(walk.route.points, 1, walk.fromX, walk.fromY) > 0) && !walk.ahead) {
            dropWalk(sim);
            return WalkStep{WalkStep::Kind::Done, true};
        }
        walk.startMs = now;
        walk.checkAt = 1;
        planWalkFor(sim, walk);
        if (walk.turn && turnOnTheSpot(sim, walk, now)) {
            walk.turn.reset();
        }
        if (!walk.turn) {
            followWalk(sim, walk, 0, now);
        }
        return WalkStep{WalkStep::Kind::Wait};
    };
    const auto next = [&](BlockedStep step) { return step.planAgain ? planAgain() : std::move(step.step); };
    // Waiting on a Sim asked to move, or after the blocked animation, an interruption ends the route.
    if (walk.block.step != RouteBlock::Step::None) {
        const bool waits = walk.block.step == RouteBlock::Step::AskToMove || walk.block.step == RouteBlock::Step::Animation;
        if (waits && interruptPending(sim)) {
            return routeInterrupted(sim);
        }
        return next(blockedTreeReturned(sim, walk));
    }
    // Back from a stair's Portal tree: false fails the route, interrupted by the tree (0x10). My data
    // 0x44 flag 0x40 set by the tree plans it again from where I stand. Otherwise the walk goes on
    // from the stair's exit.
    if (walk.climbing) {
        constexpr std::size_t kRouteFlagsField = 0x44;
        constexpr std::int16_t kPlanAgain = 0x40, kStairFailed = 0x10;
        walk.climbing = false;
        const bool climbed = walk.climbed.value_or(false);
        walk.climbed.reset();
        if (!climbed) {
            return next(routeFailed(sim, walk, kStairFailed, 0));
        }
        if (sim.data[kRouteFlagsField] & kPlanAgain) {
            sim.data[kRouteFlagsField] = static_cast<std::int16_t>(sim.data[kRouteFlagsField] & ~kPlanAgain);
            auto again = plan(&walk);
            if (!again) {
                return unplanned(walk);
            }
            walk.route = std::move(again->route);
            rememberSims(walk);
        } else if (walk.ahead) {
            walk.route = std::move(*walk.ahead);
        }
        walk.ahead.reset();
        cutAtStair(walk);
        walk.fromX = static_cast<float>(sim.x) / 65536.0f;
        walk.fromY = static_cast<float>(sim.y) / 65536.0f;
        walk.arrived = false;
        walk.turn.reset();
        walk.turnAfter.reset();
        if (distanceLeft(walk.route.points, 1, walk.fromX, walk.fromY) > 0) {
            walk.startMs = now;
            walk.checkAt = 1;
            planWalkFor(sim, walk);
            if (walk.turn && turnOnTheSpot(sim, walk, now)) {
                walk.turn.reset();
            }
            if (!walk.turn) {
                followWalk(sim, walk, 0, now);
            }
            return WalkStep{WalkStep::Kind::Wait};
        }
        walk.arrived = true;
    }
    // Interrupted as I walk (see interruptPending), the walk stops soon (see interruptWalk) and the
    // route then ends interrupted, with no goal checks, look-ahead or post-route tree.
    if (!walk.interrupted && interruptPending(sim)) {
        interruptWalk(sim, walk, now);
        if (walk.stopped) {
            return routeInterrupted(sim);
        }
    }
    // A turn on the spot before the walk (it starts once the turn is over) or after it.
    if (walk.turn) {
        if (!turnOnTheSpot(sim, walk, now)) {
            return WalkStep{WalkStep::Kind::Wait};
        }
        walk.turn.reset();
        if (walk.interrupted) {
            return routeInterrupted(sim);
        }
        walk.startMs = now;
        if (!walk.arrived) {
            followWalk(sim, walk, 0, now);
            return WalkStep{WalkStep::Kind::Wait};
        }
    }
    // Each tile walked the goal check runs again, given what is left. False fails the route where I
    // am, the goal not available.
    if (walk.checking) {
        walk.checking = false;
        if (!walk.interrupted && !walk.goalCheck.value_or(true)) {
            return next(routeFailed(sim, walk, kGoalNotAvailable, 0));
        }
    }
    if (!walk.arrived && !walk.interrupted) {
        const float walked = walkedAt(walk.plan, static_cast<float>(std::max(0.0, now - walk.startMs)));
        if (walked >= walk.checkAt && walked < walk.plan.length) {
            while (walk.checkAt <= walked) {
                walk.checkAt += 1;
            }
            if (auto check = goalCheck(sim, f, walk.plan.length - walked)) {
                return *check;
            }
        }
    }
    // The look-ahead for other Sims runs before the step (see sweepBlocker).
    if (!walk.arrived && !walk.interrupted) {
        if (const std::int16_t ahead = sweepBlocker(sim, walk, now)) {
            stopWalk(sim, walk, now);
            return next(routeBlocked(sim, walk, ahead));
        }
    }
    if (!walk.arrived) {
        std::int16_t blocker = 0;
        const bool there = walkTo(sim, walk, now, blocker);
        if (blocker != 0) {
            stopWalk(sim, walk, now);
            return walk.interrupted ? routeInterrupted(sim) : next(routeBlocked(sim, walk, blocker));
        }
        if (!there) {
            return WalkStep{WalkStep::Kind::Wait};
        }
    }
    if (walk.interrupted) {
        return routeInterrupted(sim);
    }
    if (walk.turnAfter) {
        walk.turn = Walking::Turn{walk.endHeading, *walk.turnAfter};
        walk.turnAfter.reset();
        if (!turnOnTheSpot(sim, walk, now)) {
            return WalkStep{WalkStep::Kind::Wait};
        }
        walk.turn.reset();
    }
    // A step-aside that got there is over: the route it stepped aside from goes on from here.
    if (walk.aside != 0) {
        endAside(walk);
        return planAgain();
    }
    // At a stair's entry its Portal tree climbs it.
    if (walk.ahead) {
        return climbStair(sim, walk);
    }
    // My post-route tree then runs, after a walk my pre-route tree let start, given the seat
    // reached (which it sits me in) and the route's result.
    const bool nestedWalk = walk.nested;
    const std::int16_t seat = walk.route.goalObject;
    dropWalk(sim);
    if (auto post = nestedWalk ? std::nullopt : routeTree(sim, kPostRouteEntry)) {
        sim.postRoute = PostRouteWait{frame, f.group, f.instance, f.pc, std::nullopt};
        WalkStep step{WalkStep::Kind::PostRoute};
        step.group = post->group;
        step.instance = post->instance;
        step.code = std::move(post->code);
        step.args = {seat, 0, 0, 0, 0, 0, 0};
        return step;
    }
    return WalkStep{WalkStep::Kind::Done, true};
}

// A stair's Portal tree (OBJf entry 15 of its entry part, else its exit part) for me to run at its
// entry, with the entry part as stack object. It is given the rooms at the entry and the exit, their
// levels, parts and slots, and 1 (walking). The tree clears my data 0x44 flag 0x40 first.
Execution::WalkStep Execution::climbStair(SimObject& sim, Walking& walk) {
    constexpr std::size_t kPortalFunction = 15, kRouteFlagsField = 0x44;
    constexpr std::int16_t kPlanAgain = 0x40, kStairFailed = 0x10, kWalking = 1;
    const StairCrossing& stair = walk.route.stairs.front();
    const SimObject* entry = world().find(stair.entryPart);
    const SimObject* exit = world().find(stair.exitPart);
    const auto portalTree = [&](const SimObject* part) -> std::uint16_t {
        const ObjectType* type = part != nullptr ? code_.objectType(part->definition.guid()) : nullptr;
        const auto* functions = type != nullptr ? code_.objectFunctions(*type) : nullptr;
        return functions != nullptr && kPortalFunction < functions->size() ? (*functions)[kPortalFunction].action : 0;
    };
    const SimObject* owner = portalTree(entry) != 0 ? entry : exit;
    const std::uint16_t tree = portalTree(owner);
    if (entry == nullptr || exit == nullptr || tree == 0 || !walk.ahead || walk.ahead->points.empty() || walk.route.points.empty()) {
        sim.personData[kRouteResultPersonData] = kStairFailed;
        dropWalk(sim);
        return WalkStep{WalkStep::Kind::Done, false};
    }
    WalkStep step{WalkStep::Kind::Stair};
    step.instance = tree;
    step.group = groupFor(tree, owner->group);
    step.code = code_.behavior(step.group, tree);
    if (!step.code) {
        throw Stop{Outcome::MissingCode, "BHAV not found", hex(step.group, 8) + " " + hex(tree, 4)};
    }
    step.codeOwner = owner->group;
    step.stackObject = entry->id;
    const auto room = [&](int level, const std::pair<float, float>& at) {
        return static_cast<std::int16_t>(world().rooms().roomAt(level - 1, at.first, at.second));
    };
    step.args = {room(stair.entryLevel, walk.route.points.back()),
                 room(stair.exitLevel, walk.ahead->points.front()),
                 static_cast<std::int16_t>(stair.entryLevel),
                 static_cast<std::int16_t>(stair.exitLevel),
                 entry->id,
                 exit->id,
                 static_cast<std::int16_t>(stair.entrySlot),
                 static_cast<std::int16_t>(stair.exitSlot),
                 kWalking};
    sim.data[kRouteFlagsField] = static_cast<std::int16_t>(sim.data[kRouteFlagsField] & ~kPlanAgain);
    walk.climbing = true;
    walk.climbed.reset();
    return step;
}

// Go To Relative Position 0x1B: walks me to a spot by the stack object as Go To Routing Slot walks
// to a slot, the spot given as a routing slot made from the operands:
//   byte 2  where: -2 on top of the object (its own spot), -1 anywhere near, 0-7 a side (0 in
//           front, 2 right, 4 behind, 6 left, odd ones both neighbouring sides)
//   byte 3  my facing at the end: -2 facing the object, -1 any, 0-7 relative to its facing
//   byte 6  bit 1 no failure trees, bit 2 a different level allowed
// False when the stack object is missing or off the lot, or byte 3 is out of range.
Execution::WalkStep Execution::goToRelativePosition(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    const auto where = static_cast<std::int8_t>(o[2]);
    const auto facing = static_cast<std::int8_t>(o[3]);
    if (facing < -2 || facing > 7) {
        return WalkStep{WalkStep::Kind::Done, false};
    }
    constexpr std::uint32_t kFailureTrees = 0x4000, kOtherLevels = 0x8000;
    slot::Entry entry;
    entry.type = slot::kRoutingSlot;
    entry.routingInts = {1, 0, 0};
    float distance = 1.0f;
    std::uint32_t sides = 0;
    switch (where) {
    case -2: sides = 0; break;
    case 0: case 2: case 4: case 6: sides = 1u << where; break;
    case 1: sides = 0x05; break;
    case 3: sides = 0x14; break;
    case 5: sides = 0x50; break;
    case 7: sides = 0x41; break;
    default: sides = 0x55; distance = 2.0f; break;
    }
    entry.routingFlags = sides | ((o[6] & 0x02) ? 0 : kFailureTrees) | ((o[6] & 0x04) ? kOtherLevels : 0);
    entry.minDistance = 1.0f;
    entry.maxDistance = distance;
    entry.optimalDistance = 1.0f;
    entry.facing = facing == -1 ? -3 : facing;
    return goToRoutingSlot(in, f, &entry);
}

// Moves me as far along the route as the walk's speed has carried me, facing the route's way
// there. True once there, on the route's last point exactly (its goal, which later goals
// compare with my spot), facing the way the walk ends in and holding the stop's last pose (else
// the walk's standing pose). A walk cut short to stop soon ends where its stop does. Each step
// must be free, as the game tests a walking Sim's place every tick: when a Sim or an object is in
// the way, `blocker` is its id and I stay where I am.
bool Execution::walkTo(SimObject& sim, Walking& walk, double now, std::int16_t& blocker) {
    const float ms = static_cast<float>(std::max(0.0, now - walk.startMs));
    const float walked = walkedAt(walk.plan, ms);
    const WalkedTo at = walkPoint(walk, walked);
    if (const auto& inTheWay = world().stepBlocker()) {
        blocker = inTheWay(sim, at.x, at.y, sim.level);
        if (blocker != 0) {
            return false;
        }
    }
    if (at.heading && !walk.plan.keepsFacing) {
        faceHeading(sim, *at.heading);
    }
    sim.x = static_cast<std::int32_t>(std::lround(at.x * 65536.0f));
    sim.y = static_cast<std::int32_t>(std::lround(at.y * 65536.0f));
    if (at.x >= 0 && at.y >= 0 && at.x < 256 && at.y < 256) {
        sim.tile = Tile{static_cast<std::uint8_t>(at.x), static_cast<std::uint8_t>(at.y), static_cast<std::uint8_t>(sim.level)};
        world().updateRoom(sim);
    }
    if (ms < walkMs(walk.plan)) {
        followWalk(sim, walk, walked, now);
        return false;
    }
    // The foot my stop clip began on stays for a walk begun right after (see planWalkFor).
    sim.lastWalkEnd.reset();
    if (!walk.plan.segments.empty() && walk.plan.segments.back().kind == WalkSegment::Kind::Stop) {
        const LocoPair& stop = walk.plan.segments.back().clips;
        sim.lastWalkEnd = SimObject::WalkEnd{now, stop.mirrored ? std::fmod(stop.clip.loopPhase + 0.5f, 1.0f) : stop.clip.loopPhase};
    }
    // A walk cut short to stop soon ends where its stop does, facing along the path.
    if (!walk.stopsSoon) {
        if (!walk.route.points.empty()) {
            const auto& [endX, endY] = walk.route.points.back();
            sim.x = static_cast<std::int32_t>(std::lround(endX * 65536.0f));
            sim.y = static_cast<std::int32_t>(std::lround(endY * 65536.0f));
        }
        faceHeading(sim, walk.endHeading);
    }
    endWalk(sim, walk, now);
    walk.arrived = true;
    return true;
}

// A turn on the spot within a walk, as Turn Body Towards turns: the turn clip plays and my
// facing follows its root. It is true once I face the turn's end. A turn too small for a clip faces
// its end at once (what the game does then is not known yet).
bool Execution::turnOnTheSpot(SimObject& sim, Walking& walk, double now) {
    Walking::Turn& turn = *walk.turn;
    if (!turn.started) {
        turn.started = true;
        const auto clip = turnClip(turn.from, turn.to, sim.personData[kLifeStagePersonData]);
        const auto& lengths = world().animationLengths();
        const auto length = clip && lengths ? lengths(clip->name) : std::nullopt;
        if (!length) {
            faceHeading(sim, turn.to);
            return true;
        }
        PlayingAnimation play;
        play.name = clip->name;
        play.startMs = now;
        play.speed = kTurnInPlaceSpeed;
        play.lengthMs = *length / kTurnInPlaceSpeed;
        play.flipped = clip->mirrored;
        play.baseHeading = turn.from;
        sim.animation = std::move(play);
        faceHeading(sim, turn.from);
        return false;
    }
    if (!sim.animation || sim.animation->finished(now)) {
        faceHeading(sim, turn.to);
        return true;
    }
    if (const auto& turns = world().animationTurns()) {
        const PlayingAnimation& clip = *sim.animation;
        if (const auto root = turns(clip.name, static_cast<float>((now - clip.startMs) * clip.speed))) {
            faceHeading(sim, turn.from + (clip.flipped ? -*root : *root));
        }
    }
    return false;
}

// The walk set's clips for my age and sex, as the planner reads them.
std::optional<LocoClip> Execution::locoClip(const std::string& name) {
    const auto& clips = world().locoClips();
    return clips && !name.empty() ? clips(name) : std::nullopt;
}

namespace {

// The walk set's fastest walk loop speed.
float topLoopSpeed(const std::optional<LocoClip>& loopShort, const std::optional<LocoClip>& loop) {
    const float top = std::max(loopShort ? loopShort->speed : 0.0f, loop ? loop->speed : 0.0f);
    return top > 0 ? top : kFallbackLoopSpeed;
}

// Each walk style's share of my walk loop, from person data in percent (each taken within 0-100):
// 0x70 and 0x71 are the two plain walks', 0x72 names a personality style (2-19) that takes 0x73,
// and 0x74 a mood style (2-19) that gets 0x75 on top, coming off all the others.
std::array<float, 20> walkStyleShares(const SimObject& sim) {
    constexpr std::size_t kPlain = 0x70, kPersonality = 0x72, kPersonalityShare = 0x73, kMood = 0x74, kMoodShare = 0x75;
    const auto percent = [&](std::size_t at) { return std::clamp(sim.personData[at] * 0.01f, 0.0f, 1.0f); };
    std::array<float, 20> shares{};
    const float mood = percent(kMoodShare), rest = 1 - mood;
    shares[0] = percent(kPlain) * rest;
    shares[1] = percent(kPlain + 1) * rest;
    const int personality = sim.personData[kPersonality], moodStyle = sim.personData[kMood];
    if (personality >= 2 && personality < static_cast<int>(shares.size())) {
        shares[static_cast<std::size_t>(personality)] = percent(kPersonalityShare) * rest;
    }
    if (moodStyle >= 2 && moodStyle < static_cast<int>(shares.size())) {
        shares[static_cast<std::size_t>(moodStyle)] += mood;
    }
    return shares;
}

} // namespace

// A start's or stop's clips: the short one, mixed with the long one.
std::optional<LocoPair> Execution::locoPair(const std::optional<WalkTurnClips>& names) {
    if (!names) {
        return std::nullopt;
    }
    const auto shortOne = locoClip(names->shortOne), full = locoClip(names->full);
    if (!shortOne && !full) {
        return std::nullopt;
    }
    LocoPair pair;
    pair.clip = shortOne ? *shortOne : *full;
    if (shortOne && full) {
        pair.alt = *full;
    }
    pair.mirrored = names->mirrored;
    return pair;
}

// Plans my walk along its route: its path from my facing to the slot's facing (else the way the
// route's last stretch runs), started and stopped by the clips that turn from my facing onto the
// path and from the path's way at its end into the facing I end in.
void Execution::planWalkFor(const SimObject& sim, Walking& walk) {
    walk.stopped = false;
    const auto& points = walk.route.points;
    const float facing = sim.heading ? *sim.heading : compassHeading(sim.data[kDirectionField]);
    float in = facing;
    for (std::size_t i = points.size(); i >= 2; --i) {
        const auto& a = points[i - 2];
        const auto& b = points[i - 1];
        if (std::hypot(b.first - a.first, b.second - a.second) > kWalkSliver) {
            in = std::atan2(-(b.second - a.second), b.first - a.first);
            break;
        }
    }
    const std::int16_t lifeStage = sim.personData[kLifeStagePersonData];
    // From where I stand along the route's points, each once (a point repeated adds no stretch).
    std::vector<std::pair<float, float>> way{{walk.fromX, walk.fromY}};
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (std::hypot(points[i].first - way.back().first, points[i].second - way.back().second) > kSamePoint) {
            way.push_back(points[i]);
        }
    }
    if (way.size() == 1 && points.size() > 1) {
        way.push_back(points.back());
    }
    // Short routes may turn on the spot before or after the walk (see routeShape).
    const auto stepNames = stepClips(0, lifeStage);
    const bool hasSteps = stepNames && locoClip(stepNames->full);
    const RouteShape shape = routeShape(way, facing, walk.route.facing ? compassHeading(*walk.route.facing) : in, hasSteps);
    if (std::abs(wrapAngle(shape.turnTo - facing)) > kSameHeading) {
        walk.turn = Walking::Turn{facing, shape.turnTo};
    }
    walk.turnAfter = shape.turnAfter;
    const float facingThen = shape.walkFrom;
    walk.endHeading = shape.walkTo;
    walk.startHeading = facingThen;
    walk.path = WalkPath(way, facingThen, walk.endHeading, shape.straight);
    float onto = facingThen;
    if (const WalkedTo first = walkPoint(walk, kWalkSliver); first.heading) {
        onto = *first.heading;
    }
    if (const WalkedTo end = walkPoint(walk, std::max(0.0f, walk.path.length() - kWalkSliver)); end.heading) {
        in = *end.heading;
    }
    const std::int16_t stage = sim.personData[kLifeStagePersonData], gender = sim.personData[kGenderPersonData];
    WalkPlanInput input;
    input.length = walk.path.length();
    // A short straight move that keeps my facing is a step.
    if (!points.empty()) {
        const float dx = points.back().first - walk.fromX, dy = points.back().second - walk.fromY;
        if (takesStep(input.length, std::hypot(dx, dy), walk.endHeading - facingThen)) {
            const auto clipAt = [&](float angle) -> std::optional<StepClip> {
                const auto names = stepClips(angle, stage);
                auto clips = locoPair(names);
                return clips ? std::optional<StepClip>(StepClip{*clips, names->turn}) : std::nullopt;
            };
            const auto& scale = world().simScale();
            const float move = wrapAngle(std::atan2(-dy, dx) - facingThen);
            if (auto step = planStep(input.length, move, scale ? scale(sim.id) : 1.0f, clipAt, locoClip(stepInPlaceClip(stage)))) {
                walk.plan = std::move(*step);
                return;
            }
        }
    }
    // A walk from standing begins on foot 0.5 (see turnClipsForFoot), or, begun within 140 ms of my
    // last walk's end in its stop clip, on the foot that stop began on. Its start is picked for that
    // foot, and so is its stop, which is also picked for the other foot (see WalkPlanInput).
    constexpr double kCarryFootMs = 140;
    float foot = 0.5f;
    if (sim.lastWalkEnd && std::abs(world().clockMs() - sim.lastWalkEnd->ms) < kCarryFootMs) {
        foot = sim.lastWalkEnd->foot;
    }
    const auto turnClips = [&](const std::optional<WalkTurnClips>& names, float foot) -> std::optional<LocoPair> {
        if (!names) {
            return std::nullopt;
        }
        std::vector<LocoClip> clips;
        for (const auto& name : {names->shortOne, names->full, names->shortOtherFoot, names->fullOtherFoot}) {
            if (const auto c = locoClip(name)) {
                clips.push_back(*c);
            }
        }
        return turnClipsForFoot(clips, names->turn, foot);
    };
    input.start = turnClips(walkStartClips(facingThen, onto, stage), foot);
    const auto stopNames = walkStopClips(in, walk.endHeading, stage);
    input.stop = turnClips(stopNames, foot);
    input.stopOtherFoot = turnClips(stopNames, std::abs(foot - 0.5f));
    const auto loopShort = locoClip(walkShortClip(stage, gender)), loop = locoClip(walkClip(stage, gender));
    // The walk loop's styles by their shares (see walkStyleShares). A style the set lacks for my age
    // counts for nothing. The walk's speed is their long loops' together.
    const auto shares = walkStyleShares(sim);
    float loopSpeed = 0, counted = 0;
    for (std::size_t style = 0; style < shares.size(); ++style) {
        if (style != 0 && !(shares[style] > 0)) {
            continue;
        }
        const int i = static_cast<int>(style);
        const auto shortOne = locoClip(walkStyleClip(stage, gender, i, true)), longOne = locoClip(walkStyleClip(stage, gender, i, false));
        if (!shortOne && !longOne) {
            continue;
        }
        LoopStyle s{{shortOne ? *shortOne : *longOne, std::nullopt, false}, shares[style]};
        if (shortOne && longOne) {
            s.clips.alt = *longOne;
        }
        input.loops.push_back(s);
        loopSpeed += s.share * (longOne ? longOne->speed : shortOne->speed);
        counted += s.share;
    }
    // The set's speeds: its fastest walk loop, and its fastest start or stop that doesn't turn.
    const float topLoop = topLoopSpeed(loopShort, loop);
    float topStraight = 0;
    for (const auto& names : {walkStartClips(0, 0, stage), walkStopClips(0, 0, stage)}) {
        if (names) {
            for (const auto& name : {names->shortOne, names->full}) {
                if (const auto c = locoClip(name)) {
                    topStraight = std::max(topStraight, c->speed);
                }
            }
        }
    }
    if (!(topStraight > 0)) {
        topStraight = kFallbackStraightShare * topLoop;
    }
    input.speed = walkSpeed(counted > 0 && loopSpeed > 0 ? loopSpeed / counted : loop && loop->speed > 0 ? loop->speed : topLoop, input.length);
    input.topLoopSpeed = topLoop;
    input.topStraightSpeed = topStraight;
    if (const auto& scale = world().simScale()) {
        input.scale = scale(sim.id);
    }
    walk.plan = planWalk(input);
}

// The walk's part at `walked` tiles, and its clip as my animation (for scripts, as the walk is
// drawn from the plan itself): the start, the walk loop, or the stop.
void Execution::followWalk(SimObject& sim, Walking& walk, float walked, double now) {
    using Kind = WalkSegment::Kind;
    const WalkSegment* part = nullptr;
    for (const auto& s : walk.plan.segments) {
        if ((s.kind == Kind::Start && walked < s.end) || ((s.kind == Kind::Stop || s.kind == Kind::Step) && walked >= s.start)) {
            part = &s;
            break;
        }
    }
    if (part == nullptr) {
        for (const auto& s : walk.plan.segments) {
            if (s.kind == Kind::Loop) {
                part = &s;
            }
        }
    }
    const Walking::Phase phase = !part ? Walking::Phase::Walk
                               : part->kind == Kind::Start ? Walking::Phase::Start
                               : part->kind == Kind::Stop || part->kind == Kind::Step ? Walking::Phase::Stop
                                                           : Walking::Phase::Walk;
    if (sim.animation && phase == walk.phase && sim.animation->startMs >= walk.startMs) {
        return;
    }
    walk.phase = phase;
    if (part == nullptr) {
        return;
    }
    const LocoClip& clip = part->clips.alt ? *part->clips.alt : part->clips.clip;
    PlayingAnimation play;
    play.name = clip.name;
    play.startMs = now;
    play.lengthMs = clip.lengthMs;
    play.flipped = part->clips.mirrored;
    play.loops = part->kind == Kind::Loop;
    play.baseHeading = sim.heading ? *sim.heading : compassHeading(sim.data[kDirectionField]);
    if (part->kind == Kind::Step) {
        // Turned by the step's direction, as the walk draws it (see WalkPose::turn).
        *play.baseHeading += part->clips.mirrored ? -clip.rootTurn : clip.rootTurn;
    }
    sim.animation = play;
}

// At the walk's end my animation holds the stop's last pose, played from the way the route's
// last stretch runs (a step's from my facing turned as the step was). Without a stop, it holds the walk's
// standing pose.
void Execution::endWalk(SimObject& sim, const Walking& walk, double now) {
    std::vector<WalkPose> poses;
    walkPoses(walk.plan, walk.plan.length, poses);
    const WalkPose* last = nullptr;
    for (const auto& p : poses) {
        if (!last || p.weight > last->weight) {
            last = &p;
        }
    }
    if (last != nullptr && walk.phase == Walking::Phase::Stop) {
        PlayingAnimation hold;
        hold.name = last->clip->name;
        hold.startMs = now - last->clip->lengthMs;
        hold.lengthMs = last->clip->lengthMs;
        hold.flipped = last->mirrored;
        if (walk.plan.keepsFacing) {
            hold.baseHeading = walk.endHeading + last->turn;
        } else if (const WalkedTo end = walkPoint(walk, std::max(0.0f, walk.plan.length - kWalkSliver)); end.heading) {
            hold.baseHeading = *end.heading;
        }
        sim.animation = hold;
        return;
    }
    standAfterWalk(sim, now);
}

// A walk's standing pose as my animation, as a walk without a stop ends.
void Execution::standAfterWalk(SimObject& sim, double now) {
    const std::string pose = walkPoseClip(sim.personData[kLifeStagePersonData]);
    const auto& lengths = world().animationLengths();
    if (const auto length = !pose.empty() && lengths ? lengths(pose) : std::nullopt) {
        PlayingAnimation stand;
        stand.name = pose;
        stand.startMs = now;
        stand.lengthMs = *length;
        sim.animation = stand;
    } else if (sim.animation && sim.animation->loops) {
        sim.animation.reset();
    }
}

// A blocked step ends the walk where I stand, as a walk's end does: the pose I am in blends into
// the walk's standing pose, whatever part of the walk was playing.
void Execution::stopWalk(SimObject& sim, Walking& walk, double now) {
    walk.stopped = true;
    sim.lastWalkEnd.reset();
    standAfterWalk(sim, now);
}

// My route is interrupted while I walk: the walk is told to stop soon (see stopSoon) and the
// route ends once it is over. A walk that isn't moving stops where I stand. A turn on the spot
// plays out first (what the game does with it is not known yet).
void Execution::interruptWalk(SimObject& sim, Walking& walk, double now) {
    walk.interrupted = true;
    if (!alongPlan(walk)) {
        return;
    }
    const auto& scale = world().simScale();
    const auto stop = locoPair(walkStopClips(0, 0, sim.personData[kLifeStagePersonData]));
    const std::int16_t stage = sim.personData[kLifeStagePersonData], gender = sim.personData[kGenderPersonData];
    const float topLoop = topLoopSpeed(locoClip(walkShortClip(stage, gender)), locoClip(walkClip(stage, gender)));
    const auto ms = static_cast<float>(std::max(0.0, now - walk.startMs));
    switch (stopSoon(walk.plan, ms, stop, scale ? scale(sim.id) : 1.0f, topLoop)) {
    case StopSoon::Soon: walk.stopsSoon = true; break;
    case StopSoon::AsPlanned: break;
    case StopSoon::Now: stopWalk(sim, walk, now); break;
    }
}

// My route ends interrupted (route result 3), with no failure feedback.
Execution::WalkStep Execution::routeInterrupted(SimObject& sim) {
    constexpr std::int16_t kInterrupted = 3;
    sim.personData[kRouteResultPersonData] = kInterrupted;
    dropWalk(sim);
    return WalkStep{WalkStep::Kind::Done, false};
}

// Turn Body Towards (Sims only). Bytes 0-1 pick what to face:
//   0 nothing (true), 1 the camera, 2 the stack object, 3 away from it, 4-11 compass direction (mode - 4),
//   12-18 the current facing turned by 45, 90, 135, -45, -90, -135 or 180 degrees,
//   25 the compass direction in temp 0, 26 temp 0 plus my direction.
// Within 22.5 degrees it is true at once. Otherwise my pre-route tree runs first (a false return
// fails the turn), then the turn-in-place clip plays (see turnClip) and my facing follows the
// clip's root, a tick at a time. The turn ends on the nearest 45 degrees, true. A failed turn
// is false when facing an object, else true.
Execution::TurnStep Execution::turnBodyTowards(const bhav::Instruction& in, Frame& f) {
    SimObject& sim = me();
    if (!sim.isPerson()) {
        throw Stop{Outcome::Error, "Turn Body Towards on an object that is not a person", ""};
    }
    const std::size_t frame = thread_.frames.size() - 1;
    const auto startedHere = [&](const auto& s) {
        return s.frame == frame && s.group == f.group && s.tree == f.instance && s.pc == f.pc;
    };
    if (sim.turning && !startedHere(*sim.turning)) {
        sim.turning.reset();
    }
    const auto done = [&](bool turned) {
        const bool failsFalse = sim.turning && sim.turning->failsFalse;
        sim.turning.reset();
        return TurnStep{TurnStep::Kind::Done, turned || !failsFalse};
    };
    if (!sim.turning) {
        const auto mode = static_cast<std::uint16_t>(in.operands[0] | in.operands[1] << 8);
        if (mode == 0) {
            return TurnStep{TurnStep::Kind::Done, true};
        }
        // A heading left from an earlier turn counts while my direction still agrees with it.
        const bool headingKept = sim.heading && compassDirection(*sim.heading) == sim.data[kDirectionField];
        const float current = headingKept ? *sim.heading : compassHeading(sim.data[kDirectionField]);
        const bool failsFalse = mode >= 1 && mode <= 3;
        const auto target = turnTarget(mode, f, sim, current);
        if (!target) {
            return TurnStep{TurnStep::Kind::Done, !failsFalse};
        }
        if (std::abs(wrapAngle(*target - current)) <= kNoTurnAngle) {
            return TurnStep{TurnStep::Kind::Done, true};
        }
        sim.turning = Turning{frame, f.group, f.instance, f.pc, current, snapHeading(*target), failsFalse, std::nullopt};
        if (auto step = routeTree(sim, kPreRouteEntry)) {
            return std::move(*step);
        }
        sim.turning->preRoute = true;
    }
    const Turning& turning = *sim.turning;
    if (!turning.preRoute || !*turning.preRoute) {
        return done(false);
    }
    const double now = world().clockMs();
    if (sim.animation && startedHere(*sim.animation)) {
        const PlayingAnimation& clip = *sim.animation;
        if (clip.finished(now)) {
            const float end = turning.end;
            sim.animation.reset();
            faceHeading(sim, end);
            return done(true);
        }
        if (const auto& turns = world().animationTurns()) {
            if (const auto root = turns(clip.name, static_cast<float>((now - clip.startMs) * clip.speed))) {
                faceHeading(sim, turning.start + (clip.flipped ? -*root : *root));
            }
        }
        return TurnStep{TurnStep::Kind::Wait};
    }
    const auto clip = turnClip(turning.start, turning.end, sim.personData[kLifeStagePersonData]);
    const auto& lengths = world().animationLengths();
    const auto length = clip && lengths ? lengths(clip->name) : std::nullopt;
    if (!length) {
        return done(false);
    }
    PlayingAnimation play;
    play.name = clip->name;
    play.startMs = now;
    play.speed = kTurnInPlaceSpeed;
    play.lengthMs = *length / kTurnInPlaceSpeed;
    play.flipped = clip->mirrored;
    play.baseHeading = turning.start;
    play.frame = frame;
    play.group = f.group;
    play.tree = f.instance;
    play.pc = f.pc;
    sim.animation = std::move(play);
    return TurnStep{TurnStep::Kind::Wait};
}

void Execution::faceHeading(SimObject& sim, float heading) {
    sim.heading = heading;
    sim.data[kDirectionField] = compassDirection(heading);
}

// The heading a turn mode faces, nullopt when the object to face is gone.
std::optional<float> Execution::turnTarget(std::uint16_t mode, const Frame& f, const SimObject& sim, float current) {
    constexpr float kPi = 3.14159265358979f;
    if (mode == 1) {
        const auto camera = world().cameraPoint() ? world().cameraPoint()() : std::nullopt;
        if (!camera) {
            throw Stop{Outcome::Unsupported, "Turn Body Towards needs the camera", ""};
        }
        const double worldDx = (*camera)[1] - sim.y / 65536.0, worldDy = (*camera)[0] - sim.x / 65536.0;
        return static_cast<float>(std::atan2(-worldDx, worldDy));
    }
    if (mode == 2 || mode == 3) {
        const SimObject* target = world().find(f.stackObjectId);
        if (target == nullptr || target->x == kOffLot || target->y == kOffLot) {
            return std::nullopt;
        }
        double tx = target->x, ty = target->y;
        if (mode == 3) {
            tx = 2.0 * sim.x - tx;
            ty = 2.0 * sim.y - ty;
        }
        // The world's x and y are the route's y and x.
        const double worldDx = ty - sim.y, worldDy = tx - sim.x;
        return static_cast<float>(std::atan2(-worldDx, worldDy));
    }
    if (mode >= 4 && mode <= 11) {
        return compassHeading(mode - 4);
    }
    if (mode >= 12 && mode <= 18) {
        constexpr float kTurns[] = {kPi / 4, kPi / 2, 3 * kPi / 4, -kPi / 4, -kPi / 2, -3 * kPi / 4, kPi};
        return current + kTurns[mode - 12];
    }
    if (mode == 25) {
        return compassHeading(thread_.temps[0] & 7);
    }
    if (mode == 26) {
        return compassHeading((thread_.temps[0] + sim.data[kDirectionField]) & 7);
    }
    if (mode >= 19 && mode <= 24) {
        throw Stop{Outcome::Unsupported, "Turn Body Towards mode not implemented", std::to_string(mode)};
    }
    throw Stop{Outcome::Error, "Turn Body Towards mode out of range", std::to_string(mode)};
}

std::optional<Execution::TurnStep> Execution::routeTree(const SimObject& sim, std::size_t entry) {
    const ObjectType* type = code_.objectType(sim.definition.guid());
    const auto* functions = type ? code_.objectFunctions(*type) : nullptr;
    if (functions == nullptr || entry >= functions->size() || (*functions)[entry].action == 0) {
        return std::nullopt;
    }
    TurnStep step{TurnStep::Kind::PreRoute};
    step.instance = (*functions)[entry].action;
    step.group = groupFor(step.instance, sim.group);
    step.code = code_.behavior(step.group, step.instance);
    if (!step.code) {
        throw Stop{Outcome::MissingCode, "BHAV not found", hex(step.group, 8) + " " + hex(step.instance, 4)};
    }
    return step;
}

// The goal check (OBJf entry 0x27) of the object my walking tree runs on, if it has one, to run on my
// stack with that tree's stack object. It is given my route's slot as named and its goal's joint
// (person data 0x25 and 0x26), a value not known yet (passed as 0, while the game's is an object some TVs' checks
// look at), and the path still to walk in hundredths of a tile.
std::optional<Execution::WalkStep> Execution::goalCheck(SimObject& sim, const Frame& f, float remaining) {
    const SimObject* on = world().find(f.runningOnId);
    if (on == nullptr || !sim.walking) {
        return std::nullopt;
    }
    auto tree = routeTree(*on, kGoalCheckEntry);
    if (!tree) {
        return std::nullopt;
    }
    WalkStep step{WalkStep::Kind::GoalCheck};
    step.group = tree->group;
    step.instance = tree->instance;
    step.code = std::move(tree->code);
    step.codeOwner = on->group;
    step.stackObject = f.stackObjectId;
    const auto hundredths = static_cast<std::int16_t>(std::clamp(remaining * 100.0f, 0.0f, 32767.0f));
    step.args = {sim.personData[kRouteSlotPersonData], sim.personData[kRouteJointPersonData], 0, hundredths};
    sim.walking->checking = true;
    sim.walking->goalCheck.reset();
    return step;
}

// Get Distance To: the distance from an object to the stack object, in tiles (a level counts as
// 10), into a temp. Operands:
//   bytes 0-1  temp index (0-7)
//   byte 2     bit 0: the object is (owner byte 3, data bytes 4-5), else my object data 0x0B
//   byte 6     bit 1: in hundredths of a tile
bool Execution::getDistanceTo(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    const auto temp = static_cast<std::int16_t>(o[0] | o[1] << 8);
    if (temp < 0 || temp >= static_cast<std::int16_t>(kTempCount)) {
        throw Stop{Outcome::Error, "temporary reference out of range", std::to_string(temp)};
    }
    const std::int16_t fromId = (o[2] & 0x01) ? read(o[3], static_cast<std::uint16_t>(o[4] | o[5] << 8), f)
                                              : read(owner::kMyData, 0x0B, f);
    const SimObject* from = world().find(fromId);
    const SimObject* to = world().find(stackObject());
    if (from == nullptr || to == nullptr) {
        throw Stop{Outcome::Error, "could not find object reference",
                   std::to_string(from == nullptr ? fromId : stackObject())};
    }
    const float dx = static_cast<float>(from->x) / 65536.0f - static_cast<float>(to->x) / 65536.0f;
    const float dy = static_cast<float>(from->y) / 65536.0f - static_cast<float>(to->y) / 65536.0f;
    const float dz = static_cast<float>(to->level - from->level) * kLevelHeight;
    float distance = std::sqrt(dz * dz + dx * dx + dy * dy);
    if (o[6] & 0x02) {
        distance *= 100.0f;
    }
    thread_.temps[static_cast<std::size_t>(temp)] = static_cast<std::int16_t>(static_cast<int>(distance));
    return true;
}

// Get Direction To: the direction from an object to the stack object. Operands:
//   bytes 0-1, byte 2  result (data, owner)
//   byte 4             bit 0: the object is (owner byte 5, data bytes 6-7), else my object data 0x0B
//   byte 8             bit 1: in degrees (0-359), else one of 8 directions (0-7)
bool Execution::getDirectionTo(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    const bool explicitObject = (o[4] & 0x01) != 0;
    const std::int16_t fromId = explicitObject ? read(o[5], static_cast<std::uint16_t>(o[6] | o[7] << 8), f)
                                               : read(owner::kMyData, 0x0B, f);
    const SimObject* from = world().find(fromId);
    if (from == nullptr) {
        throw Stop{Outcome::Error, "could not find object reference", std::to_string(fromId)};
    }
    const SimObject& to = stack();
    const float dx = static_cast<float>(to.x) / 65536.0f - static_cast<float>(from->x) / 65536.0f;
    const float dy = static_cast<float>(to.y) / 65536.0f - static_cast<float>(from->y) / 65536.0f;
    std::int16_t result = 0;
    if (o[8] & 0x02) {
        constexpr float kDegreesPerRadian = 360.0f / 6.2831853f;
        const auto degrees = static_cast<int>(std::atan2(dy, dx) * kDegreesPerRadian + 90.0f);
        result = static_cast<std::int16_t>((degrees + 360) % 360);
    } else {
        result = octant(dx, dy);
    }
    write(o[2], static_cast<std::uint16_t>(o[0] | o[1] << 8), f, result);
    return true;
}

// One of 8 directions for a vector, split at 22.5 degrees from each axis: 0 is -y, 2 +x, 4 +y,
// 6 -x, with the diagonals between.
std::int16_t Execution::octant(float dx, float dy) {
    constexpr float kTan22_5 = 0.41421354f;
    const float ax = std::fabs(dx);
    const float ay = std::fabs(dy);
    int direction = 2;
    if (ax * kTan22_5 <= ay) {
        direction = kTan22_5 * ay <= ax ? 3 : 4;
    }
    if (dx < 0) {
        direction = direction == 2 ? 6 : direction == 3 ? 5 : direction;
    }
    if (dy < 0) {
        direction = direction == 3 ? 1 : direction == 4 ? 0 : direction == 5 ? 7 : direction;
    }
    return static_cast<std::int16_t>(direction);
}

} // namespace ts2::sim::vm_detail
