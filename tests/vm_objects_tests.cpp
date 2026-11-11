// Primitives that find, make, compare and remove objects, neighbors and relationships.
#include "sim/forget_object.hpp"
#include "sim/object_model.hpp"
#include "sim/object_reset.hpp"
#include "sim/snap.hpp"
#include "test_support.hpp"

namespace {

Instruction testObjectType(std::uint32_t guid, std::uint8_t idOwner, std::uint16_t idData,
                           std::uint16_t t, std::uint16_t f) {
    Instruction in;
    in.opcode = 0x0020;
    in.trueTarget = t;
    in.falseTarget = f;
    for (std::size_t i = 0; i < 4; ++i) {
        in.operands[i] = static_cast<std::uint8_t>(guid >> (8 * i));
    }
    in.operands[4] = static_cast<std::uint8_t>(idData);
    in.operands[5] = static_cast<std::uint8_t>(idData >> 8);
    in.operands[6] = idOwner;
    return in;
}

// What only a Sim uses takes no room on other objects. The person data is made on its first write,
// and a walk, a running interaction and the like are boxed. They read, write and copy as the plain
// arrays and optionals they replace.
void aSimObjectKeepsWhatOnlySimsUseOnTheHeap() {
    ts2::sim::World world;
    auto& chair = world.create(kObjectGroup, typed(0x51515152));
    const auto& readOnly = chair;
    CHECK(readOnly.personData[0x3A] == 0 && readOnly.personData.size() == 256 && !chair.personData.allocated());  // a look is not a write
    CHECK(!chair.walking && !chair.running && !chair.runningSub && !chair.turning && !chair.walkTail && !chair.postRoute && !chair.reaching);
    chair.personData[0x3A] = 27;  // a write makes it
    CHECK(chair.personData.allocated() && readOnly.personData[0x3A] == 27 && readOnly.personData[0x3B] == 0);
    auto copy = chair;  // a copy has a copy of the values
    copy.personData[0x3A] = 5;
    CHECK(chair.personData[0x3A] == 27 && copy.personData[0x3A] == 5);
    chair.running.emplace().object = 7;
    CHECK(chair.running && chair.running->object == 7 && !chair.runningSub);
    auto moved = chair;
    chair.running->object = 8;
    CHECK(moved.running->object == 7 && chair.running->object == 8);
    chair.running = std::nullopt;
    CHECK(!chair.running && moved.running);
    moved.running.reset();
    CHECK(!moved.running);
    const ts2::sim::Boxed<int> empty;
    CHECK(!empty.has_value() && empty.value_or(3) == 3 && empty == std::nullopt);
    ts2::sim::Boxed<int> one = 4;
    one = 9;
    CHECK(*one == 9 && one.value_or(3) == 9 && one != std::nullopt);
}

// "My slot" / "stack object's slot" (owners 0x14 / 0x10): the object held in a container slot.
void slotOwnersReadTheHeldObject() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, ts2::sim::owner::kMySlot, 1, 1, E),
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kMyData, 0x0B, 2, E),
        expr(owner::kTemp, 1, op::kAssign, ts2::sim::owner::kStackObjectSlot, 0, T, E),
    }));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kTemp, 0, op::kAssign, ts2::sim::owner::kMySlot, 2, T, E),
    }));
    ts2::sim::World world;
    auto& counter = world.create(kObjectGroup, typed(kFridgeGuid), 2);
    auto& plate = world.create(kObjectGroup, typed(kChairGuid));
    counter.slots[1] = plate.id;
    CHECK(counter.slots.size() == 2 && plate.slots.empty());
    Vm vm(code, {}, &world);
    CHECK(vm.run(counter, 0x1000).outcome == Outcome::True);
    CHECK(counter.thread.temps[0] == plate.id && counter.thread.temps[1] == 0);
    const RunResult out = vm.run(counter, 0x1001);
    CHECK(out.outcome == Outcome::Error && out.reason == "slot number out of range");
}

// Find Location For placing a Sim by itself: the centre of its own tile, facing the first allowed
// direction (here only the Sim's own). With no facing allowed it keeps its own.
void findLocationForCentresASimOnItsTile() {
    MemoryCodeSource code;
    Instruction find = call(0x0010, T, F);
    const std::uint8_t operands[10] = {0, 0, 0x0A, 0xFE, owner::kMyData, 0x0B, 0, owner::kMyData, 0x0B, 0};
    std::copy(std::begin(operands), std::end(operands), find.operands.begin());
    code.add(kObjectGroup, 0x1000, bhav({find}));
    find.operands[3] = 0xFF;
    code.add(kObjectGroup, 0x1001, bhav({find}));
    SmallWorld w;
    ts2::sim::SimObject& sim = *w.world.find(5);
    sim.tile = ts2::sim::Tile{3, 4, 1};
    sim.x = 3 * 65536 + 100;
    sim.y = 4 * 65536 + 60000;
    sim.data[1] = 2;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(sim.x == 3 * 65536 + 32768 && sim.y == 4 * 65536 + 32768 && sim.data[1] == 2);
    sim.data[1] = 3;
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::True && sim.data[1] == 3);
    // Mode 1 takes the stack object out of the slot it was in and out of the world.
    Instruction stay = call(0x0010, T, F);
    stay.operands = {1, 0, 0x02};
    code.add(kObjectGroup, 0x1002, bhav({stay}));
    sim.thread.stackObjectId = 2;
    auto& held = *w.world.find(2);
    sim.slots.assign(2, 0);
    sim.slots[0] = held.id;
    held.data[2] = sim.id;
    held.data[3] = 0;
    CHECK(vm.run(sim, 0x1002).outcome == Outcome::True && held.x == ts2::sim::kOffLot && held.y == ts2::sim::kOffLot && !held.tile);
    CHECK(sim.slots[0] == 0 && held.data[2] == 0 && held.data[3] == -1);
}

// Find Location For "on level ground" (flag 0x10) clears bit 0x100 of the placed object's data 0x2A
// when it finds a place, and leaves the data as it was when it finds none.
void findLocationForOnLevelGroundClearsAPlacementBit() {
    MemoryCodeSource code;
    Instruction find = call(0x0010, T, F);
    const std::uint8_t operands[10] = {0, 0, 0x1A, 0, owner::kMyData, 0x0B, 0, owner::kMyData, 0x0B, 0};
    std::copy(std::begin(operands), std::end(operands), find.operands.begin());
    code.add(kObjectGroup, 0x1000, bhav({find}));
    find.operands[3] = 0x55;  // only diagonal turns allowed
    code.add(kObjectGroup, 0x1001, bhav({find}));
    SmallWorld w;
    ts2::sim::SimObject& sim = *w.world.find(5);
    sim.tile = ts2::sim::Tile{3, 4, 1};
    sim.x = 3 * 65536 + 32768;
    sim.y = 4 * 65536 + 32768;
    sim.data[0x2A] = 0x0181;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True && sim.data[0x2A] == 0x0081);
    sim.data[0x2A] = 0x0181;
    sim.data[1] = 0;
    sim.data[0x17] = 2;  // a Sim that never faces diagonally: nowhere to go
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::False && sim.data[0x2A] == 0x0181);
}

// A Sim reset leaves a standing Sim in its slot and moves a seated one out of it, keeping its
// facing, to the first tile the search finds: its own when nothing is in the way, else the next.
void aSimResetMovesASeatedSimOutOfItsSlot() {
    SmallWorld w;
    ts2::sim::SimObject& sim = *w.world.find(5);
    ts2::sim::SimObject& chair = *w.world.find(3);
    chair.slots.assign(1, 0);
    chair.data[0x1C] = 1;  // capacity
    const auto seat = [&] {
        chair.slots[0] = sim.id;
        sim.data[2] = chair.id;
        sim.data[3] = 0;
        sim.x = 3 * 65536 + 100;
        sim.y = 4 * 65536 + 60000;
        sim.level = 1;
        sim.tile.reset();
        sim.data[1] = 3;
    };
    ts2::sim::SimRandom rng{1};
    seat();
    ts2::sim::placeAfterReset(w.world, sim, rng);
    CHECK(chair.slots[0] == sim.id && sim.data[2] == chair.id && sim.data[3] == 0 && sim.x == 3 * 65536 + 100);
    seat();
    sim.personData[0] = 1;  // Sitting?
    ts2::sim::placeAfterReset(w.world, sim, rng);
    CHECK(chair.slots[0] == 0 && sim.data[2] == 0 && sim.data[3] == -1);
    CHECK(sim.x == 3 * 65536 + 32768 && sim.y == 4 * 65536 + 32768 && sim.tile && sim.data[1] == 3);
    // A standing Sim that no longer fits the slot leaves it too, and the tile in the way is passed by.
    w.world.setObjectInTheWay([](const ts2::sim::SimObject&, float x, float y, std::int32_t, std::int16_t) {
        return static_cast<int>(x) == 3 && static_cast<int>(y) == 4;
    });
    seat();
    sim.personData[0] = 0;
    sim.data[0x1B] = 2;  // bigger than the chair holds
    ts2::sim::placeAfterReset(w.world, sim, rng);
    CHECK(chair.slots[0] == 0 && sim.data[2] == 0 && sim.tile && !(sim.tile->x == 3 && sim.tile->y == 4) && sim.data[1] == 3);
}

// A Sim who goes takes its head slot holder with it, and what the holder holds goes too. A Sim in
// an NPC template's slot is taken out and reset instead.
void removedObjectsTakeWhatTheirSlotsHold() {
    SmallWorld w;
    ts2::sim::SimObject& sim = *w.world.find(5);
    ts2::sim::SimObject& holder = *w.world.find(2);
    ts2::sim::SimObject& held = *w.world.find(4);
    const auto put = [](ts2::sim::SimObject& object, ts2::sim::SimObject& container, std::int16_t slot) {
        container.slots.resize(std::max<std::size_t>(container.slots.size(), static_cast<std::size_t>(slot) + 1), 0);
        container.slots[static_cast<std::size_t>(slot)] = object.id;
        object.data[2] = container.id;
        object.data[3] = slot;
    };
    put(holder, sim, 2);
    put(held, holder, 0);
    int resets = 0;
    const auto reset = [&](ts2::sim::SimObject&) { ++resets; };
    w.world.remove(sim);
    ts2::sim::removeSlotContents(w.world, reset);
    CHECK(holder.removed && held.removed && !w.world.find(6)->removed && resets == 0);

    ts2::sim::SimObject& npcTemplate = w.world.create(kObjectGroup, typed(0x10101010, 0x10));
    ts2::sim::SimObject& other = *w.world.find(7);
    put(other, npcTemplate, 0);
    w.world.remove(npcTemplate);
    ts2::sim::removeSlotContents(w.world, reset);
    CHECK(!other.removed && other.data[2] == 0 && other.data[3] == -1 && npcTemplate.slots[0] == 0 && resets == 1);
}

// Johnny's drop: the stack object goes next to the Sim in local 0, whose own tile isn't empty.
void findLocationForSkipsTheSimsTile() {
    MemoryCodeSource code;
    Instruction find = call(0x0010, T, F);
    const std::uint8_t operands[10] = {2, 0, 0x0D, 0, owner::kStackObjectId, 0, 0, owner::kLocal, 0, 0};
    std::copy(std::begin(operands), std::end(operands), find.operands.begin());
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kLocal, 0, op::kAssign, owner::kMyData, 0x0B, 1, E),
        find,
    }, 0, 1));
    SmallWorld w;
    ts2::sim::SimObject& sim = *w.world.find(5);
    sim.tile = ts2::sim::Tile{3, 4, 1};
    sim.x = 3 * 65536 + 32768;
    sim.y = 4 * 65536 + 32768;
    sim.thread.stackObjectId = 2;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    const ts2::sim::SimObject& placed = *w.world.find(2);
    CHECK(placed.x == 2 * 65536 + 32768 && placed.y == 4 * 65536 + 32768);
    CHECK(placed.tile && placed.tile->x == 2 && placed.tile->y == 4);
    CHECK(placed.data[1] >= 0 && placed.data[1] < 8);
}

// Find Location For flag 0x40 (the flies around a reference) starts in front of the reference: the walk
// goes round from its facing, and a start tile that isn't that way from the reference's position gives
// way to the tile in front, while one that is stays.
void findLocationForStartsInFrontOfTheReference() {
    MemoryCodeSource code;
    Instruction find = call(0x0010, T, F);
    find.operands[0] = 2;
    find.operands[2] = 0x40;
    code.add(kObjectGroup, 0x1000, bhav({find}));
    find.operands[2] = 0x42;
    code.add(kObjectGroup, 0x1001, bhav({find}));
    SmallWorld w;
    ts2::sim::SimObject& sim = *w.world.find(5);
    sim.tile = ts2::sim::Tile{3, 4, 1};
    sim.x = 3 * 65536 + 32768;
    sim.y = 4 * 65536 + 32768;
    sim.data[1] = 2;
    sim.thread.stackObjectId = 2;
    Vm vm(code, {}, &w.world);
    const ts2::sim::SimObject& placed = *w.world.find(2);
    // My own tile is taken, so the first step goes my way (+x) rather than the default walk's -x.
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(placed.x == 4 * 65536 + 32768 && placed.y == 4 * 65536 + 32768);
    // My tile's centre is no direction from my position, which counts as the +x+y diagonal: facing +x
    // moves the walk in front of me, facing the diagonal keeps my tile.
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::True);
    CHECK(placed.x == 4 * 65536 + 32768 && placed.y == 4 * 65536 + 32768);
    sim.data[1] = 3;
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::True);
    CHECK(placed.x == 3 * 65536 + 32768 && placed.y == 4 * 65536 + 32768);
    // Facing -y from the tile's top edge: the tile's centre lies +y, so the walk moves to the tile above.
    sim.data[1] = 0;
    sim.y = 4 * 65536;
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::True);
    CHECK(placed.x == 3 * 65536 + 32768 && placed.y == 3 * 65536 + 32768);
}

// Find Location For mode 3 (a car's driver placed by its portal) walks the line the reference faces,
// both ways in turn: 1 tile ahead, 1 back, 2 ahead... and never round. Mode 4 turns the line a quarter.
void findLocationForWalksTheReferencesLine() {
    MemoryCodeSource code;
    Instruction find = call(0x0010, T, F);
    find.operands[0] = 3;
    code.add(kObjectGroup, 0x1000, bhav({find}));
    find.operands[0] = 4;
    code.add(kObjectGroup, 0x1001, bhav({find}));
    SmallWorld w;
    ts2::sim::SimObject& sim = *w.world.find(5);
    sim.tile = ts2::sim::Tile{3, 4, 1};
    sim.x = 3 * 65536 + 32768;
    sim.y = 4 * 65536 + 32768;
    sim.data[1] = 2;
    sim.thread.stackObjectId = 2;
    Vm vm(code, {}, &w.world);
    const ts2::sim::SimObject& placed = *w.world.find(2);
    // My own tile is taken, so the first tile is the one ahead (+x).
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(placed.x == 4 * 65536 + 32768 && placed.y == 4 * 65536 + 32768);
    // With that one taken too, the tile behind comes next, not the ones beside me.
    w.world.find(3)->tile = ts2::sim::Tile{4, 4, 1};
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(placed.x == 2 * 65536 + 32768 && placed.y == 4 * 65536 + 32768);
    // Mode 4: the line +y.
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::True);
    CHECK(placed.x == 3 * 65536 + 32768 && placed.y == 5 * 65536 + 32768);
}

// Jill's floor sit marker: Find Location For mode 8 puts the stack object on the first spot of global
// slot 0x37 by me that it may face, facing the slot's way (-1: my own facing on my spot, away from me
// elsewhere). An object refusing diagonals skips a spot it would face diagonally. No spots is false,
// a slot past the list is an error, and without the lot's route grid it is unsupported.
void findLocationForPlacesByAGlobalSlot() {
    MemoryCodeSource code;
    Instruction find = call(0x0010, T, F);
    find.operands = {8, 0, 0x0A, 0, owner::kStackObjectId, 0, 0, owner::kMyData, 0x0B, 0, 0x37};
    code.add(kObjectGroup, 0x1000, bhav({find}));
    code.globals.resize(0x38);
    code.globals[0x37].type = ts2::slot::kRoutingSlot;
    code.globals[0x37].routingFlags = 0x41000;
    code.globals[0x37].facing = -1;
    SmallWorld w;
    ts2::sim::SimObject& sim = *w.world.find(5);
    sim.tile = ts2::sim::Tile{3, 4, 1};
    sim.x = 3 * 65536 + 16384;
    sim.y = 4 * 65536 + 49152;
    sim.data[1] = 2;
    sim.thread.stackObjectId = 2;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::Unsupported);

    std::vector<std::pair<float, float>> spots;
    w.world.setPlacementSpots([&](const ts2::sim::SimObject& placed, const ts2::sim::SimObject& owner, const ts2::slot::Entry& slot,
                                  std::span<const std::optional<ts2::sim::ModelBone>>) {
        CHECK(placed.id == 2 && owner.id == sim.id && &slot == &code.globals[0x37]);
        return spots;
    });
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::False);
    spots = {{3.25f, 4.75f}};
    ts2::sim::SimObject& marker = *w.world.find(2);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(marker.x == sim.x && marker.y == sim.y && marker.data[1] == 2 && marker.tile && marker.tile->x == 3);
    spots = {{4.25f, 3.75f}, {3.25f, 2.75f}};
    marker.data[0x17] = 2;
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(marker.x == 3 * 65536 + 16384 && marker.y == 2 * 65536 + 49152 && marker.data[1] == 0);

    // A bone slot: its joints are handed over and the object faces as Snap faces the joint's spot.
    code.globals[0x37].objectFlags = 0x20000;
    code.globals[0x37].bone = 0;
    code.addStrings(ts2::sim::kGlobalGroup, 0x90, {"Seat_A"});
    w.world.setModelBones([](std::int16_t, const std::string& name) -> std::optional<ts2::sim::ModelBone> {
        return name == "seat_a" ? std::optional<ts2::sim::ModelBone>({0.5f, 0.25f, 1.5f}) : std::nullopt;
    });
    const auto joint = ts2::sim::routingSlotTarget(sim, code.globals[0x37], ts2::sim::ModelBone{0.5f, 0.25f, 1.5f});
    CHECK(joint.has_value());
    std::size_t jointsSeen = 0;
    w.world.setPlacementSpots([&](const ts2::sim::SimObject&, const ts2::sim::SimObject&, const ts2::slot::Entry&,
                                  std::span<const std::optional<ts2::sim::ModelBone>> joints) {
        jointsSeen = joints.size();
        return std::vector<std::pair<float, float>>{{joint->x / 65536.0f, joint->y / 65536.0f}};
    });
    marker.data[0x17] = 0;
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(jointsSeen == 1 && marker.x == joint->x && marker.y == joint->y && marker.data[1] == joint->facing);

    // A part of a multi-tile object other than its lead takes its joints from the lead's model, placed
    // where the lead stands.
    std::optional<ts2::sim::ModelBone> seen;
    w.world.setPlacementSpots([&](const ts2::sim::SimObject&, const ts2::sim::SimObject&, const ts2::slot::Entry&,
                                  std::span<const std::optional<ts2::sim::ModelBone>> joints) {
        seen = joints.empty() ? std::nullopt : joints[0];
        return std::vector<std::pair<float, float>>{};
    });
    sim.data[0x13] = 2;
    vm.run(sim, 0x1000);
    const auto viaPart = seen ? ts2::sim::routingSlotTarget(sim, code.globals[0x37], seen) : std::nullopt;
    const auto viaLead = ts2::sim::routingSlotTarget(marker, code.globals[0x37], ts2::sim::ModelBone{0.5f, 0.25f, 1.5f});
    CHECK(viaPart && viaLead && std::abs(viaPart->x - viaLead->x) <= 2 && std::abs(viaPart->y - viaLead->y) <= 2 && seen->yaw == 1.5f);
    sim.data[0x13] = 0;

    find.operands[10] = 0x38;
    code.add(kObjectGroup, 0x1001, bhav({find}));
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::Error);
}

// Find Location For modes 5 and 6 place by the reference's routing slot whose number is in the param
// (5) or the local (6) that bytes 10-11 name, as a carried baby is put down by its drop spot. A param or
// local past the frame's is an error, and so is a number past the reference's slots.
void findLocationForTakesTheSlotNumberFromAParamOrLocal() {
    MemoryCodeSource code;
    ts2::slot::Entry slot;
    slot.type = ts2::slot::kRoutingSlot;
    slot.facing = -1;
    code.addObjectType(kObjectGroup, typed(kFridgeGuid), {slot, slot});
    const auto& slots = code.objectType(kFridgeGuid)->routingSlots;
    Instruction find = call(0x0010, T, F);
    find.operands = {6, 0, 0x08, 0, owner::kMyData, 0x0B, 0, owner::kStackObjectId, 0, 0, 1};
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kLocal, 1, op::kAssign, owner::kLiteral, 1, 1, E), find}, 0, 2));
    find.operands[0] = 5;
    code.add(kObjectGroup, 0x1001, bhav({find}, 2));
    find.operands[10] = 4;
    code.add(kObjectGroup, 0x1002, bhav({find}, 2));
    SmallWorld w;
    ts2::sim::SimObject& sim = *w.world.find(5);
    sim.thread.stackObjectId = 2;
    const ts2::slot::Entry* seen = nullptr;
    w.world.setPlacementSpots([&](const ts2::sim::SimObject&, const ts2::sim::SimObject&, const ts2::slot::Entry& s,
                                  std::span<const std::optional<ts2::sim::ModelBone>>) {
        seen = &s;
        return std::vector<std::pair<float, float>>{{3.5f, 4.5f}};
    });
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True && seen == &slots[1]);
    CHECK(sim.x == 3 * 65536 + 32768 && sim.y == 4 * 65536 + 32768);
    const std::array<std::int16_t, 2> first{0, 0}, second{0, 1}, past{0, 2};
    CHECK(vm.run(sim, 0x1001, first).outcome == Outcome::True && seen == &slots[0]);
    CHECK(vm.run(sim, 0x1001, second).outcome == Outcome::True && seen == &slots[1]);
    CHECK(vm.run(sim, 0x1001, past).outcome == Outcome::Error);
    CHECK(vm.run(sim, 0x1002, first).outcome == Outcome::Error);
}

