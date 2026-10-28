// Walking: Go To Routing Slot and Go To Relative Position, the pre- and post-route trees, walk clips,
// Turn Body Towards, and routes blocked by other Sims.
#include "test_support.hpp"

#include "content/animation_lists.hpp"
#include "sim/animation.hpp"
#include "sim/ground_click.hpp"
#include "sim/idle_spot.hpp"
#include "sim/route_destinations.hpp"
#include "sim/route_portals.hpp"
#include "sim/walking.hpp"
#include "sim/path_passes.hpp"
#include "sim/route_world.hpp"
#include "sim/turn.hpp"
#include "sim/walk_path.hpp"
#include "sim/walk_plan.hpp"
#include <map>
#include <random>

namespace {

// Turn Body Towards: picks the turn clip by angle (mirrored for the other way), runs the Sim's
// pre-route tree first, follows the clip's root a tick at a time and ends on the nearest 45 degrees.
// Go To Routing Slot walks the route the simulation plans at the walk clip's pace, facing the way,
// and is true at its end, facing as the route says. With no route it is false and person data 0x3E
// says why. Without a router (no route grids) it can't run.
void goToRoutingSlotWalksTheRoute() {
    Instruction go;
    go.opcode = 0x002D;
    go.trueTarget = T;
    go.falseTarget = F;
    go.operands[2] = 1;  // slot 0, given as a number
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1100, bhav({go}));
    ts2::slot::Entry slot;
    slot.type = ts2::slot::kRoutingSlot;
    code.addObjectType(kObjectGroup, typed(kChairGuid), {slot});
    code.addObjectType(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));

    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& chair = world.create(kObjectGroup, typed(kChairGuid));
    sim.x = sim.y = 5 << 16;
    chair.x = 5 << 16;
    chair.y = 7 << 16;
    sim.thread.stackObjectId = chair.id;
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, kObjectGroup, 0x1100).outcome == Outcome::Unsupported);

    ts2::sim::SlotRoute planned;
    planned.points = {{5.0f, 5.0f}, {5.0f, 5.08f}};
    planned.facing = static_cast<std::int16_t>(6);
    world.setSlotRouter([&](const ts2::sim::SimObject&, const ts2::sim::SimObject& owner, const ts2::slot::Entry&,
                            const std::vector<std::optional<ts2::sim::ModelBone>>&) {
        CHECK(owner.id == chair.id);
        return planned;
    });
    // A woman's walk without start and stop clips: her walk loop alone, at the planned speed (on a
    // path this short, slower than the loop's own), facing the slot's way once there.
    sim.personData[0x3A] = 30;
    sim.personData[0x41] = 1;
    world.setAnimationLengths([](const std::string& name) -> std::optional<float> {
        if (name == "a-female-walk-alt0-normal") return 1000.0f;
        if (name == "a-walkPose") return 500.0f;
        return std::nullopt;
    });
    world.setLocoClips([](const std::string& name) -> std::optional<ts2::sim::LocoClip> {
        if (name != "a-female-walk-alt0-normal") return std::nullopt;
        return ts2::sim::LocoClip{name, 1000, 1.0f, 0, 0, 0.001f, 0};
    });
    CHECK(vm.run(sim, kObjectGroup, 0x1100).outcome == Outcome::Sleeping && sim.walking && sim.personData[0x3E] == 0);
    CHECK(sim.animation && sim.animation->name == "a-female-walk-alt0-normal" && sim.animation->loops);
    const float total = sim.walking ? ts2::sim::walkMs(sim.walking->plan) : 0.0f;
    CHECK(total > 100 && total < 200);
    world.advanceClock(50);
    CHECK(vm.resume(sim).outcome == Outcome::Sleeping && sim.y > (5 << 16) && sim.y < static_cast<std::int32_t>(std::lround(5.08 * 65536)));
    CHECK(sim.data[1] == 4);  // walking toward +y, which is the world's +x
    world.advanceClock(total);
    CHECK(vm.resume(sim).outcome == Outcome::True && sim.y == static_cast<std::int32_t>(std::lround(5.08 * 65536)));
    CHECK(sim.data[1] == 6 && !sim.walking && sim.animation && sim.animation->name == "a-walkPose" && !sim.animation->loops);

    // Already on the goal: true at once when facing its way, else a turn on the spot first (with no
    // turn clip, the facing changes on the next tick), as the game turns a Sim back into a bed.
    planned.points = {{5.0f, 5.08f}};
    CHECK(vm.run(sim, kObjectGroup, 0x1100).outcome == Outcome::True && !sim.walking);
    sim.heading.reset();
    sim.data[1] = 2;
    CHECK(vm.run(sim, kObjectGroup, 0x1100).outcome == Outcome::Sleeping && sim.walking && sim.walking->turnOnly);
    world.advanceClock(50);
    CHECK(vm.resume(sim).outcome == Outcome::True && sim.data[1] == 6 && !sim.walking);

    planned.points.clear();
    planned.failure = 2;
    CHECK(vm.run(sim, kObjectGroup, 0x1100).outcome == Outcome::False && sim.personData[0x3E] == 2);
}

// Go To Relative Position routes to a routing slot made from its operands: on top of the stack
// object a point goal, beside it the sides named (a diagonal names both neighbours, "anywhere near"
// all four up to 2 tiles away), with the failure-tree and other-level flags from byte 6. A facing out
// of range or a missing stack object is false without a route.
void goToRelativePositionMakesASlot() {
    struct Case {
        std::uint8_t where, facing, flags;
        std::uint32_t routingFlags;
        float maxDistance;
        std::int32_t slotFacing;
    };
    for (const Case c : {Case{0xFE, 0xFF, 0x05, 0xC000, 1.0f, -3}, Case{3, 0xFE, 0x02, 0x14, 1.0f, -2},
                         Case{0xFF, 2, 0x00, 0x4055, 2.0f, 2}}) {
        Instruction go;
        go.opcode = 0x001B;
        go.trueTarget = T;
        go.falseTarget = F;
        go.operands[2] = c.where;
        go.operands[3] = c.facing;
        go.operands[6] = c.flags;
        MemoryCodeSource code;
        code.add(kObjectGroup, 0x1100, bhav({go}));
        code.addObjectType(kObjectGroup, typed(kChairGuid));
        code.addObjectType(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
        ts2::sim::World world;
        auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
        auto& spot = world.create(kObjectGroup, typed(kChairGuid));
        sim.x = sim.y = 5 << 16;
        spot.x = 5 << 16;
        spot.y = 7 << 16;
        sim.thread.stackObjectId = spot.id;
        std::optional<ts2::slot::Entry> asked;
        world.setSlotRouter([&](const ts2::sim::SimObject&, const ts2::sim::SimObject& owner, const ts2::slot::Entry& entry,
                                const std::vector<std::optional<ts2::sim::ModelBone>>&) {
            CHECK(owner.id == spot.id);
            asked = entry;
            ts2::sim::SlotRoute route;
            route.points = {{5.0f, 5.0f}};  // already there
            return route;
        });
        Vm vm(code, {}, &world);
        CHECK(vm.run(sim, kObjectGroup, 0x1100).outcome == Outcome::True);
        CHECK(asked && asked->type == ts2::slot::kRoutingSlot && asked->routingFlags == c.routingFlags);
        CHECK(asked && asked->minDistance == 1.0f && asked->maxDistance == c.maxDistance && asked->optimalDistance == 1.0f);
        CHECK(asked && asked->facing == c.slotFacing && asked->routingInts[0] == 1);
    }

    Instruction go;
    go.opcode = 0x001B;
    go.trueTarget = T;
    go.falseTarget = F;
    go.operands[2] = 0xFE;
    go.operands[3] = 8;
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1100, bhav({go}));
    code.addObjectType(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    bool routed = false;
    world.setSlotRouter([&](const ts2::sim::SimObject&, const ts2::sim::SimObject&, const ts2::slot::Entry&,
                            const std::vector<std::optional<ts2::sim::ModelBone>>&) {
        routed = true;
        return ts2::sim::SlotRoute{};
    });
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, kObjectGroup, 0x1100).outcome == Outcome::False && !routed);  // facing 8
    go.operands[3] = 0xFF;
    code.add(kObjectGroup, 0x1101, bhav({go}));
    sim.thread.stackObjectId = 99;
    CHECK(vm.run(sim, kObjectGroup, 0x1101).outcome == Outcome::False && !routed);  // no such object
}

