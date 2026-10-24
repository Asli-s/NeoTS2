// Set To Next: the searches over objects, multi-tile parts, neighbors, family ties and relationships.
#include "test_support.hpp"

#include "sim/animation.hpp"
#include "sim/ground_click.hpp"
#include "sim/idle_spot.hpp"
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

int countWithLoop(Instruction search, const std::function<void(SmallWorld&)>& setup = {}) {
    MemoryCodeSource code;
    addSmallWorldTypes(code);
    search.trueTarget = 2;
    search.falseTarget = T;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 0, 1, E),
        search,
        expr(owner::kTemp, 0, op::kAdd, owner::kLiteral, 1, 1, E),
    }));
    SmallWorld w;
    if (setup) {
        setup(w);
    }
    Vm vm(code, {}, &w.world);
    const RunResult r = vm.run(*w.controller, 0x1000);
    return r.outcome == Outcome::True ? w.controller->thread.temps[0] : -1;
}

void setToNextVisitsEveryObjectOfAType() {
    CHECK(countWithLoop(setToNext(0x04, owner::kStackObjectId, 0, kFridgeGuid, 0, 0)) == 3);
    CHECK(countWithLoop(setToNext(0x04, owner::kStackObjectId, 0, kChairGuid, 0, 0)) == 1);
    CHECK(countWithLoop(setToNext(0x04, owner::kStackObjectId, 0, 0xDEADBEEF, 0, 0)) == 0);
}

void setToNextSeparatesPeopleFromObjects() {
    CHECK(countWithLoop(setToNext(0x01, owner::kStackObjectId, 0, 0, 0, 0)) == 2);
    CHECK(countWithLoop(setToNext(0x02, owner::kStackObjectId, 0, 0, 0, 0)) == 5);
    CHECK(countWithLoop(setToNext(0x00, owner::kStackObjectId, 0, 0, 0, 0)) == 7);
}

Instruction withFlags(Instruction in, std::uint8_t flags, std::uint16_t field = 0,
                      std::int16_t value = 0) {
    in.operands[8] = flags;
    in.operands[9] = static_cast<std::uint8_t>(field);
    in.operands[10] = static_cast<std::uint8_t>(field >> 8);
    in.operands[11] = static_cast<std::uint8_t>(value);
    in.operands[12] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) >> 8);
    return in;
}

void setToNextSkipsFlaggedObjectsUnlessAsked() {
    const Instruction fridges = setToNext(0x04, owner::kStackObjectId, 0, kFridgeGuid, 0, 0);
    auto flagSecondFridge = [](SmallWorld& w) { w.world.find(4)->data[0x38] = 0x04; };
    auto markThirdFridge = [](SmallWorld& w) { w.world.find(6)->data[0x6B] = 1; };
    CHECK(countWithLoop(fridges, flagSecondFridge) == 2);
    CHECK(countWithLoop(withFlags(fridges, 0x01), flagSecondFridge) == 3);
    CHECK(countWithLoop(fridges, markThirdFridge) == 2);
    CHECK(countWithLoop(withFlags(fridges, 0x04), markThirdFridge) == 3);
}

void setToNextCanRequireADataFieldValue() {
    const Instruction fridges = setToNext(0x04, owner::kStackObjectId, 0, kFridgeGuid, 0, 0);
    auto setField = [](SmallWorld& w) { w.world.find(6)->data[0x0E] = -3; };
    CHECK(countWithLoop(withFlags(fridges, 0x02, 0x0E, -3), setField) == 1);
    CHECK(countWithLoop(withFlags(fridges, 0x02, 0x0E, 0), setField) == 2);
}

void personSearchesFilterBySpecies() {
    auto oneIsAPet = [](SmallWorld& w) { w.world.find(7)->personData[0xBA] = 1; };
    CHECK(countWithLoop(setToNext(0x01, owner::kStackObjectId, 0, 0, 0, 0), oneIsAPet) == 1);
    CHECK(countWithLoop(setToNext(0x28, owner::kStackObjectId, 0, 0, 0, 0), oneIsAPet) == 2);
    CHECK(countWithLoop(setToNext(0x29, owner::kStackObjectId, 0, 0, 0, 0), oneIsAPet) == 1);
    CHECK(countWithLoop(setToNext(0x2B, owner::kStackObjectId, 0, 0, 0, 0), oneIsAPet) == 1);
    CHECK(countWithLoop(setToNext(0x2C, owner::kStackObjectId, 0, 0, 0, 0), oneIsAPet) == 0);
}