// Make New Character adds a neighbor with the age, gender and species from my locals and puts its id
// in the stack object's array 0. Ids: the first free one from the counter (one past the highest),
// or, past the neighbor count, the first gap from 1.
void makeNewCharacterAddsANeighbor() {
    constexpr std::uint32_t kTemplatePerson = 0x7FD96B54, kTemplateDog = 0x0F83C946;
    MemoryCodeSource code;
    code.addObjectType(kObjectGroup, typed(kTemplatePerson, 0x10));
    code.addObjectType(kObjectGroup, typed(kTemplateDog, 0x10));
    Instruction make = primitiveCall(0x0013, T, F);
    make.operands = {0, 0, 1, 0, 0, 0, 0, 0, 0, 0x00, 0, 0, 0, 0, 2, 0};  // skin, age, gender, ..., species
    SmallWorld w;
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {3, 8};
    w.world.setNeighborhood(hood);
    w.controller->arrays.assign(1, {7, 7});
    // Human first names and surnames, by gender. The pets' tables are missing.
    w.world.setNameTables({{0x96, {"Ted"}}, {0x97, {"Ann", "Brittany"}}, {0x98, {"Smith"}}, {0xCC, {"Wendland"}}});
    Vm vm(code, {}, &w.world);
    auto make1 = [&](std::int16_t age, std::int16_t gender, std::int16_t species) {
        code.add(kObjectGroup, 0x1001, bhav({
            expr(owner::kLocal, 0, op::kAssign, owner::kLiteral, static_cast<std::uint16_t>(age), 1, E),
            expr(owner::kLocal, 1, op::kAssign, owner::kLiteral, static_cast<std::uint16_t>(gender), 2, E),
            expr(owner::kLocal, 2, op::kAssign, owner::kLiteral, static_cast<std::uint16_t>(species), 3, E),
            expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 1, 4, E),
            make,
        }, 0, 3));
        return vm.run(*w.controller, 0x1001);
    };
    CHECK(make1(0x40, 1, 0).outcome == Outcome::True);
    CHECK(w.controller->arrays[0] == std::vector<std::int16_t>{9});
    const auto* made = w.world.neighbor(9);
    CHECK(made != nullptr && made->personData[0x3A] == 19 && made->personData[0xAF] == 1 && made->personData[0x41] == 1);
    // She is named from the female tables. A dog gets no names without the pets' tables.
    CHECK(w.world.neighborFirstName(9) && !w.world.neighborFirstName(9)->empty() && w.world.neighborSurname(9) == "Wendland");
    CHECK(make1(4, 2, 1).outcome == Outcome::True && w.controller->arrays[0] == std::vector<std::int16_t>{10});
    made = w.world.neighbor(10);
    CHECK(made != nullptr && made->personData[0x3A] == 16 && made->personData[0x41] == 0 && made->personData[0xBA] == 1);
    CHECK(!w.world.neighborFirstName(10) && !w.world.neighborSurname(10));
    // Each new Sim is a person made from its species' template, under a GUID of its own.
    const auto* person = code.objectType(w.world.neighbor(9)->guid);
    const auto* dog = code.objectType(made->guid);
    CHECK(person != nullptr && dog != nullptr && person != dog);
    CHECK(person->definition.field(0x09) == 2 && person->definition.guid() == w.world.neighbor(9)->guid);
    CHECK(person->definition.field(0x1D) == person->definition.field(0x0E) && person->definition.field(0x1E) == person->definition.field(0x0F));
    CHECK((static_cast<std::uint32_t>(person->definition.field(0x47)) << 16 | person->definition.field(0x46)) == kTemplatePerson);
    CHECK((static_cast<std::uint32_t>(dog->definition.field(0x47)) << 16 | dog->definition.field(0x46)) == kTemplateDog);
    CHECK(make1(8, 1, 3).outcome == Outcome::False);  // no cat template
    CHECK(hood->neighborIds.size() == 2);  // the saved neighborhood itself is untouched
    CHECK(make1(8, 1, 4).outcome == Outcome::Error);  // species out of range
    w.controller->arrays.clear();
    CHECK(make1(8, 1, 0).outcome == Outcome::Error);  // no object arrays

    // "Add to family" (Generic Sims Call 4): neighbor 9 (the stack object) joins family 32767, which
    // is made. A second time it is already in it.
    Instruction add = primitiveCall(0x0001, T, F);
    add.operands[0] = 0x04;
    code.add(kObjectGroup, 0x1002, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 9, 1, E),
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 32767, 2, E),
        add,
    }));
    CHECK(vm.run(*w.controller, 0x1002).outcome == Outcome::True);
    CHECK(w.world.neighborPersonValue(9, 0x3D) == 32767 && w.world.hasFamily(32767));
    CHECK(vm.run(*w.controller, 0x1002).outcome == Outcome::False);
}

const std::string kLight = "00000001-0000-0000-0000-000000000000", kMedium = "00000002-0000-0000-0000-000000000000",
                  kMediumDark = "00000003-0000-0000-0000-000000000000", kDark = "00000004-0000-0000-0000-000000000000",
                  kAlien = "6baf064a-85ad-4e37-8d81-a987e9f8da46";
const std::string kBlack = "00000001-0000-0000-0000-000000000000", kBlond = "00000003-0000-0000-0000-000000000000";
const std::string kBrownEyes = "32dee745-b6ce-419f-9e86-ae93802d2682", kBlueEyes = "e43f3360-3a08-4755-8b83-a0d37a6c424b";

// The base game's skin, hair and eye values that the tests use.
ts2::content::GeneticsCatalog testGeneticsCatalog() {
    ts2::content::GeneticsCatalog c;
    c.skins = {{kLight, 0.1f}, {kMedium, 0.3f}, {kMediumDark, 0.6f}, {kDark, 0.9f}, {kAlien, 0}};
    c.hairs = {{kBlack, 1}, {kBlond, 2}};
    c.eyes = {{kBrownEyes, 1}, {kBlueEyes, 2}};
    return c;
}

// A baby's genes as the game draws them: hair and eyes take the more dominant colour first, skin
// shows a catalog skin between the parents' and keeps their alleles in gene 2, an alien skin (0) is
// shown as passed and leads when its parent's two skins match, and the face regions follow their
// odds (one in two from the carrier, two in three from the other parent). Parents carry gene 2 as
// well as gene 6, as every Sim's DNA does.
void babyGenesFollowTheGameRules() {
    using ts2::sim::Gene;
    using ts2::sim::Genome;
    const auto catalog = testGeneticsCatalog();
    const std::string allRegions = "nose,brow,mouth,eyes,jaw,ears,";
    Genome a{{1, {kBlack, kBlack}}, {2, {kLight, kLight}}, {3, {kBlueEyes, kBlueEyes}}, {6, {kLight, kLight}},
             {5, {allRegions, ""}}};
    Genome b{{1, {kBlond, kBlond}}, {2, {kDark, kDark}}, {3, {kBrownEyes, kBrownEyes}}, {6, {kDark, kDark}},
             {5, {"", ""}}};
    std::set<std::string> shownSkins;
    int noseFromA = 0;
    constexpr int kRuns = 3000;
    for (std::uint32_t seed = 0; seed < kRuns; ++seed) {
        ts2::sim::SimRandom random(seed);
        const Genome child = ts2::sim::childGenome(a, b, catalog, random);
        CHECK(child.at(1) == (Gene{kBlack, kBlond}) && child.at(3) == (Gene{kBrownEyes, kBlueEyes}));
        CHECK(child.at(2) == (Gene{kLight, kDark}) && child.at(6).dominant == child.at(6).recessive);
        shownSkins.insert(child.at(6).dominant);
        noseFromA += child.at(5).dominant.find("nose") != std::string::npos ? 1 : 0;
    }
    CHECK(shownSkins == (std::set<std::string>{kLight, kMedium, kMediumDark, kDark}));
    CHECK(noseFromA > kRuns * 45 / 100 && noseFromA < kRuns * 55 / 100);

    // A region only the other parent passes dominant stays two times in three, one both pass always.
    std::swap(a[5], b[5]);
    int noseFromB = 0;
    for (std::uint32_t seed = 0; seed < kRuns; ++seed) {
        ts2::sim::SimRandom random(seed);
        noseFromB += ts2::sim::childGenome(a, b, catalog, random).at(5).dominant.find("nose") != std::string::npos ? 1 : 0;
    }
    CHECK(noseFromB > kRuns * 62 / 100 && noseFromB < kRuns * 72 / 100);
    a[5] = b[5];
    ts2::sim::SimRandom once(7);
    CHECK(ts2::sim::childGenome(a, b, catalog, once).at(5).dominant == allRegions);

    // A pure alien's skin leads, whichever parent carried the baby.
    for (std::uint32_t seed = 0; seed < 50; ++seed) {
        const Genome alien{{2, {kAlien, kAlien}}, {6, {kAlien, kAlien}}}, human{{2, {kMedium, kMedium}}, {6, {kMedium, kMedium}}};
        ts2::sim::SimRandom r1(seed), r2(seed);
        CHECK(ts2::sim::childGenome(alien, human, catalog, r1).at(6) == (Gene{kAlien, kMedium}));
        CHECK(ts2::sim::childGenome(human, alien, catalog, r2).at(2) == (Gene{kAlien, kMedium}));
    }
}

// Each gene either parent has costs an allele draw from each parent first (the carrier's first),
// then genes 7-15 one more for their order. The six face regions draw last.
void babyGenesDrawInTheGamesOrder() {
    const ts2::sim::Genome a{{7, {"p", "q"}}}, b{{7, {"r", "s"}}};
    ts2::sim::SimRandom expected(1234);
    auto coin = [&] { return ((static_cast<std::uint64_t>(expected()) * 100) >> 32) < 50; };
    const std::string fromA = coin() ? "p" : "q";
    const std::string fromB = coin() ? "r" : "s";
    const ts2::sim::Gene gene = coin() ? ts2::sim::Gene{fromA, fromB} : ts2::sim::Gene{fromB, fromA};
    for (int region = 0; region < 6; ++region) {
        expected();
    }
    ts2::sim::SimRandom random(1234);
    const auto child = ts2::sim::childGenome(a, b, testGeneticsCatalog(), random);
    CHECK(child.at(7) == gene && random.state() == expected.state());
    const auto read = ts2::sim::genomeFromProperties({{"1", "x"}, {"268435457", "y"}, {"5", "mouth,"}, {"16", "q"}, {"name", "z"}});
    CHECK(read.size() == 2 && read.at(1) == (ts2::sim::Gene{"x", "y"}) && read.at(5).dominant == "mouth,");
}

// Make New Character with two neighbor-id parents: temp 0 babies of their species go into array 0,
// then it waits a tick and is true. With only the mother marked (person data 0xA5) the unmarked
// father stands for both in the genes and the baby takes his opposite gender and its name by it.
// It takes the first parent's surname, the played one or else its character's. A count outside 1-10
// and parents of two species are errors.
void makeNewCharacterMakesBabiesOfTwoParents() {
    MemoryCodeSource code;
    code.addObjectType(kObjectGroup, typed(0x7FD96B54, 0x10));
    Instruction make = primitiveCall(0x0013, T, F);
    // Parent 2 is literal 8 (bytes 3-5), parent 1 literal 3 (bytes 6-8), flags 0x07.
    make.operands = {0, 0, 0, owner::kLiteral, 8, 0, owner::kLiteral, 3, 0, 0x07, 0, 0, 0, 0, 0, 1};
    SmallWorld w;
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {3, 8};
    for (const std::int16_t id : {std::int16_t{3}, std::int16_t{8}}) {
        hood->neighbors[id].guid = 0x0D000000u + static_cast<std::uint32_t>(id);
        hood->neighbors[id].personData.assign(0xE7, 0);
    }
    // The mother's character is Ana Patel, the father's Jo Jones.
    for (const auto& [id, surname] : {std::pair{3, "Patel"}, std::pair{8, "Jones"}}) {
        auto parent = typed(0x0D000000u + static_cast<std::uint32_t>(id), 2);
        parent.fields[ts2::objd::field::kCatalogStringsId] = 0x7D0;
        const std::uint32_t group = 0x7F0000A0u + static_cast<std::uint32_t>(id);
        code.addObjectType(group, parent);
        code.addCatalogStrings(group, 0x7D0, {"Parent", "", surname});
    }
    hood->neighbors[3].personData[0x41] = 1;  // the mother
    hood->neighbors[3].personData[0xA5] = 1;
    hood->neighbors[3].genes[1] = {kBlond, kBlond};
    hood->neighbors[8].genes[1] = {kBlack, kBlack};
    w.world.setNeighborhood(hood);
    w.world.setBabyFirstNames("Baby Boy", "Baby Girl");
    const ts2::content::GeneticsCatalog catalog = testGeneticsCatalog();
    w.world.setGeneticsCatalog([&] { return &catalog; });
    w.controller->arrays.assign(1, {7});
    Vm vm(code, {}, &w.world);
    auto makeBabies = [&](std::int16_t count) {
        code.add(kObjectGroup, 0x1001, bhav({
            expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, static_cast<std::uint16_t>(count), 1, E),
            expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 1, 2, E),
            make,
        }));
        return vm.run(*w.controller, 0x1001);
    };
    CHECK(makeBabies(2).outcome == Outcome::Sleeping && w.controller->arrays[0].size() == 2);
    CHECK(vm.resume(*w.controller).outcome == Outcome::True);
    for (const auto id : w.controller->arrays[0]) {
        const auto* baby = w.world.neighbor(static_cast<std::int16_t>(id));
        CHECK(baby != nullptr && baby->personData[0x3A] == 1 && baby->personData[0xBA] == 0 && baby->personData[0x41] == 1);
        CHECK(baby != nullptr && baby->personData[0x52] == 500 && code.objectType(baby->guid) != nullptr);
        CHECK(w.world.neighborFirstName(static_cast<std::int16_t>(id)) == "Baby Girl");
        CHECK(w.world.neighborSurname(static_cast<std::int16_t>(id)) == "Patel");
        CHECK(baby != nullptr && baby->genes.at(1) == (ts2::sim::Gene{kBlack, kBlack}));
    }
    w.world.setNeighborSurname(3, "Curious");
    CHECK(makeBabies(1).outcome == Outcome::Sleeping && vm.resume(*w.controller).outcome == Outcome::True);
    CHECK(w.world.neighborSurname(static_cast<std::int16_t>(w.controller->arrays[0].back())) == "Curious");
    CHECK(makeBabies(0).outcome == Outcome::Error);
    CHECK(makeBabies(11).outcome == Outcome::Error);
    w.world.neighborPersonData(8)->at(0xBA) = 3;  // a cat
    CHECK(makeBabies(1).outcome == Outcome::Error);
}

// "Add to family" with family 0 joins the household living on the lot (the first family the game
// lists whose home is this lot) and leaves temp 0 at 0. The Sim's GUID is added after the family's members. A lot no
// family lives on stops as unsupported.
void addToFamilyZeroJoinsTheLotsHousehold() {
    Instruction add = primitiveCall(0x0001, T, F);
    add.operands[0] = 0x04;
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 9, 1, E),
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 0, 2, E),
        add,
    }));
    SmallWorld w;
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {9};
    hood->neighbors[9].guid = 0x0D000009;
    hood->neighbors[9].personData.assign(0xE7, 0);
    hood->familyLots[1] = 4;
    hood->familyLots[6] = 10;
    hood->familyMembers[6] = {0x0D000001};
    // Of two families on the lot, the higher rank is listed first, then the higher id.
    hood->familyLots[9] = 10;
    hood->familyRanks[6] = 1;
    w.world.setNeighborhood(hood);
    w.world.setLotNumber(10);
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(w.world.neighborPersonValue(9, 0x3D) == 6 && w.controller->thread.temps[0] == 0);
    const auto members = w.world.familyMemberGuids(6);
    CHECK(members.size() == 2 && members[0] == 0x0D000001 && members[1] == 0x0D000009);
    w.world.setLotNumber(5);
    w.world.neighborPersonData(9)->at(0x3D) = 0;
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::Unsupported);
}

// Neighbor data field 6 ("house number") is the lot of the neighbor's family (person data 0x3D).
void neighborHouseNumberIsTheFamilysLot() {
    ts2::sim::World world;
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {8, 9};
    hood->neighbors[8].personData.assign(0x40, 0);
    hood->neighbors[8].personData[0x3D] = 3;
    hood->neighbors[9].personData.assign(0x40, 0);
    hood->familyLots[3] = 10;
    world.setNeighborhood(hood);
    CHECK(world.neighborData(8, 6) == std::int16_t{10} && world.neighborData(9, 6) == std::int16_t{0});
}

// "Neighbor in stack object" (owner 0x18): fields computed from the neighbor whose id is in the
// stack object. Field 1: whether the neighbor is in the family living on this lot. Fields 7, 8 and 10
// are bits 0, 1 and 3 of the flags of the neighbor's family, only while that family has a lot. Field 9 is
// how many neighbors with a family of their own are friends of one of its members.
void neighborFieldsComeFromTheNeighbor() {
    constexpr std::uint32_t kNeighborGuid = 0x0A000101;
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kParam, 0, 1, E),
        expr(owner::kTemp, 0, op::kAssign, ts2::sim::owner::kNeighborInStackObject, 0, 2, E),
        expr(owner::kTemp, 1, op::kAssign, ts2::sim::owner::kNeighborInStackObject, 2, 3, E),
        expr(owner::kTemp, 2, op::kAssign, ts2::sim::owner::kNeighborInStackObject, 4, 4, E),
        expr(owner::kTemp, 3, op::kAssign, ts2::sim::owner::kNeighborInStackObject, 11, T, E),
    }, 1));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kParam, 0, 1, E),
        expr(owner::kTemp, 0, op::kAssign, ts2::sim::owner::kNeighborInStackObject, 1, 2, E),
        expr(owner::kTemp, 1, op::kAssign, ts2::sim::owner::kNeighborInStackObject, 9, T, E),
    }, 1));
    code.add(kObjectGroup, 0x1002, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kParam, 0, 1, E),
        expr(owner::kTemp, 4, op::kAssign, ts2::sim::owner::kNeighborInStackObject, 7, 2, E),
        expr(owner::kTemp, 5, op::kAssign, ts2::sim::owner::kNeighborInStackObject, 8, 3, E),
        expr(owner::kTemp, 6, op::kAssign, ts2::sim::owner::kNeighborInStackObject, 10, T, E),
    }, 1));
    ts2::sim::World world;
    auto& me = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& neighborSim = world.create(kObjectGroup, typed(kNeighborGuid, ts2::sim::kPersonType));
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {14, 15};
    std::vector<std::int16_t> personData(231, 0);
    personData[0x3A] = 19;
    personData[0x3D] = 3;
    hood->neighbors[14] = {kNeighborGuid, personData};
    personData[0x3D] = 4;
    hood->neighbors[15] = {0x0A000102, personData};
    hood->familyLots = {{3, 5}, {4, 0}};
    hood->familyFlags = {{3, 0x9}, {4, 0xB}};
    hood->familyMembers = {{3, {kNeighborGuid}}, {4, {0x0A000102}}};
    // 14 and 15 are friends both ways (relationship value 1 bit 0x10).
    const std::int16_t a = 14, b = 15;
    const std::pair fromA{a, b}, fromB{b, a};
    hood->relationships[fromA] = std::vector<std::int32_t>(10, 0);
    hood->relationships[fromA][1] = 0x10;
    hood->relationships[fromB] = hood->relationships[fromA];
    world.setNeighborhood(hood);
    world.setLotNumber(5);
    Vm vm(code, {}, &world);

    const std::int16_t onLot[] = {14};
    CHECK(vm.run(me, 0x1000, onLot).outcome == Outcome::True);
    CHECK(me.thread.temps[0] == neighborSim.id && me.thread.temps[1] == 19 && me.thread.temps[2] == 0 &&
          me.thread.temps[3] == 0);
    const std::int16_t elsewhere[] = {15};
    CHECK(vm.run(me, 0x1000, elsewhere).outcome == Outcome::True);
    CHECK(me.thread.temps[0] == 0 && me.thread.temps[1] == 19);
    const std::int16_t unknown[] = {99};
    CHECK(vm.run(me, 0x1000, unknown).outcome == Outcome::True);
    CHECK(me.thread.temps[0] == 0 && me.thread.temps[1] == 0);
    CHECK(vm.run(me, 0x1002, onLot).outcome == Outcome::True);
    CHECK(me.thread.temps[4] == 1 && me.thread.temps[5] == 0 && me.thread.temps[6] == 1);
    CHECK(vm.run(me, 0x1002, elsewhere).outcome == Outcome::True);  // no lot: no flags
    CHECK(me.thread.temps[4] == 0 && me.thread.temps[5] == 0 && me.thread.temps[6] == 0);
    CHECK(vm.run(me, 0x1001, onLot).outcome == Outcome::True && me.thread.temps[0] == 1 && me.thread.temps[1] == 1);
    CHECK(vm.run(me, 0x1001, elsewhere).outcome == Outcome::True && me.thread.temps[0] == 0 && me.thread.temps[1] == 1);
    hood->relationships[fromA][3] = 1;  // value 3 set: not friends
    CHECK(vm.run(me, 0x1001, onLot).outcome == Outcome::True && me.thread.temps[1] == 0);
}

Instruction arrayOp(std::uint8_t op, std::uint8_t whose, std::uint16_t array, std::uint8_t valueOwner,
                    std::uint16_t valueData, std::uint8_t iteratorOwner, std::uint16_t iteratorData,
                    std::uint16_t t, std::uint16_t f) {
    Instruction in = primitiveCall(0x0076, t, f);
    in.operands[1] = op;
    in.operands[2] = whose;
    in.operands[3] = static_cast<std::uint8_t>(array);
    in.operands[4] = static_cast<std::uint8_t>(array >> 8);
    in.operands[5] = valueOwner;
    in.operands[6] = static_cast<std::uint8_t>(valueData);
    in.operands[7] = static_cast<std::uint8_t>(valueData >> 8);
    in.operands[8] = iteratorOwner;
    in.operands[9] = static_cast<std::uint8_t>(iteratorData);
    in.operands[10] = static_cast<std::uint8_t>(iteratorData >> 8);
    return in;
}

// Array Operation on "my" object arrays. A new object has as many arrays as OBJD field 59.
void arrayOperationsWorkOnObjectArrays() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        arrayOp(5, 0, 1, owner::kLiteral, 30, 0, 0, 1, E),                // push back 30
        arrayOp(4, 0, 1, owner::kLiteral, 10, 0, 0, 2, E),                // push front 10
        arrayOp(5, 0, 1, owner::kLiteral, 20, 0, 0, 3, E),                // push back 20: 10 30 20
        arrayOp(13, 0, 1, 0, 0, 0, 0, 4, E),                              // sort ascending: 10 20 30
        arrayOp(1, 0, 1, owner::kTemp, 0, 0, 0, 5, E),                    // size -> temp 0
        expr(owner::kTemp, 1, op::kAssign, owner::kLiteral, 0, 6, E),
        arrayOp(10, 0, 1, owner::kLiteral, 30, owner::kTemp, 1, 7, E),    // find 30 -> temp 1
        arrayOp(12, 0, 1, 0, 0, 0, 0, 8, E),                              // sort descending: 30 20 10
        arrayOp(9, 0, 1, 0, 0, owner::kLiteral, 0, 9, E),                 // remove at 0: 20 10
        arrayOp(10, 0, 1, owner::kLiteral, 99, owner::kTemp, 1, E, T),    // find 99: false
    }));
    code.add(kObjectGroup, 0x1001, bhav({arrayOp(0, 0, 2, 0, 0, 0, 0, T, E)}));          // array 2: none
    code.add(kObjectGroup, 0x1002, bhav({arrayOp(7, 0, 0, 0, 0, 0, 0, T, E)}));          // remove from empty
    code.add(kObjectGroup, 0x1003, bhav({arrayOp(9, 0, 0, 0, 0, owner::kLiteral, 3, T, E)}));
    auto definition = typed(kFridgeGuid);
    definition.fields.resize(60, 0);
    definition.fields[59] = 2;
    ts2::sim::World world;
    auto& a = world.create(kObjectGroup, definition);
    CHECK(a.arrays.size() == 2);
    Vm vm(code, {}, &world);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::True);
    CHECK(a.thread.temps[0] == 3 && a.thread.temps[1] == 2);
    CHECK(a.arrays[1] == (std::vector<std::int16_t>{20, 10}));
    const RunResult bad = vm.run(a, 0x1001);
    CHECK(bad.outcome == Outcome::Error && bad.reason == "bad object array");
    CHECK(vm.run(a, 0x1002).reason == "array iterator does not exist");
    CHECK(vm.run(a, 0x1003).reason == "array iterator does not exist");

    // Owners 0x29 / 0x2B: array 1's iterator, and its element at the iterator (array 1 is 20 10).
    code.add(kObjectGroup, 0x1004, bhav({
        expr(ts2::sim::owner::kMyArrayIterator, 1, op::kAssign, owner::kLiteral, 1, 1, E),
        expr(owner::kTemp, 2, op::kAssign, ts2::sim::owner::kMyArrayAtIterator, 1, 2, E),
        expr(ts2::sim::owner::kMyArrayAtIterator, 1, op::kAssign, owner::kLiteral, 77, T, E),
    }));
    code.add(kObjectGroup, 0x1005, bhav({
        expr(ts2::sim::owner::kMyArrayIterator, 1, op::kAssign, owner::kLiteral, 5, 1, E),
        expr(owner::kTemp, 2, op::kAssign, ts2::sim::owner::kMyArrayAtIterator, 1, T, E),
    }));
    CHECK(vm.run(a, 0x1004).outcome == Outcome::True);
    CHECK(a.thread.temps[2] == 10 && a.arrays[1][1] == 77 && a.arrayIterators[1] == 1);
    CHECK(vm.run(a, 0x1005).reason == "array iterator does not exist");

    // Owner 0x2D: array 1's element at temp 0, read and written. Past its end it is an error.
    code.add(kObjectGroup, 0x1007, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 0, 1, E),
        expr(owner::kTemp, 2, op::kAssign, ts2::sim::owner::kMyArrayAtTemp, 1, 2, E),
        expr(ts2::sim::owner::kMyArrayAtTemp, 1, op::kAssign, owner::kLiteral, 55, 3, E),
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 2, 4, E),
        expr(owner::kTemp, 3, op::kAssign, ts2::sim::owner::kMyArrayAtTemp, 1, T, E),
    }));
    const RunResult pastEnd = vm.run(a, 0x1007);
    CHECK(a.thread.temps[2] == 20 && a.arrays[1][0] == 55 && pastEnd.reason == "array element does not exist");

    // A full array (its size would no longer fit 16 bits) takes no more elements.
    code.add(kObjectGroup, 0x1006, bhav({arrayOp(5, 0, 1, owner::kLiteral, 1, 0, 0, T, E)}));
    a.arrays[1].assign(INT16_MAX - 1, 0);
    CHECK(vm.run(a, 0x1006).outcome == Outcome::True);
    const RunResult full = vm.run(a, 0x1006);
    CHECK(full.outcome == Outcome::Error && full.reason == "array is full");
    CHECK(a.arrays[1].size() == INT16_MAX);
}