// Before walking, the Sim's pre-route tree (OBJf 0x25) runs on its stack with the slot's object as
// its first value and the route's length rounded to a tile as its sixth. A false return gives up the walk.
void walksRunThePreRouteTreeFirst() {
    for (const bool agrees : {true, false}) {
        Instruction go;
        go.opcode = 0x002D;
        go.trueTarget = T;
        go.falseTarget = F;
        go.operands[2] = 1;
        MemoryCodeSource code;
        code.add(kObjectGroup, 0x1100, bhav({go}));
        code.add(kObjectGroup, 0x1200, bhav({expr(owner::kMyPersonData, 0xF0, op::kAssign, owner::kParam, 0, 1, E),
                                             expr(owner::kMyPersonData, 0xF1, op::kAssign, owner::kParam, 5, agrees ? T : F, E)}, 7));
        std::vector<ts2::objf::Function> functions(0x30);
        functions[0x25].action = 0x1200;
        code.addFunctions(kObjectGroup, functions);
        ts2::slot::Entry slot;
        slot.type = ts2::slot::kRoutingSlot;
        code.addObjectType(kObjectGroup, typed(kChairGuid), {slot});
        code.addObjectType(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
        ts2::sim::World world;
        auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
        auto& chair = world.create(kObjectGroup, typed(kChairGuid));
        sim.x = sim.y = 5 << 16;
        chair.x = 5 << 16;
        chair.y = 7 << 16;
        sim.thread.stackObjectId = chair.id;
        ts2::sim::SlotRoute planned;
        planned.points = {{5.0f, 5.0f}, {5.0f, 7.3f}};
        world.setSlotRouter([&](const ts2::sim::SimObject&, const ts2::sim::SimObject&, const ts2::slot::Entry&,
                                const std::vector<std::optional<ts2::sim::ModelBone>>&) { return planned; });
        Vm vm(code, {}, &world);
        const auto run = vm.run(sim, kObjectGroup, 0x1100);
        const auto outcome = run.outcome;
        CHECK(sim.personData[0xF0] == chair.id && sim.personData[0xF1] == 2);
        if (agrees) {
            CHECK(outcome == Outcome::Sleeping && sim.walking && sim.walking->started);
        } else {
            CHECK(outcome == Outcome::False && !sim.walking);
        }
    }
}

// Before setting off, the goal check (OBJf 0x27) of the object the walking tree runs on runs on the
// Sim with that tree's stack object, given the route's slot as named, its goal's joint, 0 and the
// path in hundredths of a tile. A false return fails the route, the goal not available (13), with
// "CT - Route Failure Feedback".
void walksRunTheGoalCheckBeforeSettingOff() {
    for (const bool agrees : {true, false}) {
        Instruction go;
        go.opcode = 0x002D;
        go.trueTarget = T;
        go.falseTarget = F;
        go.operands[2] = 1;
        MemoryCodeSource code;
        code.add(kObjectGroup, 0x1100, bhav({go}));
        code.add(kObjectGroup, 0x1300, bhav({expr(owner::kMyPersonData, 0xF0, op::kAssign, owner::kParam, 3, 1, E),
                                             expr(owner::kMyPersonData, 0xF1, op::kAssign, owner::kParam, 2, 2, E),
                                             expr(owner::kMyPersonData, 0xF2, op::kAssign, owner::kStackObjectId, 0, agrees ? T : F, E)}, 4));
        std::vector<ts2::objf::Function> functions(0x30);
        functions[0x27].action = 0x1300;
        code.addFunctions(kObjectGroup, functions);
        code.add(ts2::sim::kGlobalGroup, 0x018E, bhav({expr(owner::kMyPersonData, 0xE0, op::kAssign, owner::kParam, 0, 1, E),
                                                         expr(owner::kMyPersonData, 0xE1, op::kAssign, owner::kParam, 1, T, E)}, 4));
        ts2::slot::Entry slot;
        slot.type = ts2::slot::kRoutingSlot;
        code.addObjectType(kObjectGroup, typed(kChairGuid), {slot});
        code.addObjectType(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
        ts2::sim::World world;
        auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
        auto& chair = world.create(kObjectGroup, typed(kChairGuid));
        sim.x = sim.y = 5 << 16;
        chair.x = 5 << 16;
        chair.y = 7 << 16;
        sim.thread.stackObjectId = chair.id;
        sim.personData[0xE1] = -1;
        ts2::sim::SlotRoute planned;
        planned.points = {{5.0f, 5.0f}, {5.0f, 7.3f}};
        world.setSlotRouter([&](const ts2::sim::SimObject&, const ts2::sim::SimObject&, const ts2::slot::Entry&,
                                const std::vector<std::optional<ts2::sim::ModelBone>>&) { return planned; });
        Vm vm(code, {}, &world);
        const auto outcome = vm.run(sim, kObjectGroup, 0x1100).outcome;
        CHECK(sim.personData[0xF0] == 229 || sim.personData[0xF0] == 230);  // 2.3 tiles, truncated
        CHECK(sim.personData[0xF1] == 0 && sim.personData[0xF2] == chair.id);
        if (agrees) {
            CHECK(outcome == Outcome::Sleeping && sim.walking && sim.walking->started);
        } else {
            CHECK(outcome == Outcome::False && !sim.walking && !sim.postRoute);
            CHECK(sim.personData[0x3E] == 13 && sim.personData[0xE0] == 13 && sim.personData[0xE1] == 0);
        }
    }
}

// A pre-route tree that moves the Sim (a seated Sim stands up) has the route planned again from
// where it stands. One that leaves it in place keeps the route.
void walksAreReplannedWhenThePreRouteTreeMovesTheSim() {
    for (const bool moves : {true, false}) {
        Instruction go;
        go.opcode = 0x002D;
        go.trueTarget = T;
        go.falseTarget = F;
        go.operands[2] = 1;
        MemoryCodeSource code;
        code.add(kObjectGroup, 0x1100, bhav({go}));
        code.add(kObjectGroup, 0x1200, bhav({expr(owner::kParam, 1, op::kAssign, owner::kLiteral, 1, 1, E), sleepFor(1, T)}, 7));
        std::vector<ts2::objf::Function> functions(0x30);
        functions[0x25].action = 0x1200;
        code.addFunctions(kObjectGroup, functions);
        ts2::slot::Entry slot;
        slot.type = ts2::slot::kRoutingSlot;
        code.addObjectType(kObjectGroup, typed(kChairGuid), {slot});
        code.addObjectType(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
        ts2::sim::World world;
        auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
        auto& chair = world.create(kObjectGroup, typed(kChairGuid));
        sim.x = sim.y = 5 << 16;
        chair.x = 5 << 16;
        chair.y = 9 << 16;
        sim.thread.stackObjectId = chair.id;
        int plans = 0;
        world.setSlotRouter([&](const ts2::sim::SimObject& who, const ts2::sim::SimObject&, const ts2::slot::Entry&,
                                const std::vector<std::optional<ts2::sim::ModelBone>>&) {
            ++plans;
            ts2::sim::SlotRoute route;
            const float x = static_cast<float>(who.x) / 65536.0f, y = static_cast<float>(who.y) / 65536.0f;
            route.points = {{x, y}, {5.0f, 9.3f}};
            return route;
        });
        Vm vm(code, {}, &world);
        CHECK(vm.run(sim, kObjectGroup, 0x1100).outcome == Outcome::Sleeping && plans == 1);
        if (moves) {
            sim.y = (5 << 16) + 0x8000;
        }
        CHECK(vm.resume(sim).outcome == Outcome::Sleeping && sim.walking && sim.walking->started);
        CHECK(plans == (moves ? 2 : 1));
        CHECK(sim.walking && sim.walking->fromY == (moves ? 5.5f : 5.0f) && sim.walking->route.points.front().second == sim.walking->fromY);
    }
}

// A plan again after the pre-route tree goes to the slot the first plan chose, even when the slot
// number came from temp 0 and the pre-route tree changed it.
void walksPlannedAgainKeepTheirSlot() {
    Instruction go;
    go.opcode = 0x002D;
    go.trueTarget = T;
    go.falseTarget = F;
    go.operands[2] = 1;
    go.operands[4] = 0x02;  // the slot number from temp 0
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1100, bhav({go}));
    code.add(kObjectGroup, 0x1200, bhav({expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 1, 1, E), expr(owner::kParam, 1, op::kAssign, owner::kLiteral, 1, 2, E), sleepFor(1, T)}, 7));
    std::vector<ts2::objf::Function> functions(0x30);
    functions[0x25].action = 0x1200;
    code.addFunctions(kObjectGroup, functions);
    ts2::slot::Entry first, second;
    first.type = second.type = ts2::slot::kRoutingSlot;
    second.x = 1;
    code.addObjectType(kObjectGroup, typed(kChairGuid), {first, second});
    code.addObjectType(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& chair = world.create(kObjectGroup, typed(kChairGuid));
    sim.x = sim.y = 5 << 16;
    chair.x = 5 << 16;
    chair.y = 9 << 16;
    sim.thread.stackObjectId = chair.id;
    std::vector<float> slotsPlanned;
    world.setSlotRouter([&](const ts2::sim::SimObject& who, const ts2::sim::SimObject&, const ts2::slot::Entry& entry,
                            const std::vector<std::optional<ts2::sim::ModelBone>>&) {
        slotsPlanned.push_back(entry.x);
        ts2::sim::SlotRoute route;
        const float x = static_cast<float>(who.x) / 65536.0f, y = static_cast<float>(who.y) / 65536.0f;
        route.points = {{x, y}, {5.0f, 9.3f}};
        return route;
    });
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, kObjectGroup, 0x1100).outcome == Outcome::Sleeping);
    sim.y = (5 << 16) + 0x8000;
    CHECK(vm.resume(sim).outcome == Outcome::Sleeping && sim.walking && sim.walking->started);
    CHECK(sim.thread.temps[0] == 1);
    CHECK((slotsPlanned == std::vector<float>{0.0f, 0.0f}));
}

