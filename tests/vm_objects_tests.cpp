// Primitives that find, make, compare and remove objects, neighbors and relationships.
#include "sim/forget_object.hpp"
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
    // Mode 1 leaves the stack object where it is, but out of the slot it was in.
    Instruction stay = call(0x0010, T, F);
    stay.operands = {1, 0, 0x02};
    code.add(kObjectGroup, 0x1002, bhav({stay}));
    sim.thread.stackObjectId = 2;
    auto& held = *w.world.find(2);
    sim.slots.assign(2, 0);
    sim.slots[0] = held.id;
    held.data[2] = sim.id;
    held.data[3] = 0;
    const auto x = held.x;
    CHECK(vm.run(sim, 0x1002).outcome == Outcome::True && held.x == x);
    CHECK(sim.slots[0] == 0 && held.data[2] == 0 && held.data[3] == -1);
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
    CHECK(make1(4, 2, 1).outcome == Outcome::True && w.controller->arrays[0] == std::vector<std::int16_t>{10});
    made = w.world.neighbor(10);
    CHECK(made != nullptr && made->personData[0x3A] == 16 && made->personData[0x41] == 0 && made->personData[0xBA] == 1);
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

void reachPutMovesTheObjectAtTheClipsEvent() {
    using namespace ts2::sim;
    const auto reach = [](std::uint8_t mode) {
        Instruction in = primitiveCall(0x0074, T, F);
        in.operands[3] = owner::kStackObjectId;
        in.operands[10] = mode;
        return in;
    };
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({reach(0)}));
    code.add(kObjectGroup, 0x1001, bhav({reach(2)}));
    std::vector<std::string> clips(16);
    clips[0] = "a2o-floor-get";
    clips[8] = "a2o-floor-put";
    code.addStrings(0x7FD46CD0, 0x92, clips);
    code.addStrings(kLampGroup, 0x81, {"a2o-cup-grasp,a2o-cup-carry"});
    code.addObjectType(kLampGroup, typed(kLampGuid));
    World world;
    world.setAnimationLengths([](const std::string& name) -> std::optional<float> {
        if (name == "a2o-cup-grasp" || name == "a2o-cup-carry") {
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
    Vm vm(code, {}, &world);

    // The cup comes into my hand at the clip's event 0. The primitive is true when the clip ends.
    const double start = world.clockMs();
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::Sleeping && sim.animation && sim.animation->name == "a2o-floor-get");
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
    world.advanceClock(600);
    CHECK(vm.resume(sim).outcome == Outcome::True);
    // Grabbing it again is true at once.
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    // Put in the middle of the tile 0.7 tiles in front of me, facing back at me (direction 6), turned
    // one step on by facing rule 4.
    cup.data[0x17] = 4;
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::Sleeping && sim.animation->name == "a2o-floor-put");
    // The hand aims at that spot.
    const auto& aim = sim.animation->ikObject;
    CHECK(aim && aim->x == 5.5f && aim->y == 5.5f && aim->heading == compassHeading(7));
    // A put plays the grasp again and fades the carry out.
    const auto named = [&](const char* name, bool stopped) {
        return std::ranges::any_of(sim.overlays, [&](const PlayingOverlay& o) { return o.name == name && o.stopped == stopped; });
    };
    CHECK(named("a2o-cup-carry", true) && !named("a2o-cup-carry", false));
    CHECK(sim.overlays.back().name == "a2o-cup-grasp" && sim.overlays.back().startMs == world.clockMs());
    world.advanceClock(1000);
    CHECK(vm.resume(sim).outcome == Outcome::True && sim.slots[0] == 0 && cup.data[2] == 0);
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
    World world;
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
}

// Placements 1 "on top of me" and 2 "in my hand" both put the new object in my slot 0, at my place.
// Off the lot it is false. A slot 0 that is missing or full needs the game's fit test.
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
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::Unsupported);
    sim.slots[0] = 0;
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::True && sim.slots[0] == sim.thread.stackObjectId);
    sim.slots[0] = 0;
    sim.x = ts2::sim::kOffLot;
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::False && sim.slots[0] == 0);
    sim.slots.clear();
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::Unsupported);
}

// Placements 4 and 0xA put the new object in slot byte 9 of the stack object or of the object in
// temp 0, at its place. No such object is false. A missing or full slot needs the game's fit test.
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
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::Unsupported);

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