// "Verify Neighbor ID in Stack Object": the stack object holds a neighbor id.
void verifyNeighborChecksTheNeighborhood() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kParam, 0, 1, E),
        setToNext(0x1D, owner::kStackObjectId, 0, 0, T, F),
    }, 1));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    const std::int16_t known[] = {7};
    const std::int16_t unknown[] = {8};
    const RunResult noHood = vm.run(*w.controller, 0x1000, known);
    CHECK(noHood.outcome == Outcome::Unsupported && noHood.reason == "Set to Next needs the neighborhood");
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {1, 7, 300};
    w.world.setNeighborhood(hood);
    CHECK(vm.run(*w.controller, 0x1000, known).outcome == Outcome::True);
    CHECK(vm.run(*w.controller, 0x1000, unknown).outcome == Outcome::False);
    CHECK(w.controller->thread.stackObjectId == 8);
}

void testObjectTypeComparesGuids() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 2, 1, E),
        testObjectType(kFridgeGuid, owner::kStackObjectId, 0, 2, F),
        expr(owner::kLocal, 0, op::kAssign, owner::kLiteral, 3, 3, E),
        testObjectType(kFridgeGuid, owner::kLocal, 0, F, T),
    }, 0, 1));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
}

// Flag 0x01 compares the GUID the object's type was cloned from (OBJD fields 0x46-0x47), when
// that type exists, instead of its own.
void testObjectTypeComparesTheOriginalGuid() {
    constexpr std::uint32_t kRecolorGuid = 0x0D0D0D0D;
    auto original = [](std::uint16_t t, std::uint16_t f) {
        Instruction in = testObjectType(kFridgeGuid, owner::kStackObjectId, 0, t, f);
        in.operands[7] = 0x01;
        return in;
    };
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({original(T, F)}));
    code.add(kObjectGroup, 0x1001, bhav({testObjectType(kFridgeGuid, owner::kStackObjectId, 0, T, F)}));
    code.addObjectType(kObjectGroup, typed(kFridgeGuid));
    ts2::sim::World world;
    auto& me = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto recolor = typed(kRecolorGuid);
    recolor.fields[0x46] = static_cast<std::uint16_t>(kFridgeGuid);
    recolor.fields[0x47] = static_cast<std::uint16_t>(kFridgeGuid >> 16);
    auto& fridge = world.create(kObjectGroup, recolor);
    Vm vm(code, {}, &world);
    me.thread.stackObjectId = fridge.id;
    CHECK(vm.run(me, kObjectGroup, 0x1000).outcome == Outcome::True);
    me.thread.stackObjectId = fridge.id;
    CHECK(vm.run(me, kObjectGroup, 0x1001).outcome == Outcome::False);
}

// A Sim made during play has as many container slots as the person it was made from, when newer
// packs give that person more than the Sim's own copy has. A Sim's Cleanup empties slot 19.
void madeSimsHaveTheirOriginalPersonsSlots() {
    constexpr std::uint32_t kTemplate = 0x0E0E0E0E, kSim = 0x0F0F0F0F;
    MemoryCodeSource code;
    code.addObjectType(kObjectGroup, typed(kTemplate, ts2::sim::kPersonType), {}, std::vector<ts2::slot::Entry>(23));
    auto sim = typed(kSim, ts2::sim::kPersonType);
    sim.fields[0x46] = static_cast<std::uint16_t>(kTemplate);
    sim.fields[0x47] = static_cast<std::uint16_t>(kTemplate >> 16);
    code.addObjectType(kObjectGroup, sim, {}, std::vector<ts2::slot::Entry>(19));
    code.addObjectType(kObjectGroup, typed(kFridgeGuid), {}, std::vector<ts2::slot::Entry>(2));
    ts2::sim::World world;
    Vm vm(code, {}, &world);
    const ts2::sim::SimObject* made = vm.create(kSim);
    CHECK(made != nullptr && made->slots.size() == 23 && made->data[ts2::sim::kSlotCountField] == 23);
    const ts2::sim::SimObject* fridge = vm.create(kFridgeGuid);
    CHECK(fridge != nullptr && fridge->slots.size() == 2);
}

// A Sim made during play is a visitor (person data 0x20 = 1) unless it belongs to the lot's
// household. Any other Person Type it already has is kept.
void madeSimsAreVisitorsUnlessHousehold() {
    constexpr std::uint32_t kGuest = 0x0E0E0E0E, kResident = 0x0F0F0F0F, kFamily = 7;
    MemoryCodeSource code;
    code.addObjectType(kObjectGroup, typed(kGuest, ts2::sim::kPersonType));
    code.addObjectType(kObjectGroup, typed(kResident, ts2::sim::kPersonType));
    code.addObjectType(kObjectGroup, typed(kFridgeGuid));
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->familyLots[kFamily] = 12;
    hood->familyMembers[kFamily] = {kResident};
    ts2::sim::World world;
    world.setNeighborhood(hood);
    world.setLotNumber(12);
    Vm vm(code, {}, &world);
    const ts2::sim::SimObject* guest = vm.create(kGuest);
    const ts2::sim::SimObject* resident = vm.create(kResident);
    CHECK(guest != nullptr && guest->personData[0x20] == 1);
    CHECK(resident != nullptr && resident->personData[0x20] == 0);
}

// A part of a multi-tile object matches its master's GUID too, as the sunken hot tub's Get In Tub
// needs for the seat it gets in at. Any other GUID is still false.
void testObjectTypeMatchesAPartsMaster() {
    constexpr std::uint32_t kSeatGuid = 0x0C0C0C0C, kTubGuid = 0x0B0B0B0B;
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({testObjectType(kTubGuid, owner::kStackObjectId, 0, T, F)}));
    code.add(kObjectGroup, 0x1001, bhav({testObjectType(kFridgeGuid, owner::kStackObjectId, 0, T, F)}));
    auto seatType = typed(kSeatGuid);
    seatType.fields[ts2::objd::field::kMultiTileMasterId] = 7;
    code.addObjectType(kObjectGroup, seatType);
    code.addObjectType(kObjectGroup, typed(kTubGuid));
    code.setMultiTileMaster(kSeatGuid, kTubGuid);
    ts2::sim::World world;
    auto& me = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& seat = world.create(kObjectGroup, seatType);
    Vm vm(code, {}, &world);
    me.thread.stackObjectId = seat.id;
    CHECK(vm.run(me, kObjectGroup, 0x1000).outcome == Outcome::True);
    me.thread.stackObjectId = seat.id;
    CHECK(vm.run(me, kObjectGroup, 0x1001).outcome == Outcome::False);
}

void testObjectTypeOnMissingObjectIsFalse() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({testObjectType(kFridgeGuid, owner::kLiteral, 99, T, F)}));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::False);
}

void testObjectTypeCanAskWhetherAGuidExists() {
    MemoryCodeSource code;
    code.addObjectType(kObjectGroup, typed(kChairGuid));
    Instruction known = testObjectType(kChairGuid, owner::kLiteral, 0, T, F);
    known.operands[7] = 0x04;
    Instruction unknown = testObjectType(0xDEADBEEF, owner::kLiteral, 0, F, T);
    unknown.operands[7] = 0x04;
    code.add(kObjectGroup, 0x1000, bhav({known}));
    code.add(kObjectGroup, 0x1001, bhav({unknown}));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(static_cast<std::uint16_t>(w.controller->thread.temps[0]) == (kChairGuid & 0xFFFF));
    CHECK(static_cast<std::uint16_t>(w.controller->thread.temps[1]) == (kChairGuid >> 16));
    CHECK(vm.run(*w.controller, 0x1001).outcome == Outcome::True);
}

void createObjectCanTakeItsGuidFromTemps() {
    MemoryCodeSource code;
    code.addObjectType(kLampGroup, typed(kLampGuid));
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, static_cast<std::uint16_t>(kLampGuid), 1, E),
        expr(owner::kTemp, 1, op::kAssign, owner::kLiteral, static_cast<std::uint16_t>(kLampGuid >> 16), 2, E),
        createObject(0, 0x06, 0x80, T, F),
    }));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(world.size() == 2);
}

// A multi-tile master's GUID makes its parts, lead first: each is reset and runs its own Init,
// names the lead in data 0x13, and the lead's Main runs before the primitive returns. The lead's
// data 0x29 is the master's price, and the lead becomes the stack object.
void createObjectMakesAMultiTileObjectsParts() {
    constexpr std::uint32_t kMasterGuid = 0x0A000001, kLeadGuid = 0x0A000002, kPartGuid = 0x0A000003;
    constexpr std::uint32_t kLeadGroup = 0x7F000201, kPartGroup = 0x7F000202;
    auto tile = [](std::uint32_t guid, std::uint16_t subIndex, bool lead) {
        auto d = typed(guid);
        d.fields[ts2::objd::field::kMultiTileMasterId] = 7;
        d.fields[ts2::objd::field::kMultiTileSubIndex] = subIndex;
        d.fields[0x3F] = lead ? 1 : 0;
        return d;
    };
    MemoryCodeSource code;
    auto master = tile(kMasterGuid, 0xFFFF, false);
    master.fields[ts2::objd::field::kPrice] = 250;
    code.addObjectType(kLampGroup, master);
    code.addObjectType(kLeadGroup, tile(kLeadGuid, 0x0005, true));
    code.addObjectType(kPartGroup, tile(kPartGuid, 0x0000, false));
    code.setMultiTileParts(kMasterGuid, {kLeadGuid, kPartGuid});
    for (const std::uint32_t group : {kLeadGroup, kPartGroup}) {
        code.add(group, 0x1001, bhav({expr(owner::kMyData, 0x20, op::kAssign, owner::kLiteral, 1, T, E)}));
        std::vector<ts2::objf::Function> functions(2);
        functions[0].action = 0x1001;
        functions[1].action = group == kLeadGroup ? 0x1000 : 0;
        code.addFunctions(group, functions);
    }
    code.add(kLeadGroup, 0x1000, bhav({expr(owner::kMyData, 0x21, op::kAssign, owner::kLiteral, 5, T, E)}));
    code.add(kObjectGroup, 0x1000, bhav({createObject(kMasterGuid, 0x06, 0x00, T, F)}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(world.size() == 3);
    const auto* lead = world.find(sim.thread.stackObjectId);
    const auto* part = world.find(static_cast<std::int16_t>(sim.id + 2));
    CHECK(lead != nullptr && lead->id == sim.id + 1 && lead->definition.guid() == kLeadGuid);
    CHECK(part != nullptr && part->definition.guid() == kPartGuid);
    if (lead != nullptr && part != nullptr) {
        CHECK(lead->data[0x13] == lead->id && part->data[0x13] == lead->id);
        CHECK(lead->data[0x20] == 1 && part->data[0x20] == 1);
        CHECK(lead->data[0x21] == 5 && part->data[0x21] == 0);
        CHECK(lead->data[0x29] == 250);
    }
}

// A multi-tile object moves as one: when a part moves, the others go to their offsets from it,
// turned with its facing, as the game keeps them (a window "0,1" facing 2 sits a tile to the -x of
// its "0,0" lead). A part off the lot goes out of the world.
void movedPartsFollow() {
    auto tile = [](std::uint32_t guid, std::uint16_t subIndex, std::uint16_t storey) {
        auto d = typed(guid);
        d.fields[ts2::objd::field::kMultiTileMasterId] = 3;
        d.fields[ts2::objd::field::kMultiTileSubIndex] = subIndex;
        d.fields[0x38] = storey;
        return d;
    };
    ts2::sim::World world;
    auto& lead = world.create(kObjectGroup, tile(0x0B000001, 0x0000, 0));
    auto& side = world.create(kObjectGroup, tile(0x0B000002, 0x0001, 0));
    auto& corner = world.create(kObjectGroup, tile(0x0B000003, 0x0101, 1));
    for (auto* part : {&lead, &side, &corner}) {
        part->data[ts2::sim::kLeadTileField] = lead.id;
        part->x = part->y = ts2::sim::kOffLot;
    }
    const auto at = [](float tiles) { return static_cast<std::int32_t>(tiles * 65536.0f); };
    struct Case {
        std::int16_t facing;
        float sideX, sideY, cornerX, cornerY;
    };
    for (const Case c : {Case{0, 10.5f, 11.5f, 11.5f, 11.5f}, Case{2, 9.5f, 10.5f, 9.5f, 11.5f}, Case{4, 10.5f, 9.5f, 9.5f, 9.5f},
                         Case{6, 11.5f, 10.5f, 11.5f, 9.5f}}) {
        ts2::sim::snapTo(world, lead, {at(10.5f), at(10.5f), 1, c.facing});
        CHECK(side.x == at(c.sideX) && side.y == at(c.sideY) && side.level == 1 && side.data[1] == c.facing);
        CHECK(corner.x == at(c.cornerX) && corner.y == at(c.cornerY) && corner.level == 2);
    }
    ts2::sim::snapTo(world, side, {at(0.5f), at(0.5f), 1, std::int16_t{0}});
    CHECK(lead.x == ts2::sim::kOffLot && lead.y == ts2::sim::kOffLot && corner.x == at(1.5f) && corner.y == at(0.5f));
}

void snappedCarsTakeTheirPassengers() {
    ts2::sim::World world;
    auto& car = world.create(kObjectGroup, typed(0x0B000001), 2);
    auto& passenger = world.create(kObjectGroup, typed(0x0B000002));
    CHECK(car.slots.size() == 2 && car.data[ts2::sim::kSlotCountField] == 2);
    car.slots[1] = passenger.id;
    passenger.data[2] = car.id;
    passenger.data[3] = 1;
    const auto at = [](float tiles) { return static_cast<std::int32_t>(tiles * 65536.0f); };
    ts2::sim::snapTo(world, car, {at(20.5f), at(34.5f), 1, std::int16_t{6}});
    CHECK(passenger.x == car.x && passenger.y == car.y && passenger.level == 1);
}

void removingAPartRemovesTheWholeObject() {
    auto tile = [](std::uint32_t guid, std::uint16_t subIndex) {
        auto d = typed(guid);
        d.fields[ts2::objd::field::kMultiTileMasterId] = 3;
        d.fields[ts2::objd::field::kMultiTileSubIndex] = subIndex;
        return d;
    };
    ts2::sim::World world;
    auto& lead = world.create(kObjectGroup, tile(0x0B000001, 0x0000));
    auto& side = world.create(kObjectGroup, tile(0x0B000002, 0x0001));
    auto& otherLead = world.create(kObjectGroup, tile(0x0B000001, 0x0000));
    for (auto* part : {&lead, &side}) {
        part->data[ts2::sim::kLeadTileField] = lead.id;
    }
    otherLead.data[ts2::sim::kLeadTileField] = otherLead.id;
    world.remove(side);
    CHECK(lead.removed && side.removed && !otherLead.removed);
}

void snapTestsEveryPartAtItsSpot() {
    auto tile = [](std::uint32_t guid, std::uint16_t subIndex) {
        auto d = typed(guid);
        d.fields[ts2::objd::field::kMultiTileMasterId] = 3;
        d.fields[ts2::objd::field::kMultiTileSubIndex] = subIndex;
        return d;
    };
    ts2::sim::World world;
    auto& lead = world.create(kObjectGroup, tile(0x0B000001, 0x0000));
    auto& side = world.create(kObjectGroup, tile(0x0B000002, 0x0001));
    for (auto* part : {&lead, &side}) {
        part->data[ts2::sim::kLeadTileField] = lead.id;
    }
    const auto at = [](float tiles) { return static_cast<std::int32_t>(tiles * 65536.0f); };
    ts2::sim::snapTo(world, lead, {at(10.5f), at(10.5f), 1, std::int16_t{0}});
    // Something stands at 20.5, 21.5. With facing 2 the side part lands there.
    std::vector<std::int16_t> tested;
    world.setObjectInTheWay([&](const ts2::sim::SimObject& placed, float x, float y, std::int32_t, std::int16_t facing) {
        tested.push_back(placed.id);
        return placed.id == side.id && x == 20.5f && y == 21.5f && facing == 2;
    });
    CHECK(!ts2::sim::placementCode(world, lead, {at(20.5f), at(21.5f), 1, std::int16_t{0}}));
    CHECK(tested == std::vector<std::int16_t>({lead.id, side.id}));
    CHECK(ts2::sim::placementCode(world, lead, {at(21.5f), at(21.5f), 1, std::int16_t{2}}) == 0x0C);
    // A part landing off the lot: placement code 2.
    CHECK(ts2::sim::placementCode(world, lead, {at(0.5f), at(0.5f), 1, std::int16_t{2}}) == 2);
    CHECK(lead.x == at(10.5f) && side.x == at(10.5f) && side.y == at(11.5f));
}

// The placement test keeps the lot's far edge on the lot. The room check's tile test does not.
void placementTestKeepsTheFarEdgeOnTheLot() {
    ts2::sim::World world;
    world.setRooms(ts2::sim::RoomMap(squareRoom(false)));
    auto& me = world.create(kObjectGroup, definition("Me", 0));
    const auto at = [](float tiles) { return static_cast<std::int32_t>(tiles * 65536.0f); };
    CHECK(!ts2::sim::placementCode(world, me, {at(6.0f), at(6.0f), 1, std::int16_t{0}}));
    CHECK(ts2::sim::placementCode(world, me, {at(6.5f), at(1.5f), 1, std::int16_t{0}}) == 2);
    CHECK(ts2::sim::snapRefusal(world, me, {at(6.0f), at(1.5f), 1, std::int16_t{0}}) == ts2::sim::SnapRefusal::OffLot);
}

void reachPutMovesTheObjectAtTheClipsEvent() {
    using namespace ts2::sim;
    const auto reach = [](std::uint8_t mode) {
        Instruction in = primitiveCall(0x0074, T, F);
        in.operands[3] = owner::kStackObjectId;
        in.operands[10] = mode;
        return in;
    };
    MemoryCodeSource code;
    // The grab names the reset pose by its flag, the put the cup's list entry 2, which the cup plays
    // as it changes hands.
    Instruction grab = reach(0), put = reach(2);
    grab.operands[9] = 0x10;
    put.operands[13] = 2;
    code.add(kObjectGroup, 0x1000, bhav({grab}));
    code.add(kObjectGroup, 0x1001, bhav({put}));
    code.addStrings(kLampGroup, 0x86, {"o2a-cup-carry", "", "o-generic-resetToZero-pose"});
    std::vector<std::string> clips(16);
    clips[0] = "a2o-floor-get";
    clips[8] = "a2o-floor-put";
    code.addStrings(0x7FD46CD0, 0x92, clips);
    code.addStrings(kLampGroup, 0x81, {"a2o-cup-grasp,a2o-cup-carry"});
    code.addObjectType(kLampGroup, typed(kLampGuid));
    // At the hand-off the cup's "CT - Grab" (after a grab) or "CT - Put" runs with the cup as stack
    // object: the grab shows the cup and keeps param 1, the put marks data 0x31.
    Behavior handGrab = bhav({expr(owner::kStackObjectData, 0x22, op::kAssign, owner::kLiteral, 0, 1, F),
                              expr(owner::kStackObjectData, 0x30, op::kAssign, owner::kParam, 1, T, F)}, 5);
    handGrab.name = "CT - Grab";
    Behavior handPut = bhav({expr(owner::kStackObjectData, 0x31, op::kAssign, owner::kLiteral, 7, T, F)}, 5);
    handPut.name = "CT - Put";
    code.add(kLampGroup, 0x1002, std::move(handGrab));
    code.add(kLampGroup, 0x1003, std::move(handPut));
    World world;
    world.setAnimationLengths([](const std::string& name) -> std::optional<float> {
        if (name == "a2o-cup-grasp" || name == "a2o-cup-carry" || name == "o-generic-resettozero-pose" || name == "o-generic-resetToZero-pose") {
            return 0.0f;
        }
        return name == "a2o-floor-get" || name == "a2o-floor-put" ? std::optional<float>(1000.0f) : std::nullopt;
    });
    world.setAnimationEventTime([](const std::string&, int id) -> std::optional<float> {
        return id == 0 ? std::optional<float>(400.0f) : std::nullopt;
    });
    auto& sim = world.create(kObjectGroup, typed(0x51515151, kPersonType), 2);
    auto& cup = world.create(kLampGroup, typed(kLampGuid));
    sim.x = 5 << 16;
    sim.y = 5 << 16;
    sim.data[1] = 2;  // facing the world's +y, which is the position's +x
    cup.x = 5 << 16;
    cup.y = (5 << 16) + 0x8000;
    sim.thread.stackObjectId = cup.id;
    cup.data[0x22] = 1;  // hidden until grabbed
    Vm vm(code, {}, &world);

    // The cup comes into my hand at the clip's event 0. The primitive is true when the clip ends.
    const double start = world.clockMs();
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::Sleeping && sim.animation && sim.animation->name == "a2o-floor-get");
    // The reach clip plays as a medium Sim animation, fading in and out over 233.33 ms each.
    CHECK(sim.animation->layer == 2 && sim.animation->fades && sim.animation->fadeInMs == 233.33f && sim.animation->fadeOutMs == 233.33f);
    // The grasp fades in towards the event and hands over to the carry pose as the clip ends. The
    // carry stays until something stops it.
    CHECK(sim.overlays.size() == 2);
    const PlayingOverlay& grasp = sim.overlays[0];
    CHECK(grasp.name == "a2o-cup-grasp" && grasp.priority == 1 && grasp.plays == 1 && std::abs(grasp.startMs - (start + 240)) < 0.01);
    CHECK(std::abs(*grasp.blendInMs - 110) < 0.01f && std::abs(grasp.lengthMs - (1000 - 240 - 233.33f)) < 0.01f);
    const PlayingOverlay& carry = sim.overlays[1];
    CHECK(carry.name == "a2o-cup-carry" && carry.priority == 2 && carry.plays == -1 && !carry.blendsOut);
    CHECK(carry.category == ts2::content::animation_category::kCarry && std::abs(carry.startMs - (start + 1000 - 466.66)) < 0.01);
    world.advanceClock(450);
    CHECK(vm.resume(sim).outcome == Outcome::Sleeping && sim.slots[0] == cup.id && cup.data[2] == sim.id && cup.data[3] == 0);
    CHECK(cup.animation && cup.animation->name == "o-generic-resettozero-pose");
    CHECK(cup.data[0x22] == 0 && cup.data[0x30] == cup.id && cup.data[0x31] == 0);
    world.advanceClock(600);
    CHECK(vm.resume(sim).outcome == Outcome::True);
    // Grabbing it again is true at once.
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    // Put in the middle of the tile 0.7 tiles in front of me, facing back at me (direction 6), turned
    // one step on by facing rule 4.
    cup.data[0x17] = 4;
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::Sleeping && sim.animation->name == "a2o-floor-put");
    // The hand aims at that spot, facing the way I face rather than the way the spot does.
    const auto& aim = sim.animation->ikObject;
    CHECK(aim && aim->x == 5.5f && aim->y == 5.5f && aim->heading == compassHeading(sim.data[1]) && aim->heading != compassHeading(7));
    // A put plays the grasp again and fades the carry out.
    const auto named = [&](const char* name, bool stopped) {
        return std::ranges::any_of(sim.overlays, [&](const PlayingOverlay& o) { return o.name == name && o.stopped == stopped; });
    };
    CHECK(named("a2o-cup-carry", true) && !named("a2o-cup-carry", false));
    // From its full weight down to none over 233.33 ms, as Animate Stop fades it.
    const auto stoppedCarry = std::ranges::find_if(sim.overlays, [](const PlayingOverlay& o) { return o.name == "a2o-cup-carry"; });
    CHECK(stoppedCarry->stoppedAt && *stoppedCarry->stoppedAt == 1.0f && stoppedCarry->blendOutMs && std::fabs(*stoppedCarry->blendOutMs - 233.33f) < 0.01f);
    CHECK(sim.overlays.back().name == "a2o-cup-grasp" && sim.overlays.back().startMs == world.clockMs());
    cup.animation->name = "o2a-cup-carry";
    world.advanceClock(1000);
    CHECK(vm.resume(sim).outcome == Outcome::True && sim.slots[0] == 0 && cup.data[2] == 0);
    // The put's clip replaces the carry clip the cup played in hand.
    CHECK(cup.animation && cup.animation->name == "o-generic-resetToZero-pose" && cup.ending && cup.ending->name == "o2a-cup-carry");
    CHECK(cup.data[0x31] == 7);
    CHECK(cup.x == (5 << 16) + 0x8000 && cup.y == (5 << 16) + 0x8000 && cup.data[1] == 7);
    // With nothing in hand there is nothing to put.
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::False);
    // When the spot fails the placement test, the cup stays in hand and the put is false.
    world.setObjectFits([](const SimObject&, float, float, std::int32_t, std::int16_t) { return false; });
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::Sleeping);
    world.advanceClock(450);
    CHECK(vm.resume(sim).outcome == Outcome::Sleeping);
    world.advanceClock(600);
    CHECK(vm.resume(sim).outcome == Outcome::True && sim.slots[0] == cup.id);
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::Sleeping);
    world.advanceClock(450);
    CHECK(vm.resume(sim).outcome == Outcome::False && sim.slots[0] == cup.id && cup.data[2] == sim.id);
}