// At the end of a walk the Sim's post-route tree (OBJf 0x26) runs on its stack, given the seat the
// route reached (here the chair) and the route's result. It is what sits the Sim down. A false return
// fails the route (result 12). A route that is already there runs neither route tree.
void walksEndWithThePostRouteTree() {
    for (const bool agrees : {true, false}) {
        Instruction go;
        go.opcode = 0x002D;
        go.trueTarget = T;
        go.falseTarget = F;
        go.operands[2] = 1;
        MemoryCodeSource code;
        code.add(kObjectGroup, 0x1100, bhav({go}));
        code.add(kObjectGroup, 0x1200, bhav({expr(owner::kMyPersonData, 0xF2, op::kAssign, owner::kLiteral, 1, T, E)}));
        code.add(kObjectGroup, 0x1300, bhav({expr(owner::kMyPersonData, 0xF0, op::kAssign, owner::kParam, 0, 1, E),
                                             expr(owner::kMyPersonData, 0xF1, op::kAssign, owner::kParam, 1, agrees ? T : F, E)}));
        std::vector<ts2::objf::Function> functions(0x30);
        functions[0x25].action = 0x1200;
        functions[0x26].action = 0x1300;
        code.addFunctions(kObjectGroup, functions);
        ts2::slot::Entry slot;
        slot.type = ts2::slot::kRoutingSlot;
        code.addObjectType(kObjectGroup, typed(kChairGuid), {slot});
        code.addObjectType(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
        ts2::sim::World world;
        auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
        auto& chair = world.create(kObjectGroup, typed(kChairGuid));
        sim.x = sim.y = 5 << 16;
        chair.x = 5 << 16;
        chair.y = 8 << 16;
        sim.thread.stackObjectId = chair.id;
        ts2::sim::SlotRoute planned;
        planned.points = {{5.0f, 5.0f}, {5.0f, 7.3f}};
        planned.goalObject = chair.id;
        planned.goalToken = 3;
        world.setSlotRouter([&](const ts2::sim::SimObject&, const ts2::sim::SimObject&, const ts2::slot::Entry&,
                                const std::vector<std::optional<ts2::sim::ModelBone>>&) { return planned; });
        Vm vm(code, {}, &world);
        CHECK(vm.run(sim, kObjectGroup, 0x1100).outcome == Outcome::Sleeping && sim.walking && sim.personData[0xF2] == 1);
        CHECK(sim.personData[0x28] == chair.id && sim.personData[0x25] == 0 && sim.personData[0x26] == 3);
        world.advanceClock(sim.walking ? ts2::sim::walkMs(sim.walking->plan) + 100 : 0);
        const auto outcome = vm.resume(sim).outcome;
        CHECK(sim.personData[0xF0] == chair.id && sim.personData[0xF1] == 0 && !sim.walking && !sim.postRoute);
        CHECK(agrees ? outcome == Outcome::True : (outcome == Outcome::False && sim.personData[0x3E] == 12));

        sim.personData[0xF0] = sim.personData[0xF2] = 0;
        const float hereX = sim.x / 65536.0f, hereY = sim.y / 65536.0f;
        planned.points = {{hereX, hereY}, {hereX, hereY}};
        CHECK(vm.run(sim, kObjectGroup, 0x1100).outcome == Outcome::True && sim.personData[0xF2] == 0 && sim.personData[0xF0] == 0);
    }
}

void walksStartAndStopWithTheirClips() {
    constexpr float kPi = 3.14159265f;
    // A quarter turn the negative way plays the clips as they are, and the other way mirrored.
    const auto right = ts2::sim::walkStartClips(0, -kPi / 2, 30);
    CHECK(right && right->full == "a-turnAndGo-alt-90" && right->shortOne == "a-turnAndGoShort-alt-90" && !right->mirrored);
    CHECK(right && std::abs(right->turn + kPi / 2) < 1e-5f);
    const auto child = ts2::sim::walkStopClips(0, kPi / 4, 10);
    CHECK(child && child->full == "c-turnAndStop-alt-45" && child->mirrored);
    const auto toddler = ts2::sim::walkStartClips(0, 0.1f, 2);
    CHECK(toddler && toddler->shortOne == "p-walkTurnAndGoShort-0" && toddler->turn == 0);

    Instruction go;
    go.opcode = 0x002D;
    go.trueTarget = T;
    go.falseTarget = F;
    go.operands[2] = 1;
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1100, bhav({go}));
    ts2::slot::Entry slot;
    slot.type = ts2::slot::kRoutingSlot;
    code.addObjectType(kObjectGroup, typed(kChairGuid), {slot});
    code.addObjectType(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& chair = world.create(kObjectGroup, typed(kChairGuid));
    sim.x = sim.y = 5 << 16;
    chair.x = 5 << 16;
    chair.y = 9 << 16;
    sim.thread.stackObjectId = chair.id;
    sim.personData[0x3A] = 30;
    sim.personData[0x41] = 1;
    sim.data[1] = 4;  // already facing along the route
    ts2::sim::SlotRoute planned;
    planned.points = {{5.0f, 5.0f}, {5.0f, 8.0f}};
    world.setSlotRouter([&](const ts2::sim::SimObject&, const ts2::sim::SimObject&, const ts2::slot::Entry&,
                            const std::vector<std::optional<ts2::sim::ModelBone>>&) { return planned; });
    const std::map<std::string, ts2::sim::LocoClip> clips{
        {"a-female-walk-alt0-normal", {"a-female-walk-alt0-normal", 1000, 1.0f, 0, 0, 0.001f, 0}},
        {"a-turnAndGo-alt-0", {"a-turnAndGo-alt-0", 750, 0.75f, 0.5f, 0, 0.001f, 0}},
        {"a-turnAndStop-alt-0", {"a-turnAndStop-alt-0", 500, 0.25f, 0.5f, 0, 0.0005f, 0}}};
    world.setAnimationLengths([&](const std::string& name) -> std::optional<float> {
        const auto it = clips.find(name);
        return it != clips.end() ? std::optional<float>(it->second.lengthMs) : std::nullopt;
    });
    world.setLocoClips([&](const std::string& name) -> std::optional<ts2::sim::LocoClip> {
        const auto it = clips.find(name);
        return it != clips.end() ? std::optional<ts2::sim::LocoClip>(it->second) : std::nullopt;
    });
    Vm vm(code, {}, &world);
    const auto at = [&](float y) { return std::abs(sim.y - static_cast<std::int32_t>(std::lround(y * 65536))) <= 2; };

    // It steps off with the start clip, walks the loop, and stops with the stop clip on the goal,
    // holding the stop's last pose, as the plan lays them along the route.
    CHECK(vm.run(sim, kObjectGroup, 0x1100).outcome == Outcome::Sleeping);
    CHECK(sim.animation && sim.animation->name == "a-turnAndGo-alt-0" && sim.animation->baseHeading);
    CHECK(sim.walking && sim.walking->plan.segments.size() == 3);
    const float total = sim.walking ? ts2::sim::walkMs(sim.walking->plan) : 0.0f;
    world.advanceClock(total / 2);
    CHECK(vm.resume(sim).outcome == Outcome::Sleeping && sim.y > (6 << 16) && sim.y < (7 << 16));
    CHECK(sim.animation && sim.animation->name == "a-female-walk-alt0-normal" && sim.animation->loops);
    world.advanceClock(total / 2 - 100);
    CHECK(vm.resume(sim).outcome == Outcome::Sleeping && sim.animation && sim.animation->name == "a-turnAndStop-alt-0");
    world.advanceClock(200);
    CHECK(vm.resume(sim).outcome == Outcome::True && at(8.0f) && !sim.walking);
    CHECK(sim.animation && sim.animation->name == "a-turnAndStop-alt-0" && sim.data[1] == 4);
}

// A walk begun within 140 ms of the last one's end in its stop clip begins on the foot that stop
// began on. A later one begins on foot 0.5 again. Here the first 2.4-tile walk's loop counts 3 half cycles, so
// it ends on the straight stop mirrored (foot 0), and the next walk's straight start is mirrored too.
void walksBegunRightAfterOneKeepItsFoot() {
    Instruction go;
    go.opcode = 0x002D;
    go.trueTarget = T;
    go.falseTarget = F;
    go.operands[2] = 1;
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1100, bhav({go}));
    ts2::slot::Entry slot;
    slot.type = ts2::slot::kRoutingSlot;
    code.addObjectType(kObjectGroup, typed(kChairGuid), {slot});
    code.addObjectType(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& chair = world.create(kObjectGroup, typed(kChairGuid));
    sim.x = sim.y = 5 << 16;
    chair.x = 5 << 16;
    chair.y = 20 << 16;
    sim.thread.stackObjectId = chair.id;
    sim.personData[0x3A] = 30;
    sim.personData[0x41] = 1;
    sim.personData[0x70] = 100;
    sim.data[1] = 4;
    ts2::sim::SlotRoute planned;
    world.setSlotRouter([&](const ts2::sim::SimObject&, const ts2::sim::SimObject&, const ts2::slot::Entry&,
                            const std::vector<std::optional<ts2::sim::ModelBone>>&) { return planned; });
    const std::map<std::string, ts2::sim::LocoClip> clips{
        {"a-female-walk-alt0-normal", {"a-female-walk-alt0-normal", 1000, 1.0f, 0, 0, 0.001f, 0}},
        {"a-turnAndGo-alt-0", {"a-turnAndGo-alt-0", 750, 0.75f, 0.5f, 0, 0.001f, 0}},
        {"a-turnAndStop-alt-0", {"a-turnAndStop-alt-0", 500, 0.25f, 0.5f, 0, 0.0005f, 0}}};
    world.setAnimationLengths([&](const std::string& name) -> std::optional<float> {
        const auto it = clips.find(name);
        return it != clips.end() ? std::optional<float>(it->second.lengthMs) : std::nullopt;
    });
    world.setLocoClips([&](const std::string& name) -> std::optional<ts2::sim::LocoClip> {
        const auto it = clips.find(name);
        return it != clips.end() ? std::optional<ts2::sim::LocoClip>(it->second) : std::nullopt;
    });
    Vm vm(code, {}, &world);
    const auto walkThere = [&](float y) {
        planned.points = {{5.0f, static_cast<float>(sim.y) / 65536.0f}, {5.0f, y}};
        auto outcome = vm.run(sim, kObjectGroup, 0x1100).outcome;
        const bool startMirrored = sim.walking && !sim.walking->plan.segments.empty() && sim.walking->plan.segments.front().clips.mirrored;
        for (int tick = 0; tick < 400 && outcome == Outcome::Sleeping; ++tick) {
            world.advanceClock(20);
            outcome = vm.resume(sim).outcome;
        }
        CHECK(outcome == Outcome::True);
        return startMirrored;
    };
    CHECK(!walkThere(7.4f));
    CHECK(sim.lastWalkEnd && sim.lastWalkEnd->foot == 0.0f);
    CHECK(walkThere(9.8f));
    world.advanceClock(1000);
    CHECK(!walkThere(12.2f));
}

// A walk whose interaction is cancelled stops soon: it walks on to the next foot, plays the
// straight stop there and the route ends interrupted (3), false, short of its goal. A route started
// while the Sim should be interrupted ends so at once, without walking.
void interruptedWalksStopSoon() {
    Instruction go;
    go.opcode = 0x002D;
    go.trueTarget = T;
    go.falseTarget = F;
    go.operands[2] = 1;
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1100, bhav({go}));
    ts2::slot::Entry slot;
    slot.type = ts2::slot::kRoutingSlot;
    code.addObjectType(kObjectGroup, typed(kChairGuid), {slot});
    code.addObjectType(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& chair = world.create(kObjectGroup, typed(kChairGuid));
    sim.x = sim.y = 5 << 16;
    chair.x = 5 << 16;
    chair.y = 16 << 16;
    sim.thread.stackObjectId = chair.id;
    sim.personData[0x3A] = 30;
    sim.personData[0x41] = 1;
    sim.data[1] = 4;
    ts2::sim::SlotRoute planned;
    planned.points = {{5.0f, 5.0f}, {5.0f, 15.0f}};
    world.setSlotRouter([&](const ts2::sim::SimObject&, const ts2::sim::SimObject&, const ts2::slot::Entry&,
                            const std::vector<std::optional<ts2::sim::ModelBone>>&) { return planned; });
    const std::map<std::string, ts2::sim::LocoClip> clips{
        {"a-female-walk-alt0-normal", {"a-female-walk-alt0-normal", 1000, 1.0f, 0, 0, 0.001f, 0}},
        {"a-turnAndGo-alt-0", {"a-turnAndGo-alt-0", 750, 0.75f, 0.5f, 0, 0.001f, 0}},
        {"a-turnAndStop-alt-0", {"a-turnAndStop-alt-0", 500, 0.25f, 0.5f, 0, 0.0005f, 0}}};
    world.setAnimationLengths([&](const std::string& name) -> std::optional<float> {
        const auto it = clips.find(name);
        return it != clips.end() ? std::optional<float>(it->second.lengthMs) : std::nullopt;
    });
    world.setLocoClips([&](const std::string& name) -> std::optional<ts2::sim::LocoClip> {
        const auto it = clips.find(name);
        return it != clips.end() ? std::optional<ts2::sim::LocoClip>(it->second) : std::nullopt;
    });
    Vm vm(code, {}, &world);

    CHECK(vm.run(sim, kObjectGroup, 0x1100).outcome == Outcome::Sleeping && sim.walking);
    world.advanceClock(4000);
    CHECK(vm.resume(sim).outcome == Outcome::Sleeping);
    const float y0 = static_cast<float>(sim.y) / 65536.0f;
    sim.running = ts2::sim::QueuedInteraction{};
    sim.running->cancelled = true;
    CHECK(vm.resume(sim).outcome == Outcome::Sleeping && sim.walking && sim.walking->interrupted && sim.walking->stopsSoon);
    // On to the next foot (at most half a stride) and the stop's travel on from there.
    CHECK(sim.walking && sim.walking->plan.length < (y0 - 5.0f) + 0.5f + 0.25f + 0.01f);
    auto outcome = Outcome::Sleeping;
    for (int tick = 0; tick < 60 && outcome == Outcome::Sleeping; ++tick) {
        world.advanceClock(50);
        outcome = vm.resume(sim).outcome;
    }
    CHECK(outcome == Outcome::False && sim.personData[0x3E] == 3 && !sim.walking);
    const float y1 = static_cast<float>(sim.y) / 65536.0f;
    CHECK(y1 > y0 && y1 < y0 + 0.8f && sim.data[1] == 4);
    CHECK(sim.animation && sim.animation->name == "a-turnAndStop-alt-0");

    // Still to be interrupted when the next route starts: it isn't planned.
    sim.personData[0x3E] = 0;
    CHECK(vm.run(sim, kObjectGroup, 0x1100).outcome == Outcome::False && sim.personData[0x3E] == 3 && !sim.walking);
}

void turnBodyTowardsTurnsWithItsClip() {
    constexpr float kPi = 3.14159265f;
    const auto left = ts2::sim::turnClip(0, kPi / 4, 30);
    CHECK(left && left->name == "a-TurnInPlace-45" && left->mirrored);
    const auto right = ts2::sim::turnClip(0, -kPi / 2, 16);
    CHECK(right && right->name == "t-TurnInPlace-90" && !right->mirrored);
    const auto around = ts2::sim::turnClip(0, kPi, 60);
    CHECK(around && around->name == "a-TurnInPlace-180" && !around->mirrored);
    const auto toddler = ts2::sim::turnClip(0, 3 * kPi / 4, 2);
    CHECK(toddler && toddler->name == "p-walkTurnInPlace-135");
    CHECK(!ts2::sim::turnClip(0, kPi / 2, 1) && !ts2::sim::turnClip(0, 0.5f, 30));
    for (int d = 0; d < 8; ++d) {
        CHECK(ts2::sim::compassDirection(ts2::sim::compassHeading(d)) == d);
    }

    auto turn = [](std::uint8_t mode) {
        Instruction in;
        in.opcode = 0x0016;
        in.trueTarget = T;
        in.falseTarget = F;
        in.operands[0] = mode;
        return in;
    };
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1100, bhav({turn(2)}));
    code.add(kObjectGroup, 0x1101, bhav({turn(4)}));
    code.add(kObjectGroup, 0x1102, bhav({turn(1)}));
    code.add(kObjectGroup, 0x1200, bhav({expr(owner::kTemp, 5, op::kAdd, owner::kLiteral, 1, T, E)}));
    std::vector<ts2::objf::Function> functions(0x30);
    functions[0x25].action = 0x1200;
    code.addFunctions(kObjectGroup, functions);
    code.addObjectType(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));

    ts2::sim::World world;
    world.setAnimationLengths([](const std::string& name) -> std::optional<float> {
        return name.find("TurnInPlace") != std::string::npos ? std::optional<float>(1000.0f) : std::nullopt;
    });
    world.setAnimationTurns([](const std::string&, float ms) -> std::optional<float> {
        return -kPi / 2 * std::min(ms, 1000.0f) / 1000;  // a 90 degree clip
    });
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& chair = world.create(kObjectGroup, typed(kChairGuid));
    sim.x = sim.y = 5 << 16;
    sim.data[1] = 2;
    sim.personData[0x3A] = 30;
    chair.x = 5 << 16;
    chair.y = 8 << 16;
    Vm vm(code, {}, &world);

    // The chair is towards the world's +x (compass 4) and the Sim faces +y: the 90 degree clip as authored.
    sim.thread.stackObjectId = chair.id;
    CHECK(vm.run(sim, kObjectGroup, 0x1100).outcome == Outcome::Sleeping);
    CHECK(sim.thread.temps[5] == 1 && sim.animation && sim.animation->name == "a-TurnInPlace-90" && !sim.animation->flipped);
    // Turns in place play 1.6 times faster than authored: the 1000 ms clip takes 625 ms.
    CHECK(sim.animation->speed == ts2::sim::kTurnInPlaceSpeed && sim.animation->lengthMs == 625.0f);
    world.advanceClock(300);
    CHECK(vm.resume(sim).outcome == Outcome::Sleeping && sim.data[1] == 3);
    world.advanceClock(400);
    CHECK(vm.resume(sim).outcome == Outcome::True && sim.data[1] == 4 && !sim.animation && !sim.turning);

    // Already facing it: true at once, without the pre-route tree. A gone object: false.
    CHECK(vm.run(sim, kObjectGroup, 0x1100).outcome == Outcome::True && sim.thread.temps[5] == 1);
    sim.thread.stackObjectId = 999;
    CHECK(vm.run(sim, kObjectGroup, 0x1100).outcome == Outcome::False);

    // Facing the camera: unsupported without one. A camera where the chair is needs no turn.
    CHECK(vm.run(sim, kObjectGroup, 0x1102).outcome == Outcome::Unsupported);
    world.setCameraPoint([] { return std::optional<std::array<float, 2>>({5.0f, 8.0f}); });
    CHECK(vm.run(sim, kObjectGroup, 0x1102).outcome == Outcome::True && sim.thread.temps[5] == 1);

    // Compass 0 from 4 is half a turn.
    CHECK(vm.run(sim, kObjectGroup, 0x1101).outcome == Outcome::Sleeping && sim.animation &&
          sim.animation->name == "a-TurnInPlace-180");
}