// With flag 0x02, A and B are neighbor ids and the variables are A's saved list towards B.
// Change Outfit asks a Sim (owner byte 9 / data 10-11) to wear a category's outfit: byte 8, or with
// flag 0x04 a value. The source GUID comes from the stack object (0x01), the operands (0x02) or
// temps 0-1 (0x40). Flag 0x20 clears person data 0x77-0x78. A missing object or one that isn't a Sim is an error.
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
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({changeOutfit(0x10, 8, owner::kStackObjectId, 0)}));
    code.add(kObjectGroup, 0x1001, bhav({changeOutfit(0x02 | 0x04 | 0x20, 0, owner::kStackObjectId, 0)}));
    code.add(kObjectGroup, 0x1002, bhav({changeOutfit(0x40, 1, owner::kStackObjectId, 0)}));
    code.add(kObjectGroup, 0x1003, bhav({changeOutfit(0, 1, owner::kLiteral, 99)}));
    code.add(kObjectGroup, 0x1004, bhav({changeOutfit(0, 1, owner::kMyData, 0x0B)}));
    ts2::sim::World world;
    auto& sim = world.create(kSimGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& dresser = world.create(kObjectGroup, typed(kChairGuid));
    sim.personData[0x77] = 5;
    sim.personData[0x78] = 6;
    dresser.thread.stackObjectId = sim.id;
    Vm vm(code, {}, &world);
    CHECK(vm.run(dresser, 0x1000).outcome == Outcome::True);
    CHECK(sim.outfitChange && sim.outfitChange->category == 8 && !sim.outfitChange->sourceGuid);
    dresser.thread.temps[2] = 4;
    CHECK(vm.run(dresser, 0x1001).outcome == Outcome::True);
    CHECK(sim.outfitChange->category == 4 && sim.outfitChange->sourceGuid == 0x6C22B41Bu);
    CHECK(sim.personData[0x77] == 0 && sim.personData[0x78] == 0);
    dresser.thread.temps[0] = static_cast<std::int16_t>(0xB41B);
    dresser.thread.temps[1] = 0x6C22;
    CHECK(vm.run(dresser, 0x1002).outcome == Outcome::True && sim.outfitChange->sourceGuid == 0x6C22B41Bu);
    CHECK(vm.run(dresser, 0x1003).outcome == Outcome::Error);
    CHECK(vm.run(dresser, 0x1004).outcome == Outcome::Error);  // the dresser itself is no Sim
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
        expr(owner::kTemp, 3, op::kAssign, owner::kRoomByTemp, 6, T, E),
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
        {"outside is region 0 in layer 0", outsideIsRegionZeroInLayerZero},
        {"Find Location For centres a Sim on its tile", findLocationForCentresASimOnItsTile},
        {"Find Location For starts in front of the reference", findLocationForStartsInFrontOfTheReference},
        {"Find Location For skips the Sim's own tile", findLocationForSkipsTheSimsTile},
        {"Find Location For places by a global slot", findLocationForPlacesByAGlobalSlot},
        {"make new character adds a neighbor", makeNewCharacterAddsANeighbor},
        {"A neighbor's house number is its family's lot", neighborHouseNumberIsTheFamilysLot},
        {"neighbor object definition reads", neighborObjectDefinitionReads},
        {"Get Distance To measures in tiles", getDistanceToMeasuresInTiles},
        {"Test Object Type compares the original GUID", testObjectTypeComparesTheOriginalGuid},
        {"slot owners read the held object", slotOwnersReadTheHeldObject},
        {"verify neighbor checks the neighborhood", verifyNeighborChecksTheNeighborhood},
        {"array operations work on object arrays", arrayOperationsWorkOnObjectArrays},
        {"neighbor fields come from the neighbor", neighborFieldsComeFromTheNeighbor},
        {"Create Object can take its GUID from temps", createObjectCanTakeItsGuidFromTemps},
        {"Create Object can go below the object in a local", createObjectCanGoBelowTheObjectInALocal},
        {"Create Object goes in my hand", createObjectGoesInMyHand},
        {"Create Object goes in another object's slot", createObjectGoesInAnotherObjectsSlot},
        {"removed objects leave their slot", removedObjectsLeaveTheirSlot},
        {"use count follows interaction frames", useCountFollowsInteractionFrames},
        {"Sims forget a deleted object", simsForgetADeletedObject},
        {"Snap Into puts the object in the slot", snapIntoPutsTheObjectInTheSlot},
        {"Reach/Put moves the object at the clip's event", reachPutMovesTheObjectAtTheClipsEvent},
        {"Create Object starts its Main and passes temp 0", createObjectStartsItsMainAndPassesTemp0},
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
        {"Relationship between Sims uses their neighbors", relationshipBetweenSimsUsesTheirNeighbors},
        {"Change Light sets how strongly an object's lights shine", changeLightSetsLightStrength},
        {"Change Material names a material for a mesh group", changeMaterialNamesAMaterialForAMeshGroup},
        {"Play/Stop Sound Event names the sound and its source", soundEventNamesTheSoundAndItsSource},
        {"RayTrace rules judge the hits", rayTraceRulesJudgeTheHits},
        {"RayTrace asks the scene and writes temp 0", rayTraceAsksTheSceneAndWritesTemp0},
        {"dialogs answer as the game does headless", dialogsAnswerAsTheGameDoesHeadless},
        {"Camera Control waits while the camera moves", cameraControlWaitsWhileTheCameraMoves},
        {"Alter Budget charges the funds", alterBudgetChargesTheFunds},
        {"new objects lead themselves", newObjectsLeadThemselves},
        {"Remove Object refuses protected objects", removeObjectRefusesProtectedObjects},
        {"Removed objects keep their data until purge", removedObjectsKeepTheirDataUntilPurge},
        {"removing me ends the run unless return immediately", removingMeEndsTheRunUnlessReturnImmediately},
        {"Test Object Type compares GUIDs", testObjectTypeComparesGuids},
        {"Test Object Type on missing object is false", testObjectTypeOnMissingObjectIsFalse},
        {"Test Object Type can ask whether a GUID exists", testObjectTypeCanAskWhetherAGuidExists},
    };
}