// A glass on a tray on a bar is reached for at the height the bar's bone slot holds the tray, so a
// standing Sim takes the counter reach and not the glass's own seated one.
void reachPutMeasuresTheHeightThroughContainers() {
    using namespace ts2::sim;
    constexpr std::uint32_t kTrayGroup = 0x7F000088, kTrayGuid = 0x2B3C4D5E, kBarGroup = 0x7F000099, kBarGuid = 0x3C4D5E6F;
    Instruction grab = primitiveCall(0x0074, T, F);
    grab.operands[3] = owner::kStackObjectId;
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({grab}));
    std::vector<std::string> clips(16);
    clips[0] = "a2o-floor-get";
    clips[3] = "a2o-counter-get";
    code.addStrings(0x7FD46CD0, 0x92, clips);
    code.addStrings(kLampGroup, 0x92, {"a2o-barstool-seated-get", "a2o-barstool-seated-put"});
    code.addObjectType(kLampGroup, typed(kLampGuid));
    ts2::slot::Entry onTray;
    onTray.z = 0.02f;
    code.addObjectType(kTrayGroup, typed(kTrayGuid), {}, {onTray});
    ts2::slot::Entry counter;
    counter.objectFlags = 0x20000;  // placed by a bone of the model
    counter.bone = 0;
    counter.z = 4.05f;              // as the shipped bar's, which the bone overrides
    code.addObjectType(kBarGroup, typed(kBarGuid), {}, {counter});
    code.addStrings(kBarGroup, 0x90, {"slot_counter"});
    World world;
    world.setAnimationLengths([](const std::string&) -> std::optional<float> { return 1000.0f; });
    world.setAnimationEventTime([](const std::string& name, int id) -> std::optional<float> {
        return id == 0 && name != "a2o-barstool-seated-get" ? std::optional<float>(400.0f) : std::nullopt;
    });
    auto& sim = world.create(kObjectGroup, typed(0x51515151, kPersonType), 2);
    auto& glass = world.create(kLampGroup, typed(kLampGuid));
    auto& tray = world.create(kTrayGroup, typed(kTrayGuid), 1);
    auto& bar = world.create(kBarGroup, typed(kBarGuid), 1);
    const std::int16_t barId = bar.id;
    world.setModelBones([barId](std::int16_t id, const std::string& name) -> std::optional<ModelBone> {
        return id == barId && name == "slot_counter" ? std::optional<ModelBone>(ModelBone{0.3f, 0, 0, 0.95f}) : std::nullopt;
    });
    sim.x = 5 << 16;
    sim.y = 5 << 16;
    for (SimObject* o : {&bar, &tray, &glass}) {
        o->x = 5 << 16;
        o->y = (5 << 16) + 0x4000;
        o->data[1] = 2;
    }
    bar.slots[0] = tray.id;
    tray.data[2] = bar.id;
    tray.data[3] = 0;
    tray.slots[0] = glass.id;
    glass.data[2] = tray.id;
    glass.data[3] = 0;
    sim.thread.stackObjectId = glass.id;
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::Sleeping && sim.animation && sim.animation->name == "a2o-counter-get");
}

void snapIntoPutsTheObjectInTheSlot() {
    using namespace ts2::sim;
    const auto snapInto = [](std::uint8_t slot, std::uint8_t flags) {
        Instruction in = primitiveCall(0x0071, T, F);
        in.operands[0] = owner::kStackObjectId;
        in.operands[3] = owner::kMyData;
        in.operands[4] = 0x0B;  // my id
        in.operands[6] = slot;
        in.operands[11] = flags;
        return in;
    };
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({snapInto(18, 0)}));
    code.add(kObjectGroup, 0x1001, bhav({snapInto(18, 0x02)}));
    code.add(kObjectGroup, 0x1002, bhav({snapInto(30, 0)}));
    code.add(kObjectGroup, 0x1003, bhav({snapInto(19, 0x04)}));
    World world;
    world.setAnimationLengths([](const std::string& name) -> std::optional<float> {
        return name == "o-generic-resettozero-pose" ? std::optional<float>(0.0f) : std::nullopt;
    });
    auto& sim = world.create(kObjectGroup, typed(0x51515151, kPersonType), 20);
    auto& cup = world.create(kLampGroup, typed(kLampGuid));
    auto& other = world.create(kLampGroup, typed(kLampGuid));
    sim.x = 4 << 16;
    sim.y = 6 << 16;
    sim.level = 2;
    sim.data[1] = 3;
    sim.data[0x0B] = sim.id;
    sim.data[0x1C] = 10;
    Vm vm(code, {}, &world);

    sim.thread.stackObjectId = cup.id;
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(sim.slots[18] == cup.id && cup.data[2] == sim.id && cup.data[3] == 18);
    CHECK(cup.x == sim.x && cup.y == sim.y && cup.level == 2 && cup.data[1] == 3 && sim.data[0x34] == 0);
    // The slot is taken: another object can't go there, testing or not.
    sim.thread.stackObjectId = other.id;
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::False && sim.data[0x34] == 0x0C + 2);
    CHECK(vm.run(sim, 0x1002).outcome == Outcome::False && sim.data[0x34] == 0x0E + 2);
    CHECK(other.data[2] == 0);
    // With bit 2 the placed object drops its clip for the reset pose at once, with no fade.
    other.animation = PlayingAnimation{};
    other.animation->name = "o2a-table-foodServe-servedPlate-animation";
    other.ending = other.animation;
    CHECK(vm.run(sim, 0x1003).outcome == Outcome::True && sim.slots[19] == other.id);
    CHECK(other.animation && other.animation->name == "o-generic-resettozero-pose" && !other.animation->fades && !other.ending);
}

// Placements 1 "on top of me" and 2 "in my hand" both put the new object in my slot 0, at my place.
// Off the lot it is false. A slot 0 that is missing or full fails the fit test: false, and the object
// made for the test is gone again.
void createObjectGoesInMyHand() {
    MemoryCodeSource code;
    code.addObjectType(kLampGroup, typed(kLampGuid));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    sim.slots.assign(2, 0);
    sim.x = 4 << 16 | 0x8000;
    sim.y = 6 << 16 | 0x8000;
    sim.level = 1;
    code.add(kObjectGroup, 0x1000, bhav({createObject(kLampGuid, 0x02, 0, T, F)}));
    code.add(kObjectGroup, 0x1001, bhav({createObject(kLampGuid, 0x01, 0, T, F)}));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    const ts2::sim::SimObject* made = world.find(sim.thread.stackObjectId);
    CHECK(made != nullptr && sim.slots[0] == made->id && made->data[2] == sim.id && made->data[3] == 0 && made->x == sim.x &&
          made->y == sim.y && !made->tile);
    const std::int16_t held = made->id;
    const std::size_t count = world.idsIncludingRemoved([](const ts2::sim::SimObject& o) { return !o.removed; }).size();
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::False && sim.slots[0] == held && sim.thread.stackObjectId == held);
    CHECK(world.idsIncludingRemoved([](const ts2::sim::SimObject& o) { return !o.removed; }).size() == count);
    sim.slots[0] = 0;
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::True && sim.slots[0] == sim.thread.stackObjectId);
    sim.slots[0] = 0;
    sim.x = ts2::sim::kOffLot;
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::False && sim.slots[0] == 0);
    sim.x = 4 << 16 | 0x8000;
    sim.slots.clear();
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::False);
}

// Placement 5 "underneath me" puts the new object at my exact place and level, in no slot.
void createObjectGoesUnderneathMe() {
    MemoryCodeSource code;
    code.addObjectType(kLampGroup, typed(kLampGuid));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    sim.x = 4 << 16 | 0x1234;
    sim.y = 6 << 16 | 0x8000;
    sim.level = 2;
    code.add(kObjectGroup, 0x1000, bhav({createObject(kLampGuid, 0x05, 0, T, F)}));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    const ts2::sim::SimObject* made = world.find(sim.thread.stackObjectId);
    CHECK(made != nullptr && made->x == sim.x && made->y == sim.y && made->level == 2 && made->data[2] == 0);
    CHECK(made->tile && made->tile->x == 4 && made->tile->y == 6 && made->tile->level == 2);
}

// Placements 4 and 0xA put the new object in slot byte 9 of the stack object or of the object in
// temp 0, at its place. No such object is false, and so is a missing or full slot (the fit test).
void createObjectGoesInAnotherObjectsSlot() {
    MemoryCodeSource code;
    code.addObjectType(kLampGroup, typed(kLampGuid));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& meter = world.create(kObjectGroup, typed(0x52525252));
    meter.slots.assign(3, 0);
    meter.x = 4 << 16 | 0x8000;
    meter.y = 6 << 16 | 0x8000;
    meter.level = 1;
    Instruction intoStackObject = createObject(kLampGuid, 0x04, 0, T, F);
    intoStackObject.operands[9] = 2;
    Instruction intoTemp0Object = createObject(kLampGuid, 0x0A, 0, T, F);
    intoTemp0Object.operands[9] = 1;
    code.add(kObjectGroup, 0x1000, bhav({intoStackObject}));
    code.add(kObjectGroup, 0x1001, bhav({intoTemp0Object}));
    Vm vm(code, {}, &world);
    sim.thread.stackObjectId = meter.id;
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    const ts2::sim::SimObject* made = world.find(sim.thread.stackObjectId);
    CHECK(made != nullptr && meter.slots[2] == made->id && made->data[2] == meter.id && made->data[3] == 2 &&
          made->x == meter.x && made->y == meter.y);
    sim.thread.stackObjectId = meter.id;
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::False && meter.slots[2] == made->id);

    sim.thread.temps[0] = meter.id;
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::True && meter.slots[1] == sim.thread.stackObjectId);
    sim.thread.temps[0] = 999;
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::False);
    sim.thread.stackObjectId = 999;
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::False);
}

// A removed object leaves its container's slot when removed objects are freed, as the game deletes
// it: the slot is empty again and the object's data 2 and 3 no longer name it.
void removedObjectsLeaveTheirSlot() {
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& dart = world.create(kLampGroup, typed(kLampGuid));
    sim.slots.assign(1, dart.id);
    dart.data[2] = sim.id;
    dart.data[3] = 0;
    world.remove(dart);
    CHECK(sim.slots[0] == dart.id);  // until the end of the tick
    world.purge();
    CHECK(sim.slots[0] == 0);
}

// An object's use count (data 0x3E) counts the interaction frames run on it: one more as one goes on
// a Sim's stack, one less as it comes off. Other frames don't count.
void useCountFollowsInteractionFrames() {
    ts2::sim::World world;
    auto& chat = world.create(kLampGroup, typed(kLampGuid));
    ts2::sim::Frame interaction;
    interaction.runsInteraction = true;
    interaction.runningOnId = chat.id;
    ts2::sim::Frame other;
    other.runningOnId = chat.id;
    world.countUse(interaction);
    world.countUse(interaction);
    CHECK(chat.data[0x3E] == 2);
    world.releaseUse(other);
    CHECK(chat.data[0x3E] == 2);
    world.releaseUses({interaction, other});
    CHECK(chat.data[0x3E] == 1);
}

// When an object is deleted, a Sim drops its queued interactions on it or showing it as their icon
// and stops naming it in object data 0x5A and 0x5F. Its own data 0x19 and 0x3E go to 0. A Sim
// running one of its interactions, or with it as person data 0x50 or 0x9D, must be reset instead.
void simsForgetADeletedObject() {
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    const std::int16_t gone = 77, other = 78;
    ts2::sim::QueuedInteraction onIt, showingIt, elsewhere;
    onIt.object = gone;
    showingIt.object = other;
    showingIt.partner = gone;
    elsewhere.object = other;
    sim.queue = {onIt, showingIt, elsewhere};
    sim.data[0x5A] = gone;
    sim.data[0x5F] = other;
    sim.data[0x19] = 3;
    sim.data[0x3E] = 2;
    CHECK(!ts2::sim::forgetObject(sim, gone));
    CHECK(sim.queue.size() == 1 && sim.queue[0].object == other && sim.queue[0].partner == 0);
    CHECK(sim.data[0x5A] == 0 && sim.data[0x5F] == other && sim.data[0x19] == 0 && sim.data[0x3E] == 0);
    sim.personData[0x9D] = gone;
    CHECK(ts2::sim::forgetObject(sim, gone));
    sim.personData[0x9D] = 0;
    ts2::sim::Frame running;
    running.runsInteraction = true;
    running.runningOnId = gone;
    sim.thread.frames.push_back(running);
    CHECK(ts2::sim::forgetObject(sim, gone) && sim.queue.size() == 1);
}

void createObjectCanGoBelowTheObjectInALocal() {
    MemoryCodeSource code;
    code.addObjectType(kLampGroup, typed(kLampGuid));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& tv = world.create(kLampGroup, typed(kLampGuid));
    tv.x = 7 << 16 | 0x8000;
    tv.y = 3 << 16;
    tv.level = 2;
    auto below = createObject(kLampGuid, 0x08, 0, T, F);
    below.operands[6] = 1;
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kLocal, 1, op::kAssign, owner::kLiteral, static_cast<std::uint16_t>(tv.id), 1, E), below}, 0, 2));
    code.add(kObjectGroup, 0x1001, bhav({below}, 0, 2));  // the local holds no object
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    const ts2::sim::SimObject* made = world.find(sim.thread.stackObjectId);
    CHECK(made != nullptr && made->id != tv.id && made->x == tv.x && made->y == tv.y && made->level == 2);
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::False);

    // Placement 7 takes the object from param 0. A tree without params is an error.
    auto byParam = createObject(kLampGuid, 0x07, 0, T, F);
    code.add(kObjectGroup, 0x1002, bhav({byParam}, 1));
    code.add(kObjectGroup, 0x1003, bhav({byParam}));
    const std::int16_t args[] = {tv.id};
    CHECK(vm.run(sim, kObjectGroup, 0x1002, args).outcome == Outcome::True);
    made = world.find(sim.thread.stackObjectId);
    CHECK(made != nullptr && made->id != tv.id && made->x == tv.x && made->y == tv.y);
    CHECK(vm.run(sim, 0x1003).outcome == Outcome::Error);

    // Placement 3: the centre of the tile in front of the stack object, on its level.
    tv.x = 5 << 16 | 0x8000;
    tv.y = 5 << 16 | 0x8000;
    tv.data[1] = 2;
    code.add(kObjectGroup, 0x1004, bhav({createObject(kLampGuid, 0x03, 0, T, F)}));
    sim.thread = {};
    sim.thread.stackObjectId = tv.id;
    CHECK(vm.run(sim, 0x1004).outcome == Outcome::True);
    made = world.find(sim.thread.stackObjectId);
    CHECK(made != nullptr && made->id != tv.id && made->x == (6 << 16 | 0x8000) && made->y == (5 << 16 | 0x8000) && made->level == 2);
    sim.thread = {};
    sim.thread.stackObjectId = 999;
    CHECK(vm.run(sim, 0x1004).outcome == Outcome::False);

    // Placement 9: the tile next to me, my direction turned by local 0, facing back at me. Flag 0x08
    // refuses a tile with something on it, 0x01 only one with the same type.
    sim.x = 2 << 16 | 0x8000;
    sim.y = 2 << 16 | 0x8000;
    sim.level = 2;
    sim.data[1] = 0;
    auto beside = [](std::uint8_t flags) {
        Instruction in = createObject(kLampGuid, 0x09, flags, T, F);
        in.operands[10] = 0;
        return in;
    };
    code.add(kObjectGroup, 0x1005, bhav({expr(owner::kLocal, 0, op::kAssign, owner::kLiteral, 2, 1, E), beside(0)}, 0, 1));
    code.add(kObjectGroup, 0x1006, bhav({expr(owner::kLocal, 0, op::kAssign, owner::kLiteral, 2, 1, E), beside(0x08)}, 0, 1));
    code.add(kObjectGroup, 0x1007, bhav({expr(owner::kLocal, 0, op::kAssign, owner::kLiteral, 2, 1, E), beside(0x01)}, 0, 1));
    sim.thread = {};
    CHECK(vm.run(sim, 0x1005).outcome == Outcome::True);
    made = world.find(sim.thread.stackObjectId);
    CHECK(made != nullptr && made->x == (3 << 16 | 0x8000) && made->y == (2 << 16 | 0x8000) && made->data[1] == 6);
    sim.thread = {};
    CHECK(vm.run(sim, 0x1006).outcome == Outcome::False);  // the lamp just made is there
    sim.thread = {};
    CHECK(vm.run(sim, 0x1007).outcome == Outcome::False);  // so is one of its type
    world.find(made->id)->definition = typed(kChairGuid);
    sim.thread = {};
    CHECK(vm.run(sim, 0x1007).outcome == Outcome::True && vm.run(sim, 0x1006).outcome == Outcome::False);
}

// A lamp whose Init marks data 0x41 and whose Main (two params) waits first, then copies param 0 to
// data 0x40, as the game's bar tumbler does.
void addStartingLamp(MemoryCodeSource& code, std::uint8_t mainArgs) {
    code.addObjectType(kLampGroup, typed(kLampGuid));
    code.addFunctions(kLampGroup, {{0, 0x1000}, {0, 0x1001}});
    code.add(kLampGroup, 0x1000, bhav({expr(owner::kMyData, 0x41, op::kAssign, owner::kLiteral, 7, T, E)}));
    code.add(kLampGroup, 0x1001, bhav({
        expr(owner::kParam, 1, op::kAssign, owner::kLiteral, 5, 1, E),
        sleepFor(1, 2),
        expr(owner::kMyData, 0x40, op::kAssign, owner::kParam, 0, T, E),
    }, mainArgs));
}

void createObjectStartsItsMainAndPassesTemp0() {
    MemoryCodeSource code;
    addStartingLamp(code, 2);
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 1019, 1, E),
        createObject(kLampGuid, 0x06, 0x10, T, F),
    }));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    const ts2::sim::SimObject* lamp = world.find(sim.thread.stackObjectId);
    CHECK(lamp != nullptr && lamp->id != sim.id);
    if (lamp == nullptr) {
        return;
    }
    CHECK(lamp->data[0x41] == 7);
    CHECK(lamp->thread.frames.size() == 1);
    CHECK(lamp->thread.frames.front().pc == 1);  // waiting in its Sleep
    CHECK(lamp->thread.frames.front().params[0] == 1019);
    CHECK(lamp->data[0x40] == 0);
}

// A new object's Init and Main count their own steps, so making objects with long ones doesn't stop
// the tree that makes them. The step limit here is 1000, and each creation runs 1200 steps.
void createdObjectsCountTheirOwnSteps() {
    MemoryCodeSource code;
    code.addObjectType(kLampGroup, typed(kLampGuid));
    code.addFunctions(kLampGroup, {{0, 0x1000}, {0, 0x1001}});
    code.add(kLampGroup, 0x1000, bhav({expr(owner::kTemp, 0, op::kIncrementAndLess, owner::kLiteral, 600, 0, T)}));
    code.add(kLampGroup, 0x1001, bhav({
        expr(owner::kTemp, 1, op::kIncrementAndLess, owner::kLiteral, 600, 0, 1),
        expr(owner::kParam, 1, op::kAssign, owner::kLiteral, 5, 2, E),
        sleepFor(1, 2),
    }, 2));
    code.add(kObjectGroup, 0x1000, bhav({
        createObject(kLampGuid, 0x06, 0, 1, F),
        createObject(kLampGuid, 0x06, 0, 2, F),
        expr(owner::kTemp, 0, op::kIncrementAndLess, owner::kLiteral, 900, 2, T),
    }));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {1000, 64}, &world);
    const RunResult r = vm.run(sim, 0x1000);
    CHECK(r.outcome == Outcome::True);
    const ts2::sim::SimObject* lamp = world.find(sim.thread.stackObjectId);
    CHECK(lamp != nullptr);
    if (lamp == nullptr) {
        return;
    }
    CHECK(!lamp->failedRun);
    CHECK(lamp->thread.frames.size() == 1);
    CHECK(!lamp->thread.frames.empty() && lamp->thread.frames.front().pc == 2);  // waiting in its Sleep
}