// Search type 0x0C: objects whose object data field 0x3B ("category") equals temp 0.
void setToNextFindsObjectsByCategory() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 0, 1, E),
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 5, 2, E),
        setToNext(0x0C, owner::kStackObjectId, 0, 0, 3, T),
        expr(owner::kTemp, 1, op::kAdd, owner::kLiteral, 1, 2, E),
    }));
    SmallWorld w;
    w.world.find(2)->data[0x3B] = 5;
    w.world.find(3)->data[0x3B] = 7;
    w.world.find(5)->data[0x3B] = 5;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(w.controller->thread.temps[1] == 2);
}

// Search type 0x06: like 0x0C with the running tree's param 0.
void setToNextFindsObjectsByCategoryInParam0() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 0, 1, E),
        setToNext(0x06, owner::kStackObjectId, 0, 0, 2, T),
        expr(owner::kTemp, 1, op::kAdd, owner::kLiteral, 1, 1, E),
    }, 1));
    code.add(kObjectGroup, 0x1000, bhav({call(0x1001, T, E, {5})}));
    SmallWorld w;
    w.world.find(2)->data[0x3B] = 5;
    w.world.find(3)->data[0x3B] = 7;
    w.world.find(5)->data[0x3B] = 5;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(w.controller->thread.temps[1] == 2);
}

// Search type 0x08 "obj on same tile": the next object on the target's tile, wrapping around and
// ending with the target itself. A target that is not on the lot is false.
void setToNextFindsObjectsOnTheSameTile() {
    using ts2::sim::Tile;
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        setToNext(0x08, owner::kTemp, 0, 0, T, F),
    }));
    SmallWorld w;
    w.world.find(2)->tile = Tile{3, 4, 1};
    w.world.find(5)->tile = Tile{3, 4, 1};
    w.world.find(1)->tile = Tile{3, 4, 1};
    w.world.find(3)->tile = Tile{3, 4, 2};
    w.world.find(6)->tile = Tile{3, 5, 1};
    Vm vm(code, {}, &w.world);
    auto next = [&](std::int16_t from) {
        w.controller->thread.temps[0] = from;
        const RunResult r = vm.run(*w.controller, 0x1000);
        return r.outcome == Outcome::True ? w.controller->thread.temps[0] : std::int16_t{-1};
    };
    CHECK(next(2) == 5);
    CHECK(next(5) == 1);
    CHECK(next(1) == 2);
    CHECK(next(3) == 3);
    CHECK(next(4) == -1 && w.controller->thread.temps[0] == 4);
    CHECK(next(0) == -1);
    w.world.find(5)->data[0x38] = 0x04;
    CHECK(next(2) == 1);
    w.world.find(2)->data[0x2A] = 0x2000;
    w.controller->thread.temps[0] = 2;
    const RunResult refused = vm.run(*w.controller, 0x1000);
    CHECK(refused.outcome == Outcome::Error && refused.reason == "could not find object reference");
}

// Search type 0x03 "part of a multi-tile object": parts share object data 0x13 (the lead's id).
// It gives the next part in falling id order, wrapping around. A single-tile target is false and becomes 0.
void setToNextWalksMultiTileParts() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        setToNext(0x03, owner::kTemp, 0, 0, T, F),
    }));
    SmallWorld w;
    for (std::int16_t id = 1; id <= 7; ++id) {
        w.world.find(id)->data[0x13] = id;
    }
    for (const std::int16_t id : {std::int16_t{2}, std::int16_t{4}, std::int16_t{6}}) {
        w.world.find(id)->definition.fields[ts2::objd::field::kMultiTileMasterId] = 0x55;
        w.world.find(id)->data[0x13] = 4;
    }
    w.world.find(3)->definition.fields[ts2::objd::field::kMultiTileMasterId] = 0x66;
    Vm vm(code, {}, &w.world);
    auto next = [&](std::int16_t from) {
        w.controller->thread.temps[0] = from;
        const RunResult r = vm.run(*w.controller, 0x1000);
        return r.outcome == Outcome::True ? w.controller->thread.temps[0] : std::int16_t{-1};
    };
    CHECK(next(6) == 4);
    CHECK(next(4) == 2);
    CHECK(next(2) == 6);
    CHECK(next(3) == 3);
    CHECK(next(5) == -1 && w.controller->thread.temps[0] == 0);
    CHECK(next(0) == -1);
}