// Turn Body Towards modes 19-24 face a slot of an object, and every turn finds its target again once
// the pre-route tree has returned.
void turnBodyTowardsSlotsAndTargetAfterPreRoute() {
    auto turn = [](std::uint8_t mode, std::uint8_t slot = 0) {
        Instruction in;
        in.opcode = 0x0016;
        in.trueTarget = T;
        in.falseTarget = F;
        in.operands[0] = mode;
        in.operands[2] = owner::kStackObjectId;
        in.operands[5] = slot;
        return in;
    };
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1100, bhav({turn(19)}));
    code.add(kObjectGroup, 0x1101, bhav({turn(20)}));
    code.add(kObjectGroup, 0x1102, bhav({turn(23)}));
    code.add(kObjectGroup, 0x1103, bhav({turn(20, 1)}));
    code.add(kObjectGroup, 0x1104, bhav({turn(21)}));
    code.add(kObjectGroup, 0x1105, bhav({turn(4)}));
    // The pre-route tree turns me to compass 1 when temp 5 is 1.
    code.add(kObjectGroup, 0x1200, bhav({expr(owner::kTemp, 5, op::kEquals, owner::kLiteral, 1, 1, T),
                                         expr(owner::kMyData, 1, op::kAssign, owner::kLiteral, 1, T, E)}));
    std::vector<ts2::objf::Function> functions(0x30);
    functions[0x25].action = 0x1200;
    code.addFunctions(kObjectGroup, functions);
    code.addObjectType(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    ts2::slot::Entry routing;
    routing.type = ts2::slot::kRoutingSlot;
    ts2::slot::Entry target;
    target.type = ts2::slot::kTargetSlot;
    target.y = 3;  // three tiles to the chair's world -y at facing 2
    code.addObjectType(kObjectGroup, typed(kChairGuid), {routing}, {}, {target});

    ts2::sim::World world;
    world.setAnimationLengths([](const std::string& name) -> std::optional<float> {
        return name.find("TurnInPlace") != std::string::npos ? std::optional<float>(1000.0f) : std::nullopt;
    });
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& chair = world.create(kObjectGroup, typed(kChairGuid));
    sim.x = sim.y = 5 << 16;
    sim.personData[0x3A] = 30;
    chair.x = 5 << 16;
    chair.y = 8 << 16;
    chair.data[1] = 2;
    Vm vm(code, {}, &world);
    const auto run = [&](std::uint16_t tree, std::int16_t temp5 = 0) {
        sim.data[1] = 2;
        sim.heading.reset();
        sim.animation.reset();
        sim.turning.reset();
        sim.thread = {};
        sim.thread.stackObjectId = chair.id;
        sim.thread.temps[5] = temp5;
        return vm.run(sim, kObjectGroup, tree).outcome;
    };

    // The target slot is three tiles from the chair to the world's -y: 135 degrees to my right.
    CHECK(run(0x1100) == Outcome::Sleeping && sim.animation && sim.animation->name == "a-TurnInPlace-135" && !sim.animation->flipped);
    // The routing slot is at the chair: 90 degrees to the right, and facing away 90 degrees to the left.
    CHECK(run(0x1101) == Outcome::Sleeping && sim.animation && sim.animation->name == "a-TurnInPlace-90" && !sim.animation->flipped);
    CHECK(run(0x1102) == Outcome::Sleeping && sim.animation && sim.animation->name == "a-TurnInPlace-90" && sim.animation->flipped);
    // A slot the chair doesn't have is an error.
    CHECK(run(0x1103) == Outcome::Error && run(0x1104) == Outcome::Error);
    // An object off the lot: true, as for every mode but 1-3.
    chair.x = ts2::sim::kOffLot;
    CHECK(run(0x1101) == Outcome::True && !sim.animation);

    // Compass 0 is 90 degrees to my left. Once my pre-route tree has turned me to compass 1, the turn
    // is 45 degrees.
    CHECK(run(0x1105) == Outcome::Sleeping && sim.animation && sim.animation->name == "a-TurnInPlace-90" && sim.animation->flipped);
    CHECK(run(0x1105, 1) == Outcome::Sleeping && sim.animation && sim.animation->name == "a-TurnInPlace-45" && sim.animation->flipped);

    // A finished turn runs my post-route tree with 0 and 1. Facing an object, the turn is then false
    // when the object has gone off the lot meanwhile.
    code.add(kObjectGroup, 0x1106, bhav({turn(2)}));
    code.add(kObjectGroup, 0x1201, bhav({expr(owner::kMyPersonData, 0xF0, op::kAdd, owner::kLiteral, 1, 1, E),
                                         expr(owner::kMyPersonData, 0xF1, op::kAssign, owner::kParam, 1, T, E)}, 2));
    functions[0x26].action = 0x1201;
    code.addFunctions(kObjectGroup, functions);
    chair.x = 5 << 16;
    CHECK(run(0x1106) == Outcome::Sleeping && sim.personData[0xF0] == 0);
    // The 625 ms clip ends the turn as it starts its last 233.33 ms fade out, and fades on.
    world.advanceClock(380);
    CHECK(vm.resume(sim).outcome == Outcome::Sleeping && sim.animation);
    world.advanceClock(20);
    CHECK(vm.resume(sim).outcome == Outcome::True && sim.personData[0xF0] == 1 && sim.personData[0xF1] == 1 && sim.data[1] == 4);
    CHECK(!sim.animation && sim.ending && sim.ending->name == "a-TurnInPlace-90" && sim.ending->fadeOutMs > 233.0f);
    CHECK(run(0x1106) == Outcome::Sleeping);
    world.advanceClock(700);
    chair.x = ts2::sim::kOffLot;
    CHECK(vm.resume(sim).outcome == Outcome::False && sim.personData[0xF0] == 2 && !sim.turning);
}