// An object made out of the world is in no room: its room (object data 0x1D) is -1 already while its
// Init and Main first run.
void createObjectOutOfWorldIsInNoRoom() {
    MemoryCodeSource code;
    code.addObjectType(kLampGroup, typed(kLampGuid));
    code.addFunctions(kLampGroup, {{0, 0x1000}});
    code.add(kLampGroup, 0x1000, bhav({expr(owner::kMyData, 0x40, op::kAssign, owner::kMyData, 0x1D, T, E)}));
    code.add(kObjectGroup, 0x1000, bhav({createObject(kLampGuid, 0x06, 0, T, F)}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    const ts2::sim::SimObject* lamp = world.find(sim.thread.stackObjectId);
    CHECK(lamp != nullptr && lamp->data[0x1D] == -1 && lamp->data[0x40] == -1);
}

void createObjectPassesMyIdAndStackObject() {
    MemoryCodeSource code;
    addStartingLamp(code, 2);
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 77, 1, E),
        createObject(kLampGuid, 0x06, 0x02, T, F),
    }));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    const ts2::sim::SimObject* lamp = world.find(sim.thread.stackObjectId);
    CHECK(lamp != nullptr && lamp->id != sim.id && !lamp->thread.frames.empty());
    if (lamp == nullptr || lamp->thread.frames.empty()) {
        return;
    }
    CHECK(lamp->thread.frames.front().params[0] == sim.id);
    CHECK(lamp->thread.frames.front().stackObjectId == 77);
}

void createObjectPassesNothingToMainWithoutParams() {
    MemoryCodeSource code;
    addStartingLamp(code, 0);
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 1019, 1, E),
        createObject(kLampGuid, 0x06, 0x10, T, F),
    }));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    const ts2::sim::SimObject* lamp = world.find(sim.thread.stackObjectId);
    CHECK(lamp != nullptr && !lamp->thread.frames.empty());
    if (lamp == nullptr || lamp->thread.frames.empty()) {
        return;
    }
    CHECK(lamp->thread.frames.front().params[0] == 0);
}

void createObjectWithUnknownGuidReturnsFalse() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({createObject(0xDEADBEEF, 0x06, 0x00, T, F)}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::False);
    CHECK(world.size() == 1);
}

// Flag 0x04 creates the Sim of the neighbor whose id is the stack object, whatever the operand GUID
// and flag 0x80 say. An unknown neighbor is false.
void createObjectCanMakeANeighborsSim() {
    constexpr std::uint32_t kNeighborSimGuid = 0x5A5A5A5A;
    MemoryCodeSource code;
    code.addObjectType(kLampGroup, typed(kLampGuid));
    code.addObjectType(kLampGroup, typed(kNeighborSimGuid, ts2::sim::kPersonType));
    code.add(kObjectGroup, 0x1000, bhav({createObject(kLampGuid, 0x06, 0x04 | 0x80, T, F)}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {41};
    hood->neighbors[41].guid = kNeighborSimGuid;
    world.setNeighborhood(hood);
    Vm vm(code, {}, &world);
    sim.thread.stackObjectId = 41;
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    const ts2::sim::SimObject* made = world.find(sim.thread.stackObjectId);
    CHECK(made != nullptr && made->definition.guid() == kNeighborSimGuid && made->personData[0x1F] == 41);
    sim.thread.stackObjectId = 42;
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::False);
}

void createObjectRefusesTemplates() {
    MemoryCodeSource code;
    code.addObjectType(kLampGroup, typed(kLampGuid, 0x10));
    code.add(kObjectGroup, 0x1000, bhav({createObject(kLampGuid, 0x06, 0x00, T, F)}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::Error);
    CHECK(world.size() == 1);
}

void createObjectRejectsTwoPassingValues() {
    MemoryCodeSource code;
    code.addObjectType(kLampGroup, typed(kLampGuid));
    code.add(kObjectGroup, 0x1000, bhav({createObject(kLampGuid, 0x06, 0x12, T, F)}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::Error);
}

Instruction relationship(std::uint8_t variable, std::uint8_t flags, std::uint8_t aOwner,
                         std::uint16_t aData, std::uint8_t bOwner, std::uint16_t bData,
                         std::uint8_t valueOwner, std::uint16_t valueData, std::uint16_t t,
                         std::uint16_t f) {
    Instruction in = primitiveCall(0x001A, t, f);
    in.operands[0] = variable;
    in.operands[1] = flags;
    in.operands[2] = aOwner;
    in.operands[3] = static_cast<std::uint8_t>(aData);
    in.operands[4] = static_cast<std::uint8_t>(aData >> 8);
    in.operands[5] = bOwner;
    in.operands[6] = static_cast<std::uint8_t>(bData);
    in.operands[7] = static_cast<std::uint8_t>(bData >> 8);
    in.operands[8] = valueOwner;
    in.operands[9] = static_cast<std::uint8_t>(valueData);
    in.operands[10] = static_cast<std::uint8_t>(valueData >> 8);
    return in;
}

void relationshipWritesAndReadsBack() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        relationship(2, 0x04, owner::kLiteral, 1, owner::kLiteral, 2, owner::kLiteral, 75, 1, E),
        relationship(2, 0x01, owner::kLiteral, 1, owner::kLiteral, 2, owner::kTemp, 0, 2, E),
        relationship(2, 0x01, owner::kLiteral, 2, owner::kLiteral, 1, owner::kTemp, 1, E, T),
    }));
    ts2::sim::World world;
    auto& a = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    world.create(kObjectGroup, typed(kFridgeGuid));
    Vm vm(code, {}, &world);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::True);
    CHECK(a.thread.temps[0] == 75);
    CHECK(world.relationship(1, 2).size() == 3);
    CHECK(world.relationship(2, 1).empty());
}

void relationshipReadCreatesTheVariableUnlessAsked() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 9, 1, E),
        relationship(4, 0x00, owner::kLiteral, 1, owner::kLiteral, 2, owner::kTemp, 0, T, E),
    }));
    ts2::sim::World world;
    auto& a = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    world.create(kObjectGroup, typed(kFridgeGuid));
    Vm vm(code, {}, &world);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::True);
    CHECK(a.thread.temps[0] == 0);
    CHECK(world.relationship(1, 2).size() == 5);
}

void relationshipWithMissingObjectIsAnError() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        relationship(0, 0x00, owner::kLiteral, 1, owner::kLiteral, 9, owner::kTemp, 0, T, F),
    }));
    ts2::sim::World world;
    auto& a = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::Error);

    // An object removed this tick is still there by id until the world is purged.
    auto& gone = world.create(kObjectGroup, typed(kFridgeGuid));
    code.add(kObjectGroup, 0x1001, bhav({
        relationship(0, 0x00, owner::kMyData, 0x0B, owner::kStackObjectId, 0, owner::kTemp, 0, T, F),
    }));
    world.remove(gone);
    a.thread.stackObjectId = gone.id;
    CHECK(vm.run(a, 0x1001).outcome == Outcome::True);
}

// With flag 0x08 only A must be an object: B (here a neighbor id, 9) just picks A's list.
void relationshipWithFlag8NeedsOnlyA() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        relationship(1, 0x0C, owner::kLiteral, 1, owner::kLiteral, 9, owner::kLiteral, 7, T, F),
    }));
    code.add(kObjectGroup, 0x1001, bhav({
        relationship(1, 0x0C, owner::kLiteral, 5, owner::kLiteral, 9, owner::kLiteral, 7, T, F),
    }));
    ts2::sim::World world;
    auto& a = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::True);
    CHECK(world.relationship(1, 9).size() == 2 && world.relationship(1, 9)[1] == 7);
    CHECK(vm.run(a, 0x1001).outcome == Outcome::Error);
}

// With the working copy of the Sim's age data, Change Outfit is told the source and flags 0x10 and
// 0x08, and checks the copy's lists. A source's outfit keeps its GUID in person data 0x77-0x78
// unless it is the Sim's own.
void changeOutfitDressesFromTheWorkingCopy() {
    auto changeOutfit = [](std::uint8_t flags, std::uint8_t category) {
        Instruction in;
        in.opcode = 0x0079;
        in.trueTarget = T;
        in.falseTarget = E;
        in.operands = {flags, 0, 0, 0, 0x67, 0xDF, 0x4E, 0xB4, category, owner::kStackObjectId, 0, 0, 0, 0, 0, 0};
        return in;
    };
    constexpr std::uint32_t kSimGuid = 0x51515151, kSimFile = 0x7F000010, kUniform = 0xB44EDF67;
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({changeOutfit(0x02 | 0x08 | 0x10, 0)}));
    code.add(kObjectGroup, 0x1001, bhav({changeOutfit(0, 8)}));
    code.addObjectType(kSimFile, typed(kSimGuid, ts2::sim::kPersonType));
    code.addAgeData(kSimFile, {{"listcnt", "0"}});
    ts2::sim::World world;
    auto& sim = world.create(kSimFile, typed(kSimGuid, ts2::sim::kPersonType));
    auto& dresser = world.create(kObjectGroup, typed(kChairGuid));
    dresser.thread.stackObjectId = sim.id;
    std::vector<ts2::sim::World::OutfitBuildRequest> asked;
    ts2::sim::World::BuiltOutfit copy{{0x00010008}, true};
    world.setOutfitBuild([&](const ts2::sim::SimObject&, const ts2::sim::World::OutfitBuildRequest& request) {
        asked.push_back(request);
        return std::optional(copy);
    });
    Vm vm(code, {}, &world);
    CHECK(vm.run(dresser, 0x1000).outcome == Outcome::True);
    CHECK(asked.size() == 1 && asked[0].sourceGuid == kUniform && asked[0].renew && asked[0].commit);
    CHECK(sim.personData[0x77] == static_cast<std::int16_t>(0xDF67) && sim.personData[0x78] == static_cast<std::int16_t>(0xB44E));
    CHECK(sim.outfitChange && sim.outfitChange->category == 0 && sim.outfitChange->sourceGuid == kUniform);
    // No formal body in the copy: the Sim stays dressed as it is.
    sim.outfitChange.reset();
    CHECK(vm.run(dresser, 0x1001).outcome == Outcome::True);
    CHECK(asked.size() == 2 && asked[1].sourceGuid == 0 && !asked[1].renew && !asked[1].commit);
    CHECK(!sim.outfitChange && sim.personData[0x08] == 0 && sim.personData[0x77] == static_cast<std::int16_t>(0xDF67));
}

// Change Outfit dresses the Sim in owner byte 9 / data 10-11 in byte 8's outfit (flag 0x04: a
// value), numbered as person data 0x08 (8 pajamas, 2 formal). It is worn, setting 0x08, only when
// the age data lists a top and bottom or a full body for it, a Sim aged on the lot checked in its new
// age's lists. Flags 0x20 and 0x10 clear the source GUID (0x77-0x78). A source's outfit (flags 0x01,
// 0x02, 0x40) isn't worn yet. Without age data only the 0x20 clear happens. A missing object or a
// non-Sim is an error.
void changeOutfitAsksTheSim() {
    auto changeOutfit = [](std::uint8_t flags, std::uint8_t category, std::uint8_t simOwner, std::uint16_t simData) {
        Instruction in;
        in.opcode = 0x0079;
        in.trueTarget = T;
        in.falseTarget = E;
        in.operands = {flags, owner::kTemp, 2, 0, 0x1B, 0xB4, 0x22, 0x6C, category, simOwner,
                       static_cast<std::uint8_t>(simData), static_cast<std::uint8_t>(simData >> 8), 0, 0, 0, 0};
        return in;
    };
    constexpr std::uint32_t kSimGuid = 0x51515151, kBareGuid = 0x52525252, kSimFile = 0x7F000010, kBareFile = 0x7F000011;
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({changeOutfit(0x10, 8, owner::kStackObjectId, 0)}));
    code.add(kObjectGroup, 0x1001, bhav({changeOutfit(0x02 | 0x04 | 0x20, 0, owner::kStackObjectId, 0)}));
    code.add(kObjectGroup, 0x1002, bhav({changeOutfit(0x40, 1, owner::kStackObjectId, 0)}));
    code.add(kObjectGroup, 0x1003, bhav({changeOutfit(0, 1, owner::kLiteral, 99)}));
    code.add(kObjectGroup, 0x1004, bhav({changeOutfit(0, 1, owner::kMyData, 0x0B)}));
    code.add(kObjectGroup, 0x1005, bhav({changeOutfit(0, 2, owner::kStackObjectId, 0)}));
    code.add(kObjectGroup, 0x1006, bhav({changeOutfit(0x20, 8, owner::kStackObjectId, 0)}));
    code.addObjectType(kSimFile, typed(kSimGuid, ts2::sim::kPersonType));
    code.addObjectType(kBareFile, typed(kBareGuid, ts2::sim::kPersonType));
    // Pajamas (0x100) have a full body, formal (0x08) only a top, and an empty bottom list.
    code.addAgeData(kSimFile, {{"listcnt", "3"}, {"lk0", "0x01000008"}, {"ls0", "1"},  // leakcheck: ok
                               {"lk1", "0x00080004"}, {"ls1", "1"}, {"lk2", "0x00080010"}, {"ls2", "0"}});
    ts2::sim::World world;
    auto& sim = world.create(kSimFile, typed(kSimGuid, ts2::sim::kPersonType));
    auto& bare = world.create(kBareFile, typed(kBareGuid, ts2::sim::kPersonType));
    auto& dresser = world.create(kObjectGroup, typed(kChairGuid));
    sim.personData[0x77] = 5;
    sim.personData[0x78] = 6;
    dresser.thread.stackObjectId = sim.id;
    Vm vm(code, {}, &world);
    CHECK(vm.run(dresser, 0x1000).outcome == Outcome::True);
    CHECK(sim.outfitChange && sim.outfitChange->category == 8 && !sim.outfitChange->sourceGuid);
    CHECK(sim.personData[0x08] == 8 && sim.personData[0x77] == 0 && sim.personData[0x78] == 0);
    sim.personData[0x77] = 5;
    CHECK(vm.run(dresser, 0x1005).outcome == Outcome::True);  // formal has no bottom
    CHECK(sim.personData[0x08] == 8 && sim.outfitChange->category == 8 && sim.personData[0x77] == 5);
    // Aged on the lot: its new age's lists are checked, which have a formal top and bottom.
    world.agedOutfitLists[kSimGuid] = {0x00080004, 0x00080010};
    CHECK(vm.run(dresser, 0x1005).outcome == Outcome::True && sim.personData[0x08] == 2 && sim.outfitChange->category == 2);
    CHECK(vm.run(dresser, 0x1000).outcome == Outcome::True && sim.personData[0x08] == 2);  // no pajamas any more
    world.agedOutfitLists.clear();
    sim.personData[0x08] = 8;
    sim.outfitChange->category = 8;
    sim.personData[0x77] = 5;
    dresser.thread.temps[2] = 4;
    CHECK(vm.run(dresser, 0x1001).outcome == Outcome::True);
    CHECK(sim.personData[0x77] == 0 && sim.personData[0x08] == 8 && sim.outfitChange->category == 8);
    dresser.thread.temps[0] = static_cast<std::int16_t>(0xB41B);
    dresser.thread.temps[1] = 0x6C22;
    CHECK(vm.run(dresser, 0x1002).outcome == Outcome::True && sim.outfitChange->category == 8);
    CHECK(vm.run(dresser, 0x1003).outcome == Outcome::Error);
    CHECK(vm.run(dresser, 0x1004).outcome == Outcome::Error);  // the dresser itself is no Sim
    bare.personData[0x77] = 5;
    dresser.thread.stackObjectId = bare.id;
    CHECK(vm.run(dresser, 0x1000).outcome == Outcome::True);
    CHECK(!bare.outfitChange && bare.personData[0x08] == 0 && bare.personData[0x77] == 5);
    CHECK(vm.run(dresser, 0x1006).outcome == Outcome::True && bare.personData[0x77] == 0 && !bare.outfitChange);
}

// Age makes me the stage in operand 0 at once, as the game's table gives it: person data 0x3A in
// years (young adults 19 with 0xAF set), the outfit source GUID cleared, what my slots hold put back
// into the same slots. Stages outside the table change nothing. Always true.
void ageSetsTheLifeStage() {
    auto age = [](std::uint8_t stage) {
        Instruction in;
        in.opcode = 0x0075;
        in.trueTarget = T;
        in.falseTarget = E;
        in.operands = {stage, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        return in;
    };
    MemoryCodeSource code;
    code.add(kSimGroup, 0x1000, bhav({age(0x0E)}));
    code.add(kSimGroup, 0x1001, bhav({age(0x03)}));
    code.add(kSimGroup, 0x1002, bhav({age(0x09)}));
    code.add(kSimGroup, 0x1003, bhav({age(0x04)}));
    code.add(kObjectGroup, 0x1003, bhav({age(0x04)}));
    ts2::sim::World world;
    auto& sim = world.create(kSimGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& cup = world.create(kObjectGroup, typed(kChairGuid));
    sim.slots.assign(2, 0);
    sim.slots[1] = cup.id;
    cup.data[2] = sim.id;
    cup.data[3] = 1;
    sim.personData[0x3A] = 16;
    sim.personData[0x77] = 5;
    sim.personData[0x78] = 6;
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(sim.personData[0x3A] == 19 && sim.personData[0xAF] == 1 && sim.newAge == 0x40u);
    CHECK(sim.personData[0x77] == 0 && sim.personData[0x78] == 0);
    CHECK(sim.slots[1] == cup.id && cup.data[2] == sim.id && cup.data[3] == 1);
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::True);
    CHECK(sim.personData[0x3A] == 16 && sim.personData[0xAF] == 0 && sim.newAge == 0x04u);
    sim.personData[0x77] = 5;
    CHECK(vm.run(sim, 0x1002).outcome == Outcome::True);
    CHECK(sim.personData[0x3A] == 16 && sim.personData[0x77] == 5);
    CHECK(vm.run(sim, 0x1003).outcome == Outcome::True && sim.personData[0x3A] == 51 && sim.newAge == 0x10u);
    CHECK(vm.run(cup, 0x1003).outcome == Outcome::Unsupported);
    // The new age's data is built as Age runs, and the Sim gets a new body only when it was.
    std::vector<std::uint32_t> asked;
    bool builds = true;
    world.setAgeDataBuild([&](const ts2::sim::SimObject&, std::uint32_t age) -> std::optional<std::set<std::uint32_t>> {
        asked.push_back(age);
        return builds ? std::optional<std::set<std::uint32_t>>({0x00080004}) : std::nullopt;
    });
    sim.newAge.reset();
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::True && sim.newAge == 0x04u);
    CHECK(asked == std::vector<std::uint32_t>{0x04} && world.agedOutfitLists[0x51515151] == std::set<std::uint32_t>{0x00080004});
    builds = false;
    sim.newAge.reset();
    CHECK(vm.run(sim, 0x1003).outcome == Outcome::True && !sim.newAge && sim.personData[0x3A] == 51);
}

// A Sim created during play is the neighbor of its GUID: it gets that neighbor's person data and
// its id in person data 0x1F before Init runs (Init reads them).
void createdSimsAreTheirNeighbor() {
    constexpr std::uint32_t kNpcGuid = 0x745B11D1;
    MemoryCodeSource code;
    code.addObjectType(kObjectGroup, typed(kNpcGuid, ts2::sim::kPersonType));
    code.add(kObjectGroup, 0x1001, bhav({expr(ts2::sim::owner::kMyPersonData, 0x20, op::kAssign, ts2::sim::owner::kMyPersonData, 0x1F, T, E)}));
    code.addFunctions(kObjectGroup, {{0, 0x1001}});
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {644};
    hood->neighbors[644] = {kNpcGuid, std::vector<std::int16_t>(ts2::sim::kNeighborPersonDataCount, 0)};
    hood->neighbors[644].personData[0x3A] = 19;
    ts2::sim::World world;
    world.setNeighborhood(hood);
    Vm vm(code, {}, &world);
    ts2::sim::SimObject* npc = vm.create(kNpcGuid);
    CHECK(npc != nullptr && npc->personData[0x1F] == 644 && npc->personData[0x3A] == 19);
    CHECK(npc->personData[0x20] == 644);  // Init saw the id
}

// With flag 0x02, A and B are neighbor ids and the variables are A's saved list towards B.
void relationshipWithNeighborIdsUsesTheNeighborhood() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        relationship(2, 0x02, owner::kLiteral, 1, owner::kLiteral, 9, owner::kTemp, 0, T, F),
    }));
    code.add(kObjectGroup, 0x1001, bhav({
        relationship(0, 0x06, owner::kLiteral, 9, owner::kLiteral, 1, owner::kLiteral, 55, 1, F),
        relationship(0, 0x02, owner::kLiteral, 9, owner::kLiteral, 1, owner::kTemp, 1, T, F),
    }));
    code.add(kObjectGroup, 0x1002, bhav({
        relationship(0, 0x02, owner::kLiteral, 1, owner::kLiteral, 4, owner::kTemp, 0, T, F),
    }));
    ts2::sim::World world;
    auto& a = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::Unsupported);

    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {1, 9};
    hood->relationships[{std::int16_t{1}, std::int16_t{9}}] = {10, 0, 42, 49152};
    world.setNeighborhood(hood);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::True);
    CHECK(a.thread.temps[0] == 42);
    // The saved values are 32-bit. The primitive moves their low 16 bits and writes sign-extended.
    code.add(kObjectGroup, 0x1003, bhav({
        relationship(3, 0x02, owner::kLiteral, 1, owner::kLiteral, 9, owner::kTemp, 2, 1, F),
        relationship(2, 0x06, owner::kLiteral, 1, owner::kLiteral, 9, owner::kLiteral, 0xFFFB, T, F),  // -5
    }));
    CHECK(vm.run(a, 0x1003).outcome == Outcome::True && a.thread.temps[2] == -16384);
    CHECK((*world.neighborRelationshipValues(1, 9))[2] == -5 && (*world.neighborRelationshipValues(1, 9))[3] == 49152);
    CHECK(vm.run(a, 0x1001).outcome == Outcome::True);  // 9 -> 1 had no list: it grows
    CHECK(a.thread.temps[1] == 55);
    CHECK(hood->relationships.size() == 1);              // the saved neighborhood is not changed
    const RunResult missing = vm.run(a, 0x1002);
    CHECK(missing.outcome == Outcome::Error && missing.reason == "neighbor not found");
}

// Between two Sims given by object ids, the variables are their neighbors' lists (found by GUID),
// as with neighbor ids. A Sim that is no neighbor is an error, and without a neighborhood it is
// unsupported.
void relationshipBetweenSimsUsesTheirNeighbors() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        relationship(0, 0x01, owner::kLiteral, 1, owner::kLiteral, 2, owner::kTemp, 0, 1, F),
        relationship(0, 0x04, owner::kLiteral, 2, owner::kLiteral, 1, owner::kLiteral, 33, T, F),
    }));
    code.add(kObjectGroup, 0x1001, bhav({
        relationship(0, 0x01, owner::kLiteral, 1, owner::kLiteral, 3, owner::kTemp, 0, T, F),
    }));
    ts2::sim::World world;
    auto& a = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    world.create(kObjectGroup, typed(0x52525252, ts2::sim::kPersonType));
    world.create(kObjectGroup, typed(0x53535353, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::Unsupported);

    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {104, 102};
    hood->neighbors[104] = ts2::sim::SavedNeighbor{0x51515151, {}};
    hood->neighbors[102] = ts2::sim::SavedNeighbor{0x52525252, {}};
    hood->relationships[{std::int16_t{104}, std::int16_t{102}}] = {48, 0, 20};
    world.setNeighborhood(hood);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::True);
    CHECK(a.thread.temps[0] == 48);
    CHECK(world.neighborRelationshipValue(102, 104, 0) == 33);
    CHECK(world.relationship(1, 2).empty() && world.relationship(2, 1).empty());
    const RunResult noNeighbor = vm.run(a, 0x1001);
    CHECK(noNeighbor.outcome == Outcome::Error && noNeighbor.reason == "Sim has no neighbor");
}