// Search 0x12 takes the type's GUID from temps 0 and 1. An unknown type is false and leaves the
// target alone.
void setToNextObjectOfTypeInTemps() {
    MemoryCodeSource code;
    addSmallWorldTypes(code);
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, static_cast<std::uint16_t>(kChairGuid & 0xFFFF), 1, E),
        expr(owner::kTemp, 1, op::kAssign, owner::kLiteral, static_cast<std::uint16_t>(kChairGuid >> 16), 2, E),
        setToNext(0x12, owner::kLocal, 0, 0, T, F),
    }, 0, 1));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kLocal, 0, op::kAssign, owner::kLiteral, 5, 1, E),
        expr(owner::kTemp, 1, op::kAssign, owner::kLiteral, 0x0BAD, 2, E),
        setToNext(0x12, owner::kLocal, 0, 0, T, 3),
        expr(owner::kTemp, 2, op::kAssign, owner::kLocal, 0, F, E),
    }, 0, 1));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(vm.run(*w.controller, 0x1001).outcome == Outcome::False && w.controller->thread.temps[2] == 5);
}

// Searches 0x05 and 0x2E-0x33 walk neighbor ids by species (person data 0xBA). A neighbor whose
// Sim is on the lot must pass the filters.
void setToNextWalksNeighborsBySpecies() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({setToNext(0x05, owner::kTemp, 0, 0, T, F)}));
    code.add(kObjectGroup, 0x1001, bhav({setToNext(0x2F, owner::kTemp, 0, 0, T, F)}));
    SmallWorld w;
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {3, 8, 12, 20};
    auto neighbor = [&](std::int16_t id, std::uint32_t guid, std::int16_t species) {
        hood->neighbors[id].guid = guid;
        hood->neighbors[id].personData.assign(231, 0);
        hood->neighbors[id].personData[0xBA] = species;
    };
    neighbor(3, 0x0D000003, 0);
    neighbor(8, 0x51515151, 0);  // the person on the lot (object 5)
    neighbor(12, 0x0D00000C, 2);
    neighbor(20, 0x0D000014, 0);
    w.world.setNeighborhood(hood);
    Vm vm(code, {}, &w.world);
    auto next = [&](std::uint16_t tree, std::int16_t from) {
        w.controller->thread.temps[0] = from;
        const RunResult r = vm.run(*w.controller, tree);
        return r.outcome == Outcome::True ? w.controller->thread.temps[0] : std::int16_t{-1};
    };
    CHECK(next(0x1000, 0) == 3);
    CHECK(next(0x1000, 3) == 8);
    w.world.find(5)->data[0x38] = 0x01;  // now its Sim is skipped
    CHECK(next(0x1000, 3) == 20);
    CHECK(next(0x1000, 20) == -1 && w.controller->thread.temps[0] == 0);
    CHECK(next(0x1001, 0) == 12);
}

// Search 0x11: neighbors whose family (person data 0x3D) is temp 0, in id order.
void setToNextWalksNeighborsOfAFamily() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({setToNext(0x11, owner::kTemp, 1, 0, T, F)}));
    SmallWorld w;
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {3, 8, 12};
    auto neighbor = [&](std::int16_t id, std::uint32_t guid, std::int16_t family) {
        hood->neighbors[id].guid = guid;
        hood->neighbors[id].personData.assign(231, 0);
        hood->neighbors[id].personData[0x3D] = family;
    };
    neighbor(3, 0x0D000003, 4);
    neighbor(8, 0x0D000008, 2);
    neighbor(12, 0x0D00000C, 4);
    w.world.setNeighborhood(hood);
    Vm vm(code, {}, &w.world);
    auto next = [&](std::int16_t from) {
        w.controller->thread.temps[0] = 4;
        w.controller->thread.temps[1] = from;
        const RunResult r = vm.run(*w.controller, 0x1000);
        return r.outcome == Outcome::True ? w.controller->thread.temps[1] : std::int16_t{-1};
    };
    CHECK(next(0) == 3);
    CHECK(next(3) == 12);
    CHECK(next(12) == -1 && w.controller->thread.temps[1] == 0);
}