// Blocked routes. Sim a walks from (5, 5) to (5, 8) with Sim b standing at (5, 6) in its way. A step
// within half a tile of b (or of `object`, when set) is blocked. The global route trees count
// their runs in a's person data and keep the values they were given there.
struct BlockedWalk {
    static constexpr std::uint32_t kSimGuid = 0x51515151;
    MemoryCodeSource code;
    ts2::sim::World world;
    ts2::sim::SimObject* a = nullptr;
    ts2::sim::SimObject* b = nullptr;
    ts2::sim::SimObject* chair = nullptr;
    static constexpr std::uint32_t kDestinationGroup = 0x7F000003;
    std::int16_t object = 0;
    int plansOfA = 0;
    bool bThere = false;
    bool stepTest = true;  // the half-tile test of a's steps above
    std::pair<float, float> aGoal{5.0f, 8.0f}, bGoal{9.0f, 6.0f};
    std::optional<ts2::slot::Entry> madeWay;  // the slot of the last route to a destination
    std::optional<Vm> vm;
    Outcome last = Outcome::Sleeping;
    bool aNoPath = false;  // a's plans find no path (2)

    // `goFlags` is Go To Routing Slot's operand byte 4.
    explicit BlockedWalk(bool routeBlockedGoesOn = true, bool feedbackRoutes = false, std::uint8_t goFlags = 0) {
        using ts2::sim::kGlobalGroup;
        Instruction go;
        go.opcode = 0x002D;
        go.trueTarget = T;
        go.falseTarget = F;
        go.operands[2] = 1;
        go.operands[4] = goFlags;
        code.add(kObjectGroup, 0x1100, bhav({go}));
        const auto counted = [](std::uint16_t field, std::uint16_t next) {
            return expr(owner::kMyPersonData, field, op::kAdd, owner::kLiteral, 1, next, E);
        };
        const auto keep = [](std::uint16_t field, std::uint8_t from, std::uint16_t data, std::uint16_t next) {
            return expr(owner::kMyPersonData, field, op::kAssign, from, data, next, E);
        };
        code.add(kGlobalGroup, 0x01BB, bhav({counted(0xF0, 1), keep(0xF1, owner::kParam, 0, 2), keep(0xF2, owner::kParam, 1, T)}, 2));
        code.add(kGlobalGroup, 0x01BC, bhav({counted(0xF3, 1), keep(0xF4, owner::kStackObjectId, 0, T)}));
        code.add(kGlobalGroup, 0x01BD, bhav({counted(0xF5, 1), keep(0xF6, owner::kParam, 1, T)}, 2));
        if (feedbackRoutes) {
            Instruction again = go;
            again.trueTarget = again.falseTarget = T;
            code.add(kGlobalGroup, 0x018E, bhav({counted(0xE0, 1), keep(0xE1, owner::kParam, 0, 2), keep(0xE2, owner::kParam, 1, 3),
                                                  expr(owner::kStackObjectId, 0, op::kAssign, owner::kMyPersonData, 0xE7, 4, E), again}, 4));
        } else {
            code.add(kGlobalGroup, 0x018E, bhav({counted(0xE0, 1), keep(0xE1, owner::kParam, 0, 2), keep(0xE2, owner::kParam, 1, 3),
                                                  keep(0xE3, owner::kParam, 2, 4), keep(0xE4, owner::kParam, 3, T)}, 4));
        }
        Behavior routeBlocked = bhav({counted(0xE5, 1), keep(0xE8, owner::kStackObjectId, 0, 2),
                                      keep(0xE9, owner::kParam, 0, routeBlockedGoesOn ? T : F)}, 4);
        routeBlocked.name = "CT - Route Blocked";
        code.add(kSemiGroup, 0x20BE, std::move(routeBlocked));
        ts2::slot::Entry slot;
        slot.type = ts2::slot::kRoutingSlot;
        code.addObjectType(kObjectGroup, typed(kChairGuid), {slot});
        code.addObjectType(kObjectGroup, typed(kSimGuid, ts2::sim::kPersonType));
        // The Go Here destination made to make way, with its "Move" interaction (number 3).
        auto destination = typed(ts2::sim::destination::kGoHere);
        destination.fields[ts2::objd::field::kInteractionTableId] = 0x80;
        code.addObjectType(kDestinationGroup, destination);
        ts2::ttab::InteractionTable table;
        table.interactions.resize(2);
        table.interactions[0].stringIndex = 2;
        table.interactions[1].stringIndex = 3;
        table.interactions[1].action = 0x100B;
        code.addInteractionTable(kDestinationGroup, 0x80, table);
        a = &world.create(kObjectGroup, typed(kSimGuid, ts2::sim::kPersonType));
        b = &world.create(kObjectGroup, typed(kSimGuid, ts2::sim::kPersonType));
        chair = &world.create(kObjectGroup, typed(kChairGuid));
        a->x = a->y = 5 << 16;
        a->data[1] = 4;  // facing along the route
        b->x = 5 << 16;
        b->y = 6 << 16;
        chair->x = 5 << 16;
        chair->y = 9 << 16;
        a->thread.stackObjectId = b->thread.stackObjectId = chair->id;
        a->personData[0xE7] = chair->id;
        // a walks to (5, 8), b to (9, 6) (or nowhere, with `bThere`). A destination's slot is 0.2
        // tiles to +x.
        world.setSlotRouter([this](const ts2::sim::SimObject& who, const ts2::sim::SimObject& owner, const ts2::slot::Entry& entry,
                                   const std::vector<std::optional<ts2::sim::ModelBone>>&) {
            ts2::sim::SlotRoute route;
            const float x = static_cast<float>(who.x) / 65536.0f, y = static_cast<float>(who.y) / 65536.0f;
            if (owner.definition.guid() == ts2::sim::destination::kGoHere) {
                madeWay = entry;
                route.points = {{x, y}, {x + 0.2f, y}};
                return route;
            }
            plansOfA += &who == a ? 1 : 0;
            if (&who == a && aNoPath) {
                route.failure = 2;
                return route;
            }
            if (&who == b && bThere) {
                route.points = {{x, y}};
                return route;
            }
            route.points = {{x, y}, &who == a ? aGoal : bGoal};
            return route;
        });
        world.setStepBlocker([this](const ts2::sim::SimObject& sim, float x, float y, std::int32_t) -> std::int16_t {
            if (&sim != a || !stepTest) {
                return 0;
            }
            if (object != 0) {
                return object;
            }
            return std::hypot(b->x / 65536.0f - x, b->y / 65536.0f - y) < 0.5f ? b->id : std::int16_t{0};
        });
        vm.emplace(code, ts2::sim::Limits{}, &world);
    }
    void start() { last = vm->run(*a, kObjectGroup, 0x1100).outcome; }
    // b starts a route of its own, but stays where it is.
    void bRoutes() { CHECK(vm->run(*b, kObjectGroup, 0x1100).outcome == Outcome::Sleeping && ts2::sim::routing(*b)); }
    // A tick of the simulation for a.
    // A tick for both Sims, b first.
    void tickBoth() {
        world.advanceClock(100);
        world.countTick();
        if (b->thread.isRunning()) {
            vm->resume(*b);
        }
        last = vm->resume(*a).outcome;
    }
    void tick() {
        world.advanceClock(100);
        world.countTick();
        last = vm->resume(*a).outcome;
    }
    template <class Done> bool tickUntil(Done done, int ticks = 300) {
        for (int i = 0; i < ticks && last == Outcome::Sleeping; ++i) {
            tick();
            if (done()) {
                return true;
            }
        }
        return false;
    }
    ts2::sim::RouteBlock::Step step() const { return a->walking ? a->walking->block.step : ts2::sim::RouteBlock::Step::None; }
};