// Owner 0x26 "neighbor's object definition": a field of the object type of the neighbor in the
// stack object.
void neighborObjectDefinitionReads() {
    constexpr std::uint32_t kNeighborGuid = 0x0A000101;
    MemoryCodeSource code;
    auto definition = typed(kNeighborGuid, ts2::sim::kPersonType);
    definition.fields[0x2F] = 42;
    code.addObjectType(kObjectGroup, definition);
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 14, 1, E),
        expr(owner::kTemp, 0, op::kAssign, ts2::sim::owner::kNeighborObjectDefinition, 0x2F, T, E),
    }));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 14, 1, E),
        expr(owner::kTemp, 0, op::kAssign, ts2::sim::owner::kNeighborObjectDefinition, 500, T, E),
    }));
    ts2::sim::World world;
    auto& a = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {14};
    hood->neighbors[14].guid = kNeighborGuid;
    world.setNeighborhood(hood);
    Vm vm(code, {}, &world);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::True && a.thread.temps[0] == 42);
    const RunResult bad = vm.run(a, 0x1001);
    CHECK(bad.outcome == Outcome::Error && bad.reason == "index into object definition out of range");
}

// Get Distance To: from the object in temp 1 to the stack object, in tiles or hundredths. A level
// counts as 10 tiles.
void getDistanceToMeasuresInTiles() {
    auto distance = [](bool hundredths) {
        Instruction in;
        in.opcode = 0x000B;
        in.trueTarget = T;
        in.falseTarget = E;
        in.operands[0] = 0;  // into temp 0
        in.operands[2] = 0x01;
        in.operands[3] = owner::kTemp;
        in.operands[4] = 1;
        in.operands[6] = hundredths ? 0x02 : 0x00;
        return in;
    };
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({distance(false)}));
    code.add(kObjectGroup, 0x1001, bhav({distance(true)}));
    SmallWorld w;
    auto place = [&](std::int16_t id, float x, float y, std::int32_t level) {
        auto* o = w.world.find(id);
        o->x = static_cast<std::int32_t>(x * 65536);
        o->y = static_cast<std::int32_t>(y * 65536);
        o->level = level;
    };
    place(2, 10.5f, 10.5f, 1);
    place(3, 13.5f, 14.5f, 1);
    place(4, 10.5f, 10.5f, 2);
    Vm vm(code, {}, &w.world);
    auto measure = [&](std::uint16_t tree, std::int16_t to) {
        w.controller->thread.stackObjectId = to;
        w.controller->thread.temps[1] = 2;
        CHECK(vm.run(*w.controller, tree).outcome == Outcome::True);
        return w.controller->thread.temps[0];
    };
    CHECK(measure(0x1000, 3) == 5);
    CHECK(measure(0x1001, 3) == 500);
    CHECK(measure(0x1000, 4) == 10);
}

Instruction changeLight(std::uint8_t flags, std::int16_t object, std::uint8_t percent, std::uint8_t light) {
    Instruction in = primitiveCall(0x006F, T, F);
    in.operands[1] = flags;
    in.operands[2] = owner::kLiteral;
    in.operands[3] = static_cast<std::uint8_t>(object);
    in.operands[4] = static_cast<std::uint8_t>(object >> 8);
    in.operands[7] = percent;
    in.operands[8] = light;
    return in;
}

void changeLightSetsLightStrength() {
    auto run = [](const Instruction& in, std::int16_t temp0, Outcome& outcome) {
        MemoryCodeSource code;
        code.add(kObjectGroup, 0x1000, bhav({in}));
        SmallWorld w;
        w.controller->thread.temps[0] = temp0;
        Vm vm(code, {}, &w.world);
        outcome = vm.run(*w.controller, 0x1000).outcome;
        return w.world.find(3) ? w.world.find(3)->lightStrength : -1.0f;
    };
    Outcome outcome{};
    CHECK(run(changeLight(0, 3, 40, 0xFF), 0, outcome) == static_cast<float>(40 * 0.01) && outcome == Outcome::True);
    CHECK(run(changeLight(0, 3, 150, 0xFF), 0, outcome) == 0.5f);        // out of range: half
    CHECK(run(changeLight(0x02, 3, 0, 0xFF), 75, outcome) == 0.75f);     // from Temp 0
    CHECK(run(changeLight(0x02, 3, 0, 0xFF), -3, outcome) == 0.5f);
    run(changeLight(0, 3, 40, 0), 0, outcome);                            // a named light with no name table
    CHECK(outcome == Outcome::Error);
    run(changeLight(0, 99, 40, 0xFF), 0, outcome);                        // no such object
    CHECK(outcome == Outcome::Error);
    // A named light is looked up in the stack object's table, as a Sim lighting a cake's candles
    // reads the cake's.
    MemoryCodeSource code;
    constexpr std::uint32_t kCakeGroup = 0x7F0000AA;
    code.add(kObjectGroup, 0x1000, bhav({changeLight(0, 3, 40, 0)}));
    code.addStrings(kCakeGroup, 0x8E, {"candle"});
    SmallWorld w;
    auto& cake = w.world.create(kCakeGroup, typed(kChairGuid));
    w.controller->thread.stackObjectId = cake.id;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True && w.world.find(3)->lightStrength == static_cast<float>(40 * 0.01));
}

Instruction changeMaterial(std::uint16_t material, std::uint8_t flags, std::uint16_t meshGroup, std::int16_t object,
                           std::int16_t source = 0) {
    Instruction in = primitiveCall(0x006D, T, F);
    in.operands[0] = static_cast<std::uint8_t>(material);
    in.operands[1] = static_cast<std::uint8_t>(material >> 8);
    in.operands[2] = flags;
    in.operands[3] = static_cast<std::uint8_t>(meshGroup);
    in.operands[4] = static_cast<std::uint8_t>(meshGroup >> 8);
    in.operands[5] = owner::kLiteral;
    in.operands[6] = static_cast<std::uint8_t>(object);
    in.operands[7] = static_cast<std::uint8_t>(object >> 8);
    in.operands[8] = owner::kLiteral;
    in.operands[9] = static_cast<std::uint8_t>(source);
    in.operands[10] = static_cast<std::uint8_t>(source >> 8);
    return in;
}

void changeMaterialNamesAMaterialForAMeshGroup() {
    MemoryCodeSource code;
    code.addStrings(kObjectGroup, 0x88, {"wood", "metal"});
    code.addStrings(kObjectGroup, 0x87, {"frame", ""});
    code.addStrings(kSemiGroup, 0x88, {"semi"});
    code.addStrings(ts2::sim::kGlobalGroup, 0x87, {"globalgroup"});
    const std::uint32_t otherGroup = 0x7F000099;
    code.addStrings(otherGroup, 0x88, {"other"});
    SmallWorld w;
    auto& source = w.world.create(otherGroup, typed(kChairGuid));
    auto run = [&](const Instruction& in, std::int16_t temp0 = 0, std::int16_t temp1 = 0) {
        code.add(kObjectGroup, 0x1000, bhav({in}));
        w.controller->thread.temps[0] = temp0;
        w.controller->thread.temps[1] = temp1;
        Vm vm(code, {}, &w.world);
        return vm.run(*w.controller, 0x1000).outcome;
    };
    using Change = ts2::sim::MaterialChange;
    const auto& changes = w.world.find(3)->materialChanges;
    CHECK(run(changeMaterial(1, 0, 0, 3)) == Outcome::True);
    CHECK((changes == std::vector<Change>{{"frame", "metal"}}));
    const auto version = w.world.find(3)->materialVersion;
    CHECK(run(changeMaterial(1, 0, 0, 3)) == Outcome::True);                // the same again: no change
    CHECK(w.world.find(3)->materialVersion == version);
    CHECK(run(changeMaterial(0, 0x10, 0, 3), 0) == Outcome::True);          // material from temp 0
    CHECK((changes == std::vector<Change>{{"frame", "wood"}}));
    CHECK(run(changeMaterial(0, 0x04 | 0x40, 0, 3)) == Outcome::True);      // semi-global material, global group
    CHECK((changes == std::vector<Change>{{"frame", "wood"}, {"globalgroup", "semi"}}));
    CHECK(run(changeMaterial(0, 0x20, 0, 3), 0, -1) == Outcome::True);      // group -1 from temp 1: whole model
    CHECK((changes == std::vector<Change>{{"", "wood"}}));
    CHECK(run(changeMaterial(1, 0, 1, 3)) == Outcome::True);                // an empty group name: whole model
    CHECK((changes == std::vector<Change>{{"", "metal"}}));
    CHECK(run(changeMaterial(0, 0x08, 0xFFFF, 3, source.id)) == Outcome::True);  // the source's table
    CHECK((changes == std::vector<Change>{{"", "other"}}));

    CHECK(run(changeMaterial(5, 0, 0, 3)) == Outcome::Error);               // no such material
    CHECK(run(changeMaterial(0, 0x04, 0, 3, source.id)) == Outcome::True);
    CHECK((changes == std::vector<Change>{{"", "other"}, {"frame", "semi"}}));
    CHECK(run(changeMaterial(0, 0x08 | 0x04, 0, 3, source.id)) == Outcome::Error);  // no semi-global table
    CHECK(run(changeMaterial(0, 0, 0, 99)) == Outcome::Error);              // no such target
    CHECK(run(changeMaterial(0, 0x08, 0, 3, 99)) == Outcome::Error);        // no such source
    Instruction snapshot = changeMaterial(0, 0, 0, 3);
    snapshot.operands[13] = 0x02;
    CHECK(run(snapshot) == Outcome::Unsupported);
    w.world.find(3)->materialNames = {"", "", "metal_recolor"};             // the target's recolor first
    CHECK(run(changeMaterial(1, 0, 0, 3)) == Outcome::True);
    CHECK((changes == std::vector<Change>{{"", "other"}, {"frame", "metal_recolor"}}));
    CHECK(run(changeMaterial(1, 0x02, 0, 3)) == Outcome::Error);               // not for the global table
    source.materialNames = {"", "source_recolor"};
    CHECK(run(changeMaterial(0, 0x08, 0, 3, source.id)) == Outcome::True);  // the source's with flag 0x08
    CHECK((changes == std::vector<Change>{{"", "other"}, {"frame", "source_recolor"}}));
    w.world.find(3)->materialNames.clear();
    Instruction channel = changeMaterial(1, 0, 0, 3);
    channel.operands[13] = 0x01;                                            // a TV channel: still its material
    CHECK(run(channel) == Outcome::True);
    CHECK((changes == std::vector<Change>{{"", "other"}, {"frame", "metal"}}));
}

Instruction effectStopStart(std::uint8_t mode, std::int16_t name, std::uint8_t slot = 0xFF, std::uint8_t slotKind = 3, std::uint16_t flags = 0) {
    Instruction in = primitiveCall(0x0070, T, F);
    in.operands[0] = mode;
    in.operands[1] = owner::kMyData;
    in.operands[2] = 0x0B;  // my id
    in.operands[4] = static_cast<std::uint8_t>(name);
    in.operands[5] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(name) >> 8);
    in.operands[6] = slot;
    in.operands[9] = slotKind;
    in.operands[10] = static_cast<std::uint8_t>(flags);
    in.operands[11] = static_cast<std::uint8_t>(flags >> 8);
    return in;
}

void heldObjectsComeAlongWithTheirHolder() {
    ts2::sim::World world;
    ts2::objd::ObjectDefinition definition;
    auto& sim = world.create(1, definition, 1);
    auto& holder = world.create(1, definition, 1);
    auto& held = world.create(1, definition);
    sim.slots[0] = holder.id;
    holder.slots[0] = held.id;
    sim.x = 5 << 16;
    sim.y = 6 << 16;
    sim.level = 2;
    sim.data[0x1D] = 14;
    world.updateRoom(sim);
    CHECK(held.x == sim.x && held.y == sim.y && held.level == 2 && held.data[0x1D] == 14);
    CHECK(holder.x == sim.x && holder.level == 2 && holder.data[0x1D] == 14);
    // A ring of slots in a broken save ends the walk.
    held.slots.push_back(sim.id);
    sim.level = 3;
    world.updateRoom(sim);
    CHECK(held.level == 3);
}

void effectStopStartAsksForEffects() {
    using Do = ts2::sim::EffectCommand::Do;
    MemoryCodeSource code;
    code.addStrings(kObjectGroup, 0x8F, {"own_fx", "other_fx"});
    code.addStrings(ts2::sim::kGlobalGroup, 0x8F, {"global_fx"});
    SmallWorld w;
    std::vector<ts2::sim::EffectCommand> asked;
    w.world.setEffectCommands([&](const ts2::sim::EffectCommand& c) { asked.push_back(c); });
    auto run = [&](const Instruction& in) {
        code.add(kObjectGroup, 0x1000, bhav({in}));
        Vm vm(code, {}, &w.world);
        return vm.run(*w.controller, 0x1000).outcome;
    };
    const auto me = w.controller->id;
    w.controller->data[0x0B] = me;
    CHECK(run(effectStopStart(0, 1)) == Outcome::True);
    CHECK(run(effectStopStart(1, 0, 0xFF, 3, 0x1)) == Outcome::True);  // global names
    CHECK(run(effectStopStart(15, 0)) == Outcome::True);
    w.controller->thread.temps[0] = 1;
    w.controller->thread.temps[1] = 7;
    CHECK(run(effectStopStart(0, 0, 0xFF, 3, 0x8 | 0x40)) == Outcome::True);  // the name in temp 0, the number in temp 1
    CHECK(run(effectStopStart(2, 0)) == Outcome::True);
    CHECK(run(effectStopStart(3, 1)) == Outcome::True);
    CHECK(asked.size() == 6);
    CHECK(asked[0].what == Do::Start && asked[0].name == "other_fx" && asked[0].object == me && !asked[0].hard && !asked[0].keep);
    CHECK(asked[1].what == Do::Start && asked[1].name == "global_fx" && asked[1].hard);
    CHECK(asked[2].keep);
    CHECK(asked[3].name == "other_fx" && asked[3].number == 7);
    CHECK(asked[4].what == Do::Stop && !asked[4].hard);
    CHECK(asked[5].what == Do::Stop && asked[5].hard);

    // Mode 4 asks for both of the primitive's kinds, softly. Mode 9 forgets all of them, with no name needed.
    asked.clear();
    CHECK(run(effectStopStart(4, -1)) == Outcome::True);
    CHECK(run(effectStopStart(9, -1)) == Outcome::True);
    CHECK(asked.size() == 3);
    CHECK(asked[0].what == Do::StopAll && asked[0].kind == 2 && !asked[0].hard && asked[1].kind == 5);
    CHECK(asked[2].what == Do::StopAll && asked[2].kind == 0 && asked[2].hard);

    // These are errors: no name where one is needed, a name that isn't there and a slot that isn't there.
    asked.clear();
    CHECK(run(effectStopStart(0, -1)) == Outcome::Error);
    CHECK(run(effectStopStart(0, 9)) == Outcome::Error);
    CHECK(run(effectStopStart(0, 0, 3, 0)) == Outcome::Error);
    CHECK(asked.empty());

    // Pictures after a start: an object's thumbnail by its model number or temp 6, a Sim's portrait by object or
    // neighbor. Without the object it is an error, and an unknown GUID or neighbor gives no picture.
    auto withValue = [&](std::uint16_t flags, std::uint8_t ownerOfValue, std::uint8_t data, int mode = 0) {
        Instruction in = effectStopStart(static_cast<std::uint8_t>(mode), 0, 0xFF, 3, flags);
        in.operands[12] = ownerOfValue;
        in.operands[13] = data;
        return in;
    };
    constexpr std::uint8_t kTemps = owner::kTemp;
    constexpr std::uint16_t kSim = 0x51515151 & 0xFFFF;
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighbors[9].guid = 0x51515151;
    w.world.setNeighborhood(hood);
    const std::int16_t simObject = [&] {
        for (std::int16_t id = 1; id < 20; ++id) {
            if (const auto* o = w.world.find(id); o != nullptr && o->isPerson()) {
                return id;
            }
        }
        return std::int16_t{0};
    }();
    w.controller->definition.fields[0x48] = 0x34;
    w.controller->definition.fields[0x49] = 0x12;
    auto& temps = w.controller->thread.temps;
    temps[2] = me;
    temps[3] = simObject;
    temps[6] = 5;
    temps[4] = kSim;
    temps[5] = 0x5151;
    temps[7] = 9;
    CHECK(run(withValue(0x4, kTemps, 2)) == Outcome::True);
    CHECK(asked.back().pictureGuid == 0x0BADF00D && asked.back().picture == "BalloonEffectThumbnail_0x0badf00d" && asked.back().pictureIndex == 0x120034 && asked.back().pictureNeighbor == 0);
    CHECK(run(withValue(0x4 | 0x800, kTemps, 2, 1)) == Outcome::True);
    CHECK(asked.back().pictureIndex == 5);
    CHECK(run(withValue(0x4, kTemps, 3)) == Outcome::True);
    CHECK(asked.back().pictureGuid == 0x51515151 && asked.back().pictureNeighbor == 9);
    CHECK(run(withValue(0x10, kTemps, 7)) == Outcome::True);
    CHECK(asked.back().pictureGuid == 0x51515151 && asked.back().pictureNeighbor == 9);
    temps[7] = 99;
    CHECK(run(withValue(0x10, kTemps, 7)) == Outcome::True);
    CHECK(asked.back().picture.empty());
    CHECK(run(withValue(0x4 | 0x400, kTemps, 2)) == Outcome::True);  // a type this code source doesn't have
    CHECK(asked.back().picture.empty());
    CHECK(run(withValue(0x4, kTemps, 2, 6)) == Outcome::True);  // only starts take pictures
    CHECK(asked.back().picture.empty());
    // Queued starts carry temp 2 and temp 3, and mode 8 also forgets the queue.
    temps[2] = 7;
    temps[3] = 100;
    CHECK(run(effectStopStart(1, 0, 0xFF, 3, 0x80)) == Outcome::True);
    CHECK(asked.back().queued && asked.back().hard && asked.back().queuePriority == 7 && asked.back().queueWait == 100);
    CHECK(run(effectStopStart(8, -1)) == Outcome::True);
    CHECK(asked.back().clearsQueue);
    temps[2] = me;
    temps[3] = simObject;
    // A want or fear of a Sim by temp 4 and temp 5. An empty place shows nothing, and past the list it is an error.
    w.world.neighborWants(9)[1].guid = kFridgeGuid;
    temps[4] = 0;
    temps[5] = 1;
    CHECK(run(withValue(0x2000, kTemps, 3)) == Outcome::True);
    CHECK(asked.back().pictureWant && asked.back().pictureWant->guid == kFridgeGuid);
    temps[5] = 0;
    CHECK(run(withValue(0x2000, kTemps, 3)) == Outcome::True);
    CHECK(!asked.back().pictureWant);
    temps[4] = 1;
    temps[5] = 20;
    CHECK(run(withValue(0x2000, kTemps, 3)) == Outcome::Error);
    CHECK(run(withValue(0x2000, kTemps, 2)) == Outcome::Error);  // not a Sim
    temps[4] = kSim;
    temps[5] = 0x5151;
    // A score from temp 6, which wins over the other pictures.
    temps[6] = -1500;
    CHECK(run(withValue(0x1000 | 0x4, kTemps, 2)) == Outcome::True);
    CHECK(asked.back().pictureNumber == -1500 && asked.back().pictureGuid == 0);
    CHECK(run(withValue(0x1000, kTemps, 2, 6)) == Outcome::True);
    CHECK(!asked.back().pictureNumber);
    // A memory's own picture, with its GUID in my token or the temps.
    code.addObjectType(kObjectGroup, typed(kFridgeGuid));
    w.controller->token.guid = kFridgeGuid;
    CHECK(run(withValue(0x4 | 0x100 | 0x200, kTemps, 2)) == Outcome::True);
    CHECK(asked.back().pictureGuid == kFridgeGuid && asked.back().pictureIsMemory);
    temps[4] = static_cast<std::int16_t>(kFridgeGuid & 0xFFFF);
    temps[5] = static_cast<std::int16_t>(kFridgeGuid >> 16);
    CHECK(run(withValue(0x4 | 0x400 | 0x200, kTemps, 2)) == Outcome::True);
    CHECK(asked.back().pictureGuid == kFridgeGuid && asked.back().pictureIsMemory);
    CHECK(run(withValue(0x4 | 0x400, kTemps, 2)) == Outcome::True);
    CHECK(asked.back().pictureGuid == kFridgeGuid && !asked.back().pictureIsMemory);
    temps[2] = 999;
    CHECK(run(withValue(0x4, kTemps, 2)) == Outcome::Error);
}

Instruction soundEvent(std::int16_t id, std::uint8_t flags, std::uint8_t indexOwner = 0, std::uint16_t indexData = 0) {
    Instruction in = primitiveCall(0x0017, T, F);
    in.operands[0] = static_cast<std::uint8_t>(id);
    in.operands[1] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(id) >> 8);
    in.operands[4] = flags;
    in.operands[6] = indexOwner;
    in.operands[7] = static_cast<std::uint8_t>(indexData);
    in.operands[8] = static_cast<std::uint8_t>(indexData >> 8);
    return in;
}

void soundEventNamesTheSoundAndItsSource() {
    MemoryCodeSource code;
    code.addStrings(kObjectGroup, 0x4132, {"own_a", "own_b"});
    code.addStrings(ts2::sim::kGlobalGroup, 0x4132, {"", "global_b"});
    code.addStrings(kSemiGroup, 0x4132, {"semi_a"});
    SmallWorld w;
    std::vector<ts2::sim::SoundEvent> heard;
    w.world.setSoundEvents([&](const ts2::sim::SoundEvent& e) { heard.push_back(e); });
    auto run = [&](const Instruction& in) {
        code.add(kObjectGroup, 0x1000, bhav({in}));
        Vm vm(code, {}, &w.world);
        return vm.run(*w.controller, 0x1000).outcome;
    };
    const auto me = w.controller->id;
    w.controller->thread.stackObjectId = 3;
    w.controller->thread.temps[2] = 1;
    CHECK(run(soundEvent(1, 0)) == Outcome::True);
    CHECK(run(soundEvent(10001, 0x02)) == Outcome::True);                  // global, the stack object plays it
    CHECK(run(soundEvent(20000, 0x42)) == Outcome::True);                  // semi-global, stopped
    CHECK(run(soundEvent(-1, 0xFF, owner::kTemp, 2)) == Outcome::True);    // own table, entry in temp 2
    CHECK(run(soundEvent(10000, 0)) == Outcome::True);                     // an empty name: nothing
    CHECK(run(soundEvent(7, 0)) == Outcome::True);                         // no such entry: nothing
    CHECK(heard.size() == 4);
    CHECK(heard[0].name == "own_b" && heard[0].source == me && !heard[0].stop);
    CHECK(heard[1].name == "global_b" && heard[1].source == 3);
    CHECK(heard[2].name == "semi_a" && heard[2].source == 3 && heard[2].stop);
    CHECK(heard[3].name == "own_b" && heard[3].source == me);
    heard.clear();
    w.world.find(3)->data[0x22] = 0x02;                                    // hidden: silent
    CHECK(run(soundEvent(1, 0x02)) == Outcome::True && heard.empty());
    w.controller->thread.stackObjectId = 5;                                // a Sim off the lot: silent
    CHECK(run(soundEvent(1, 0x02)) == Outcome::True && heard.empty());
}