// Searches 0x15-0x17 and 0x1F walk the family ties of the neighbor in temp 0: siblings in the
// record's order, then false with 0. Parents: father, mother, then 0. Neighbors gone are skipped.
void setToNextWalksFamilyTies() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({setToNext(0x16, owner::kTemp, 1, 0, T, F)}));
    code.add(kObjectGroup, 0x1001, bhav({setToNext(0x15, owner::kTemp, 1, 0, T, F)}));
    code.add(kObjectGroup, 0x1002, bhav({setToNext(0x1F, owner::kTemp, 1, 0, T, F)}));
    SmallWorld w;
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {3, 8, 12, 20};
    using ts2::famt::TieType;
    auto tie = [](TieType type, std::int16_t other) { return ts2::famt::Tie{static_cast<std::uint32_t>(type), other}; };
    hood->familyTies[3] = {tie(TieType::Sibling, 20), tie(TieType::Sibling, 99), tie(TieType::Sibling, 12), tie(TieType::Mother, 8)};
    w.world.setNeighborhood(hood);
    Vm vm(code, {}, &w.world);
    auto next = [&](std::uint16_t tree, std::int16_t from) {
        w.controller->thread.temps[0] = 3;
        w.controller->thread.temps[1] = from;
        const RunResult r = vm.run(*w.controller, tree);
        return r.outcome == Outcome::True ? w.controller->thread.temps[1] : std::int16_t{-1};
    };
    CHECK(next(0x1000, 0) == 20);
    CHECK(next(0x1000, 20) == 12);
    CHECK(next(0x1000, 12) == -1 && w.controller->thread.temps[1] == 0);
    // No father: from 0 straight to the mother, then 0.
    CHECK(next(0x1001, 0) == 8);
    CHECK(next(0x1001, 8) == -1 && w.controller->thread.temps[1] == 0);
    CHECK(next(0x1002, 0) == -1 && w.controller->thread.temps[1] == 0);
    w.controller->thread.temps[0] = 77;
    CHECK(vm.run(*w.controller, 0x1000).reason == "neighbor not found");
}

// Search 0x24: neighbors toward whom temp 0's relationship variable 1 has any bit of temp 1, by id.
void setToNextWalksRelationshipFlags() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({setToNext(0x24, owner::kTemp, 2, 0, T, F)}));
    SmallWorld w;
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {1, 5, 9, 12};
    hood->relationships[{std::int16_t{1}, std::int16_t{5}}] = {0, 0x02};
    hood->relationships[{std::int16_t{1}, std::int16_t{9}}] = {0, 0x10};
    hood->relationships[{std::int16_t{12}, std::int16_t{1}}] = {0, 0x08};
    hood->relationships[{std::int16_t{1}, std::int16_t{12}}] = {0, 0x09};
    w.world.setNeighborhood(hood);
    Vm vm(code, {}, &w.world);
    auto next = [&](std::int16_t from) {
        w.controller->thread.temps[0] = 1;
        w.controller->thread.temps[1] = 0x18;
        w.controller->thread.temps[2] = from;
        const RunResult r = vm.run(*w.controller, 0x1000);
        return r.outcome == Outcome::True ? w.controller->thread.temps[2] : std::int16_t{-1};
    };
    CHECK(next(0) == 9);
    CHECK(next(9) == 12);
    CHECK(next(12) == -1 && w.controller->thread.temps[2] == 0);
}

// Search 0x23: neighbors toward whom temp 0's relationship variable 0 is above temp 1, by id. A
// neighbor with no variables toward them doesn't count.
void setToNextWalksRelationshipsAbove() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({setToNext(0x23, owner::kTemp, 2, 0, T, F)}));
    SmallWorld w;
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {1, 5, 9, 12};
    hood->relationships[{std::int16_t{1}, std::int16_t{5}}] = {-20};
    hood->relationships[{std::int16_t{1}, std::int16_t{9}}] = {};
    hood->relationships[{std::int16_t{1}, std::int16_t{12}}] = {30};
    w.world.setNeighborhood(hood);
    Vm vm(code, {}, &w.world);
    auto next = [&](std::int16_t from, std::int16_t above) {
        w.controller->thread.temps[0] = 1;
        w.controller->thread.temps[1] = above;
        w.controller->thread.temps[2] = from;
        const RunResult r = vm.run(*w.controller, 0x1000);
        return r.outcome == Outcome::True ? w.controller->thread.temps[2] : std::int16_t{-1};
    };
    CHECK(next(0, -999) == 5);
    CHECK(next(5, -999) == 12);
    CHECK(next(0, 0) == 12);
    CHECK(next(12, -999) == -1);
}