// A Sim standing in the way is asked to move: a Go Here destination is made where it stands, with
// the sidestep slot (the asker's goal is far from it), and the destination's "Move" is queued on it
// with the asker's priority less 1. The block ends the asker's walk where it stands. The asker
// runs "CT - Route Ask To Move Animation" with it as stack object, then "CT - Route Waiting Animation"
// once a tick, given the count so far and the distance left to 1.25 tiles in 24ths. The wait ends
// when the blocker's route in "Move" ends, when it goes farther than 1.25 tiles, or after 126 ticks
// (420 when its object data 8 has bit 0x40). Then, still waiting on it, the asker doesn't ask again but takes the last resort.
void blockedRoutesAskAStandingSimToMove() {
    using Step = ts2::sim::RouteBlock::Step;
    enum class End { RouteEnds, MovesAway, TimeUp, LongTimeUp };
    for (const End end : {End::RouteEnds, End::MovesAway, End::TimeUp, End::LongTimeUp}) {
        BlockedWalk w;
        w.a->personData[0x21] = 49;
        if (end == End::LongTimeUp) {
            w.b->data[8] = 0x40;
        }
        w.start();
        CHECK(w.last == Outcome::Sleeping && w.plansOfA == 1);
        // A foot left by an earlier walk's stop isn't carried past a block: the next walk begins on 0.5.
        w.a->lastWalkEnd = ts2::sim::SimObject::WalkEnd{w.world.clockMs(), 0.0f};
        CHECK(w.tickUntil([&] { return w.a->personData[0xF3] == 1; }));
        const std::uint64_t askedAt = w.world.ticks();
        const float d = std::hypot(w.a->x / 65536.0f - 5.0f, w.a->y / 65536.0f - 6.0f);
        CHECK(w.a->personData[0xF4] == w.b->id && w.step() == Step::AskToMove && w.a->personData[0xF0] == 0);
        // The block ended the walk where a stands: it shows no walk while it waits.
        CHECK(w.a->walking->stopped && !ts2::sim::alongPlan(*w.a->walking) && !w.a->lastWalkEnd);
        CHECK(w.a->personData[0xF5] == 1 && w.a->personData[0xF6] == std::max(1, static_cast<int>((1.25f - d) * 24.0f)));
        CHECK(w.b->queue.size() == 1 && w.b->destinations.size() == 1 && w.a->walking && w.a->walking->block.waitingOn == w.b->id);
        const ts2::sim::SimObject* made = w.world.find(w.b->destinations.front());
        CHECK(made && made->x == w.b->x && made->y == w.b->y && made->ownRoutingSlot && made->ownRoutingSlot->maxDistance == 0.3f);
        CHECK(w.b->queue.front().object == made->id && w.b->queue.front().index == 3 && w.b->queue.front().action == 0x100B);
        CHECK(w.b->queue.front().priority == 48 && w.a->walking->block.waitInteraction == w.b->queue.front().id);
        // One waiting animation a tick, however often the walk runs.
        CHECK(w.vm->resume(*w.a).outcome == Outcome::Sleeping && w.a->personData[0xF5] == 1);
        w.tick();
        CHECK(w.a->personData[0xF5] == 2 && w.a->walking && w.a->walking->block.waits == 2);
        if (end == End::RouteEnds) {
            // b runs "Move" and its route there ends.
            w.b->running = w.b->queue.front();
            w.b->queue.clear();
            w.bThere = true;
            CHECK(w.vm->run(*w.b, kObjectGroup, 0x1100).outcome == Outcome::True);
            CHECK(w.a->walking && w.a->walking->block.waitingOn == 0);
            w.tick();
            CHECK(w.plansOfA == 2 && w.step() == Step::None && !w.a->walking->stopped && ts2::sim::alongPlan(*w.a->walking));
            continue;
        }
        if (end == End::MovesAway) {
            w.b->y = w.a->y + 0x15000;
            w.tick();
            CHECK(w.plansOfA == 2 && w.step() == Step::None && w.a->walking && w.a->walking->block.waitingOn == w.b->id);
            continue;
        }
        CHECK(w.tickUntil([&] { return w.a->personData[0xE5] == 1; }, 500));
        CHECK(w.world.ticks() - askedAt == (end == End::LongTimeUp ? 420u : 126u) && w.a->personData[0xF3] == 1);
    }
}

// The blocker counts as moved when it stands elsewhere than when the route was planned: a Sim that
// walked on while the walker's route was planned has the route planned again at the first block,
// without an ask.
void blockedRoutesPlanAgainWhenTheBlockerMovedSincePlanning() {
    BlockedWalk w;
    w.a->personData[0x21] = 49;
    w.start();
    w.b->y += 0x1000;
    CHECK(w.tickUntil([&] { return w.plansOfA == 2; }));
    CHECK(w.a->personData[0xF3] == 0 && w.b->queue.empty() && w.a->walking && w.a->walking->block.tries == 1);
}

// A standing Sim that is already making way (running or queuing a destination's "Move") is not
// asked again: the walker steps aside. One whose Sleep state (motive 11) is below 0 is never asked:
// the first block plans again and the next fails the route, blocked by a person.
void blockedRoutesSkipBusyAndSleepingSims() {
    BlockedWalk busy;
    busy.a->personData[0x21] = 49;
    auto& made = busy.world.create(BlockedWalk::kDestinationGroup, typed(ts2::sim::destination::kGoHere));
    ts2::sim::QueuedInteraction move;
    move.object = made.id;
    move.index = 3;
    busy.b->running = move;
    busy.start();
    CHECK(busy.tickUntil([&] { return busy.a->walking && busy.a->walking->aside != 0; }));
    CHECK(busy.a->personData[0xF3] == 0 && busy.b->queue.empty() && busy.b->destinations.empty());

    BlockedWalk sleeping;
    sleeping.a->personData[0x21] = 49;
    sleeping.b->motives[11] = -10.0f;
    sleeping.start();
    CHECK(sleeping.tickUntil([&] { return sleeping.plansOfA == 2; }));
    CHECK(sleeping.tickUntil([&] { return sleeping.last != Outcome::Sleeping; }) || sleeping.last != Outcome::Sleeping);
    CHECK(sleeping.last == Outcome::False && sleeping.a->personData[0x3E] == 14 && sleeping.a->personData[0xF3] == 0);
    CHECK(sleeping.b->queue.empty() && sleeping.a->personData[0xE0] == 1);
}

// A route that ends while it still waits on the Sim it asked to move cancels that Sim's "Move": a
// queued one leaves its queue.
void endedRoutesCancelTheMoveTheyAskedFor() {
    BlockedWalk w(false);
    w.a->personData[0x21] = 49;
    w.start();
    CHECK(w.tickUntil([&] { return w.b->queue.size() == 1; }));
    CHECK(w.a->walking && w.a->walking->block.waitingOn == w.b->id);
    CHECK(w.tickUntil([&] { return w.last != Outcome::Sleeping; }, 500) || w.last != Outcome::Sleeping);
    CHECK(w.last == Outcome::False && w.b->queue.empty());
}