// Cinematic needs an object with object arrays (its actors) and is false: no cinematic starts.
void cinematicChecksItsActorsAndStartsNone() {
    MemoryCodeSource code;
    SmallWorld w;
    auto run = [&](std::uint8_t actorsOwner, std::uint16_t actorsData, std::uint8_t flags = 0) {
        Instruction in = primitiveCall(0x007B, T, F);
        in.operands[0] = 1;
        in.operands[5] = flags;
        in.operands[6] = owner::kTemp;
        in.operands[7] = 9;  // temp 9 is past the temps: an error when the entry is read from it
        in.operands[9] = actorsOwner;
        in.operands[10] = static_cast<std::uint8_t>(actorsData);
        code.add(kObjectGroup, 0x1000, bhav({in}));
        Vm vm(code, {}, &w.world);
        return vm.run(*w.controller, 0x1000);
    };
    w.controller->thread.temps[0] = 3;
    w.world.find(3)->arrays.assign(1, {});
    CHECK(run(owner::kTemp, 0).outcome == Outcome::False);
    CHECK(run(owner::kTemp, 0, 0x10).outcome == Outcome::Error);
    w.world.find(3)->arrays.clear();
    CHECK(run(owner::kTemp, 0).reason == "bad object array");
    w.controller->thread.temps[0] = 99;
    CHECK(run(owner::kTemp, 0).reason == "could not find object reference");
}

// RayTrace's rules over the hits nearest first, as the game decides them: the watching Sim's own body
// (from its slot) is passed, the TV ends it clear with its id, another Sim in the way blocks with its id.
// One wall face may be passed but leaves the ray blocked, and two block at once. Floors block.
void rayTraceRulesJudgeTheHits() {
    using ts2::sim::RayHit;
    using Kind = RayHit::Kind;
    ts2::sim::RayRules rules;
    rules.source = 454;
    rules.target = 224;
    rules.sourceIsSlot = true;
    auto judge = [&](std::vector<RayHit> hits) { return ts2::sim::judgeRay(hits, rules); };
    auto object = [](std::int16_t id, float t) { return RayHit{Kind::Object, id, t}; };
    CHECK(judge({}).clear && judge({}).hit == 0);
    CHECK(judge({object(454, 0.0f), object(224, 0.6f)}).clear && judge({object(454, 0.0f), object(224, 0.6f)}).hit == 224);
    CHECK(!judge({object(454, 0.0f), object(389, 0.3f), object(224, 0.6f)}).clear);
    CHECK(judge({object(389, 0.3f), object(224, 0.6f)}).hit == 389);
    CHECK(judge({{Kind::Wall, 0, 0.2f}, object(224, 0.6f)}).clear);
    CHECK(!judge({{Kind::Wall, 0, 0.2f}}).clear && judge({{Kind::Wall, 0, 0.2f}}).hit == -1);
    CHECK(judge({{Kind::Wall, 0, 0.2f}, {Kind::Wall, 0, 0.25f}, object(224, 0.6f)}).hit == -1);
    CHECK(judge({{Kind::Floor, 0, 0.2f}}).hit == -1);
    CHECK(judge({object(389, 0.3f), object(389, 0.3002f), object(224, 0.6f)}).hit == 389);
    // A target point that is a slot is reached only at the very end.
    rules.targetIsSlot = true;
    CHECK(judge({object(224, 0.6f)}).hit == 224 && !judge({object(224, 0.6f)}).clear);
    CHECK(judge({object(224, 1.0f)}).clear);
    rules.targetIsSlot = false;
    // Ignored windows let the ray through the wall around them. The same window again blocks.
    rules.windowsIgnored = true;
    rules.isWindow = [](std::int16_t id) { return id == 77; };
    CHECK(judge({{Kind::Wall, 0, 0.2f}, object(77, 0.21f), {Kind::Wall, 0, 0.22f}, {Kind::Wall, 0, 0.23f}, object(224, 0.6f)}).clear);
    CHECK(judge({object(77, 0.21f), object(77, 0.4f)}).hit == 77);
    // From a slot, what holds the source or is held by the target is passed.
    rules.containerOf = [](std::int16_t id) { return id == 500 ? std::int16_t{224} : std::int16_t{0}; };
    CHECK(judge({object(500, 0.5f), object(224, 0.6f)}).clear);
}

void rayTraceAsksTheSceneAndWritesTemp0() {
    MemoryCodeSource code;
    Instruction in = primitiveCall(0x0078, T, F);
    in.operands[0] = 0;
    in.operands[1] = owner::kLiteral;
    in.operands[4] = 3;
    in.operands[5] = 3;
    in.operands[8] = owner::kLiteral;
    in.operands[9] = 3;
    SmallWorld w;
    in.operands[2] = static_cast<std::uint8_t>(w.controller->id);  // me
    code.add(kObjectGroup, 0x1000, bhav({in}));
    Vm vm(code, {}, &w.world);
    auto run = [&] { return vm.run(*w.controller, 0x1000).outcome; };
    w.controller->thread.temps[0] = 9;
    CHECK(run() == Outcome::Unsupported && w.controller->thread.temps[0] == 9);  // no scene to trace in
    ts2::sim::RayRequest asked;
    ts2::sim::RayAnswer answer;
    w.world.setRayQuery([&](const ts2::sim::RayRequest& r) -> std::optional<ts2::sim::RayAnswer> { asked = r; return answer; });
    answer.hits = {{ts2::sim::RayHit::Kind::Object, w.controller->id, 0.0f}, {ts2::sim::RayHit::Kind::Object, 3, 0.7f}};
    CHECK(run() == Outcome::True && w.controller->thread.temps[0] == 3);
    CHECK(asked.from.object == w.controller->id && asked.from.kind == 3 && asked.from.slot == 3 && asked.to && asked.to->object == 3);
    answer.hits = {{ts2::sim::RayHit::Kind::Object, 5, 0.3f}};
    CHECK(run() == Outcome::False && w.controller->thread.temps[0] == 5);
    answer.slotMissing = true;
    CHECK(run() == Outcome::Error);
    answer.slotMissing = false;
    in.operands[9] = 99;  // no such target
    code.add(kObjectGroup, 0x1000, bhav({in}));
    CHECK(run() == Outcome::Error);
    in.operands[9] = 3;
    in.operands[0] = 2;  // no such mode
    code.add(kObjectGroup, 0x1000, bhav({in}));
    CHECK(run() == Outcome::False);
}

Instruction dialog(std::uint8_t type, std::uint8_t flags) {
    Instruction in = primitiveCall(0x0024, T, F);
    in.operands[5] = type;
    in.operands[7] = flags;
    return in;
}

// Alter Budget charges the funds (income with flag 0x02), times bytes 7-8, and fails when they don't
// reach. Test only changes nothing.
void alterBudgetChargesTheFunds() {
    MemoryCodeSource code;
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    auto budget = [](std::uint16_t amount, std::uint8_t flags, std::uint16_t times) {
        Instruction in;
        in.opcode = 0x0019;
        in.trueTarget = T;
        in.falseTarget = F;
        in.operands = {0, 0, static_cast<std::uint8_t>(amount), static_cast<std::uint8_t>(amount >> 8), flags, 0, 0,
                       static_cast<std::uint8_t>(times), static_cast<std::uint8_t>(times >> 8)};
        return in;
    };
    auto run = [&](const Instruction& in) {
        code.add(kObjectGroup, 0x1000, bhav({in}));
        w.controller->thread = {};
        return vm.run(*w.controller, 0x1000).outcome;
    };
    ts2::sim::setFunds(vm.globals, 20000);
    CHECK(run(budget(500, 0, 0)) == Outcome::True && ts2::sim::funds(vm.globals) == 19500);
    CHECK(run(budget(500, 0x02, 3)) == Outcome::True && ts2::sim::funds(vm.globals) == 21000);
    CHECK(run(budget(30000, 0, 0)) == Outcome::False && ts2::sim::funds(vm.globals) == 21000);
    CHECK(run(budget(1000, 0x01, 0)) == Outcome::True && ts2::sim::funds(vm.globals) == 21000);
    Instruction bad = budget(1, 0, 0);
    bad.operands[6] = 11;
    CHECK(run(bad) == Outcome::Error);
}

// Camera Control marks the camera busy and takes two more ticks. While it is busy it is true at once.
void cameraControlWaitsWhileTheCameraMoves() {
    MemoryCodeSource code;
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    Instruction camera;
    camera.opcode = 0x0023;
    camera.trueTarget = T;
    camera.falseTarget = E;
    camera.operands = {0x0A, 0x00, 0x01, 0x02, 0x43, 0x00};
    code.add(kObjectGroup, 0x1000, bhav({camera}));
    w.world.find(3)->x = w.world.find(3)->y = 2 << 16;
    w.controller->thread.stackObjectId = 3;
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::Sleeping);
    CHECK(vm.globals[0x34] == 0x43 && vm.globals[0x0B] == 1);
    CHECK(vm.resume(*w.controller).outcome == Outcome::Sleeping);
    CHECK(vm.resume(*w.controller).outcome == Outcome::True);
    w.controller->thread = {};
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);  // still busy
    vm.globals[0x34] = 0;
    w.controller->thread = {};
    w.controller->thread.stackObjectId = 999;
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::Sleeping);
    CHECK(vm.resume(*w.controller).outcome == Outcome::Error);
}

// Dialog: notifications get ids and can be stopped once. Tutorial popups go on. Other dialogs wait a
// tick and answer with their first button. Pickers need the stack object's arrays.
void dialogsAnswerAsTheGameDoesHeadless() {
    MemoryCodeSource code;
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    auto run = [&](const Instruction& in) {
        code.add(kObjectGroup, 0x1000, bhav({in}));
        w.controller->thread = {};
        return vm.run(*w.controller, 0x1000).outcome;
    };
    auto& temps = w.controller->thread.temps;
    CHECK(run(dialog(0x08, 0x30)) == Outcome::True);   // notification id into temp 3
    const std::int16_t shown = w.controller->thread.temps[3];
    CHECK(shown > 0);
    Instruction stop = dialog(0x09, 0x30);
    code.add(kObjectGroup, 0x1000, bhav({stop}));
    w.controller->thread = {};
    temps[3] = shown;
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    w.controller->thread = {};
    temps[3] = shown;
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::False);  // already gone
    CHECK(run(dialog(0x16, 0x02)) == Outcome::True);
    CHECK(run(dialog(0x01, 0x01)) == Outcome::True);   // shown without waiting
    CHECK(run(dialog(0x01, 0x00)) == Outcome::Sleeping);
    CHECK(vm.resume(*w.controller).outcome == Outcome::True);
    // The Game Tips question is answered No when the world declines the tips.
    code.add(ts2::sim::kGameTipsGroup, 0x1000, bhav({dialog(0x01, 0x00)}));
    w.world.setDeclinesGameTips(true);
    w.controller->thread = {};
    CHECK(vm.run(*w.controller, ts2::sim::kGameTipsGroup, 0x1000).outcome == Outcome::Sleeping);
    CHECK(vm.resume(*w.controller).outcome == Outcome::False);
    CHECK(run(dialog(0x01, 0x00)) == Outcome::Sleeping);
    CHECK(vm.resume(*w.controller).outcome == Outcome::True);
    w.world.setDeclinesGameTips(false);
    code.add(kObjectGroup, 0x1000, bhav({dialog(0x06, 0x00)}));
    w.world.find(3)->arrays.clear();
    w.controller->thread = {};
    w.controller->thread.stackObjectId = 3;
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::Error);  // a picker without arrays
    w.world.find(3)->arrays.resize(1);
    w.controller->thread = {};
    w.controller->thread.stackObjectId = 3;
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::Sleeping);
}

// A text notification's text is string table 0x12D's line bytes 13-14 with its tokens filled in,
// found without regard to case. $Neighbor and $FullName also make the neighbor its picture. Tokens
// the engine can't fill stay as written. Append adds to the shown text, and a style the game has
// no bubble for is false.
void textNotificationsFillTheirTokens() {
    MemoryCodeSource code;
    code.addStrings(kObjectGroup, 0x12D, {"Hi $neighbor of $Family!", " Bye $FullName, $LastName."});
    SmallWorld w;
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {9};
    hood->neighbors[9].personData.assign(0xE7, 0);
    w.world.setNeighborhood(hood);
    w.world.setNeighborFirstName(9, "Mary");
    w.world.setNeighborSurname(9, "Sue");
    Vm vm(code, {}, &w.world);
    auto run = [&](std::uint8_t type, std::uint16_t line, std::uint8_t style) {
        Instruction in = dialog(type, 0x30);
        in.operands[9] = 3;
        in.operands[10] = 0x14;
        in.operands[12] = style;
        in.operands[13] = static_cast<std::uint8_t>(line);
        code.add(kObjectGroup, 0x1000, bhav({in}));
        const std::int16_t id = w.controller->thread.temps[3];
        w.controller->thread = {};
        w.controller->thread.temps[3] = id;
        w.controller->thread.stackObjectId = 9;
        return vm.run(*w.controller, 0x1000).outcome;
    };
    CHECK(run(0x08, 1, 0) == Outcome::True);
    const auto& shown = w.world.notifications();
    CHECK(shown.size() == 1 && shown[0].id == w.controller->thread.temps[3]);
    CHECK(shown[0].text == "Hi Mary of $Family!" && shown[0].neighbor == 9 && shown[0].extra == 0);
    CHECK(shown[0].priority == 3 && shown[0].timeout == 0x14 && shown[0].style == 0);
    CHECK(run(0x1C, 2, 0) == Outcome::True);
    CHECK(w.world.notifications()[0].text == "Hi Mary of $Family! Bye Mary Sue, Sue.");
    CHECK(run(0x08, 1, 3) == Outcome::False);
    CHECK(w.world.notifications().size() == 1);
    // A text naming no picture shows the stack object's own type, with the object as the extra.
    code.addStrings(kObjectGroup, 0x12D, {"Hi $neighbor of $Family!", " Bye $FullName, $LastName.", "Plain."});
    Instruction plain = dialog(0x08, 0x30);
    plain.operands[13] = 3;
    code.add(kObjectGroup, 0x1000, bhav({plain}));
    w.controller->thread = {};
    w.controller->thread.stackObjectId = 3;
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    const auto& last = w.world.notifications().back();
    CHECK(last.text == "Plain." && last.neighbor == 0 && last.objectType == kChairGuid && last.extra == 3);
}

// "Set Baby Name" asks to name the neighbors in temps 0-3 and waits for the answer once someone shows
// it. Each keeps its place, a temp naming no neighbor counts as 0. OK names them, trimmed of
// spaces and tabs. Namings wait in one list in the order asked, one per object, and show one at a
// time, as in the game. Without anyone showing it the dialog waits a tick and is true.
void setBabyNameWaitsForTheNames() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({dialog(0x17, 0x04)}));
    SmallWorld w;
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {9, 10};
    for (const std::int16_t id : hood->neighborIds) {
        hood->neighbors[id].guid = 0x0D000000u | static_cast<std::uint32_t>(id);
        hood->neighbors[id].personData.assign(0xE7, 0);
    }
    w.world.setNeighborhood(hood);
    Vm vm(code, {}, &w.world);
    auto& temps = w.controller->thread.temps;
    auto start = [&](std::int16_t first, std::int16_t second) {
        w.controller->thread = {};
        temps[0] = first;
        temps[1] = second;
        temps[2] = 77;  // not a neighbor
        return vm.run(*w.controller, 0x1000).outcome;
    };
    CHECK(start(9, 10) == Outcome::Sleeping);
    CHECK(vm.resume(*w.controller).outcome == Outcome::True);  // nobody shows it
    w.world.setAsksNames(true);
    CHECK(start(9, 10) == Outcome::Sleeping);
    CHECK((w.world.namesAsked() == std::vector<std::int16_t>{9, 10, 0, 0}));
    CHECK(vm.resume(*w.controller).outcome == Outcome::Sleeping);
    w.world.answerNames(true, {"  Ada\t", "Bo"});
    CHECK(vm.resume(*w.controller).outcome == Outcome::True);
    CHECK(w.world.neighborFirstName(9) == "Ada" && w.world.neighborFirstName(10) == "Bo");
    CHECK(w.world.namesAsked().empty());
    CHECK(start(10, 0) == Outcome::Sleeping);
    w.world.answerNames(false, {"Cy"});
    CHECK(vm.resume(*w.controller).outcome == Outcome::False);  // Cancel
    CHECK(w.world.neighborFirstName(10) == "Bo");
    // Requests wait in the order asked and show one at a time. Object 3's shows first.
    w.world.askNames(3, {9});
    CHECK(start(10, 0) == Outcome::Sleeping && w.world.namesAsked() == std::vector<std::int16_t>{9});  // waits its turn
    w.world.answerNames(true, {"Di"});
    CHECK((w.world.namesAsked() == std::vector<std::int16_t>{10, 0, 0, 0}));  // the next one shows
    CHECK(vm.resume(*w.controller).outcome == Outcome::Sleeping);
    w.world.answerNames(true, {"Ed"});
    CHECK(vm.resume(*w.controller).outcome == Outcome::True && w.world.neighborFirstName(10) == "Ed");
    // Object 3 was reset before it took its answer: its next naming takes that answer.
    w.world.askNames(3, {10});
    CHECK(w.world.takeNamesAnswer(3) == std::optional<bool>(true) && w.world.neighborFirstName(9) == "Di");
    // A naming started while its own earlier one shows closes it and asks anew.
    w.world.askNames(3, {9});
    w.world.askNames(3, {10});
    CHECK(w.world.namesAsked() == std::vector<std::int16_t>{10});
    // A removed asker's naming is cancelled.
    w.world.remove(*w.world.find(3));
    CHECK(w.world.namesAsked().empty());
    CHECK(start(0, 0) == Outcome::True);  // none to name
}

// "Phone Invite" (the Personal Phonebook) asks for a pick among the stack object's array 0 once
// someone shows it, with the title and caption numbers from bytes 13-14 and 6 and the code owner's
// texts. OK is true and leaves only the pick in the array, Cancel is false and leaves the array. Without
// anyone showing it the dialog waits a tick and is true.
void phoneInviteWaitsForThePick() {
    MemoryCodeSource code;
    Instruction in = dialog(0x06, 0x00);
    in.operands[6] = 0x12;
    in.operands[13] = 0x15;
    code.add(kObjectGroup, 0x1000, bhav({in}));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    w.world.find(3)->arrays = {{195, 194, 55, 54, 48}};
    auto start = [&] {
        w.controller->thread = {};
        w.controller->thread.stackObjectId = 3;
        return vm.run(*w.controller, 0x1000).outcome;
    };
    CHECK(start() == Outcome::Sleeping);
    CHECK(vm.resume(*w.controller).outcome == Outcome::True);  // nobody shows it
    w.world.setAsksPhoneBook(true);
    CHECK(start() == Outcome::Sleeping);
    const auto* asked = w.world.phoneBookAsked();
    CHECK(asked != nullptr && asked->asker == w.controller->id);
    CHECK((asked->neighbors == std::vector<std::int16_t>{195, 194, 55, 54, 48}));
    CHECK(asked->title == 0x15 && asked->caption == 0x12 && asked->textGroup == kObjectGroup);
    CHECK(vm.resume(*w.controller).outcome == Outcome::Sleeping);
    w.world.answerPhoneBook(std::int16_t{55});
    CHECK(vm.resume(*w.controller).outcome == Outcome::True);
    CHECK((w.world.find(3)->arrays[0] == std::vector<std::int16_t>{55}));
    CHECK(w.world.phoneBookAsked() == nullptr);
    CHECK(start() == Outcome::Sleeping);
    w.world.answerPhoneBook(std::nullopt);
    CHECK(vm.resume(*w.controller).outcome == Outcome::False);  // Cancel
    CHECK((w.world.find(3)->arrays[0] == std::vector<std::int16_t>{55}));
    // Requests show one at a time, and a removed asker's request goes.
    w.world.askPhoneBook({3, {1}, 0, 0, 0});
    CHECK(start() == Outcome::Sleeping && w.world.phoneBookAsked()->asker == 3);  // waits its turn
    w.world.remove(*w.world.find(3));
    CHECK(w.world.phoneBookAsked() != nullptr && w.world.phoneBookAsked()->asker == w.controller->id);
}

// The object type picker (type 0x34, "Call Sim Options") lists the GUIDs array 0 holds in halves,
// title from byte 6 and caption from bytes 13-14. OK leaves only the picked GUID's halves in the
// array, Cancel is false and leaves the array. Without anyone showing it the dialog is true.
void typePickerWaitsForThePick() {
    MemoryCodeSource code;
    Instruction in = dialog(0x34, 0x00);
    in.operands[6] = 0x20;
    in.operands[13] = 0x21;
    code.add(kObjectGroup, 0x1000, bhav({in}));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    // Just Talk 0xB1A6C732, Invite Household 0xB1A6C74E, Invite Over 0x71A6C717, as the game held them.
    const std::vector<std::int16_t> options{-14542, -20058, -14514, -20058, -14569, 29094};
    w.world.find(3)->arrays = {options};
    auto start = [&] {
        w.controller->thread = {};
        w.controller->thread.stackObjectId = 3;
        return vm.run(*w.controller, 0x1000).outcome;
    };
    CHECK(start() == Outcome::Sleeping);
    CHECK(vm.resume(*w.controller).outcome == Outcome::True);  // nobody shows it
    w.world.setAsksTypePicker(true);
    CHECK(start() == Outcome::Sleeping);
    const auto* asked = w.world.typePickerAsked();
    CHECK(asked != nullptr && asked->asker == w.controller->id);
    CHECK((asked->types == std::vector<std::uint32_t>{0xB1A6C732, 0xB1A6C74E, 0x71A6C717}));
    CHECK(asked->title == 0x20 && asked->caption == 0x21 && asked->textGroup == kObjectGroup);
    w.world.answerTypePicker(std::nullopt);
    CHECK(vm.resume(*w.controller).outcome == Outcome::False);  // Cancel
    CHECK(w.world.find(3)->arrays[0] == options);
    CHECK(start() == Outcome::Sleeping);
    w.world.answerTypePicker(0xB1A6C732u);
    CHECK(vm.resume(*w.controller).outcome == Outcome::True);
    CHECK((w.world.find(3)->arrays[0] == std::vector<std::int16_t>{-14542, -20058}));
    CHECK(w.world.typePickerAsked() == nullptr);
}