// Search 0x09: objects beside the object in a local, in the same room, at +x, +y, -x, -y.
void setToNextFindsAdjacentObjects() {
    using ts2::sim::Tile;
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kLocal, 0, op::kAssign, owner::kLiteral, 2, 1, E),
        [] {
            Instruction in = setToNext(0x09, owner::kTemp, 0, 0, T, F);
            in.operands[6] = 0;
            return in;
        }(),
    }, 0, 1));
    SmallWorld w;
    w.world.find(2)->tile = Tile{10, 10, 1};
    w.world.find(4)->tile = Tile{10, 11, 1};  // +y
    w.world.find(6)->tile = Tile{9, 10, 1};   // -x
    w.world.find(7)->tile = Tile{11, 10, 1};  // +x, but another room
    w.world.find(3)->tile = Tile{10, 9, 2};   // -y on another level
    w.world.find(7)->data[0x1D] = 5;
    Vm vm(code, {}, &w.world);
    auto next = [&](std::int16_t from) {
        w.controller->thread.temps[0] = from;
        const RunResult r = vm.run(*w.controller, 0x1000);
        return r.outcome == Outcome::True ? w.controller->thread.temps[0] : std::int16_t{-1};
    };
    CHECK(next(0) == 4);
    CHECK(next(4) == 6);
    CHECK(next(6) == -1 && w.controller->thread.temps[0] == 6);
}

void setToNextFindsObjectsInView() {
    using ts2::sim::Tile;
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({setToNext(0x18, owner::kTemp, 1, 0, T, F)}));
    ts2::sim::World world;
    auto place = [&](ts2::sim::SimObject& o, int x, int y) {
        o.tile = Tile{static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y), 1};
        o.x = x * 65536 + 32768;
        o.y = y * 65536 + 32768;
        return o.id;
    };
    auto& me = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    place(me, 10, 10);
    me.data[1] = 2;  // facing +x
    const auto ahead = place(world.create(kObjectGroup, typed(kFridgeGuid)), 12, 10);
    place(world.create(kObjectGroup, typed(kFridgeGuid)), 8, 10);  // behind
    auto& otherRoom = world.create(kObjectGroup, typed(kFridgeGuid));
    place(otherRoom, 12, 10);
    otherRoom.data[0x1D] = 5;
    place(world.create(kObjectGroup, typed(kFridgeGuid)), 13, 10);  // past the wall
    const auto held = place(world.create(kObjectGroup, typed(kFridgeGuid)), 11, 10);
    me.slots = {held};
    place(world.create(kObjectGroup, typed(kChairGuid, 5)), 11, 10);  // object type 5
    const auto beside = place(world.create(kObjectGroup, typed(kFridgeGuid)), 10, 11);
    place(world.create(kObjectGroup, typed(kFridgeGuid)), 15, 10);  // too far
    place(world.create(kObjectGroup, typed(kFridgeGuid)), 12, 12);  // the line touches a wall's end
    ts2::sim::WallTiles walls;
    walls.add(0, 13, 9, 13, 12);
    walls.add(0, 12, 12, 12, 13);
    world.setSightWalls(walls);
    Vm vm(code, {}, &world);
    auto next = [&](std::int16_t from) {
        me.thread.temps[0] = 4;
        me.thread.temps[1] = from;
        const RunResult r = vm.run(me, 0x1000);
        return r.outcome == Outcome::True ? me.thread.temps[1] : std::int16_t{-1};
    };
    CHECK(next(0) == ahead);
    CHECK(next(ahead) == beside);
    CHECK(next(beside) == -1 && me.thread.temps[1] == 0);

    // The list stays as it was made until the Sim's next tick, even for a new search.
    place(me, 10, 20);
    CHECK(next(0) == ahead);
    me.inViewStale = true;
    CHECK(next(0) == -1);
}