// A step blocked by an object that is gone fails the route for an unknown reason (1), with the
// failure feedback.
void blockedRoutesByAGoneObjectFailAsUnknown() {
    BlockedWalk w;
    w.object = 999;
    w.b->x = 20 << 16;  // out of the way
    w.start();
    CHECK(w.tickUntil([&] { return w.last != Outcome::Sleeping; }) || w.last != Outcome::Sleeping);
    CHECK(w.last == Outcome::False && w.a->personData[0x3E] == 1 && w.a->personData[0xE0] == 1 && w.a->personData[0xE1] == 1);
    CHECK(w.a->personData[0xE2] == 999);
}

// The sidestep slot: on the forward diagonal of the other Sim on the side the mover stands on,
// ending toward it, and none from more than 2 tiles. The 1-to-3-tile slot goes to directions 1, 2, 6
// and 7. A destination goes once nothing the Sim does uses it.
void makeWaySlotsAndTheirCleanup() {
    ts2::sim::World world;
    auto& other = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& mover = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    other.x = other.y = 5 << 16;
    other.data[1] = 0;
    mover.x = (5 << 16) + 0x8000;
    mover.y = 5 << 16;
    const auto side = ts2::sim::makeWaySlot(0, mover, other);
    CHECK(side && side->routingFlags == (0x48000u | 0x02) && side->facing == 6 && side->minDistance == 0.0625f);
    CHECK(side && side->maxDistance == 0.3f && side->optimalDistance == 0.3f && side->step == 0.05f);
    mover.x = (5 << 16) - 0x8000;
    const auto left = ts2::sim::makeWaySlot(0, mover, other);
    CHECK(left && left->routingFlags == (0x48000u | 0x80) && left->facing == 2);
    mover.x = 8 << 16;
    CHECK(!ts2::sim::makeWaySlot(0, mover, other));
    const auto far = ts2::sim::makeWaySlot(1, mover, other);
    CHECK(far && far->routingFlags == 0x0014A0C6 && far->maxDistance == 3.0f && far->step == 0.25f && far->facing == -3);

    auto& kept = world.create(kObjectGroup, typed(kChairGuid));
    auto& unused = world.create(kObjectGroup, typed(kChairGuid));
    mover.destinations = {kept.id, unused.id, 999};
    ts2::sim::QueuedInteraction move;
    move.object = kept.id;
    mover.queue.push_back(move);
    ts2::sim::cleanUpDestinations(world, mover, false);
    CHECK((mover.destinations == std::vector<std::int16_t>{kept.id}) && unused.removed && !kept.removed);
    ts2::sim::cleanUpDestinations(world, mover, true);
    CHECK(mover.destinations.empty() && kept.removed);
}

// Two Sims on routes blocking each other: the walker steps aside once, to a sidestep destination
// made where it stands, then its own route goes on. When they meet again it plays the blocked
// animation. A blocker that is stepping aside itself doesn't count the block.
void blockedRoutesStepAsideHeadOn() {
    BlockedWalk w;
    w.bRoutes();
    w.b->walking->block.blocker = w.a->id;
    w.start();
    CHECK(w.tickUntil([&] { return w.a->walking && w.a->walking->aside != 0; }));
    CHECK(w.madeWay && w.madeWay->maxDistance == 0.3f && w.a->destinations.size() == 1 && w.a->personData[0xF0] == 0);
    CHECK(w.a->walking->block.steppingAside && w.a->walking->outerBlock && w.a->walking->outerBlock->stepAsides == 1);
    CHECK(w.tickUntil([&] { return w.a->walking && w.a->walking->aside == 0; }));
    CHECK(w.plansOfA == 2 && w.a->walking && w.a->walking->block.stepAsides == 1 && !w.a->walking->block.steppingAside);
    CHECK(w.tickUntil([&] { return w.a->personData[0xF0] == 1; }));
    CHECK(w.a->destinations.size() == 1);

    // A step-aside that fails is quiet: no failure feedback, no route result, and Sims waiting on
    // the walker keep waiting. The route it stepped aside from is planned again with its count of
    // blocks as it was, and makes no second step-aside.
    BlockedWalk quiet;
    quiet.bRoutes();
    quiet.b->walking->block.blocker = quiet.a->id;
    quiet.start();
    CHECK(quiet.tickUntil([&] { return quiet.a->walking && quiet.a->walking->aside != 0; }));
    const std::uint16_t tries = quiet.a->walking->outerBlock->tries;
    quiet.a->running = ts2::sim::QueuedInteraction{};
    quiet.a->running->id = 77;
    quiet.b->walking->block.waitingOn = quiet.a->id;
    quiet.b->walking->block.waitInteraction = 77;
    quiet.object = quiet.chair->id;
    quiet.tick();
    CHECK(quiet.last == Outcome::Sleeping && quiet.a->walking && quiet.a->walking->aside == 0 && quiet.plansOfA == 2);
    CHECK(quiet.a->walking->block.tries == tries && quiet.a->walking->block.stepAsides == 1);
    CHECK(quiet.a->personData[0xE0] == 0 && quiet.a->personData[0x3E] != 15 && quiet.b->walking->block.waitingOn == quiet.a->id);

    BlockedWalk yields;
    yields.bRoutes();
    yields.b->walking->block.blocker = yields.a->id;
    yields.b->walking->block.steppingAside = true;
    yields.start();
    yields.a->walking->block.stepAsides = 1;
    CHECK(yields.tickUntil([&] { return yields.a->personData[0xF0] == 1; }));
    CHECK(yields.a->walking && yields.a->walking->block.tries == 0 && yields.a->walking->aside == 0);
}

// A Sim on a route in the way: the first block plays "CT - Route Blocked Animation" with the count
// so far and the blocker, then plans again. A second block by the same Sim, which hasn't moved,
// runs "CT - Route Blocked" once (true plans again). The third fails the route, blocked by a person
// (14), and "CT - Route Failure Feedback" is given 14, the blocker, 1 and 1.
void blockedRoutesWaitForAWalkingSimThenFail() {
    using Step = ts2::sim::RouteBlock::Step;
    BlockedWalk w;
    w.bRoutes();
    w.start();
    CHECK(w.tickUntil([&] { return w.a->personData[0xF0] == 1; }));
    CHECK(w.a->personData[0xF1] == 0 && w.a->personData[0xF2] == w.b->id && w.plansOfA == 2 && w.step() == Step::None);
    CHECK(w.a->personData[0xF3] == 0 && w.a->walking && w.a->walking->block.animations == 1);
    CHECK(w.tickUntil([&] { return w.a->personData[0xE5] == 1; }));
    CHECK(w.plansOfA == 3 && w.a->personData[0xF0] == 1 && w.last == Outcome::Sleeping);
    // "CT - Route Blocked" runs with the blocker as stack object, given the route's slot owner first.
    CHECK(w.a->personData[0xE8] == w.b->id && w.a->personData[0xE9] == w.chair->id);
    CHECK(w.tickUntil([&] { return w.last != Outcome::Sleeping; }) || w.last != Outcome::Sleeping);
    CHECK(w.last == Outcome::False && w.a->personData[0x3E] == 14 && w.a->personData[0xE5] == 1 && w.a->personData[0xF0] == 1);
    CHECK(w.a->personData[0xE0] == 1 && w.a->personData[0xE1] == 14 && w.a->personData[0xE2] == w.b->id);
    CHECK(w.a->personData[0xE3] == 1 && w.a->personData[0xE4] == 1 && !w.a->walking && !w.a->postRoute);

    // A blocker that has moved since the plan has the route planned again at its second block.
    BlockedWalk moved;
    moved.bRoutes();
    moved.start();
    CHECK(moved.tickUntil([&] { return moved.a->personData[0xF0] == 1; }));
    moved.b->x += 0x2000;
    CHECK(moved.tickUntil([&] { return moved.plansOfA == 3; }));
    CHECK(moved.a->personData[0xE5] == 0 && moved.a->personData[0xF0] == 1 && moved.last == Outcome::Sleeping);

    // "CT - Route Blocked" returning false fails the route at once.
    BlockedWalk refused(false);
    refused.bRoutes();
    refused.start();
    CHECK(refused.tickUntil([&] { return refused.last != Outcome::Sleeping; }) || refused.last != Outcome::Sleeping);
    CHECK(refused.last == Outcome::False && refused.a->personData[0xE5] == 1 && refused.a->personData[0x3E] == 14);
    CHECK(refused.a->personData[0xE0] == 1 && refused.plansOfA == 2);

    // Go To Routing Slot's byte 4 bit 0 turns the failure feedback off. The route result stays.
    BlockedWalk quiet(false, false, 0x01);
    quiet.bRoutes();
    quiet.start();
    CHECK(quiet.tickUntil([&] { return quiet.last != Outcome::Sleeping; }) || quiet.last != Outcome::Sleeping);
    CHECK(quiet.last == Outcome::False && quiet.a->personData[0x3E] == 14 && quiet.a->personData[0xE0] == 0);
}

// A plan that finds no route fails the route as a block does: "CT - Route Failure Feedback" runs
// with the planner's reason and no blocker, unless Go To Routing Slot's byte 4 bit 0 turns it off.
void unplannedRoutesShowFeedback() {
    BlockedWalk w;
    w.aNoPath = true;
    w.start();
    CHECK(w.tickUntil([&] { return w.last != Outcome::Sleeping; }) || w.last != Outcome::Sleeping);
    CHECK(w.last == Outcome::False && w.a->personData[0x3E] == 2 && w.a->personData[0xE0] == 1);
    CHECK(w.a->personData[0xE1] == 2 && w.a->personData[0xE2] == 0 && w.a->personData[0xE3] == 1 && w.a->personData[0xE4] == 1);
    CHECK(!w.a->walking && !w.a->postRoute);

    BlockedWalk quiet(true, false, 0x01);
    quiet.aNoPath = true;
    quiet.start();
    CHECK(quiet.last == Outcome::False && quiet.a->personData[0x3E] == 2 && quiet.a->personData[0xE0] == 0);
}