// The message box as the newspaper's job offer asks it: type 2 with the title, text and three
// buttons from string table 0x12D, tokens filled. Take Job is true, Look Again false with 0 in the
// temp byte 7 names, End Search false with 1. A missing button text is left empty for the usual one.
void messageBoxWaitsForAButton() {
    MemoryCodeSource code;
    Instruction in = dialog(0x02, 0x48);  // temp 4
    in.operands[6] = 1;
    in.operands[13] = 2;
    in.operands[3] = 10;
    in.operands[4] = 11;
    in.operands[0] = 12;
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kLocal, 1, op::kAssign, owner::kLiteral, 2, 1, E), in}, 0, 2));
    std::vector<std::string> texts(12);
    texts[0] = "SimCity Times";
    texts[1] = "An opening for $Local:0 days as a $JobTitle:0:1.";
    texts[9] = "Take Job";
    texts[10] = "Look Again";
    code.addStrings(kObjectGroup, 0x12D, texts);
    code.addStrings(kObjectGroup, 0x93, {"Unemployed", "File Clerk"});
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    auto start = [&] {
        w.controller->thread = {};
        return vm.run(*w.controller, 0x1000).outcome;
    };
    CHECK(start() == Outcome::Sleeping);
    CHECK(vm.resume(*w.controller).outcome == Outcome::True);  // nobody shows it
    w.world.setAsksMessages(true);
    CHECK(start() == Outcome::Sleeping);
    const auto* asked = w.world.messageAsked();
    CHECK(asked != nullptr && asked->asker == w.controller->id && asked->type == 2);
    CHECK(asked->title == "SimCity Times" && asked->message == "An opening for 0 days as a Unemployed.");
    CHECK((asked->buttons == std::vector<std::string>{"Take Job", "Look Again", ""}));
    w.world.answerMessage(1);
    CHECK(vm.resume(*w.controller).outcome == Outcome::False && w.controller->thread.temps[4] == 0);
    CHECK(start() == Outcome::Sleeping);
    w.world.answerMessage(2);
    CHECK(vm.resume(*w.controller).outcome == Outcome::False && w.controller->thread.temps[4] == 1);
    CHECK(start() == Outcome::Sleeping);
    w.world.answerMessage(0);
    CHECK(vm.resume(*w.controller).outcome == Outcome::True);
    CHECK(w.world.messageAsked() == nullptr);
    // An asker removed from the lot takes its box with it.
    CHECK(start() == Outcome::Sleeping);
    w.world.remove(*w.controller);
    CHECK(w.world.messageAsked() == nullptr);
}

// New objects lead themselves (object data 0x13), as every saved single-tile object does, so code
// that takes "stack obj's lead tile object ID" as the stack object (CT - Intersect IDs) keeps it.
void newObjectsLeadThemselves() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kStackObjectData, 0x13, 1, E),
        expr(owner::kTemp, 0, op::kAssign, owner::kStackObjectData, 0x0B, T, E),
    }));
    ts2::sim::World world;
    auto& me = world.create(kObjectGroup, typed(kChairGuid));
    auto& marker = world.create(kObjectGroup, typed(kFridgeGuid));
    CHECK(marker.data[0x13] == marker.id && me.data[0x13] == me.id);
    me.thread.stackObjectId = marker.id;
    Vm vm(code, {}, &world);
    CHECK(vm.run(me, 0x1000).outcome == Outcome::True && me.thread.temps[0] == marker.id);
}

void removeObjectRefusesProtectedObjects() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({removeObject(1, 0x00, T, F)}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& special = world.create(kObjectGroup, typed(0xCC7B08F8));
    sim.thread.stackObjectId = special.id;
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::False);
    CHECK(world.find(special.id) != nullptr);
}

void removingMeEndsTheRunUnlessReturnImmediately() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({removeObject(0, 0x00, T, F)}));
    code.add(kObjectGroup, 0x1001, bhav({
        removeObject(0, 0x01, 1, F),
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 4, T, E),
    }));
    ts2::sim::World world;
    auto& a = world.create(kObjectGroup, typed(kFridgeGuid));
    auto& b = world.create(kObjectGroup, typed(kFridgeGuid));
    Vm vm(code, {}, &world);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::MeRemoved);
    CHECK(!a.thread.isRunning());
    CHECK(vm.run(b, 0x1001).outcome == Outcome::True);
    CHECK(b.thread.temps[0] == 4);
    CHECK(world.size() == 0);
}

// The game only queues the delete: until the end of the tick a removed object keeps its id, so the
// remover may still write its data and removing it again is true, while searches skip it.
void removedObjectsKeepTheirDataUntilPurge() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        removeObject(1, 0x00, 1, F),
        expr(owner::kStackObjectSemiAttribute, 2, op::kAssign, owner::kLiteral, 7, 2, E),
        removeObject(1, 0x00, T, F),
    }));
    ts2::sim::World world;
    auto& me = world.create(kObjectGroup, typed(kChairGuid));
    auto& gone = world.create(kObjectGroup, typed(kFridgeGuid));
    const std::int16_t goneId = gone.id;
    me.thread.stackObjectId = goneId;
    Vm vm(code, {}, &world);
    CHECK(vm.run(me, 0x1000).outcome == Outcome::True);
    CHECK(world.find(goneId) == nullptr && world.size() == 1);
    CHECK(world.findIncludingRemoved(goneId) != nullptr && world.findIncludingRemoved(goneId)->semiAttributes[2] == 7);
    world.purge();
    CHECK(world.findIncludingRemoved(goneId) == nullptr);
}

// A removed object leaves its container's slot and the lot at once, so the hand it was in can take
// the next object in the same tick (a stove swaps the food in a Sim's hand for a baking pan).
void removedObjectsLeaveTheirSlotAtOnce() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({removeObject(1, 0x00, T, F)}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType), 1);
    auto& food = world.create(kObjectGroup, typed(kFridgeGuid));
    sim.slots[0] = food.id;
    food.data[2] = sim.id;
    food.data[3] = 0;
    food.x = sim.x = 3 << 16;
    food.y = sim.y = 4 << 16;
    sim.thread.stackObjectId = food.id;
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(sim.slots[0] == 0 && food.data[2] == 0 && food.data[3] == -1);
    CHECK(food.x == ts2::sim::kOffLot && food.data[0x1D] == -1);
}

// Hidden Flags (object data 0x22) hide the model at bits 0x01 and 0x20 only. Bit 0x02 silences an
// object and bit 0x04 leaves it drawn. A value of -1, as the plumbob and cars set, has them all.
void hiddenFlagsHideTheModel() {
    ts2::sim::SimObject object;
    CHECK(!ts2::sim::modelHidden(object));
    for (const auto& [flags, hidden] : {std::pair{0x01, true}, {0x20, true}, {0x21, true}, {0x02, false}, {0x04, false}, {0x10, false}, {-1, true}}) {
        object.data[0x22] = static_cast<std::int16_t>(flags);
        CHECK(ts2::sim::modelHidden(object) == hidden);
    }
}

// Food changes its model as it is cooked by writing its graphic (object data 0): the object takes
// that row of its model names at once, and the drawn model follows. An empty row keeps the model. A
// script setting a temp or comparing the graphic changes nothing.
void graphicWritesChangeTheModel() {
    MemoryCodeSource code;
    code.addStrings(kObjectGroup, 0x85, {"", "nullModel", "foodPrepSpaghetti", "foodCookSpaghetti", ""});
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectData, 0, op::kAssign, owner::kLiteral, 2, 1, E),
        expr(owner::kStackObjectData, 0, op::kEquals, owner::kLiteral, 2, 2, E),
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 3, T, E),
    }));
    code.add(kObjectGroup, 0x1001, bhav({expr(owner::kMyData, 0, op::kAssign, owner::kLiteral, 3, T, E)}));
    code.add(kObjectGroup, 0x1002, bhav({expr(owner::kMyData, 0, op::kAssign, owner::kLiteral, 4, T, E)}));
    code.add(kObjectGroup, 0x1003, bhav({expr(owner::kMyData, 0, op::kAssign, owner::kLiteral, 1, T, E)}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType), 1);
    auto& food = world.create(kObjectGroup, typed(kFridgeGuid));
    sim.thread.stackObjectId = food.id;
    Vm vm(code, {}, &world);
    const auto names = [&](std::size_t row) { return code.string(kObjectGroup, 0x85, row); };
    auto version = world.objectsVersion();
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(food.model == "foodPrepSpaghetti" && sim.model.empty() && world.objectsVersion() > version);
    CHECK(ts2::sim::createdModelName(food, names) == "foodPrepSpaghetti");
    CHECK(vm.run(food, 0x1001).outcome == Outcome::True && food.model == "foodCookSpaghetti");
    version = world.objectsVersion();
    CHECK(vm.run(food, 0x1002).outcome == Outcome::True && food.data[0] == 4 && food.model == "foodCookSpaghetti");
    CHECK(world.objectsVersion() == version);
    // nullModel is a model that draws nothing.
    CHECK(vm.run(food, 0x1003).outcome == Outcome::True && food.model == "nullModel");
    CHECK(ts2::sim::createdModelName(food, names).empty());
}

// A saved Placeholder or TemplatePerson among the ids the object map's wait table covers is marked
// for delete as the lot loads, as the game does. Purges keep it until a script removes it.
void loadedPlaceholdersAreKeptUntilUnload() {
    constexpr std::uint32_t kPlaceholder = 0x3C08965F, kTemplatePerson = 0x7FD96B54;
    MemoryCodeSource code;
    code.addObjectType(kObjectGroup, typed(kFridgeGuid));
    code.addObjectType(kObjectGroup, typed(kPlaceholder));
    code.addObjectType(kObjectGroup, typed(kTemplatePerson, 0x10));
    ts2::content::Lot lot;
    for (const auto& [id, guid] : {std::pair{1u, kFridgeGuid}, {2u, kPlaceholder}, {3u, kTemplatePerson}, {4u, kPlaceholder}}) {
        ts2::content::LotObject saved;
        saved.id = id;
        saved.type = ts2::objt::Entry{};
        saved.type->guid = guid;
        lot.objects.push_back(saved);
    }
    lot.objectMap.waitTicks.assign(3, 0);  // ids 1 to 3
    ts2::sim::World world;
    CHECK(ts2::sim::buildWorld(lot, code, world).keptUntilUnload == 2);
    CHECK(world.find(1) != nullptr && world.find(2) == nullptr && world.find(3) == nullptr);
    CHECK(world.find(4) != nullptr);  // past the wait table
    CHECK(world.size() == 2 && world.removedIds().empty());
    world.purge();
    CHECK(world.findIncludingRemoved(2) != nullptr && world.findIncludingRemoved(3) != nullptr);
    world.remove(*world.findIncludingRemoved(2));
    CHECK(world.removedIds() == std::vector<std::int16_t>{2});
    world.purge();
    CHECK(world.findIncludingRemoved(2) == nullptr && world.findIncludingRemoved(3) != nullptr && world.size() == 2);
}

// A Sim saved inside an interaction its Idle for Input started (the Idle's frame saved with its
// waiting flag) runs that Idle again when the interaction ends, as the game does, whatever the
// interaction returned. Before, an interaction ending false took the Idle's false branch.
void savedIdleForInputRunsAgainAfterItsInteraction() {
    Instruction idle;
    idle.opcode = 0x0011;
    idle.trueTarget = T;
    idle.falseTarget = E;
    idle.operands[2] = 1;  // a waiting interaction may start
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({idle}));
    code.add(kObjectGroup, 0x1001, bhav({expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 1, F, E)}));
    code.addObjectType(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    ts2::content::Lot lot;
    ts2::objt::Entry entry;
    entry.guid = 0x51515151;
    entry.index = 1;
    lot.types.entries.push_back(entry);
    ts2::content::LotObject saved;
    saved.id = 1;
    saved.type = entry;
    saved.state.tail.emplace();
    ts2::xobj::StackFrame waiting;
    waiting.objectId = 1;
    waiting.treeId = 0x1000;
    waiting.params = {0, 0};
    waiting.value3 = 1;
    waiting.codeOwner = 1;
    ts2::xobj::StackFrame interaction = waiting;
    interaction.treeId = 0x1001;
    interaction.value3 = 0;
    saved.state.tail->stack = {waiting, interaction};
    lot.objects.push_back(saved);
    ts2::sim::World world;
    ts2::sim::buildWorld(lot, code, world);
    auto* sim = world.find(1);
    CHECK(sim != nullptr && sim->thread.frames.size() == 2);
    Vm vm(code, {}, &world);
    CHECK(sim != nullptr && vm.resume(*sim).outcome == Outcome::True);
}

void roomFieldsAreReadThroughTempZero() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 5, 1, E),
        expr(owner::kTemp, 1, op::kAssign, owner::kRoomByTemp, 3, 2, E),
        expr(owner::kTemp, 2, op::kAssign, owner::kRoomByTemp, 2, T, E),
    }));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kTemp, 3, op::kAssign, owner::kRoomByTemp, 4, T, E),
    }));
    code.add(kObjectGroup, 0x1002, bhav({
        expr(owner::kTemp, 3, op::kAssign, owner::kRoomByTemp, 5, T, E),
    }));
    code.add(kObjectGroup, 0x1003, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 0xFFFF, 1, E),
        expr(owner::kTemp, 1, op::kAssign, owner::kRoomByTemp, 2, 2, E),
        expr(owner::kTemp, 2, op::kAssign, owner::kRoomByTemp, 3, 3, E),
        expr(owner::kTemp, 3, op::kAssign, owner::kRoomByTemp, 4, T, E),
    }));
    ts2::sim::World world;
    world.setRooms(ts2::sim::RoomMap(squareRoom(false)));
    auto& me = world.create(kObjectGroup, definition("Me", 0));
    Vm vm(code, {}, &world);
    CHECK(vm.run(me, 0x1000).outcome == Outcome::True);
    CHECK(me.thread.temps[1] == 4);
    CHECK(me.thread.temps[2] == 1);
    // A room with no pool water: field 4 is 0.
    CHECK(vm.run(me, 0x1001).outcome == Outcome::True && me.thread.temps[3] == 0);
    const RunResult unknown = vm.run(me, 0x1002);
    CHECK(unknown.outcome == Outcome::Unsupported && unknown.reason == "room field not implemented");
    // Room -1, an object in no room, has level 0, area 0 and no pool, as the game keeps no tiles for it.
    CHECK(vm.run(me, 0x1003).outcome == Outcome::True);
    CHECK(me.thread.temps[1] == 0 && me.thread.temps[2] == 0 && me.thread.temps[3] == 0);
}

// "Outside" (field 1) is 1 when the room's region in layer 0 is 0 (not enclosed), else 0. Field 7
// asks the same of layer 1.
void outsideIsRegionZeroInLayerZero() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 1, op::kAssign, owner::kRoomByTemp, 1, T, E),
    }));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kTemp, 1, op::kAssign, owner::kRoomByTemp, 7, T, E),
    }));
    ts2::sim::RoomMap rooms(squareRoom(true));
    rooms.assignLayer(0, ts2::sim::RoomMap(squareRoom(false)));
    ts2::sim::World world;
    world.setRooms(std::move(rooms));
    auto& me = world.create(kObjectGroup, definition("Me", 0));
    Vm vm(code, {}, &world);
    for (const auto [room, outside] : {std::pair{5, 0}, std::pair{6, 0}, std::pair{0, 1}}) {
        me.thread.temps[0] = static_cast<std::int16_t>(room);
        CHECK(vm.run(me, 0x1000).outcome == Outcome::True);
        CHECK(me.thread.temps[1] == outside);
    }
    // Room -1, an object in no room, is not outside in either layer.
    me.thread.temps[0] = -1;
    CHECK(vm.run(me, 0x1000).outcome == Outcome::True && me.thread.temps[1] == 0);
    me.thread.temps[0] = -1;
    CHECK(vm.run(me, 0x1001).outcome == Outcome::True && me.thread.temps[1] == 0);
    me.thread.temps[0] = 5;
    const RunResult layer1 = vm.run(me, 0x1001);
    CHECK(layer1.outcome == Outcome::Unsupported && layer1.reason == "room has no region in that layer");
}

} // namespace

std::vector<Test> vmObjectTests() {
    return {
        {"room fields are read through temp 0", roomFieldsAreReadThroughTempZero},
        {"text notifications fill their tokens", textNotificationsFillTheirTokens},
        {"outside is region 0 in layer 0", outsideIsRegionZeroInLayerZero},
        {"Find Location For centres a Sim on its tile", findLocationForCentresASimOnItsTile},
        {"a Sim reset moves a seated Sim out of its slot", aSimResetMovesASeatedSimOutOfItsSlot},
        {"removed objects take what their slots hold", removedObjectsTakeWhatTheirSlotsHold},
        {"Find Location For starts in front of the reference", findLocationForStartsInFrontOfTheReference},
        {"Find Location For walks the reference's line", findLocationForWalksTheReferencesLine},
        {"Find Location For skips the Sim's own tile", findLocationForSkipsTheSimsTile},
        {"Find Location For on level ground clears a placement bit", findLocationForOnLevelGroundClearsAPlacementBit},
        {"Find Location For places by a global slot", findLocationForPlacesByAGlobalSlot},
        {"Find Location For takes the slot number from a param or local", findLocationForTakesTheSlotNumberFromAParamOrLocal},
        {"make new character adds a neighbor", makeNewCharacterAddsANeighbor},
        {"make new character makes babies of two parents", makeNewCharacterMakesBabiesOfTwoParents},
        {"add to family 0 joins the lot's household", addToFamilyZeroJoinsTheLotsHousehold},
        {"A neighbor's house number is its family's lot", neighborHouseNumberIsTheFamilysLot},
        {"neighbor object definition reads", neighborObjectDefinitionReads},
        {"Get Distance To measures in tiles", getDistanceToMeasuresInTiles},
        {"Test Object Type compares the original GUID", testObjectTypeComparesTheOriginalGuid},
        {"made Sims have their original person's slots", madeSimsHaveTheirOriginalPersonsSlots},
        {"made Sims are visitors unless household", madeSimsAreVisitorsUnlessHousehold},
        {"Change Outfit dresses from the working copy", changeOutfitDressesFromTheWorkingCopy},
        {"slot owners read the held object", slotOwnersReadTheHeldObject},
        {"verify neighbor checks the neighborhood", verifyNeighborChecksTheNeighborhood},
        {"array operations work on object arrays", arrayOperationsWorkOnObjectArrays},
        {"neighbor fields come from the neighbor", neighborFieldsComeFromTheNeighbor},
        {"Create Object can take its GUID from temps", createObjectCanTakeItsGuidFromTemps},
        {"Create Object makes a multi-tile object's parts", createObjectMakesAMultiTileObjectsParts},
        {"a moved multi-tile part takes the others with it", movedPartsFollow},
        {"Snap tests every part of a multi-tile object at its spot", snapTestsEveryPartAtItsSpot},
        {"the placement test keeps the lot's far edge on the lot", placementTestKeepsTheFarEdgeOnTheLot},
        {"removing a part removes the whole multi-tile object", removingAPartRemovesTheWholeObject},
        {"snapped cars take their passengers", snappedCarsTakeTheirPassengers},
        {"Create Object can go below the object in a local", createObjectCanGoBelowTheObjectInALocal},
        {"Create Object goes in my hand", createObjectGoesInMyHand},
        {"Create Object goes in another object's slot", createObjectGoesInAnotherObjectsSlot},
        {"Create Object goes underneath me", createObjectGoesUnderneathMe},
        {"removed objects leave their slot", removedObjectsLeaveTheirSlot},
        {"use count follows interaction frames", useCountFollowsInteractionFrames},
        {"Sims forget a deleted object", simsForgetADeletedObject},
        {"Snap Into puts the object in the slot", snapIntoPutsTheObjectInTheSlot},
        {"Reach/Put moves the object at the clip's event", reachPutMovesTheObjectAtTheClipsEvent},
        {"Reach/Put measures the height through containers", reachPutMeasuresTheHeightThroughContainers},
        {"Create Object starts its Main and passes temp 0", createObjectStartsItsMainAndPassesTemp0},
        {"created objects count their own steps", createdObjectsCountTheirOwnSteps},
        {"Create Object out of the world is in no room", createObjectOutOfWorldIsInNoRoom},
        {"Create Object passes my id and stack object", createObjectPassesMyIdAndStackObject},
        {"Create Object passes nothing to a Main without params", createObjectPassesNothingToMainWithoutParams},
        {"Create Object with unknown GUID returns false", createObjectWithUnknownGuidReturnsFalse},
        {"Create Object can make a neighbor's Sim", createObjectCanMakeANeighborsSim},
        {"Create Object refuses templates", createObjectRefusesTemplates},
        {"Create Object rejects two passing values", createObjectRejectsTwoPassingValues},
        {"Relationship writes and reads back", relationshipWritesAndReadsBack},
        {"Relationship read creates the variable unless asked", relationshipReadCreatesTheVariableUnlessAsked},
        {"Relationship with a missing object is an error", relationshipWithMissingObjectIsAnError},
        {"Relationship with flag 0x08 needs only A", relationshipWithFlag8NeedsOnlyA},
        {"Relationship with neighbor ids uses the neighborhood", relationshipWithNeighborIdsUsesTheNeighborhood},
        {"created Sims are their neighbor", createdSimsAreTheirNeighbor},
        {"Change Outfit asks the Sim", changeOutfitAsksTheSim},
        {"Age sets the life stage", ageSetsTheLifeStage},
        {"Relationship between Sims uses their neighbors", relationshipBetweenSimsUsesTheirNeighbors},
        {"Change Light sets how strongly an object's lights shine", changeLightSetsLightStrength},
        {"Change Material names a material for a mesh group", changeMaterialNamesAMaterialForAMeshGroup},
        {"Play/Stop Sound Event names the sound and its source", soundEventNamesTheSoundAndItsSource},
        {"held objects come along with their holder", heldObjectsComeAlongWithTheirHolder},
        {"Effect Stop/Start asks for effects", effectStopStartAsksForEffects},
        {"Cinematic checks its actors and starts none", cinematicChecksItsActorsAndStartsNone},
        {"RayTrace rules judge the hits", rayTraceRulesJudgeTheHits},
        {"RayTrace asks the scene and writes temp 0", rayTraceAsksTheSceneAndWritesTemp0},
        {"dialogs answer as the game does headless", dialogsAnswerAsTheGameDoesHeadless},
        {"set baby name waits for the names", setBabyNameWaitsForTheNames},
        {"phone invite waits for the pick", phoneInviteWaitsForThePick},
        {"object type picker waits for the pick", typePickerWaitsForThePick},
        {"message box waits for a button", messageBoxWaitsForAButton},
        {"baby genes follow the game's rules", babyGenesFollowTheGameRules},
        {"baby genes draw in the game's order", babyGenesDrawInTheGamesOrder},
        {"Camera Control waits while the camera moves", cameraControlWaitsWhileTheCameraMoves},
        {"Alter Budget charges the funds", alterBudgetChargesTheFunds},
        {"new objects lead themselves", newObjectsLeadThemselves},
        {"Remove Object refuses protected objects", removeObjectRefusesProtectedObjects},
        {"removed objects leave their slot at once", removedObjectsLeaveTheirSlotAtOnce},
        {"Removed objects keep their data until purge", removedObjectsKeepTheirDataUntilPurge},
        {"Hidden Flags hide the model", hiddenFlagsHideTheModel},
        {"graphic writes change the model", graphicWritesChangeTheModel},
        {"loaded Placeholders and TemplatePersons are kept until unload", loadedPlaceholdersAreKeptUntilUnload},
        {"a saved Idle for Input runs again after its interaction", savedIdleForInputRunsAgainAfterItsInteraction},
        {"removing me ends the run unless return immediately", removingMeEndsTheRunUnlessReturnImmediately},
        {"A SimObject keeps what only Sims use on the heap", aSimObjectKeepsWhatOnlySimsUseOnTheHeap},
        {"Test Object Type compares GUIDs", testObjectTypeComparesGuids},
        {"Test Object Type matches a part's master", testObjectTypeMatchesAPartsMaster},
        {"Test Object Type on missing object is false", testObjectTypeOnMissingObjectIsFalse},
        {"Test Object Type can ask whether a GUID exists", testObjectTypeCanAskWhetherAGuidExists},
    };
}