void setToNextCanTargetALocal() {
    MemoryCodeSource code;
    addSmallWorldTypes(code);
    code.add(kObjectGroup, 0x1000, bhav({
        setToNext(0x04, owner::kLocal, 1, kChairGuid, 1, F),
        expr(owner::kTemp, 0, op::kAssign, owner::kLocal, 1, T, E),
    }, 0, 2));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(w.controller->thread.temps[0] == 3);
    CHECK(w.controller->thread.stackObjectId == 0);
}

void verifySearchesCheckTheTargetWithoutMovingIt() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kParam, 0, 1, E),
        setToNext(0x1A, owner::kStackObjectId, 0, 0, 2, F),
        setToNext(0x1E, owner::kStackObjectId, 0, 0, T, F),
    }, 1));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    const std::int16_t sim[] = {5};
    const std::int16_t fridge[] = {2};
    const std::int16_t missing[] = {42};
    CHECK(vm.run(*w.controller, 0x1000, sim).outcome == Outcome::True);
    CHECK(w.controller->thread.stackObjectId == 5);
    CHECK(vm.run(*w.controller, 0x1000, fridge).outcome == Outcome::False);
    CHECK(w.controller->thread.stackObjectId == 2);
    CHECK(vm.run(*w.controller, 0x1000, missing).outcome == Outcome::False);
    CHECK(w.controller->thread.stackObjectId == 42);
}

void setToNextWrapsAroundLikeHideAll() {
    MemoryCodeSource code;
    addSmallWorldTypes(code);
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kLocal, 0, op::kAssign, owner::kStackObjectId, 0, 1, E),
        expr(owner::kStackObjectAttribute, 0, op::kAdd, owner::kLiteral, 1, 2, E),
        setToNext(0x04, owner::kStackObjectId, 0, kFridgeGuid, 3, 2),
        expr(owner::kStackObjectId, 0, op::kEquals, owner::kLocal, 0, T, 1),
    }, 0, 1));
    SmallWorld w;
    w.controller->thread.stackObjectId = 4;
    Vm vm(code, {1000, 64}, &w.world);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(w.world.find(2)->attributes[0] == 1);
    CHECK(w.world.find(4)->attributes[0] == 1);
    CHECK(w.world.find(6)->attributes[0] == 1);
    CHECK(w.controller->thread.stackObjectId == 4);
}

} // namespace

std::vector<Test> vmSetToNextTests() {
    return {
        {"Set to Next visits every object of a type", setToNextVisitsEveryObjectOfAType},
        {"Set to Next separates people from objects", setToNextSeparatesPeopleFromObjects},
        {"Set to Next skips flagged objects unless asked", setToNextSkipsFlaggedObjectsUnlessAsked},
        {"Set to Next can require a data field value", setToNextCanRequireADataFieldValue},
        {"person searches filter by species", personSearchesFilterBySpecies},
        {"Set to Next finds objects by category in param 0", setToNextFindsObjectsByCategoryInParam0},
        {"Set to Next walks neighbors of a family", setToNextWalksNeighborsOfAFamily},
        {"Set to Next finds objects by category", setToNextFindsObjectsByCategory},
        {"Set to Next finds objects on the same tile", setToNextFindsObjectsOnTheSameTile},
        {"Set to Next walks multi-tile parts", setToNextWalksMultiTileParts},
        {"Set to Next finds an object type from temps", setToNextObjectOfTypeInTemps},
        {"Set to Next walks neighbors by species", setToNextWalksNeighborsBySpecies},
        {"Set to Next walks family ties", setToNextWalksFamilyTies},
        {"Set to Next walks relationship flags", setToNextWalksRelationshipFlags},
        {"Set to Next walks relationships above a value", setToNextWalksRelationshipsAbove},
        {"Set to Next finds adjacent objects", setToNextFindsAdjacentObjects},
        {"Set to Next finds objects in a Sim's view", setToNextFindsObjectsInView},
        {"Set to Next can target a local", setToNextCanTargetALocal},
        {"verify searches check the target without moving it", verifySearchesCheckTheTargetWithoutMovingIt},
        {"Set to Next wraps around like 'Hide all'", setToNextWrapsAroundLikeHideAll},
    };
}