// An object in the way fails the route at once (15). The failure feedback tree may walk the Sim
// again (the game's walks it to a door that blocked it). That route is nested in the failed one,
// so its own failure shows no feedback.
void blockedRoutesShowFeedbackOnlyForTheOnlyRoute() {
    BlockedWalk w(true, true);
    w.object = w.chair->id;
    w.b->x = 20 << 16;  // far from the way
    w.start();
    CHECK(w.tickUntil([&] { return w.last != Outcome::Sleeping; }) || w.last != Outcome::Sleeping);
    CHECK(w.last == Outcome::False && w.a->personData[0x3E] == 15 && w.a->personData[0xE0] == 1);
    CHECK(w.a->personData[0xE1] == 15 && w.a->personData[0xE2] == w.chair->id && w.plansOfA == 2);
    CHECK(!w.a->walking && !w.a->postRoute && w.a->personData[0xF0] == 0 && w.a->personData[0xE5] == 0);
}

// A walking Sim's step, as the game tests it: its square against the objects' footprints. From a
// clear spot a step into a footprint is blocked by that object. A Sim whose square overlaps a chair
// where it stands takes every step, even into the table beside it, until it stands clear.
void blockedStepsNeedAClearSpot() {
    using namespace ts2::sim;
    RouteGrid grid(8, 8);
    grid.add({{0, 0, 8, 8}, 1, 0, RouteItemKind::Floor, true, 0});
    grid.add({{3.5f, 3.25f, 4.0f, 3.75f}, kBlockedCost, 9, RouteItemKind::Object, false, 5});  // the chair
    grid.add({{3.5f, 4.5f, 4.0f, 5.0f}, kBlockedCost, 9, RouteItemKind::Object, false, 6});    // the table
    const auto test = [&](float x, float y) -> std::optional<std::int16_t> {
        const auto object = objectInTheWayOfSim(grid, x, y, 1, {}, GoalFit::Square);
        return object ? std::optional<std::int16_t>(static_cast<std::int16_t>(*object)) : std::nullopt;
    };
    CHECK(!test(3.75f, 2.5f) && test(3.75f, 3.3f) == 5 && test(3.75f, 4.6f) == 6 && !test(3.75f, 4.1f));
    CHECK(blockedStep(test, 3.75f, 3.3f, 3.75f, 2.5f) == 5);
    CHECK(blockedStep(test, 3.75f, 2.9f, 3.75f, 2.5f) == 0);
    // Inside the chair: out of it, and on into the table.
    CHECK(blockedStep(test, 3.75f, 3.6f, 3.75f, 3.5f) == 0 && blockedStep(test, 3.75f, 4.6f, 3.75f, 3.5f) == 0);
    // Clear of it, the table ahead blocks.
    CHECK(blockedStep(test, 3.75f, 4.6f, 3.75f, 4.1f) == 6);
}

// The look-ahead for other Sims blocks a walk well before the step test would: two Sims walking at
// each other meet about 1.5 tiles apart, a Sim standing in the way is met about half a tile before
// they touch. Sims walking apart, and a Sim crossing ahead that is gone by then, don't block.
void walksLookAheadForOtherSims() {
    const auto gap = [](const BlockedWalk& w) { return std::hypot((w.a->x - w.b->x) / 65536.0f, (w.a->y - w.b->y) / 65536.0f); };
    const auto blocked = [](const BlockedWalk& w) { return w.a->walking && w.a->walking->block.blocker != 0; };
    {
        BlockedWalk w;
        w.stepTest = false;
        w.b->y = (8 << 16) + 0x8000;
        w.bGoal = {5.0f, 3.0f};
        w.bRoutes();
        w.start();
        const auto either = [&] { return blocked(w) || (w.b->walking && w.b->walking->block.blocker != 0); };
        for (int i = 0; i < 40 && !either(); ++i) {
            w.tickBoth();
        }
        CHECK(either());
        CHECK(gap(w) > 1.0f && gap(w) < 1.8f);
    }
    {
        BlockedWalk w;
        w.stepTest = false;
        w.b->y = 7 << 16;
        w.start();
        CHECK(w.tickUntil([&] { return blocked(w); }));
        CHECK(w.a->walking->block.blocker == w.b->id && gap(w) > 0.9f && gap(w) < 1.25f);  // 0.554 + 0.49 ahead, + a tick's walk
    }
    // A Sim in a container (object data 2 not 0), the walker or the one in the way, doesn't count.
    for (const bool walker : {true, false}) {
        BlockedWalk w;
        w.stepTest = false;
        w.b->y = 7 << 16;
        (walker ? w.a : w.b)->data[2] = -1;
        w.start();
        for (int i = 0; i < 20; ++i) {
            w.tick();
        }
        CHECK(!blocked(w));
    }
    {
        // a starts 0.3 tiles behind b, and they walk away from each other.
        BlockedWalk w;
        w.stepTest = false;
        w.b->y = (5 << 16) + 0x4CCD;
        w.bGoal = {5.0f, 9.0f};
        w.aGoal = {5.0f, 2.0f};
        w.a->data[1] = 0;
        w.bRoutes();
        w.start();
        for (int i = 0; i < 10; ++i) {
            w.tickBoth();
        }
        CHECK(!blocked(w) && w.a->walking && w.a->y < (5 << 16));
    }
    {
        // b crosses a's way at y 7 from x 4.5, gone before a gets there.
        BlockedWalk w;
        w.stepTest = false;
        w.b->x = (4 << 16) + 0x8000;
        w.b->y = 7 << 16;
        w.bGoal = {8.0f, 7.0f};
        w.bRoutes();
        w.start();
        for (int i = 0; i < 25 && w.last == Outcome::Sleeping; ++i) {
            w.tickBoth();
        }
        CHECK(!blocked(w) && w.a->y > (6 << 16));
    }
}

// A standing Sim is asked to move only when the walker's priority (person data 0x21) less 1 is
// above its own, or its person data 0x9F has bit 0x2. When it refuses, the walker steps aside.
void blockedRoutesAskByPriority() {
    for (const bool askable : {false, true}) {
        BlockedWalk w;
        w.a->personData[0x21] = 10;
        w.b->personData[0x21] = 20;
        w.b->personData[0x9F] = askable ? 0x02 : 0;
        w.start();
        CHECK(w.tickUntil([&] { return w.a->personData[0xF3] != 0 || (w.a->walking && w.a->walking->aside != 0); }));
        CHECK(w.a->personData[0xF3] == (askable ? 1 : 0) && w.a->walking && (w.a->walking->aside != 0) == !askable);
        CHECK(w.b->queue.size() == (askable ? 1u : 0u) && w.a->personData[0xE5] == 0);
    }
    BlockedWalk lowest;
    lowest.a->personData[0x21] = 1;  // counts as 1, above 0
    lowest.start();
    CHECK(lowest.tickUntil([&] { return lowest.a->personData[0xF3] != 0 || lowest.a->personData[0xE5] != 0; }));
    CHECK(lowest.a->personData[0xF3] == 1);
}

} // namespace

std::vector<Test> vmWalkingTests() {
    return {
        {"Go To Routing Slot walks the route", goToRoutingSlotWalksTheRoute},
        {"Go To Relative Position makes a slot", goToRelativePositionMakesASlot},
        {"walks run the pre-route tree first", walksRunThePreRouteTreeFirst},
        {"walks run the goal check before setting off", walksRunTheGoalCheckBeforeSettingOff},
        {"walks are planned again when the pre-route tree moves the Sim", walksAreReplannedWhenThePreRouteTreeMovesTheSim},
        {"walks planned again keep their slot", walksPlannedAgainKeepTheirSlot},
        {"walks end with the post-route tree", walksEndWithThePostRouteTree},
        {"walks start and stop with their clips", walksStartAndStopWithTheirClips},
        {"interrupted walks stop soon", interruptedWalksStopSoon},
        {"walks begun right after one keep its foot", walksBegunRightAfterOneKeepItsFoot},
        {"Turn Body Towards turns with its clip", turnBodyTowardsTurnsWithItsClip},
        {"Turn Body Towards faces slots and finds its target after the pre-route", turnBodyTowardsSlotsAndTargetAfterPreRoute},
        {"blocked routes ask a standing Sim to move", blockedRoutesAskAStandingSimToMove},
        {"blocked routes wait for a walking Sim, then fail", blockedRoutesWaitForAWalkingSimThenFail},
        {"blocked routes show feedback only for the only route", blockedRoutesShowFeedbackOnlyForTheOnlyRoute},
        {"routes with no plan show feedback", unplannedRoutesShowFeedback},
        {"blocked routes skip busy and sleeping Sims", blockedRoutesSkipBusyAndSleepingSims},
        {"blocked routes by a gone object fail as unknown", blockedRoutesByAGoneObjectFailAsUnknown},
        {"ended routes cancel the Move they asked for", endedRoutesCancelTheMoveTheyAskedFor},
        {"blocked routes ask by priority", blockedRoutesAskByPriority},
        {"blocked routes plan again when the blocker moved since planning", blockedRoutesPlanAgainWhenTheBlockerMovedSincePlanning},
        {"make-way slots and their cleanup", makeWaySlotsAndTheirCleanup},
        {"blocked routes step aside head-on", blockedRoutesStepAsideHeadOn},
        {"walks look ahead for other Sims", walksLookAheadForOtherSims},
        {"blocked steps need a clear spot", blockedStepsNeedAClearSpot},
    };
}
