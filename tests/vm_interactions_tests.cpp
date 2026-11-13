// Interactions: queues, pie menus, autonomy's picks, wants, motives and running trees.
#include "test_support.hpp"

#include "content/animation_lists.hpp"
#include "sim/cheats.hpp"
#include "sim/find_best_interaction.hpp"
#include "sim/game_sort.hpp"
#include "sorted_choices_data.hpp"
#include "sorted_wants_data.hpp"

#include <bit>

namespace {

// Generic call 0x3C with temp 0 = 0 gives a Sim its own skin: nothing changes and it is true.
void setSkinToTheSimsOwnIsTrue() {
    MemoryCodeSource code;
    Instruction setSkin = call(0x0001, T, F);
    setSkin.operands[0] = 0x3C;
    code.add(kObjectGroup, 0x1000, bhav({setSkin}));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(*w.world.find(5), 0x1000).outcome == Outcome::True);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::False);
    w.world.find(5)->thread.temps[0] = 7;
    CHECK(vm.run(*w.world.find(5), 0x1000).outcome == Outcome::Unsupported);
}

// Generic call 0x30 maps the neighbor id in temp 0 through the lot's remap table into temp 1, -1
// when the lot has none or the id isn't in it.
void remappedNeighborIdComesFromTheRemapTable() {
    MemoryCodeSource code;
    Instruction remap = call(0x0001, T, F);
    remap.operands[0] = 0x30;
    code.add(kObjectGroup, 0x1000, bhav({remap}));
    SmallWorld w;
    auto& temps = w.controller->thread.temps;
    Vm vm(code, {}, &w.world);
    auto remapped = [&](std::int16_t id) {
        temps[0] = id;
        temps[1] = 5;
        CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
        return temps[1];
    };
    CHECK(remapped(12) == -1);
    w.world.setNeighborRemap(ts2::sim::World::NeighborRemap{{12, 0x151}, {0xFFFFFFFF, 3}, {7, 0x12345}}, false);
    CHECK(remapped(12) == 0x151);
    CHECK(remapped(-1) == 3);
    CHECK(remapped(7) == 0x2345);
    CHECK(remapped(13) == -1);
    w.world.setNeighborRemap(std::nullopt, true);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::Unsupported);
}

// Generic call 0x20: temp 1 = 1 when the object in temp 3 has a whole outfit in the category in
// temp 0 (a number turned into the category's flag), else 0. A missing object has none.
void outfitsInformationAsksTheSimsLooks() {
    MemoryCodeSource code;
    Instruction ask = call(0x0001, T, F);
    ask.operands[0] = 0x20;
    code.add(kObjectGroup, 0x1000, bhav({ask}));
    SmallWorld w;
    auto& temps = w.controller->thread.temps;
    Vm vm(code, {}, &w.world);
    temps[3] = 3;
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::Unsupported);
    std::vector<std::uint32_t> asked;
    w.world.setHasOutfit([&](const ts2::sim::SimObject& object, std::uint32_t category) -> std::optional<bool> {
        asked.push_back(category);
        return object.id == 3 && category == 8;
    });
    auto has = [&](std::int16_t who, std::int16_t number) {
        temps[0] = number;
        temps[1] = 5;
        temps[3] = who;
        CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
        return temps[1];
    };
    CHECK(has(3, 2) == 1);  // formal
    CHECK(has(3, 0) == 0 && has(3, 7) == 0 && has(3, -4) == 0 && has(3, 13) == 0);
    CHECK(has(999, 2) == 0);
    CHECK((asked == std::vector<std::uint32_t>{8, 1, 1, 1, 0x1000}));
    temps[3] = 0;
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::Unsupported);
}

// Generic calls 0x0A and 0x0B list and unlist the running object as keeping Build and Buy mode off.
// Each 0x0A adds an entry, and deleting the object drops them all.
void buildBuyCallsListTheRunningObject() {
    MemoryCodeSource code;
    Instruction disable = call(0x0001, T, F);
    disable.operands[0] = 0x0A;
    Instruction enable = call(0x0001, T, F);
    enable.operands[0] = 0x0B;
    code.add(kObjectGroup, 0x1000, bhav({disable}));
    code.add(kObjectGroup, 0x1001, bhav({enable}));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    auto* fridge = w.world.find(2);
    CHECK(vm.run(*w.controller, 0x1001).outcome == Outcome::True);
    CHECK(!w.world.buildBuyBlocked());
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(vm.run(*fridge, 0x1000).outcome == Outcome::True);
    CHECK(vm.run(*w.controller, 0x1001).outcome == Outcome::True);
    CHECK(vm.run(*w.controller, 0x1001).outcome == Outcome::True);
    CHECK(w.world.buildBuyBlocked());
    CHECK(vm.run(*fridge, 0x1000).outcome == Outcome::True);
    w.world.remove(*fridge);
    CHECK(w.world.buildBuyBlocked());
    w.world.purge();
    CHECK(!w.world.buildBuyBlocked());
}

// Generic call 0x0D makes every other Sim forget the stack object at once: queued interactions on it
// go, and a Sim whose current interaction object it is gets reset (on its next turn, if it is the
// one running the call).
void abortInteractionsTellsEverySim() {
    MemoryCodeSource code;
    Instruction abort = call(0x0001, T, F);
    abort.operands[0] = 0x0D;
    code.add(kObjectGroup, 0x1000, bhav({abort}));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    auto& queuer = *w.world.find(5);
    auto& user = *w.world.find(7);
    ts2::sim::QueuedInteraction q;
    q.object = 2;
    q.group = kObjectGroup;
    q.action = 0x1000;
    queuer.queue.push_back(q);
    q.object = 4;
    queuer.queue.push_back(q);
    queuer.data[0x5A] = 2;
    user.personData[0x50] = 2;
    std::vector<std::int16_t> reset;
    CHECK(vm.runEvent(*w.controller, kObjectGroup, 0x1000, 2, {}).outcome == Outcome::Unsupported);
    vm.resetSim = [&](ts2::sim::SimObject& sim) { reset.push_back(sim.id); };
    q.object = 2;
    queuer.queue.push_back(q);
    queuer.data[0x5A] = 2;
    CHECK(vm.runEvent(*w.controller, kObjectGroup, 0x1000, 2, {}).outcome == Outcome::True);
    CHECK(queuer.queue.size() == 1 && queuer.queue[0].object == 4 && queuer.data[0x5A] == 0);
    CHECK((reset == std::vector<std::int16_t>{7}));
    // A Sim that would need a reset while its own code runs finishes the run, and is marked for a
    // reset on its next turn instead of being reset under it.
    reset.clear();
    CHECK(!user.resetPending);
    CHECK(vm.runEvent(user, kObjectGroup, 0x1000, 2, {}).outcome == Outcome::True);
    CHECK(reset.empty());
    CHECK(user.resetPending);
    CHECK(vm.runEvent(*w.controller, kObjectGroup, 0x1000, 99, {}).outcome == Outcome::Error);
}

// Generic call 0x31 tests the stack object where it stands with footprint index temp 0, then puts
// its own footprint index back.
void fitsWithFootprintTestsTheOtherFootprint() {
    MemoryCodeSource code;
    Instruction test = call(0x0001, T, F);
    test.operands[0] = 0x31;
    code.add(kObjectGroup, 0x1000, bhav({test}));
    SmallWorld w;
    auto* chair = w.world.find(3);
    chair->x = 5 * 65536 + 32768;
    chair->y = 7 * 65536 + 32768;
    chair->level = 1;
    chair->data[9] = 2;
    std::vector<std::int16_t> tested;
    w.world.setObjectFits([&](const ts2::sim::SimObject& placed, float x, float y, std::int32_t level, std::int16_t) {
        tested.push_back(placed.data[9]);
        return x == 5.5f && y == 7.5f && level == 1 && placed.data[9] == 3;
    });
    w.controller->thread.stackObjectId = chair->id;
    Vm vm(code, {}, &w.world);
    w.controller->thread.temps[0] = 2;
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(tested.empty());
    w.controller->thread.temps[0] = 3;
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    w.controller->thread.temps[0] = 4;
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::False);
    CHECK((tested == std::vector<std::int16_t>{3, 4}));
    CHECK(chair->data[9] == 2);
}

// Generic call 0x27 is true for a Sim (temp 0 below 10) or a known neighbor in temp 1. Debug is true.
void recalcWantsAndDebugCarryOn() {
    MemoryCodeSource code;
    Instruction recalc = call(0x0001, T, F);
    recalc.operands[0] = 0x27;
    code.add(kObjectGroup, 0x1000, bhav({recalc}));
    code.add(kObjectGroup, 0x1001, bhav({call(0x0073, T, F)}));
    SmallWorld w;
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {3};
    w.world.setNeighborhood(hood);
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(*w.world.find(5), 0x1000).outcome == Outcome::True);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::Error);
    w.controller->thread.temps[0] = 14;
    w.controller->thread.temps[1] = 3;
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    w.controller->thread.temps[1] = 4;
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::Error);
    CHECK(vm.run(*w.controller, 0x1001).outcome == Outcome::True);
}

// Generic call 0x17 ends the family tie between the neighbors in temps 0 and 1: each loses its first
// tie to the other, of any type, and both must be neighbors.
void removeFamilyTieEndsTheTieBothWays() {
    using ts2::famt::TieType;
    MemoryCodeSource code;
    Instruction remove = call(0x0001, T, F);
    remove.operands[0] = 0x17;
    code.add(kObjectGroup, 0x1000, bhav({remove}));
    SmallWorld w;
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {3, 4, 6};
    hood->familyTies[3] = {{2, 4}, {4, 6}, {2, 4}};  // spouse 4 twice, child 6
    w.world.setNeighborhood(hood);
    Vm vm(code, {}, &w.world);
    w.controller->thread.temps[0] = 3;
    w.controller->thread.temps[1] = 4;
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(w.world.familyTies(3, TieType::Spouse) == std::vector<std::int16_t>{4});  // only the first goes
    CHECK(w.world.familyTies(3, TieType::Child) == std::vector<std::int16_t>{6});
    CHECK(w.world.hasFamilyTies(4) && hood->familyTies.at(3).size() == 3);  // the saved record is untouched
    w.controller->thread.temps[1] = 5;
    CHECK(vm.run(*w.controller, 0x1000).reason == "neighbor not found");
}

// Generic call 0x26 unlinks the neighbor in temp 0 when its Sim has a character file of its own:
// its Sims on the lot go, it loses its relationships both ways and its family, and person data 0xA5
// marks it. It is always true.
void unlinkCharacterDeletesItsSims() {
    MemoryCodeSource code;
    Instruction unlink = call(0x0001, T, F);
    unlink.operands[0] = 0x26;
    code.add(kObjectGroup, 0x1000, bhav({unlink}));
    SmallWorld w;  // Sims 5 and 7 are of type 0x51515151
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {3, 4, 6};
    hood->neighbors[3].guid = 0x51515151;
    hood->neighbors[3].personData.assign(ts2::sim::kNeighborPersonDataCount, 0);
    hood->neighbors[3].personData[0x3D] = 9;
    hood->neighbors[4].guid = 0x42;
    hood->neighbors[6].guid = 0x43;
    const std::int16_t unlinked = 3, other = 4, third = 6;
    hood->relationships[std::pair{unlinked, other}] = {1};
    hood->relationships[std::pair{other, unlinked}] = {2};
    hood->relationships[std::pair{other, third}] = {3};
    w.world.setNeighborhood(hood);
    w.world.neighborRelationship(6, 3) = {4};
    w.world.setHasCharacterFile([](std::int16_t id) { return id != 4; });
    Vm vm(code, {}, &w.world);
    w.controller->thread.temps[0] = 4;
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(w.world.neighborRelationshipSize(4, 3) == 1 && !w.world.unlinkedNeighbor(4));
    w.controller->thread.temps[0] = 3;
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(w.world.find(5) == nullptr && w.world.find(7) == nullptr && w.world.find(3) != nullptr);
    CHECK(w.world.neighborRelationshipSize(3, 4) == 0 && w.world.neighborRelationshipSize(4, 3) == 0);
    CHECK(w.world.neighborRelationshipSize(6, 3) == 0 && w.world.neighborRelationshipSize(4, 6) == 1);
    CHECK(w.world.neighborPersonValue(3, 0xA5) == 1 && w.world.neighborPersonValue(3, 0x3D) == 0);
    CHECK(w.world.unlinkedNeighbor(3) && hood->relationships.size() == 3);  // the saved record is untouched
    w.controller->thread.temps[0] = 8;
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
}

// Generic call 0x15 makes the neighbor in temp 0 the child of the neighbor in temp 1: the first
// parent given is the father, who also makes his other children its siblings, the second the
// mother, and a third is refused.
void addChildTiesTheBabyToItsParents() {
    using ts2::famt::TieType;
    MemoryCodeSource code;
    Instruction add = call(0x0001, T, F);
    add.operands[0] = 0x15;
    code.add(kObjectGroup, 0x1000, bhav({add}));
    SmallWorld w;
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {3, 4, 5, 6, 9};
    hood->familyTies[3] = {{4, 6}};  // 3 already has child 6
    hood->familyTies[6] = {{0, 3}};
    w.world.setNeighborhood(hood);
    Vm vm(code, {}, &w.world);
    auto addChild = [&](std::int16_t child, std::int16_t parent) {
        w.controller->thread.temps[0] = child;
        w.controller->thread.temps[1] = parent;
        return vm.run(*w.controller, 0x1000).outcome;
    };
    CHECK(addChild(9, 3) == Outcome::True && addChild(9, 4) == Outcome::True && addChild(9, 5) == Outcome::True);
    CHECK(w.world.familyTies(9, TieType::Father) == std::vector<std::int16_t>{3});
    CHECK(w.world.familyTies(9, TieType::Mother) == std::vector<std::int16_t>{4});
    CHECK(w.world.familyTies(9, TieType::Sibling) == std::vector<std::int16_t>{6});
    CHECK(w.world.familyTies(6, TieType::Sibling) == std::vector<std::int16_t>{9});
    CHECK((w.world.familyTies(3, TieType::Child) == std::vector<std::int16_t>{6, 9}));
    CHECK(w.world.familyTies(4, TieType::Child) == std::vector<std::int16_t>{9});
    CHECK(w.world.familyTies(5, TieType::Child).empty());
    CHECK(addChild(9, 9) == Outcome::True && w.world.familyTies(9, TieType::Father) == std::vector<std::int16_t>{3});
    CHECK(addChild(9, 7) == Outcome::Error);
}

void multiTilePartsUseTheirMastersTable() {
    constexpr std::uint32_t kMaster = 0x0A0A0A0A, kPart = 0x0B0B0B0B, kBroken = 0x0C0C0C0C;
    MemoryCodeSource code;
    auto master = typed(kMaster);
    master.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    auto part = typed(kPart);
    part.fields[ts2::objd::field::kInteractionTableId] = 0xFFFF;
    auto broken = typed(kBroken);
    broken.fields[ts2::objd::field::kInteractionTableId] = 0x9000;  // top bit set: no table
    code.addObjectType(kObjectGroup, master);
    code.addObjectType(kObjectGroup, part);
    code.addObjectType(kObjectGroup, broken);
    code.setMultiTileMaster(kPart, kMaster);
    code.setMultiTileMaster(kBroken, kMaster);
    ts2::ttab::InteractionTable table;
    table.interactions.resize(3);
    code.addInteractionTable(kObjectGroup, 0x80, table);
    ts2::sim::World world;
    const auto found = ts2::sim::interactionsOf(code, world.create(kObjectGroup, part));
    CHECK(found.table && found.table->interactions.size() == 3 && found.group == kObjectGroup);
    CHECK(!ts2::sim::interactionsOf(code, world.create(kObjectGroup, broken)).table);
}

// Find Best Interaction scores each advertising line a Sim may use by how much happier it would make
// it, divided by distance. The best that passes its check is the pick: the stack object, with my
// object data 0x14 its id. It runs once a tick.
void findBestInteractionPicksTheHappiestNearLine() {
    constexpr std::uint32_t kSnack = 0x0E0F0E0F;
    MemoryCodeSource code;
    // Steep curves, so one advertised point is worth a lot of happiness.
    code.addStrings(ts2::sim::kGlobalGroup, 0x28B, std::vector<std::string>(16, "(-100;0) (100;2000)"));
    ts2::fcns::Tunings tunings;
    tunings.values.push_back({"random selection count", 1.0f});  // always the best
    code.setTunings(tunings);
    auto snackType = typed(kSnack);
    snackType.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    code.addObjectType(kObjectGroup, snackType);
    ts2::ttab::InteractionTable table;
    auto line = [](std::uint32_t id, std::int16_t hunger) {
        ts2::ttab::Interaction l;
        l.stringIndex = id;
        l.action = static_cast<std::uint16_t>(0x1000 + id);
        l.flags = 0x40;  // adults
        l.autonomy = 50;
        l.attenuationCode = 4;  // high: 0.1 a tile
        l.motives.assign(1, std::vector<ts2::ttab::MotiveEffect>(16));
        l.motives[0][7] = {0, hunger, 0};
        return l;
    };
    table.interactions.push_back(line(5, 1000));
    table.interactions.back().flags |= 0x400000;  // its queue icon is an object type of its own
    table.interactions.back().iconGuid = 0xABCD1234;
    table.interactions.back().iconIndex = 2;
    table.interactions.push_back(line(6, 5000));
    table.interactions.back().flags |= 0x80;  // a debug line: never autonomous
    table.interactions.push_back(line(7, 3000));
    table.interactions.back().test = 0x1003;  // the best, but its check fails
    table.interactions.push_back(line(8, 5000));
    table.interactions.back().autonomy = 200;  // above the Sim's autonomy level
    code.addInteractionTable(kObjectGroup, 0x80, table);
    code.add(kObjectGroup, 0x1003, bhav({expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 0, F, F)}));
    code.add(kObjectGroup, 0x1100, bhav({primitiveCall(0x0003, T, F)}));
    // Line 5's action tree: notes its stack object in temp 0 and my current interaction in temps 1-2.
    code.add(kObjectGroup, 0x1005, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kStackObjectId, 0, 1, F),
        expr(owner::kTemp, 1, op::kAssign, owner::kMyPersonData, 0x50, 2, F),
        expr(owner::kTemp, 2, op::kAssign, owner::kMyPersonData, 0x15, T, F),
    }));
    code.add(kObjectGroup, 0x1101, bhav({primitiveCall(0x001E, T, F)}));

    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    sim.personData[0x3A] = 27;   // adult
    sim.personData[0x24] = 100;  // autonomy level
    sim.motives.fill(50);
    sim.motives[7] = -50;
    sim.x = sim.y = 10 << 16;
    auto& far = world.create(kObjectGroup, snackType);
    far.x = 20 << 16;
    far.y = 10 << 16;
    auto& near = world.create(kObjectGroup, snackType);
    near.x = 11 << 16;
    near.y = 10 << 16;
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1100).outcome == Outcome::True);
    CHECK(sim.thread.stackObjectId == near.id && sim.data[0x14] == 5 && sim.personData[0x35] == near.id);
    // Gosub Found Action runs the pick's action tree on the Sim's stack, with the object as stack
    // object, and its queue icon is the one its line names, as a pie menu choice's would be.
    sim.thread.stackObjectId = near.id;
    CHECK(vm.run(sim, 0x1101).outcome == Outcome::True && sim.thread.temps[0] == near.id);
    CHECK(sim.actionIconGuid == 0xABCD1234 && sim.actionIconIndex == 2 && sim.actionIconObject == near.id);
    CHECK(sim.thread.temps[1] == near.id && sim.thread.temps[2] == 5 && sim.personData[0x50] == 0);
    sim.thread.stackObjectId = 0;
    CHECK(vm.run(sim, 0x1101).outcome == Outcome::Error);
    CHECK(vm.run(sim, 0x1100).outcome == Outcome::False && sim.data[0x14] == -1);  // once a tick
    world.countTick();
    sim.motives[7] = 100;  // nothing left to gain
    CHECK(vm.run(sim, 0x1100).outcome == Outcome::False && sim.data[0x14] == -1);
    // With Free Will off (global 0x1E) a selectable Sim of the household picks nothing, but one with
    // bit 0x800 of person data 0x54 or that isn't selectable still does.
    sim.motives[7] = -50;
    vm.globals[ts2::sim::global::kFreeWill] = 0;
    sim.personData[0x9E] = 1;
    world.countTick();
    CHECK(vm.run(sim, 0x1100).outcome == Outcome::False && sim.data[0x14] == -1);
    sim.personData[0x54] = 0x800;
    world.countTick();
    CHECK(vm.run(sim, 0x1100).outcome == Outcome::True && sim.data[0x14] == 5);
    sim.personData[0x54] = 0;
    sim.personData[0x9E] = 0;
    world.countTick();
    CHECK(vm.run(sim, 0x1100).outcome == Outcome::True);
    sim.personData[0x9E] = 1;
    vm.globals[ts2::sim::global::kFreeWill] = 1;
    world.countTick();
    CHECK(vm.run(sim, 0x1100).outcome == Outcome::True);
}

// A pet searches with its own kind: lines with its kind's flag, its own advertisements and its
// curves (small dogs 0x293). It likes lines on a Sim more when its relationship flags (value 1)
// towards that Sim have bit 0x800: 1 + global constant 0x15B entry 11 / 100.
void findBestInteractionWorksForPets() {
    constexpr std::uint32_t kBowl = 0x0E0F0E0F, kOwner = 0x0D0D0D0D;
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x293, std::vector<std::string>(16, "(-100;0) (100;2000)"));
    code.addConstants(ts2::sim::kGlobalGroup, 0x15B, std::vector<std::int16_t>(12, 100));
    ts2::fcns::Tunings tunings;
    tunings.values.push_back({"random selection count", 1.0f});
    code.setTunings(tunings);
    auto line = [](std::uint32_t id, std::uint32_t flags, std::size_t motive, std::int16_t value) {
        ts2::ttab::Interaction l;
        l.stringIndex = id;
        l.action = static_cast<std::uint16_t>(0x1000 + id);
        l.flags = flags;
        l.autonomy = 50;
        l.attenuationCode = 4;
        l.motives.assign(1, std::vector<ts2::ttab::MotiveEffect>(16, {0, 5000, 0}));  // humans only
        l.moreMotives.assign(3, std::vector<std::vector<ts2::ttab::MotiveEffect>>(16));
        l.moreMotives[2][motive].assign(1, ts2::ttab::MotiveEffect{0, value, 0});  // no personality: the range
        return l;
    };
    auto bowlType = typed(kBowl);
    bowlType.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    code.addObjectType(kObjectGroup, bowlType);
    ts2::ttab::InteractionTable bowlTable;
    bowlTable.interactions.push_back(line(5, 0x40, 7, 9000));         // adult humans' flag
    bowlTable.interactions.push_back(line(6, 0x10000000, 13, 9000));  // small dogs, motive 13: nothing
    bowlTable.interactions.push_back(line(7, 0x10000000, 7, 1000));   // small dogs, hunger
    code.addInteractionTable(kObjectGroup, 0x80, bowlTable);
    auto ownerType = typed(kOwner, ts2::sim::kPersonType);
    ownerType.fields[ts2::objd::field::kInteractionTableId] = 0x81;
    code.addObjectType(kObjectGroup, ownerType);
    ts2::ttab::InteractionTable ownerTable;
    ownerTable.interactions.push_back(line(8, 0x10000000, 7, 600));
    code.addInteractionTable(kObjectGroup, 0x81, ownerTable);
    code.add(kObjectGroup, 0x1100, bhav({primitiveCall(0x0003, T, F)}));

    ts2::sim::World world;
    auto& dog = world.create(kObjectGroup, typed(0x17DDADE2, ts2::sim::kPersonType));
    dog.personData[0xBA] = 2;  // small dog
    dog.personData[0x3A] = 27;
    dog.personData[0x24] = 100;
    dog.personData[0x1F] = 1;
    dog.motives.fill(50);
    dog.motives[7] = -50;
    dog.x = dog.y = 10 << 16;
    auto& bowl = world.create(kObjectGroup, bowlType);
    bowl.x = 11 << 16;
    bowl.y = 10 << 16;
    auto& owner = world.create(kObjectGroup, ownerType);
    owner.personData[0x3A] = 27;
    owner.personData[0x1F] = 2;
    owner.x = 10 << 16;
    owner.y = 11 << 16;
    Vm vm(code, {}, &world);
    CHECK(vm.run(dog, 0x1100).outcome == Outcome::True && dog.data[0x14] == 7 && dog.thread.stackObjectId == bowl.id);
    world.countTick();
    world.neighborRelationship(1, 2) = {0, 0x800};
    CHECK(vm.run(dog, 0x1100).outcome == Outcome::True && dog.data[0x14] == 8 && dog.thread.stackObjectId == owner.id);
}

// A visitor (person data 0x20 not 0) is refused every line without flag 0x01 when the line is
// checked, as a resident is not, so it picks the best line visitors may use.
void findBestInteractionKeepsVisitorsToTheirLines() {
    constexpr std::uint32_t kSnack = 0x0E0F0E0F;
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x28B, std::vector<std::string>(16, "(-100;0) (100;2000)"));
    ts2::fcns::Tunings tunings;
    tunings.values.push_back({"random selection count", 1.0f});
    code.setTunings(tunings);
    auto snackType = typed(kSnack);
    snackType.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    code.addObjectType(kObjectGroup, snackType);
    ts2::ttab::InteractionTable table;
    auto line = [](std::uint32_t id, std::int16_t hunger, std::uint32_t flags) {
        ts2::ttab::Interaction l;
        l.stringIndex = id;
        l.action = static_cast<std::uint16_t>(0x1000 + id);
        l.flags = flags;
        l.autonomy = 50;
        l.attenuationCode = 4;
        l.motives.assign(1, std::vector<ts2::ttab::MotiveEffect>(16));
        l.motives[0][7] = {0, hunger, 0};
        return l;
    };
    table.interactions.push_back(line(5, 5000, 0x40));         // adults, residents only
    table.interactions.push_back(line(6, 1000, 0x40 | 0x01));  // adults, visitors too
    code.addInteractionTable(kObjectGroup, 0x80, table);
    code.add(kObjectGroup, 0x1100, bhav({primitiveCall(0x0003, T, F)}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    sim.personData[0x3A] = 27;
    sim.personData[0x24] = 100;
    sim.motives.fill(50);
    sim.motives[7] = -50;
    sim.x = sim.y = 10 << 16;
    auto& snack = world.create(kObjectGroup, snackType);
    snack.x = 11 << 16;
    snack.y = 10 << 16;
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1100).outcome == Outcome::True && sim.data[0x14] == 5);
    world.countTick();
    sim.personData[0x20] = 1;
    CHECK(vm.run(sim, 0x1100).outcome == Outcome::True && sim.data[0x14] == 6);
}

// A pick autonomy remembers scores times its line's memory multiplier on the same object, so the
// Sim tries the farther snack next. Once the memory's time is up the nearer one wins again.
void findBestInteractionRemembersItsPicks() {
    constexpr std::uint32_t kSnack = 0x0E0F0E0F;
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x28B, std::vector<std::string>(16, "(-100;0) (100;2000)"));
    ts2::fcns::Tunings tunings;
    tunings.values.push_back({"random selection count", 1.0f});
    tunings.values.push_back({"max duration autonomy memory ", 10.0f});
    code.setTunings(tunings);
    auto snackType = typed(kSnack);
    snackType.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    code.addObjectType(kObjectGroup, snackType);
    ts2::ttab::InteractionTable table;
    ts2::ttab::Interaction line;
    line.stringIndex = 5;
    line.action = 0x1005;
    line.flags = 0x40;  // adults
    line.autonomy = 50;
    line.attenuationCode = 4;  // high: 0.1 a tile
    line.motives.assign(1, std::vector<ts2::ttab::MotiveEffect>(16));
    line.motives[0][7] = {0, 1000, 0};
    table.interactions.push_back(line);
    code.addInteractionTable(kObjectGroup, 0x80, table);
    code.add(kObjectGroup, 0x1100, bhav({primitiveCall(0x0003, T, F)}));

    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    sim.personData[0x3A] = 27;   // adult
    sim.personData[0x24] = 100;  // autonomy level
    sim.motives.fill(50);
    sim.motives[7] = -50;
    sim.x = sim.y = 10 << 16;
    auto& near = world.create(kObjectGroup, snackType);
    near.x = 11 << 16;
    near.y = 10 << 16;
    auto& far = world.create(kObjectGroup, snackType);
    far.x = 13 << 16;
    far.y = 10 << 16;
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1100).outcome == Outcome::True && sim.personData[0x35] == near.id);
    world.countTick();
    CHECK(vm.run(sim, 0x1100).outcome == Outcome::True && sim.personData[0x35] == far.id);
    for (int i = 0; i < 10; ++i) {
        world.countTick();
    }
    CHECK(vm.run(sim, 0x1100).outcome == Outcome::True && sim.personData[0x35] == near.id);
}

// NeoTS2's own: after an interaction stopped on an engine gap, autonomy leaves that line on that
// object alone for kEngineGapAvoidTicks, then picks it again. The same line elsewhere still counts.
void findBestInteractionAvoidsEngineGaps() {
    constexpr std::uint32_t kSnack = 0x0E0F0E0F;
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x28B, std::vector<std::string>(16, "(-100;0) (100;2000)"));
    ts2::fcns::Tunings tunings;
    tunings.values.push_back({"random selection count", 1.0f});
    tunings.values.push_back({"max duration autonomy memory ", 0.0f});  // no memory multiplier
    code.setTunings(tunings);
    auto snackType = typed(kSnack);
    snackType.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    code.addObjectType(kObjectGroup, snackType);
    ts2::ttab::InteractionTable table;
    ts2::ttab::Interaction line;
    line.stringIndex = 5;
    line.action = 0x1005;
    line.flags = 0x40;  // adults
    line.autonomy = 50;
    line.attenuationCode = 4;
    line.motives.assign(1, std::vector<ts2::ttab::MotiveEffect>(16));
    line.motives[0][7] = {0, 1000, 0};
    table.interactions.push_back(line);
    code.addInteractionTable(kObjectGroup, 0x80, table);
    code.add(kObjectGroup, 0x1100, bhav({primitiveCall(0x0003, T, F)}));

    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    sim.personData[0x3A] = 27;   // adult
    sim.personData[0x24] = 100;  // autonomy level
    sim.motives.fill(50);
    sim.motives[7] = -50;
    sim.x = sim.y = 10 << 16;
    auto& near = world.create(kObjectGroup, snackType);
    near.x = 11 << 16;
    near.y = 10 << 16;
    auto& far = world.create(kObjectGroup, snackType);
    far.x = 13 << 16;
    far.y = 10 << 16;
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1100).outcome == Outcome::True && sim.personData[0x35] == near.id);

    // Nothing running: nothing to avoid.
    CHECK(!ts2::sim::avoidEngineGap(sim, world, world.ticks()));
    ts2::sim::QueuedInteraction running;
    running.object = near.id;
    running.index = 5;
    sim.running = running;
    CHECK(ts2::sim::avoidEngineGap(sim, world, world.ticks()));
    CHECK(ts2::sim::avoidEngineGap(sim, world, world.ticks()) && sim.engineGapPicks.size() == 1);  // renewed, not added
    sim.running = std::nullopt;
    world.countTick();
    CHECK(vm.run(sim, 0x1100).outcome == Outcome::True && sim.personData[0x35] == far.id);
    for (std::uint64_t i = 0; i < ts2::sim::kEngineGapAvoidTicks; ++i) {
        world.countTick();
    }
    CHECK(vm.run(sim, 0x1100).outcome == Outcome::True && sim.personData[0x35] == near.id);
    CHECK(sim.engineGapPicks.empty());
    // Objects don't keep a list.
    near.running = running;
    CHECK(!ts2::sim::avoidEngineGap(near, world, world.ticks()));
}

// The social bias: a Sim keeps away from the places near a Sim it is furious at and is drawn to
// those near a Sim it is attracted to, by the tokens in its neighbor inventory.
void findBestInteractionWeighsTheSocialBias() {
    constexpr std::uint32_t kSnack = 0x0E0F0E0F;
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x28B, std::vector<std::string>(16, "(-100;0) (100;2000)"));
    ts2::fcns::Tunings tunings;
    tunings.values.push_back({"random selection count", 1.0f});
    tunings.values.push_back({"max duration autonomy memory ", 0.0f});  // no memory
    tunings.values.push_back({"min autonomy score for family", 0.001f});
    code.setTunings(tunings);
    auto snackType = typed(kSnack);
    snackType.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    code.addObjectType(kObjectGroup, snackType);
    ts2::ttab::InteractionTable table;
    ts2::ttab::Interaction line;
    line.stringIndex = 5;
    line.action = 0x1005;
    line.flags = 0x40;  // adults
    line.autonomy = 50;
    line.attenuationCode = 4;  // high: 0.1 a tile
    line.motives.assign(1, std::vector<ts2::ttab::MotiveEffect>(16));
    line.motives[0][7] = {0, 1000, 0};
    table.interactions.push_back(line);
    code.addInteractionTable(kObjectGroup, 0x80, table);
    code.add(kObjectGroup, 0x1100, bhav({primitiveCall(0x0003, T, F)}));

    ts2::sim::World world;
    auto person = [&](std::int16_t neighbor, int x) -> ts2::sim::SimObject& {
        auto& s = world.create(kObjectGroup, typed(0x51515100u + static_cast<std::uint32_t>(neighbor), ts2::sim::kPersonType));
        s.personData[0x3A] = 27;  // adult
        s.personData[0x1F] = neighbor;
        s.x = x << 16;
        s.y = 10 << 16;
        return s;
    };
    auto& sim = person(3, 10);
    sim.personData[0x24] = 100;  // autonomy level
    sim.motives.fill(50);
    sim.motives[7] = -50;
    auto& near = world.create(kObjectGroup, snackType);
    near.x = 11 << 16;
    near.y = 10 << 16;
    auto& far = world.create(kObjectGroup, snackType);
    far.x = 13 << 16;
    far.y = 10 << 16;
    person(7, 11);  // by the near snack
    person(9, 13);  // by the far one
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1100).outcome == Outcome::True && sim.personData[0x35] == near.id);

    auto& tokens = world.inventory(ts2::sim::InventoryKind::Neighbor, 3)->singular;
    tokens.push_back(std::make_shared<ts2::sim::InventoryToken>(ts2::sim::InventoryToken{0xCF505AB0, 0, 0, 0, 0, {7, 0, 100}}));
    world.countTick();
    CHECK(vm.run(sim, 0x1100).outcome == Outcome::True && sim.personData[0x35] == far.id);

    tokens.clear();
    tokens.push_back(std::make_shared<ts2::sim::InventoryToken>(ts2::sim::InventoryToken{0x2F4D6811, 0, 0, 0, 0, {100, 9}}));
    world.countTick();
    CHECK(vm.run(sim, 0x1100).outcome == Outcome::True && sim.personData[0x35] == far.id);
}

// A check tree autonomy runs before scoring offers lines with Add/Change the Action String. Each
// is weighed as a candidate of its own with the params it was offered with, even when the check
// then fails, and Gosub Found Action runs the line's tree with those params and that name.
// A line whose check offers lines is not a candidate itself, whether its check passes or not: only
// what it offered is, with its own name and params.
void weighOfferedLines(bool checkPasses) {
    constexpr std::uint32_t kToy = 0x0E0F0E11;
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x28B, std::vector<std::string>(16, "(-100;0) (100;2000)"));
    ts2::fcns::Tunings tunings;
    tunings.values.push_back({"random selection count", 1.0f});
    code.setTunings(tunings);
    auto toyType = typed(kToy);
    toyType.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    code.addObjectType(kObjectGroup, toyType);
    code.addStrings(kObjectGroup, 0x12E, {"Play Tag"});
    ts2::ttab::InteractionTable table;
    ts2::ttab::Interaction line;
    line.stringIndex = 5;
    line.action = 0x1005;
    line.test = 0x1003;
    line.flags = 0x40;  // adults
    line.autonomy = 50;
    line.attenuationCode = 4;
    line.motives.assign(1, std::vector<ts2::ttab::MotiveEffect>(16));
    line.motives[0][7] = {0, 1000, 0};
    table.interactions.push_back(line);
    code.addInteractionTable(kObjectGroup, 0x80, table);
    Instruction offer;
    offer.opcode = 0x0032;
    offer.trueTarget = 2;
    offer.falseTarget = E;
    offer.operands[14] = 1;  // "Play Tag"
    code.add(kObjectGroup, 0x1003, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 42, 1, E),
        offer,
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 0, checkPasses ? T : F, checkPasses ? T : F),
    }));
    code.add(kObjectGroup, 0x1005, bhav({
        expr(owner::kTemp, 1, op::kAssign, owner::kParam, 1, 1, F),
        expr(owner::kTemp, 2, op::kAssign, owner::kParam, 0, T, F),
    }));
    code.add(kObjectGroup, 0x1100, bhav({primitiveCall(0x0003, T, F)}));
    code.add(kObjectGroup, 0x1101, bhav({primitiveCall(0x001E, T, F)}));

    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    sim.personData[0x3A] = 27;
    sim.personData[0x24] = 100;
    sim.motives.fill(50);
    sim.motives[7] = -50;
    sim.x = sim.y = 10 << 16;
    auto& toy = world.create(kObjectGroup, toyType);
    toy.x = 11 << 16;
    toy.y = 10 << 16;
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1100).outcome == Outcome::True && sim.thread.stackObjectId == toy.id && sim.data[0x14] == 5);
    CHECK(sim.foundName == "Play Tag" && sim.foundParams[0] == toy.id && sim.foundParams[1] == 42);
    CHECK(vm.run(sim, 0x1101).outcome == Outcome::True && sim.thread.temps[1] == 42 && sim.thread.temps[2] == toy.id);
    CHECK(sim.actionString == "Play Tag");
}

void findBestInteractionWeighsOfferedLines() {
    weighOfferedLines(false);
    weighOfferedLines(true);
}

// A check tree that writes its line's advertisement runs before the line is scored, and the line
// is scored as the tree left it.
void findBestInteractionScoresWhatCheckTreesAdvertise() {
    constexpr std::uint32_t kToy = 0x0E0F0E10;
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x28B, std::vector<std::string>(16, "(-100;0) (100;2000)"));
    ts2::fcns::Tunings tunings;
    tunings.values.push_back({"random selection count", 1.0f});
    code.setTunings(tunings);
    auto toyType = typed(kToy);
    toyType.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    code.addObjectType(kObjectGroup, toyType);
    ts2::ttab::InteractionTable table;
    for (std::uint32_t id : {1u, 2u}) {
        ts2::ttab::Interaction l;
        l.stringIndex = id;
        l.action = static_cast<std::uint16_t>(0x1000 + id);
        l.flags = 0x40;
        l.autonomy = 50;
        l.attenuationCode = 1;
        l.motives.assign(1, std::vector<ts2::ttab::MotiveEffect>(16));
        l.motives[0][15] = {0, static_cast<std::int16_t>(id == 1 ? 1000 : 1), 0};
        table.interactions.push_back(l);
    }
    table.interactions[1].test = 0x1010;  // raises line 2's fun advertisement to 5000
    code.addInteractionTable(kObjectGroup, 0x80, table);
    code.add(kObjectGroup, 0x1010, bhav({expr(ts2::sim::owner::kCheckTreeAdRange, 15, op::kAssign, owner::kLiteral, 5000, T, E)}));
    code.add(kObjectGroup, 0x1100, bhav({primitiveCall(0x0003, T, F)}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    sim.personData[0x3A] = 27;
    sim.personData[0x24] = 100;
    sim.motives.fill(-50);
    sim.x = sim.y = 5 << 16;
    world.create(kObjectGroup, toyType).x = 6 << 16;
    sim.thread.temps[0] = 77;
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1100).outcome == Outcome::True && sim.data[0x14] == 2);
    CHECK(sim.thread.temps[0] == 77);  // the Sim keeps its own temps
}

// Check trees that change the line they test run before it is scored: assigning to its
// advertisement, Lua, the action string, functional and named trees, also in a tree they call.
void checkTreeKindsFindTreesThatChangeTheLine() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({expr(0x0C, 2, op::kGreater, owner::kLiteral, 0, T, F)}));  // only reads
    code.add(kObjectGroup, 0x1001, bhav({expr(0x1C, 2, op::kAssign, owner::kLiteral, 5, T, F)}));
    code.add(kObjectGroup, 0x1002, bhav({primitiveCall(0x0032, T, F)}));
    code.add(kObjectGroup, 0x1003, bhav({call(0x1002, T, F)}));
    code.add(kObjectGroup, 0x1004, bhav({call(0x1004, T, F), call(0x1000, T, F)}));  // calls itself
    ts2::sim::CheckTreeKinds kinds(code);
    CHECK(!kinds.changesLine(kObjectGroup, 0x1000));
    CHECK(kinds.changesLine(kObjectGroup, 0x1001));
    CHECK(kinds.changesLine(kObjectGroup, 0x1002));
    CHECK(kinds.changesLine(kObjectGroup, 0x1003));
    CHECK(!kinds.changesLine(kObjectGroup, 0x1004));
    CHECK(!kinds.changesLine(kObjectGroup, 0x1FFF));  // missing
}

void advertisersListObjectsByMotive() {
    constexpr std::uint32_t kFridge = 0x0D0D0D0D, kDebugOnly = 0x0E0E0E0E, kHidden = 0x0F0F0F0F;
    MemoryCodeSource code;
    auto fridge = typed(kFridge);
    fridge.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    auto debugOnly = typed(kDebugOnly);
    debugOnly.fields[ts2::objd::field::kInteractionTableId] = 0x81;
    auto hidden = typed(kHidden);
    hidden.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    hidden.fields[8] = 1;  // field 8 above 0: never advertises
    code.addObjectType(kObjectGroup, fridge);
    code.addObjectType(kObjectGroup, debugOnly);
    code.addObjectType(kObjectGroup, hidden);

    ts2::ttab::InteractionTable table;
    table.interactions.resize(3);
    for (auto& line : table.interactions) {
        line.autonomy = 50;
        line.motives.assign(2, std::vector<ts2::ttab::MotiveEffect>(16));
    }
    table.interactions[0].motives[0][7] = {0, 20, 0};    // adults: hunger
    table.interactions[2].motives[0][7] = {-5, 0, 3};    // a negative advertisement counts too
    table.interactions[2].motives[1][15] = {0, 0, 4};    // a type alone doesn't count for humans
    table.interactions[1].motives[0][11] = {10, 10, 0};  // motive 11 is never advertised
    code.addInteractionTable(kObjectGroup, 0x80, table);
    auto debugTable = table;
    for (auto& line : debugTable.interactions) {
        line.flags = 0x80;
    }
    code.addInteractionTable(kObjectGroup, 0x81, debugTable);

    ts2::sim::World world;
    const auto& f = world.create(kObjectGroup, fridge);
    world.create(kObjectGroup, debugOnly);
    world.create(kObjectGroup, hidden);
    const auto ads = ts2::sim::Advertisers::build(world, code);
    const auto& hunger = ads.advertising(0, 7);
    CHECK(hunger.size() == 1 && hunger[0].object == f.id);
    CHECK(hunger.size() == 1 && hunger[0].lines == std::vector<std::uint16_t>({0, 2}));
    CHECK(ads.advertising(1, 15).empty());
    CHECK(ads.advertising(0, 11).empty());
    CHECK(ads.counts(0)[7] == 1 && ads.counts(1)[7] == 0);

    // The three lowest motives with advertisers, lowest first. Ties go to the lower motive.
    std::array<float, ts2::sim::kMotiveCount> motives{};
    motives.fill(50);
    motives[7] = -40;
    motives[3] = -90;  // Mood is never chosen
    motives[8] = motives[9] = -10;
    motives[15] = 0;
    std::array<std::size_t, ts2::sim::kMotiveCount> counts{};
    counts.fill(1);
    CHECK(ts2::sim::worstMotives(motives, true, counts, 0) == ((1 << 7) | (1 << 8) | (1 << 9)));
    CHECK(ts2::sim::worstMotives(motives, true, counts, 1 << 8) == ((1 << 7) | (1 << 9) | (1 << 15)));
    counts[7] = 0;
    CHECK(ts2::sim::worstMotives(motives, true, counts, 0) == ((1 << 8) | (1 << 9) | (1 << 15)));
}

// Notify the Stack Object out of Idle zeroes the waiting frame's param 0: a Sleep counted there ends,
// one counted in another param goes on.
void notifyOutOfIdleEndsASleepCountedInParamZero() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({sleepFor(0, T)}, 2));
    code.add(kObjectGroup, 0x1001, bhav({sleepFor(1, T)}, 2));
    code.add(kObjectGroup, 0x1002, bhav({primitiveCall(0x0031, T, E)}));
    ts2::sim::World world;
    auto& sleeper = world.create(kObjectGroup, typed(kFridgeGuid));
    auto& other = world.create(kObjectGroup, typed(kFridgeGuid));
    auto& notifier = world.create(kObjectGroup, typed(kFridgeGuid));
    Vm vm(code, {}, &world);
    const std::int16_t args[] = {50, 50};
    CHECK(vm.run(sleeper, 0x1000, args).outcome == Outcome::Sleeping);
    CHECK(vm.run(other, 0x1001, args).outcome == Outcome::Sleeping);
    notifier.thread.stackObjectId = sleeper.id;
    CHECK(vm.run(notifier, 0x1002).outcome == Outcome::True);
    CHECK(sleeper.thread.sleepTicks == 0 && sleeper.thread.frames.back().params[0] == 0);
    notifier.thread.stackObjectId = other.id;
    CHECK(vm.run(notifier, 0x1002).outcome == Outcome::True);
    CHECK(other.thread.sleepTicks == 50 && other.thread.frames.back().params[0] == 0);
}

void interactionRunsObjectCodeInTheSimsThread() {
    MemoryCodeSource code;
    code.addConstants(kObjectGroup, 0x1002, {4});
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kStackObjectAttribute, 0, op::kSubtract, owner::kConstant, (0x02 << 7) | 0, 1, E),
        expr(owner::kMyPersonData, 0x0A, op::kAdd, owner::kLiteral, 1, T, E),
    }));
    code.add(kObjectGroup, 0x1000, bhav({call(0x1001, T, E)}));

    ts2::sim::World world;
    auto& sim = world.create(kSimGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& fridge = world.create(kObjectGroup, typed(kFridgeGuid));
    fridge.attributes[0] = 20;
    sim.thread.stackObjectId = fridge.id;

    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::True);
    CHECK(fridge.attributes[0] == 16);
    CHECK(sim.personData[0x0A] == 1);
    CHECK(fridge.thread.frames.empty());
}

void presentationOnlyGenericCallsContinue() {
    MemoryCodeSource code;
    Instruction centerView = primitiveCall(0x0001, 1, F);
    centerView.operands[0] = 0x01;
    Instruction preloadClothing = primitiveCall(0x0001, T, F);
    preloadClothing.operands[0] = 0x1E;
    code.add(kObjectGroup, 0x1000, bhav({centerView, preloadClothing}));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
}

void otherGenericCallsReportTheirId() {
    MemoryCodeSource code;
    Instruction unimplemented = primitiveCall(0x0001, T, F);
    unimplemented.operands[0] = 0x05;
    code.add(kObjectGroup, 0x1000, bhav({unimplemented}));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(r.outcome == Outcome::Unsupported);
    CHECK(r.detail == "0x05");
}

Behavior named(const char* name, std::vector<Instruction> code, std::uint8_t args = 0,
               std::uint8_t locals = 0) {
    Behavior b = bhav(std::move(code), args, locals);
    b.name = name;
    return b;
}

Instruction runTreeByName(std::uint8_t nameIndex, std::uint8_t flags, std::uint8_t how,
                          std::uint16_t t, std::uint16_t f) {
    Instruction in = primitiveCall(0x001C, t, f);
    in.operands[2] = flags;
    in.operands[4] = static_cast<std::uint8_t>(nameIndex + 1);
    in.operands[5] = how;
    return in;
}

struct NamedTreeSetup {
    MemoryCodeSource code;
    ts2::sim::World world;
    ts2::sim::SimObject* sim;
    ts2::sim::SimObject* fridge;
    NamedTreeSetup() {
        code.addStrings(kSimGroup, 0x12F, {"Restock", "Missing Tree"});
        code.addConstants(kObjectGroup, 0x1002, {10});
        code.addConstants(kSimGroup, 0x1002, {1000});
        code.add(kObjectGroup, 0x1005, named("Restock", {
            expr(owner::kStackObjectAttribute, 0, op::kAdd, owner::kParam, 0, 1, E),
            expr(owner::kStackObjectAttribute, 0, op::kAdd, owner::kConstant, (0x02 << 7) | 0, T, E),
        }, 1));
        sim = &world.create(kSimGroup, typed(0x51515151, ts2::sim::kPersonType));
        fridge = &world.create(kObjectGroup, typed(kFridgeGuid));
        sim->thread.stackObjectId = fridge->id;
    }
};

void runTreeByNameRunsTheStackObjectsTree() {
    NamedTreeSetup s;
    Instruction call = runTreeByName(0, 0x10, 0x00, 1, F);
    call.operands[6] = owner::kLiteral;
    call.operands[7] = 3;
    s.code.add(kSimGroup, 0x1000, named("Caller", {
        call,
        expr(owner::kTemp, 0, op::kAssign, owner::kConstant, (0x02 << 7) | 0, T, E),
    }));
    Vm vm(s.code, {}, &s.world);
    CHECK(vm.run(*s.sim, 0x1000).outcome == Outcome::True);
    CHECK(s.fridge->attributes[0] == 13);
    CHECK(s.sim->thread.temps[0] == 1000);
}

void runTreeByNameTakesFalseWhenTheTreeIsMissing() {
    NamedTreeSetup s;
    s.code.add(kSimGroup, 0x1000, named("Caller", {runTreeByName(1, 0x00, 0x00, T, F)}));
    Vm vm(s.code, {}, &s.world);
    CHECK(vm.run(*s.sim, 0x1000).outcome == Outcome::False);
}

void runTreeByNameCanPassTheCallersParams() {
    NamedTreeSetup s;
    s.code.add(kSimGroup, 0x1000, named("Caller", {runTreeByName(0, 0x20, 0x00, T, F)}, 1));
    Vm vm(s.code, {}, &s.world);
    const std::int16_t args[] = {5};
    CHECK(vm.run(*s.sim, 0x1000, args).outcome == Outcome::True);
    CHECK(s.fridge->attributes[0] == 15);
}

// Without either argument flag the called tree gets my temps as its params.
void runTreeByNamePassesTheTempsWithoutFlags() {
    NamedTreeSetup s;
    s.code.add(kSimGroup, 0x1000, named("Caller", {
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 7, 1, E),
        runTreeByName(0, 0x00, 0x02, T, F),
    }));
    Vm vm(s.code, {}, &s.world);
    CHECK(vm.run(*s.sim, 0x1000).outcome == Outcome::True);
    CHECK(s.fridge->attributes[0] == 17);
}

// A tree with more than four params gets as many temps as it takes (a jealousy trigger's setup
// reads its second Sim from param 5).
void runTreeByNamePassesEveryTempTheTreeTakes() {
    NamedTreeSetup s;
    s.code.addStrings(kSimGroup, 0x12F, {"Restock", "Missing Tree", "Setup"});
    s.code.add(kObjectGroup, 0x1007, named("Setup", {
        expr(owner::kStackObjectAttribute, 1, op::kAssign, owner::kParam, 5, 1, E),
        expr(owner::kStackObjectAttribute, 2, op::kAssign, owner::kParam, 6, T, E),
    }, 7));
    s.code.add(kSimGroup, 0x1000, named("Caller", {
        expr(owner::kTemp, 5, op::kAssign, owner::kLiteral, 386, 1, E),
        expr(owner::kTemp, 6, op::kAssign, owner::kLiteral, 9, 2, E),
        runTreeByName(2, 0x00, 0x00, T, F),
    }));
    Vm vm(s.code, {}, &s.world);
    CHECK(vm.run(*s.sim, 0x1000).outcome == Outcome::True);
    CHECK(s.fridge->attributes[1] == 386 && s.fridge->attributes[2] == 9);
}

// Flag 0x02 reads the name from the code owner's semi-global table, not its own.
void runTreeByNameReadsTheSemiGlobalNames() {
    NamedTreeSetup s;
    s.code.addStrings(kObjectGroup, 0x12F, {"Missing Tree"});
    s.code.addStrings(kSemiGroup, 0x12F, {"Missing Tree", "Restock"});
    Instruction call = runTreeByName(1, 0x12, 0x00, T, F);
    call.operands[6] = owner::kLiteral;
    call.operands[7] = 2;
    s.code.add(kObjectGroup, 0x1000, named("Caller", {call}));
    s.fridge->thread.stackObjectId = s.fridge->id;
    Vm vm(s.code, {}, &s.world);
    CHECK(vm.run(*s.fridge, 0x1000).outcome == Outcome::True);
    CHECK(s.fridge->attributes[0] == 12);
}

// The tree is looked up in the stack object's own, semi-global and global trees, in that order.
// Flag 0x08 leaves out the semi-global trees and flag 0x04 the global ones.
void runTreeByNameSkipsTheGroupsItsFlagsName() {
    NamedTreeSetup s;
    s.code.addStrings(kSimGroup, 0x12F, {"Restock", "Missing Tree", "Shared", "Global Only"});
    s.code.add(kSemiGroup, 0x2001, named("Shared", {
        expr(owner::kStackObjectAttribute, 0, op::kAssign, owner::kLiteral, 1, T, E)}));
    s.code.add(ts2::sim::kGlobalGroup, 0x0101, named("Shared", {
        expr(owner::kStackObjectAttribute, 0, op::kAssign, owner::kLiteral, 2, T, E)}));
    s.code.add(ts2::sim::kGlobalGroup, 0x0102, named("Global Only", {
        expr(owner::kStackObjectAttribute, 0, op::kAssign, owner::kLiteral, 3, T, E)}));
    s.code.add(kSimGroup, 0x1000, named("Both", {runTreeByName(2, 0x00, 0x00, T, F)}));
    s.code.add(kSimGroup, 0x1001, named("No Semi-Global", {runTreeByName(2, 0x08, 0x00, T, F)}));
    s.code.add(kSimGroup, 0x1002, named("Neither", {runTreeByName(2, 0x0C, 0x00, T, F)}));
    s.code.add(kSimGroup, 0x1003, named("No Global", {runTreeByName(3, 0x04, 0x00, T, F)}));
    Vm vm(s.code, {}, &s.world);
    CHECK(vm.run(*s.sim, 0x1000).outcome == Outcome::True && s.fridge->attributes[0] == 1);
    CHECK(vm.run(*s.sim, 0x1001).outcome == Outcome::True && s.fridge->attributes[0] == 2);
    s.fridge->attributes[0] = 0;
    CHECK(vm.run(*s.sim, 0x1002).outcome == Outcome::False && s.fridge->attributes[0] == 0);
    CHECK(vm.run(*s.sim, 0x1003).outcome == Outcome::False && s.fridge->attributes[0] == 0);
}

// Mode 1 runs the stack object's tree at once as the stack object, its params from the stack object's
// temps. A tree that waits leaves the stack object to be reset and is false.
void runTreeByNameRunsOnTheStackObjectsStack() {
    NamedTreeSetup s;
    s.code.addStrings(kSimGroup, 0x12F, {"Restock", "Nap"});
    s.code.add(kObjectGroup, 0x1006, named("Nap", {sleepFor(0, T)}, 1));
    s.code.add(kSimGroup, 0x1000, named("Caller", {
        runTreeByName(0, 0x00, 0x01, 1, F),
        expr(owner::kTemp, 1, op::kAssign, owner::kLiteral, 5, T, E),
    }));
    s.code.add(kSimGroup, 0x1001, named("Napper", {runTreeByName(1, 0x00, 0x01, T, F)}));
    s.sim->thread.temps[0] = 100;
    s.fridge->thread.temps[0] = 4;
    Vm vm(s.code, {}, &s.world);
    CHECK(vm.run(*s.sim, 0x1000).outcome == Outcome::True);
    CHECK(s.fridge->attributes[0] == 14 && s.sim->thread.temps[1] == 5);
    CHECK(vm.run(*s.sim, 0x1001).outcome == Outcome::False);
    CHECK(s.fridge->failedRun.has_value());
}

// Run Tree by Name modes 0 and 1 count their steps from 0, and the caller's count goes on from
// where it was: two runs of about 500 steps each fit under a limit of 1,000.
void runTreeByNameAtOnceCountsItsOwnSteps() {
    for (const std::uint8_t mode : {std::uint8_t{0x00}, std::uint8_t{0x01}}) {
        NamedTreeSetup s;
        s.code.addStrings(kSimGroup, 0x12F, {"Spend"});
        s.code.addStrings(kObjectGroup, 0x12F, {"Spend"});
        s.code.add(kObjectGroup, 0x1007, named("Spend", {
            expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 0, 1, E),
            expr(owner::kTemp, 0, op::kIncrementAndLess, owner::kLiteral, 500, 1, T),
        }));
        s.code.add(kSimGroup, 0x1000, named("Caller", {runTreeByName(0, 0x00, mode, 1, F), runTreeByName(0, 0x00, mode, T, F)}));
        Vm vm(s.code, {1000, 64}, &s.world);
        CHECK(vm.run(*s.sim, 0x1000).outcome == Outcome::True);
    }
}

// Mode 0 runs the tree at once as me, on my temps. A tree that waits leaves me to be reset and is
// false, and the caller goes on.
void runTreeByNameInMyStackRunsAtOnce() {
    NamedTreeSetup s;
    s.code.addStrings(kSimGroup, 0x12F, {"Restock", "Nap"});
    s.code.add(kObjectGroup, 0x1006, named("Nap", {sleepFor(0, T)}, 1));
    s.code.add(kSimGroup, 0x1000, named("Napper", {
        runTreeByName(1, 0x00, 0x00, T, 1),
        expr(owner::kTemp, 1, op::kAssign, owner::kLiteral, 5, F, E),
    }));
    s.sim->thread.temps[0] = 4;
    Vm vm(s.code, {}, &s.world);
    CHECK(vm.run(*s.sim, 0x1000).outcome == Outcome::False);
    CHECK(s.sim->thread.temps[1] == 5 && s.sim->failedRun.has_value());
    CHECK(!s.fridge->failedRun.has_value());
    // Until I am reset, another run at once as me runs nothing and is false.
    s.code.add(kSimGroup, 0x1001, named("Restocker", {runTreeByName(0, 0x00, 0x00, T, F)}));
    s.sim->thread = {};
    s.sim->thread.stackObjectId = s.fridge->id;
    CHECK(vm.run(*s.sim, 0x1001).outcome == Outcome::False && s.fridge->attributes[0] == 0);
}

void runTreeByNameOnTheStackObjectRunsTheTreeItFound() {
    NamedTreeSetup s;
    s.code.addStrings(kSimGroup, 0x12F, {"Odd"});
    s.code.add(kObjectGroup, 0x0800, named("Odd", {expr(owner::kStackObjectAttribute, 0, op::kAssign, owner::kLiteral, 7, T, E)}));
    s.code.add(kSimGroup, 0x1000, named("Caller", {runTreeByName(0, 0x00, 0x01, T, F)}));
    Vm vm(s.code, {}, &s.world);
    CHECK(vm.run(*s.sim, 0x1000).outcome == Outcome::True);
    CHECK(s.fridge->attributes[0] == 7);
}

void runTreeByNameOtherModesReportThemselves() {
    NamedTreeSetup s;
    s.code.add(kSimGroup, 0x1000, named("Caller", {runTreeByName(0, 0x00, 0x04, T, F)}));
    Vm vm(s.code, {}, &s.world);
    const RunResult r = vm.run(*s.sim, 0x1000);
    CHECK(r.outcome == Outcome::Unsupported);
    CHECK(r.reason == "Run Tree by Name mode 0x04 not implemented");
}

// Mode 2 pushes the stack object's tree onto my stack. Mode 3 runs a tree of the
// object type whose GUID is in temps 0 and 1, with the stack object as its stack object.
void runTreeByNamePushesOntoMyStack() {
    NamedTreeSetup s;
    s.code.addObjectType(kObjectGroup, typed(kFridgeGuid));
    Instruction push = runTreeByName(0, 0x10, 0x02, 1, F);
    push.operands[6] = owner::kLiteral;
    push.operands[7] = 4;
    s.code.add(kSimGroup, 0x1000, named("Caller", {push, expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 1, T, E)}));
    s.code.add(kSimGroup, 0x1001, named("Typed caller", {
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, static_cast<std::uint16_t>(kFridgeGuid & 0xFFFF), 1, E),
        expr(owner::kTemp, 1, op::kAssign, owner::kLiteral, static_cast<std::uint16_t>(kFridgeGuid >> 16), 2, E),
        [] {
            Instruction in = runTreeByName(0, 0x10, 0x03, T, F);
            in.operands[6] = owner::kLiteral;
            in.operands[7] = 2;
            return in;
        }(),
    }));
    s.code.add(kSimGroup, 0x1002, named("Bad GUID", {
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 0x1234, 1, E),
        runTreeByName(0, 0x10, 0x03, T, F),
    }));
    Vm vm(s.code, {}, &s.world);
    CHECK(vm.run(*s.sim, 0x1000).outcome == Outcome::True);
    CHECK(s.fridge->attributes[0] == 14 && s.sim->thread.temps[0] == 1);
    CHECK(vm.run(*s.sim, 0x1001).outcome == Outcome::True);
    CHECK(s.fridge->attributes[0] == 26);
    const RunResult bad = vm.run(*s.sim, 0x1002);
    CHECK(bad.outcome == Outcome::Error && bad.reason == "you sent me a crappy GUID, please fix it");
}

Instruction pushInteraction(std::uint8_t index, std::uint8_t objectParam, std::uint8_t priority,
                            std::uint16_t t, std::uint16_t f) {
    Instruction in;
    in.opcode = 0x000D;
    in.trueTarget = t;
    in.falseTarget = f;
    in.operands[0] = index;
    in.operands[1] = objectParam;
    in.operands[2] = priority;
    return in;
}

// Push Interaction queues interaction N of the object in a param onto the stack object (a Sim),
// after its test tree agrees (on a copy of its line, whose advertisements it may read). Failures
// leave a reason in my object data 0x34.
void pushInteractionQueuesForTheSim() {
    MemoryCodeSource code;
    auto fridgeType = typed(kFridgeGuid);
    fridgeType.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    ts2::ttab::InteractionTable table;
    table.interactions.resize(3);
    table.interactions[0].action = 0x1000;
    table.interactions[1].action = 0x1001;
    table.interactions[1].test = 0x1002;
    table.interactions[1].stringIndex = 1;
    table.interactions[2].action = 0x1000;
    table.interactions[2].test = 0x1003;
    table.interactions[2].stringIndex = 2;
    code.addInteractionTable(kObjectGroup, 0x80, table);
    code.add(kObjectGroup, 0x1002, bhav({expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 0, F, F)}));
    code.add(kObjectGroup, 0x1003, bhav({expr(owner::kTemp, 0, op::kAssign, ts2::sim::owner::kCheckTreeAdRange, 9, T, E)}));
    code.add(kObjectGroup, 0x1104, bhav({pushInteraction(2, 0, 1, T, F)}, 1));
    code.add(kObjectGroup, 0x1100, bhav({pushInteraction(0, 0, 1, T, F)}, 1));
    code.add(kObjectGroup, 0x1101, bhav({pushInteraction(1, 0, 3, T, F)}, 1));
    code.add(kObjectGroup, 0x1102, bhav({pushInteraction(0, 0, 0, T, F)}, 1));
    code.add(kObjectGroup, 0x1103, bhav({pushInteraction(5, 0, 1, T, F)}, 1));
    code.add(kObjectGroup, 0x1105, bhav({pushInteraction(0, 4, 1, T, F)}, 1));

    ts2::sim::World world;
    auto& controller = world.create(kObjectGroup, typed(0x0BADF00D));
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& fridge = world.create(kObjectGroup, fridgeType);
    controller.thread.stackObjectId = sim.id;
    controller.thread.temps[1] = fridge.id;  // not where the object is taken from
    const std::array<std::int16_t, 1> onFridge{fridge.id};
    Vm vm(code, {}, &world);
    CHECK(vm.run(controller, kObjectGroup, 0x1100, onFridge).outcome == Outcome::True);
    CHECK(sim.queue.size() == 1 && sim.queue[0].object == fridge.id && sim.queue[0].action == 0x1000 &&
          sim.queue[0].priority == 100);
    CHECK(sim.personData[0x4F] == fridge.id && sim.personData[0x2D] == 0);  // the next queued interaction
    CHECK(vm.run(controller, kObjectGroup, 0x1101, onFridge).outcome == Outcome::False && controller.data[0x34] == 2);
    CHECK(vm.run(controller, kObjectGroup, 0x1104, onFridge).outcome == Outcome::True && sim.queue.size() == 2);
    sim.queue.pop_back();
    const RunResult missing = vm.run(controller, kObjectGroup, 0x1103, onFridge);
    CHECK(missing.outcome == Outcome::Error && missing.reason == "reference to tree table entry that does not exist");
    // Byte 1 names a param: one the tree doesn't have is an error.
    const RunResult noParam = vm.run(controller, kObjectGroup, 0x1105, onFridge);
    CHECK(noParam.outcome == Outcome::Error && noParam.reason == "param index out of range");

    // A Sim pushing with its own priority needs one.
    sim.thread.stackObjectId = sim.id;
    CHECK(vm.run(sim, kObjectGroup, 0x1102, onFridge).outcome == Outcome::False && sim.data[0x34] == 1);
    sim.personData[0x21] = 40;
    CHECK(vm.run(sim, kObjectGroup, 0x1102, onFridge).outcome == Outcome::True && sim.queue.back().priority == 40);
    sim.queue.front().index = 1;
    CHECK(vm.run(controller, kObjectGroup, 0x1100, onFridge).outcome == Outcome::True && sim.personData[0x2D] == 1);
    sim.queue.pop_back();

    for (int i = 0; i < 5; ++i) {
        CHECK(vm.run(controller, kObjectGroup, 0x1100, onFridge).outcome == Outcome::True);
    }
    CHECK(sim.queue.size() == 7);
    CHECK(vm.run(controller, kObjectGroup, 0x1100, onFridge).outcome == Outcome::False && controller.data[0x34] == 3);
}

// Push Interaction byte 3 bit 0 takes the Sim it is with, its queue icon, from local [byte 4]. When
// no object has that id it has no icon and the queue leaves it out, as with the pregnancy
// controller's "*Stop Morning Sickness".
void pushWithNoSuchPartnerHasNoIcon() {
    MemoryCodeSource code;
    auto fridgeType = typed(kFridgeGuid);
    fridgeType.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    ts2::ttab::InteractionTable table;
    table.interactions.resize(1);
    table.interactions[0].action = 0x1000;
    code.addInteractionTable(kObjectGroup, 0x80, table);
    Instruction push = pushInteraction(0, 0, 3, T, F);
    push.operands[3] = 0x01;
    push.operands[4] = 1;
    code.add(kObjectGroup, 0x1100, bhav({push}, 1, 2));
    code.add(kObjectGroup, 0x1101, bhav({expr(owner::kLocal, 1, op::kAssign, owner::kParam, 0, 1, E), push}, 1, 2));

    ts2::sim::World world;
    auto& controller = world.create(kObjectGroup, typed(0x0BADF00D));
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& fridge = world.create(kObjectGroup, fridgeType);
    controller.thread.stackObjectId = sim.id;
    const std::array<std::int16_t, 1> onFridge{fridge.id};
    Vm vm(code, {}, &world);
    CHECK(vm.run(controller, kObjectGroup, 0x1100, onFridge).outcome == Outcome::True);
    CHECK(sim.queue.size() == 1 && sim.queue[0].object == fridge.id && sim.queue[0].icon() == 0);
    CHECK(vm.run(controller, kObjectGroup, 0x1101, onFridge).outcome == Outcome::True);
    CHECK(sim.queue.size() == 2 && sim.queue[1].icon() == fridge.id);
}

// A pushed line with flag 0x4 isn't queued: it runs to its end in the push, as the Sim, with the
// push's priority in the Sim's Priority person data. Then that and the Sim's temps are put back.
// Another such push from inside it fails with object data 0x34 = 6.
void pushRunsFlag4LinesAtOnce() {
    MemoryCodeSource code;
    auto fridgeType = typed(kFridgeGuid);
    fridgeType.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    ts2::ttab::InteractionTable table;
    table.interactions.resize(1);
    table.interactions[0].action = 0x1000;
    table.interactions[0].flags = 0x444;
    code.addInteractionTable(kObjectGroup, 0x80, table);
    // The action keeps the Sim's priority in its attribute 0, sets temp 0 and pushes itself again.
    Instruction again = pushInteraction(0, 0, 1, T, T);
    again.operands[3] = 0x02;  // the object from local 0
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectAttribute, 0, op::kAssign, owner::kMyPersonData, 0x21, 1, E),
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 7, 2, E),
        expr(owner::kLocal, 0, op::kAssign, owner::kStackObjectId, 0, 3, E),
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kMyData, 0x0B, 4, E),  // my object id
        again,
    }, 0, 1));
    code.add(kObjectGroup, 0x1100, bhav({pushInteraction(0, 0, 3, T, F)}, 1));

    ts2::sim::World world;
    auto& controller = world.create(kObjectGroup, typed(0x0BADF00D));
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& fridge = world.create(kObjectGroup, fridgeType);
    controller.thread.stackObjectId = sim.id;
    sim.personData[0x21] = 20;
    sim.thread.temps[0] = 3;
    const std::array<std::int16_t, 1> onFridge{fridge.id};
    Vm vm(code, {}, &world);
    const RunResult r = vm.run(controller, kObjectGroup, 0x1100, onFridge);
    CHECK(r.outcome == Outcome::True);
    CHECK(sim.queue.empty() && fridge.attributes[0] == 50);
    CHECK(sim.personData[0x21] == 20 && sim.thread.temps[0] == 3 && !world.runningAtOnce());
    CHECK(sim.data[0x34] == 6);  // the push from inside it
}

// A flag 0x4 line whose tree waits leaves the Sim it ran on to be reset, not the object that pushed
// it, and the pusher goes on.
void pushedLineThatWaitsLeavesTheSimToReset() {
    MemoryCodeSource code;
    auto fridgeType = typed(kFridgeGuid);
    fridgeType.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    ts2::ttab::InteractionTable table;
    table.interactions.resize(1);
    table.interactions[0].action = 0x1000;
    table.interactions[0].flags = 0x444;
    code.addInteractionTable(kObjectGroup, 0x80, table);
    code.add(kObjectGroup, 0x1000, bhav({sleepFor(0, T)}, 1));
    Instruction push = pushInteraction(0, 0, 3, T, F);
    push.operands[14] = 0x01;  // my params as its args: the fridge's id, ticks to sleep
    code.add(kObjectGroup, 0x1100, bhav({push}, 1));

    ts2::sim::World world;
    auto& controller = world.create(kObjectGroup, typed(0x0BADF00D));
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& fridge = world.create(kObjectGroup, fridgeType);
    controller.thread.stackObjectId = sim.id;
    const std::array<std::int16_t, 1> onFridge{fridge.id};
    Vm vm(code, {}, &world);
    CHECK(vm.run(controller, kObjectGroup, 0x1100, onFridge).outcome == Outcome::True);
    CHECK(sim.failedRun.has_value() && !controller.failedRun.has_value() && !world.runningAtOnce());
    // Until it is reset, a run at once on it runs nothing and is false.
    controller.thread = {};
    controller.thread.stackObjectId = sim.id;
    CHECK(vm.run(controller, kObjectGroup, 0x1100, onFridge).outcome == Outcome::True && sim.thread.frames.empty());
}

// An interaction made on a multi-tile part whose interaction group (OBJD field 8) is N above 0 runs
// on the part of the same object whose group is -N, from Push Interaction and from the pie menu.
// The Wedding Arch's end parts (1) hand theirs to its middle part (-1).
void pairedPartsHandTheirInteractionsOn() {
    constexpr std::uint32_t kEnd = 0x0A0A0A0A, kMiddle = 0x0B0B0B0B, kOther = 0x0C0C0C0C;
    MemoryCodeSource code;
    const auto part = [](std::uint32_t guid, std::uint16_t group) {
        auto type = typed(guid);
        type.fields[ts2::objd::field::kInteractionTableId] = 0x80;
        type.fields[ts2::objd::field::kMultiTileMasterId] = 4;
        type.fields[ts2::objd::field::kInteractionGroup] = group;
        return type;
    };
    ts2::ttab::InteractionTable table;
    table.interactions.resize(1);
    table.interactions[0].action = 0x1000;
    code.addInteractionTable(kObjectGroup, 0x80, table);
    code.add(kObjectGroup, 0x1100, bhav({pushInteraction(0, 0, 1, T, F)}, 1));

    ts2::sim::World world;
    auto& controller = world.create(kObjectGroup, typed(0x0BADF00D));
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& end = world.create(kObjectGroup, part(kEnd, 1));
    auto& middle = world.create(kObjectGroup, part(kMiddle, 0xFFFF));
    auto& otherArch = world.create(kObjectGroup, part(kMiddle, 0xFFFF));
    auto& single = world.create(kObjectGroup, part(kOther, 1));
    single.definition.fields[ts2::objd::field::kMultiTileMasterId] = 0;
    for (auto* o : {&end, &middle}) {
        o->data[ts2::sim::kLeadTileField] = end.id;
    }
    otherArch.data[ts2::sim::kLeadTileField] = otherArch.id;
    CHECK(world.interactionPart(end) == middle.id && world.interactionPart(middle) == middle.id);
    CHECK(world.interactionPart(single) == single.id);  // only multi-tile parts are paired

    controller.thread.stackObjectId = sim.id;
    const std::array<std::int16_t, 1> onEnd{end.id};
    Vm vm(code, {}, &world);
    CHECK(vm.run(controller, kObjectGroup, 0x1100, onEnd).outcome == Outcome::True);
    CHECK(sim.queue.size() == 1 && sim.queue[0].object == middle.id);
    const auto chosen = ts2::sim::queuedChoice(code, world, end, ts2::sim::PieChoice{});
    CHECK(chosen && chosen->object == middle.id);

    // A part with no partner keeps its interactions.
    middle.definition.fields[ts2::objd::field::kInteractionGroup] = 2;
    CHECK(world.interactionPart(end) == end.id);
}

// A check tree that pushes its own interaction runs that check tree again: nested check trees stop
// at the nesting limit instead of overflowing the stack.
void checkTreesNestOnlySoDeep() {
    MemoryCodeSource code;
    auto fridgeType = typed(kFridgeGuid);
    fridgeType.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    ts2::ttab::InteractionTable table;
    table.interactions.resize(1);
    table.interactions[0].action = 0x1000;
    table.interactions[0].test = 0x1002;
    code.addInteractionTable(kObjectGroup, 0x80, table);
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 0, T, T)}));
    // Each push passes its params on (byte 14 bit 0), so the check tree gets the fridge as param 0.
    Instruction push = pushInteraction(0, 0, 1, T, F);
    push.operands[14] = 0x01;
    code.add(kObjectGroup, 0x1002, bhav({expr(owner::kStackObjectId, 0, op::kAssign, owner::kTemp, 2, 1, F), push}, 1));
    code.add(kObjectGroup, 0x1100, bhav({push}, 1));

    ts2::sim::World world;
    auto& controller = world.create(kObjectGroup, typed(0x0BADF00D));
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& fridge = world.create(kObjectGroup, fridgeType);
    controller.thread.stackObjectId = sim.id;
    sim.thread.temps[2] = sim.id;
    Vm vm(code, {}, &world);
    const std::array<std::int16_t, 1> onFridge{fridge.id};
    CHECK(vm.run(controller, kObjectGroup, 0x1100, onFridge).outcome == Outcome::DepthLimit);
    CHECK(sim.queue.empty());
}

Instruction idleForInput(std::uint16_t counterParam, bool mayStart, bool immediate, std::uint16_t t,
                         std::uint16_t f) {
    Instruction in;
    in.opcode = 0x0011;
    in.trueTarget = t;
    in.falseTarget = f;
    in.operands[0] = static_cast<std::uint8_t>(counterParam);
    in.operands[2] = mayStart ? 1 : 0;
    in.operands[4] = immediate ? 1 : 0;
    return in;
}

// Idle for Input: waits, counting a parameter down each tick, and starts the first queued
// interaction (its action tree, with the object as stack object) when one is waiting.
void idleForInputStartsQueuedInteractions() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectAttribute, 0, op::kAdd, owner::kLiteral, 7, 1, E),  // the interaction
        expr(owner::kTemp, 2, op::kAssign, owner::kMyPersonData, 0x50, 2, E),
        expr(owner::kTemp, 3, op::kAssign, owner::kMyPersonData, 0x66, 3, E),
        expr(owner::kTemp, 4, op::kAssign, owner::kMyPersonData, 0x21, 4, E),
        expr(owner::kTemp, 5, op::kAssign, owner::kMyPersonData, 0xA6, F, E),
    }));
    code.add(kObjectGroup, 0x1100, bhav({
        idleForInput(0, true, false, 1, E),
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 1, T, E),
    }, 1));
    code.add(kObjectGroup, 0x1101, bhav({idleForInput(0, false, true, T, E)}));
    code.add(kObjectGroup, 0x1102, bhav({idleForInput(0, false, false, T, E)}, 1));

    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& fridge = world.create(kObjectGroup, typed(kFridgeGuid));
    Vm vm(code, {}, &world);

    // Nothing queued: waits as many ticks as the parameter says, then goes on.
    const std::array<std::int16_t, 1> twoTicks{2};
    CHECK(vm.run(sim, kObjectGroup, 0x1100, twoTicks).outcome == Outcome::Sleeping);
    CHECK(vm.resume(sim).outcome == Outcome::Sleeping);
    CHECK(vm.resume(sim).outcome == Outcome::True && sim.thread.temps[0] == 1);

    // Something queued above the Sim's Priority (person data 0x21): it starts and the count becomes
    // 0, so when it ends the primitive runs again and is true at once, even though the interaction
    // returned false. At or below it, the Sim keeps waiting.
    ts2::sim::QueuedInteraction q;
    q.object = fridge.id;
    q.group = kObjectGroup;
    q.action = 0x1000;
    q.index = 3;
    q.priority = 50;
    q.autonomous = true;
    sim.queue.push_back(q);
    sim.personData[0x4F] = fridge.id;
    sim.personData[0x21] = 50;
    sim.thread.temps[0] = 0;
    CHECK(vm.run(sim, kObjectGroup, 0x1100, twoTicks).outcome == Outcome::Sleeping && sim.queue.size() == 1);
    sim.personData[0x21] = 0;
    CHECK(vm.resume(sim).outcome == Outcome::True && (sim.data[8] & 0x40) == 0);
    CHECK(fridge.attributes[0] == 7 && sim.queue.empty() && sim.thread.temps[0] == 1 && sim.personData[0x4F] == 0);
    // While it ran my person data named it ("ci: Current Interaction Object ID", "... Running ..."). Now it is 0.
    CHECK(sim.thread.temps[2] == fridge.id && sim.thread.temps[3] == 3);
    CHECK(sim.personData[0x15] == 0 && sim.personData[0x50] == 0 && sim.personData[0x66] == 0 && sim.personData[0x67] == 0);
    // While it ran my "Priority" and "Interaction Autonomous" held its priority and flag. Now they are 0.
    CHECK(sim.thread.temps[4] == 50 && sim.thread.temps[5] == 1 && sim.personData[0x21] == 0 && sim.personData[0xA6] == 0);

    // Immediate mode with nothing queued is just true. Without "may start" a waiting one stays.
    CHECK(vm.run(sim, kObjectGroup, 0x1101).outcome == Outcome::True);
    sim.queue.push_back(q);
    CHECK(vm.run(sim, kObjectGroup, 0x1102, twoTicks).outcome == Outcome::True && sim.queue.size() == 1);

    // When an interaction it started ends, a continuation at the head of the queue starts at
    // once. Anything else ends the primitive, as does a continuation while the Sim carries
    // something (semi-attribute 0x22).
    const std::array<std::int16_t, 1> noTicks{0};
    sim.queue.clear();
    fridge.attributes[0] = 0;
    sim.queue.push_back(q);
    ts2::sim::QueuedInteraction next = q;
    next.continuation = true;
    sim.queue.push_back(next);
    sim.personData[0x21] = 0;
    CHECK(vm.run(sim, kObjectGroup, 0x1100, noTicks).outcome == Outcome::True);
    CHECK(fridge.attributes[0] == 14 && sim.queue.empty());
    sim.queue.push_back(q);
    sim.queue.push_back(q);
    CHECK(vm.run(sim, kObjectGroup, 0x1100, noTicks).outcome == Outcome::True);
    CHECK(fridge.attributes[0] == 21 && sim.queue.size() == 1);
    sim.queue.clear();
    sim.queue.push_back(q);
    sim.queue.push_back(next);
    sim.semiAttributes[0x22] = 1;
    CHECK(vm.run(sim, kObjectGroup, 0x1100, noTicks).outcome == Outcome::True);
    CHECK(fridge.attributes[0] == 28 && sim.queue.size() == 1 && sim.personData[0x48] == 1);
    sim.semiAttributes[0x22] = 0;
}

// Push Interaction byte 3 bit 7 keeps the pushed interaction's link number, bit 6 links a later push
// with it, as "Pickup - Do Carry Sim" links the carrier's "Hold Youngster" with the baby's "Be
// Held". When one ends, is cancelled or fails its start check, the other leaves its queue, or,
// already running, its Sim's person data 0x6E becomes 1.
void linkedInteractionsDropEachOther() {
    MemoryCodeSource code;
    auto fridgeType = typed(kFridgeGuid);
    fridgeType.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    ts2::ttab::InteractionTable table;
    table.interactions.resize(2);
    table.interactions[0].action = 0x1000;
    table.interactions[0].flags = 0x40;  // adults
    table.interactions[1].action = 0x1000;
    table.interactions[1].test = 0x1001;
    table.interactions[1].stringIndex = 1;
    table.interactions[1].flags = 0x40;
    code.addInteractionTable(kObjectGroup, 0x80, table);
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 0, T, T)}));
    // Line 1 passes its test while the fridge's attribute 0 is 0.
    code.add(kObjectGroup, 0x1001, bhav({expr(owner::kStackObjectAttribute, 0, op::kEquals, owner::kLiteral, 0, T, F)}));
    Instruction keep = pushInteraction(0, 0, 1, T, F);  // the link number goes to temp 3
    keep.operands[3] = 0x80;
    keep.operands[11] = owner::kTemp;
    keep.operands[12] = 3;
    code.add(kObjectGroup, 0x1110, bhav({keep}, 1));
    Instruction link = pushInteraction(0, 0, 1, T, F);  // linked with the one in temp 3
    link.operands[3] = 0x40;
    link.operands[8] = owner::kTemp;
    link.operands[9] = 3;
    code.add(kObjectGroup, 0x1111, bhav({link}, 1));
    Instruction keepLine1 = keep;
    keepLine1.operands[0] = 1;
    code.add(kObjectGroup, 0x1112, bhav({keepLine1}, 1));
    code.add(kObjectGroup, 0x1102, bhav({idleForInput(0, true, false, T, E)}, 1));

    ts2::sim::World world;
    auto& controller = world.create(kObjectGroup, typed(0x0BADF00D));
    auto& carrier = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& baby = world.create(kObjectGroup, typed(0x51515152, ts2::sim::kPersonType));
    auto& fridge = world.create(kObjectGroup, fridgeType);
    carrier.personData[0x3A] = 30;
    baby.personData[0x3A] = 30;
    const std::array<std::int16_t, 1> onFridge{fridge.id};
    const std::array<std::int16_t, 1> noTicks{0};
    Vm vm(code, {}, &world);
    const auto pushPair = [&](std::uint16_t first) {
        controller.thread.stackObjectId = carrier.id;
        CHECK(vm.run(controller, kObjectGroup, first, onFridge).outcome == Outcome::True);
        controller.thread.stackObjectId = baby.id;
        CHECK(vm.run(controller, kObjectGroup, 0x1111, onFridge).outcome == Outcome::True);
    };

    pushPair(0x1110);
    CHECK(carrier.queue.size() == 1 && baby.queue.size() == 1);
    if (carrier.queue.size() != 1 || baby.queue.size() != 1) {
        return;
    }
    CHECK(carrier.queue[0].linkId != 0 && controller.thread.temps[3] == static_cast<std::int16_t>(carrier.queue[0].linkId));
    CHECK(baby.queue[0].linkedId == carrier.queue[0].linkId && carrier.queue[0].linkedId == baby.queue[0].linkId);
    CHECK(ts2::sim::cancelInteraction(world, carrier, carrier.queue[0].id) && carrier.queue.empty() && baby.queue.empty());

    // The carrier's runs and ends: the baby's waiting one leaves, and starting cleared 0x6E.
    pushPair(0x1110);
    carrier.personData[0x6E] = 1;
    CHECK(vm.run(carrier, kObjectGroup, 0x1102, noTicks).outcome == Outcome::True);
    CHECK(!carrier.running && carrier.queue.empty() && baby.queue.empty() && carrier.personData[0x6E] == 0);

    // The carrier's fails its start check (and Idle for Input waits a tick): the baby's leaves too.
    pushPair(0x1112);
    fridge.attributes[0] = 1;
    CHECK(vm.run(carrier, kObjectGroup, 0x1102, noTicks).outcome == Outcome::Sleeping);
    CHECK(!carrier.running && carrier.queue.empty() && baby.queue.empty());
    fridge.attributes[0] = 0;

    // The baby's already runs: it stays, and the baby learns its partner is gone.
    pushPair(0x1110);
    baby.running = baby.queue[0];
    baby.queue.clear();
    CHECK(ts2::sim::cancelInteraction(world, carrier, carrier.queue[0].id));
    CHECK(baby.running && baby.personData[0x6E] == 1 && baby.running->linkedId == 0);

    // A link number no interaction has links nothing.
    controller.thread.temps[3] = 999;
    controller.thread.stackObjectId = baby.id;
    CHECK(vm.run(controller, kObjectGroup, 0x1111, onFridge).outcome == Outcome::True);
    CHECK(baby.queue.size() == 1 && baby.queue[0].linkId == 0 && baby.queue[0].linkedId == 0);
}

// What the queue shows: Push Interaction's icon (an object type from temps 4 and 5 with the index in
// byte 15) and the line's nested flag, the interaction a Sim runs while its tree runs, and the
// player's cancel, which takes a waiting interaction out at once and marks the running one.
void queuedInteractionsShowAndCancel() {
    MemoryCodeSource code;
    auto fridgeType = typed(kFridgeGuid);
    fridgeType.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    ts2::ttab::InteractionTable table;
    table.interactions.resize(2);
    table.interactions[0].action = 0x1000;
    table.interactions[0].flags = 0x40 | 0x4000;  // adults, takes sub-interactions
    table.interactions[1].action = 0x1000;
    table.interactions[1].stringIndex = 1;
    table.interactions[1].flags = 0x8000 | 0x40;
    code.addInteractionTable(kObjectGroup, 0x80, table);
    // The interaction waits two ticks (its first argument) before it ends.
    code.add(kObjectGroup, 0x1000, bhav({idleForInput(0, false, false, T, E)}));
    Instruction withIcon = pushInteraction(0, 1, 1, T, F);  // the fridge in param 1
    withIcon.operands[14] = 0x01 | 0x04;
    withIcon.operands[15] = 3;
    code.add(kObjectGroup, 0x1100, bhav({withIcon}, 2));
    code.add(kObjectGroup, 0x1101, bhav({pushInteraction(1, 1, 1, T, F)}, 2));
    code.add(kObjectGroup, 0x1102, bhav({idleForInput(0, true, false, T, E)}, 1));
    code.add(kObjectGroup, 0x1103, bhav({idleForInput(0, false, true, T, E)}));
    Instruction nestedWithArgs = pushInteraction(1, 1, 1, T, F);
    nestedWithArgs.operands[14] = 0x01;
    code.add(kObjectGroup, 0x1104, bhav({nestedWithArgs}, 2));

    ts2::sim::World world;
    auto& controller = world.create(kObjectGroup, typed(0x0BADF00D));
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& fridge = world.create(kObjectGroup, fridgeType);
    controller.thread.stackObjectId = sim.id;
    controller.thread.temps[4] = 0x5678;
    controller.thread.temps[5] = 0x1234;
    Vm vm(code, {}, &world);
    const std::array<std::int16_t, 1> twoTicks{2};
    const std::array<std::int16_t, 2> twoTicksOnFridge{2, fridge.id};
    sim.personData[0x3A] = 30;  // an adult
    // A nested line waits only while the Sim runs an interaction whose line has flag 0x4000.
    ts2::sim::QueuedInteraction takingSubs;
    takingSubs.object = fridge.id;
    takingSubs.group = kObjectGroup;
    takingSubs.index = 0;
    CHECK(vm.run(controller, kObjectGroup, 0x1100, twoTicksOnFridge).outcome == Outcome::True);
    CHECK(vm.run(controller, kObjectGroup, 0x1101, twoTicksOnFridge).outcome == Outcome::False && controller.data[0x34] == 6);
    sim.running = takingSubs;
    CHECK(vm.run(controller, kObjectGroup, 0x1101, twoTicksOnFridge).outcome == Outcome::True);
    CHECK(vm.run(controller, kObjectGroup, 0x1101, twoTicksOnFridge).outcome == Outcome::True);
    sim.running.reset();
    CHECK(sim.queue.size() == 3);
    CHECK(sim.queue[0].iconGuid == 0x12345678 && sim.queue[0].iconIndex == 3 && !sim.queue[0].nested);
    CHECK(sim.queue[1].iconGuid == 0 && sim.queue[1].nested);
    CHECK(sim.queue[0].id == 1 && sim.queue[1].id == 2 && sim.queue[2].id == 3);

    // A waiting one leaves at once.
    CHECK(ts2::sim::cancelInteraction(world, sim, 2) && sim.queue.size() == 2 && sim.queue[1].id == 3);
    CHECK(!ts2::sim::cancelInteraction(world, sim, 2));
    CHECK(ts2::sim::cancelInteraction(world, sim, 3) && sim.queue.size() == 1);

    // The first starts: it runs until its tree returns, its icon the Sim's action icon.
    CHECK(vm.run(sim, kObjectGroup, 0x1102).outcome == Outcome::Sleeping);
    CHECK(sim.running && sim.running->id == 1 && sim.queue.empty());
    CHECK(sim.actionIconGuid == 0x12345678 && sim.actionIconIndex == 3 && sim.actionIconObject == fridge.id);
    CHECK(sim.actionString.empty());  // a pushed interaction has no name of its own
    sim.personData[0x21] = 100;
    CHECK(ts2::sim::cancelInteraction(world, sim, 1) && sim.running->cancelled && sim.personData[0x21] == 0);
    CHECK(!ts2::sim::cancelInteraction(world, sim, 1));
    // Its Idle for Input is true at once now, so its tree finishes.
    CHECK(vm.resume(sim).outcome == Outcome::True && !sim.running);

    // A nested one waits in the sub-queue: person data 0x1A counts it, 0x4D/0x4E name it and
    // 0x4B/0x4C the running interaction. Started at once (byte 4 bit 0) it is the running
    // sub-interaction until its tree returns. Meanwhile 0x9D and the running pair (0x66/0x67) name
    // it and the current pair (0x15/0x50) keeps the interaction. Then 0x9D is 0 and the running
    // pair copies the current one.
    sim.running = takingSubs;
    sim.personData[0x15] = 7;
    sim.personData[0x50] = controller.id;
    CHECK(vm.run(controller, kObjectGroup, 0x1104, twoTicksOnFridge).outcome == Outcome::True);
    CHECK(sim.personData[0x1A] == 1 && sim.personData[0x4D] == 1 && sim.personData[0x4E] == fridge.id);
    CHECK(sim.personData[0x4B] == fridge.id && sim.personData[0x4C] == 0);
    CHECK(vm.run(sim, kObjectGroup, 0x1103).outcome == Outcome::Sleeping);
    CHECK(sim.runningSub && sim.runningSub->id == 4);
    CHECK(sim.personData[0x9D] == fridge.id && sim.personData[0x66] == 1 && sim.personData[0x67] == fridge.id);
    CHECK(sim.personData[0x1A] == 0 && sim.personData[0x4D] == 0 && sim.personData[0x4E] == 0);
    CHECK(sim.personData[0x15] == 7 && sim.personData[0x50] == controller.id);
    CHECK(vm.resume(sim).outcome == Outcome::Sleeping);
    CHECK(vm.resume(sim).outcome == Outcome::True && !sim.runningSub);
    CHECK(sim.personData[0x9D] == 0 && sim.personData[0x66] == 7 && sim.personData[0x67] == controller.id);
    sim.running.reset();
    CHECK(vm.run(controller, kObjectGroup, 0x1100, twoTicksOnFridge).outcome == Outcome::True);
    CHECK(vm.run(sim, kObjectGroup, 0x1102).outcome == Outcome::Sleeping);
    CHECK(sim.running && sim.personData[0x9D] == 0);
}

// Push Interaction counts each queue apart: 7 waiting interactions refuse another with object data
// 0x34 = 3, 5 waiting sub-interactions refuse another with 4, and neither limit holds the other.
void pushInteractionLimitsEachQueue() {
    MemoryCodeSource code;
    auto fridgeType = typed(kFridgeGuid);
    fridgeType.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    ts2::ttab::InteractionTable table;
    table.interactions.resize(2);
    table.interactions[0].action = 0x1000;
    table.interactions[0].flags = 0x4000;
    table.interactions[1].action = 0x1000;
    table.interactions[1].stringIndex = 1;
    table.interactions[1].flags = 0x8000;
    code.addInteractionTable(kObjectGroup, 0x80, table);
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 0, T, E)}));
    code.add(kObjectGroup, 0x1100, bhav({pushInteraction(0, 1, 1, T, F)}, 2));
    code.add(kObjectGroup, 0x1101, bhav({pushInteraction(1, 1, 1, T, F)}, 2));

    ts2::sim::World world;
    auto& controller = world.create(kObjectGroup, typed(0x0BADF00D));
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& fridge = world.create(kObjectGroup, fridgeType);
    controller.thread.stackObjectId = sim.id;
    ts2::sim::QueuedInteraction running;
    running.object = fridge.id;
    running.group = kObjectGroup;
    sim.running = running;
    Vm vm(code, {}, &world);
    const std::array<std::int16_t, 2> onFridge{0, fridge.id};
    for (int i = 0; i < 5; ++i) {
        CHECK(vm.run(controller, kObjectGroup, 0x1101, onFridge).outcome == Outcome::True);
    }
    CHECK(vm.run(controller, kObjectGroup, 0x1101, onFridge).outcome == Outcome::False && controller.data[0x34] == 4);
    for (int i = 0; i < 7; ++i) {
        CHECK(vm.run(controller, kObjectGroup, 0x1100, onFridge).outcome == Outcome::True);
    }
    CHECK(vm.run(controller, kObjectGroup, 0x1100, onFridge).outcome == Outcome::False && controller.data[0x34] == 3);
    CHECK(sim.queue.size() == 12);
}

// Idle for Input takes a sub-interaction (nested) in immediate mode and any other one otherwise, and
// tests each again as it leaves the queue. One that fails is dropped and its object's Queue Skipped
// function runs, as the object with the Sim as stack object: then a sub-interaction is true and an
// interaction waits a tick. One whose object is gone is dropped without it. Autonomy's pick is
// tested with param 0 set and the object's lockout (data 0x19).
void idleForInputTestsWhatStarts() {
    MemoryCodeSource code;
    auto fridgeType = typed(kFridgeGuid);
    fridgeType.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    ts2::ttab::InteractionTable table;
    table.interactions.resize(2);
    for (auto& line : table.interactions) {
        line.action = 0x1000;
        line.test = 0x1010;
        line.flags = 0x40;  // adults
    }
    table.interactions[1].stringIndex = 1;
    table.interactions[1].flags |= 0x8000;
    code.addInteractionTable(kObjectGroup, 0x80, table);
    code.addObjectType(kObjectGroup, fridgeType);
    std::vector<ts2::objf::Function> functions(5);
    functions[4].action = 0x1020;
    code.addFunctions(kObjectGroup, functions);
    // The action counts its runs in the fridge's attribute 0. The check keeps its param 0 in
    // attribute 2 and passes while attribute 1 is 0. Queue Skipped keeps its stack object in attribute 3.
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kStackObjectAttribute, 0, op::kAdd, owner::kLiteral, 1, T, E)}));
    code.add(kObjectGroup, 0x1010, bhav({expr(owner::kStackObjectAttribute, 2, op::kAssign, owner::kParam, 0, 1, E),
                                         expr(owner::kStackObjectAttribute, 1, op::kEquals, owner::kLiteral, 0, T, F)},
                                        4));
    code.add(kObjectGroup, 0x1020, bhav({expr(owner::kMyAttribute, 3, op::kAssign, owner::kStackObjectId, 0, T, E)}));
    code.add(kObjectGroup, 0x1100, bhav({idleForInput(0, true, false, T, E)}, 1));
    code.add(kObjectGroup, 0x1101, bhav({idleForInput(0, false, true, T, E)}));

    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& fridge = world.create(kObjectGroup, fridgeType);
    sim.personData[0x3A] = 30;  // an adult
    Vm vm(code, {}, &world);
    const auto waiting = [&](std::uint16_t line, auto source) {
        ts2::sim::QueuedInteraction q;
        q.object = fridge.id;
        q.group = kObjectGroup;
        q.index = line;
        q.action = 0x1000;
        q.priority = 50;
        q.nested = line == 1;
        q.source = source;
        return q;
    };
    constexpr auto kScript = ts2::sim::QueuedInteraction::Source::Script;
    constexpr auto kAutonomy = ts2::sim::QueuedInteraction::Source::Autonomy;
    const std::array<std::int16_t, 1> threeTicks{3};

    // Immediate mode starts the waiting sub-interaction behind an interaction, and leaves that one.
    sim.queue = {waiting(0, kScript), waiting(1, kScript)};
    CHECK(vm.run(sim, kObjectGroup, 0x1101).outcome == Outcome::True);
    CHECK(fridge.attributes[0] == 1 && sim.queue.size() == 1 && !sim.queue[0].nested);
    CHECK(sim.personData[0x9D] == 0 && fridge.attributes[2] == 0);
    sim.queue.clear();

    // A sub-interaction that fails its test: true, and the sub-queue's person data are cleared.
    fridge.attributes[1] = 1;
    sim.queue = {waiting(1, kScript)};
    CHECK(vm.run(sim, kObjectGroup, 0x1101).outcome == Outcome::True);
    CHECK(sim.queue.empty() && fridge.attributes[0] == 1 && fridge.attributes[3] == sim.id && sim.personData[0x9D] == 0);

    // An interaction that fails its test waits a tick, its count kept.
    fridge.attributes[3] = 0;
    sim.personData[0x9D] = fridge.id;
    sim.queue = {waiting(0, kScript)};
    CHECK(vm.run(sim, kObjectGroup, 0x1100, threeTicks).outcome == Outcome::Sleeping);
    CHECK(sim.queue.empty() && fridge.attributes[0] == 1 && fridge.attributes[3] == sim.id && sim.personData[0x9D] == 0);
    CHECK(sim.thread.frames.back().params[0] == 3);
    sim.thread.frames.clear();

    // One whose object is gone is dropped and waits a tick too, with no Queue Skipped.
    fridge.attributes[1] = 0;
    fridge.attributes[3] = 0;
    sim.queue = {waiting(0, kScript)};
    sim.queue[0].object = 999;
    CHECK(vm.run(sim, kObjectGroup, 0x1100, threeTicks).outcome == Outcome::Sleeping);
    CHECK(sim.queue.empty() && fridge.attributes[3] == 0);
    sim.thread.frames.clear();

    // Autonomy's pick fails on the object's lockout, a script's push doesn't.
    fridge.data[0x19] = 5;
    sim.queue = {waiting(0, kAutonomy)};
    CHECK(vm.run(sim, kObjectGroup, 0x1100, threeTicks).outcome == Outcome::Sleeping);
    CHECK(sim.queue.empty() && fridge.attributes[0] == 1 && fridge.attributes[3] == sim.id);
    sim.thread.frames.clear();
    sim.queue = {waiting(0, kScript)};
    CHECK(vm.run(sim, kObjectGroup, 0x1100, threeTicks).outcome == Outcome::True);
    CHECK(fridge.attributes[0] == 2 && fridge.attributes[2] == 0);
    fridge.data[0x19] = 0;
    sim.queue = {waiting(0, kAutonomy)};
    CHECK(vm.run(sim, kObjectGroup, 0x1100, threeTicks).outcome == Outcome::True);
    CHECK(fridge.attributes[0] == 3 && fridge.attributes[2] == 1);
}

void onTimerSetsChangesAndRemovesTimers() {
    Instruction set;
    set.opcode = 0x007A;
    set.trueTarget = T;
    set.falseTarget = F;
    set.operands = {5, 0, 0, 0x01, 0x20, 0x02, 0x07, 11, 0, 0x07, 22, 0, 0x07, 33, 0, 0};
    Instruction change = set;
    change.operands[0] = 9;
    change.operands[5] = 0x04;
    change.operands[15] = 1;
    Instruction remove = set;
    remove.operands[15] = 2;
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1100, bhav({set}));
    code.add(kObjectGroup, 0x1101, bhav({change}));
    code.add(kObjectGroup, 0x1102, bhav({remove}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& chair = world.create(kObjectGroup, typed(kChairGuid));
    sim.thread.stackObjectId = chair.id;
    Vm vm(code, {}, &world);

    CHECK(vm.run(sim, kObjectGroup, 0x1100).outcome == Outcome::True && chair.timers.size() == 1);
    const auto& timer = chair.timers.front();
    CHECK(timer.id == 1 && sim.thread.temps[0] == 1 && timer.setter == sim.id && timer.tree == 0x2001);
    CHECK(timer.countdown == 5 && timer.interval == 5 && timer.repeats);
    CHECK(timer.args[0] == 11 && timer.args[1] == 22 && timer.args[2] == 33 && timer.args[3] == 0);

    CHECK(vm.run(sim, kObjectGroup, 0x1101).outcome == Outcome::True);
    CHECK(timer.interval == 9 && timer.countdown == 9 && !timer.repeats && timer.setter == sim.id);
    CHECK(vm.run(sim, kObjectGroup, 0x1102).outcome == Outcome::True && chair.timers.empty());
    // Another set gets a new id.
    CHECK(vm.run(sim, kObjectGroup, 0x1100).outcome == Outcome::True && chair.timers.size() == 1 && sim.thread.temps[0] == 2);
}

// Each message handler counts its steps from 0: two of about 500 steps each fit under a limit of 1,000.
void messageHandlersCountTheirOwnSteps() {
    constexpr std::uint32_t kReceiverGroup = 0x7F000003;
    using namespace ts2::sim;
    Instruction direct;
    direct.opcode = 0x0077;
    direct.trueTarget = 1;
    direct.falseTarget = F;
    direct.operands = {0, 5, 0, 0, 0x02, owner::kLiteral, 0, 0, 1, owner::kLiteral, 7, 0, owner::kLiteral, 0, 0, owner::kLiteral};
    Instruction again = direct;
    again.trueTarget = T;
    MemoryCodeSource code;
    code.add(kReceiverGroup, 0x1001, bhav({expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 0, 1, E),
                                           expr(owner::kTemp, 0, op::kIncrementAndLess, owner::kLiteral, 500, 1, T)}));
    std::vector<ts2::objf::Function> functions(0x25);
    functions[0x24].action = 0x1001;
    code.addFunctions(kReceiverGroup, functions);
    code.addObjectType(kReceiverGroup, typed(kChairGuid));
    World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, kPersonType));
    auto& receiver = world.create(kReceiverGroup, typed(kChairGuid));
    direct.operands[6] = again.operands[6] = static_cast<std::uint8_t>(receiver.id);
    code.add(kObjectGroup, 0x1100, bhav({direct, again}));
    Vm vm(code, {1000, 64}, &world);
    CHECK(vm.run(sim, kObjectGroup, 0x1100).outcome == Outcome::True);
    CHECK(!sim.failedRun.has_value());
}

void messageRunsTheReceiversHandlerOnMe() {
    constexpr std::uint32_t kReceiverGroup = 0x7F000003;
    using namespace ts2::sim;
    Instruction direct;
    direct.opcode = 0x0077;
    direct.trueTarget = T;
    direct.falseTarget = F;
    direct.operands = {0, 5, 0, 0, 0x02, owner::kLiteral, 0, 0, 1, owner::kLiteral, 7, 0, owner::kLiteral, 0, 0, owner::kLiteral};
    Instruction toRoom = direct;
    toRoom.operands[3] = 7;  // every object
    toRoom.operands[4] = 0;  // in my room
    Instruction toNobody = toRoom;
    toNobody.operands[3] = 6;  // objects that aren't Sims
    toNobody.operands[0] = 1;  // on my level
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1100, bhav({direct}));
    code.add(kObjectGroup, 0x1101, bhav({toRoom}));
    code.add(kObjectGroup, 0x1102, bhav({toNobody}));
    // The handler sees the message as params and the receiver as stack object, on my temps.
    code.add(kReceiverGroup, 0x1001, bhav({expr(owner::kTemp, 1, op::kAssign, owner::kParam, 3, 1, E),
                                           expr(owner::kTemp, 2, op::kAssign, owner::kStackObjectId, 0, T, E)}));
    std::vector<ts2::objf::Function> functions(0x25);
    functions[0x24].action = 0x1001;
    code.addFunctions(kReceiverGroup, functions);
    code.addObjectType(kReceiverGroup, typed(kChairGuid));
    code.addObjectType(kObjectGroup, typed(0x51515151, kPersonType));
    World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, kPersonType));
    auto& near = world.create(kReceiverGroup, typed(kChairGuid));
    auto& far = world.create(kReceiverGroup, typed(kChairGuid));
    sim.data[0x1D] = near.data[0x1D] = 3;
    far.data[0x1D] = 4;
    far.level = 2;
    Vm vm(code, {}, &world);

    direct.operands[6] = static_cast<std::uint8_t>(far.id);
    code.add(kObjectGroup, 0x1100, bhav({direct}));
    CHECK(vm.run(sim, kObjectGroup, 0x1100).outcome == Outcome::True);
    CHECK(sim.thread.temps[1] == 7 && sim.thread.temps[2] == far.id);
    sim.thread.temps = {};
    CHECK(vm.run(sim, kObjectGroup, 0x1101).outcome == Outcome::True && sim.thread.temps[2] == near.id);
    sim.thread.temps = {};
    CHECK(vm.run(sim, kObjectGroup, 0x1102).outcome == Outcome::True && sim.thread.temps[2] == near.id);
    sim.level = 3;  // no object on my level
    CHECK(vm.run(sim, kObjectGroup, 0x1102).outcome == Outcome::False);
}

// Find Best Object for Function: of the objects serving the function (their OBJf entry), the one
// whose rating, halved every 5 tiles away, is highest and whose check tree passes becomes the stack
// object. The room flags narrow the search.
void findBestObjectPicksTheBestRatedNearest() {
    constexpr std::uint32_t kPickyGroup = 0x7F000003;
    constexpr std::uint16_t kSit = 0x1A;  // the OBJf entry for "sit", rated by object data 0x1A
    auto find = [](std::uint8_t flags) {
        Instruction in;
        in.opcode = 0x000E;
        in.trueTarget = T;
        in.falseTarget = F;
        in.operands[0] = 0x08;  // sit
        in.operands[2] = flags;
        return in;
    };
    std::vector<ts2::objf::Function> sits(0x1B);
    sits[kSit].action = 0x1001;
    std::vector<ts2::objf::Function> pickySits = sits;
    pickySits[kSit].check = 0x1000;

    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({find(0)}));
    code.add(kObjectGroup, 0x1001, bhav({find(0x04)}));
    code.add(kPickyGroup, 0x1000, bhav({expr(owner::kLiteral, 0, op::kEquals, owner::kLiteral, 1, T, F)}));  // fails
    code.addObjectType(kObjectGroup, typed(kChairGuid));
    code.addObjectType(kPickyGroup, typed(0x0C0C0C0C));
    code.addFunctions(kObjectGroup, sits);
    code.addFunctions(kPickyGroup, pickySits);

    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto place = [](ts2::sim::SimObject& o, int x, int y, std::int16_t rating, std::int16_t room) {
        o.x = x * 65536;
        o.y = y * 65536;
        o.data[0x1A] = rating;
        o.data[0x1D] = room;
    };
    place(sim, 0, 0, 0, 1);
    auto& farGood = world.create(kObjectGroup, typed(kChairGuid));
    place(farGood, 10, 0, 100, 1);  // 100 / 3 = 33.3
    auto& nearPlain = world.create(kObjectGroup, typed(kChairGuid));
    place(nearPlain, 0, 0, 40, 2);  // 40, but in another room
    auto& unrated = world.create(kObjectGroup, typed(kChairGuid));
    place(unrated, 1, 0, 0, 1);
    auto& picky = world.create(kPickyGroup, typed(0x0C0C0C0C));
    place(picky, 1, 0, 100, 1);  // the best, but its check tree fails
    Vm vm(code, {}, &world);

    ThreadState& t = sim.thread;
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::True && t.stackObjectId == nearPlain.id);
    CHECK(vm.run(sim, kObjectGroup, 0x1001).outcome == Outcome::True && t.stackObjectId == farGood.id);
    CHECK(sim.data[0x42] == 100);  // the function score of the last object rated

    // Nothing serves a function no object has.
    sits[kSit] = {};
    code.addFunctions(kObjectGroup, sits);
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::False);
}

// Run Functional Tree: calls the stack object's action tree for the function (its OBJf entry), with
// the object as stack object and its group's code. It is false when it has none.
void runFunctionalTreeCallsTheObjectsTree() {
    auto run = [](std::uint16_t function) {
        Instruction in;
        in.opcode = 0x0014;
        in.trueTarget = T;
        in.falseTarget = F;
        in.operands[0] = static_cast<std::uint8_t>(function);
        return in;
    };
    std::vector<ts2::objf::Function> functions(0x1B);
    functions[0x1A].action = 0x1001;  // sit
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({run(8)}));
    code.add(kObjectGroup, 0x1002, bhav({run(0)}));  // prepare food: the chair has none
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kStackObjectAttribute, 0, op::kAssign, owner::kLiteral, 9, T, E),
    }));
    code.addObjectType(kObjectGroup, typed(kChairGuid));
    code.addFunctions(kObjectGroup, functions);

    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& chair = world.create(kObjectGroup, typed(kChairGuid));
    chair.attributes.fill(0);
    Vm vm(code, {}, &world);
    sim.thread.stackObjectId = chair.id;
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::True && chair.attributes[0] == 9);
    sim.thread.stackObjectId = chair.id;
    CHECK(vm.run(sim, kObjectGroup, 0x1002).outcome == Outcome::False);

    // With byte 2 bit 0 the object becomes the running interaction's queue icon, picture 0.
    Instruction withIcon = run(8);
    withIcon.operands[2] = 0x01;
    code.add(kObjectGroup, 0x1003, bhav({withIcon}));
    sim.running = ts2::sim::QueuedInteraction{};
    sim.actionIconGuid = 0x1234;
    sim.actionIconIndex = 3;
    sim.thread.stackObjectId = chair.id;
    CHECK(vm.run(sim, kObjectGroup, 0x1003).outcome == Outcome::True);
    CHECK(sim.actionIconObject == chair.id && sim.actionIconGuid == 0 && sim.actionIconIndex == 0);

    // Without byte 2 bit 1 the tree's params are my temps: a stove's Cook reads the cook type in param 0.
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kStackObjectAttribute, 0, op::kAssign, owner::kParam, 0, T, E),
    }, 1));
    sim.thread.temps[0] = 6;
    sim.thread.stackObjectId = chair.id;
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::True && chair.attributes[0] == 6);
}

// The pie menu: each interaction line the Sim's age allows whose check tree agrees, named from the
// table's pie-menu strings and ordered by what the check tree set in my data 0x68. A check tree
// adding action strings offers those lines instead of its own, and leaves the Sim's temps alone.
void pieMenuOffersWhatChecksAllow() {
    auto offer = [](std::uint16_t index, std::uint8_t flagsHigh, std::uint16_t next) {
        Instruction in;
        in.opcode = 0x0032;
        in.trueTarget = next;
        in.falseTarget = E;
        in.operands[3] = flagsHigh;
        in.operands[14] = static_cast<std::uint8_t>(index);
        return in;
    };
    MemoryCodeSource code;
    auto chairType = typed(kFridgeGuid);
    chairType.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    ts2::ttab::InteractionTable table;
    table.interactions.resize(7);
    for (std::size_t i = 0; i < table.interactions.size(); ++i) {
        table.interactions[i].action = 0x1000;
        table.interactions[i].flags = 0x40;  // adults
        table.interactions[i].stringIndex = static_cast<std::uint32_t>(i);
    }
    table.interactions[1].test = 0x1010;
    table.interactions[2].flags = 0x10;  // children
    table.interactions[3].flags = 0x40 | 0x80;  // debug
    table.interactions[4].test = 0x1011;
    table.interactions[5].test = 0x1012;
    table.interactions[6].test = 0x1013;
    code.addInteractionTable(kObjectGroup, 0x80, table);
    code.addInteractionStrings(kObjectGroup, 0x80, {"Use", "Never", "Play", "Debug", "Sit", "", "Hidden"});
    code.addStrings(kObjectGroup, 0x12E, {"Red", "Blue"});
    code.add(kObjectGroup, 0x1010, bhav({expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 99, F, F)}));
    code.add(kObjectGroup, 0x1011, bhav({expr(owner::kMyData, 0x68, op::kAssign, owner::kLiteral, 5, T, E)}));
    code.add(kObjectGroup, 0x1012, bhav({offer(1, 0, 1), offer(2, 0x01, T)}));
    code.add(kObjectGroup, 0x1013, bhav({expr(owner::kMyData, 0x32, op::kAssign, owner::kLiteral, 1, T, E)}));

    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& chair = world.create(kObjectGroup, chairType);
    sim.personData[0x3A] = 30;
    sim.thread.temps[0] = 7;
    Vm vm(code, {}, &world);
    auto menu = ts2::sim::pieMenu(vm, code, sim, chair);
    CHECK(menu.problems.empty());
    CHECK(menu.choices.size() == 4);
    if (menu.choices.size() == 4) {
        CHECK(menu.choices[0].name == "Sit" && menu.choices[0].order == 5 && menu.choices[0].interaction == 4);
        CHECK(menu.choices[1].name == "Use" && menu.choices[1].interaction == 0);
        // Line 1's check tree left 99 in temp 0 for the ones after it.
        CHECK(menu.choices[2].name == "Red" && menu.choices[2].interaction == 5 && menu.choices[2].params[1] == 99);
        CHECK(menu.choices[3].name == "Blue" && menu.choices[3].disabled == ts2::sim::OfferedAction::Disabled::Propagating);
    }
    CHECK(sim.thread.temps[0] == 7);
    CHECK(sim.actionString.empty());

    // Debug lines alone, named as any other line, on a shift-click with testing cheats.
    menu = ts2::sim::pieMenu(vm, code, sim, chair, {.debug = true});
    // The game's own "Force Error" comes last and queues its global tree, with an id no table line has.
    CHECK(menu.choices.size() == 2 && menu.choices[0].name == "Debug" && menu.choices[1].name == "Force Error");
    if (menu.choices.size() == 2) {
        const auto forced = ts2::sim::queuedChoice(code, world, chair, menu.choices[1]);
        CHECK(forced && forced->action == 0x02DA && forced->index == 0xFFFE && forced->object == chair.id);
    }

    // Children see the children's line. Nothing is offered by an object with its interactions off.
    sim.personData[0x3A] = 10;
    menu = ts2::sim::pieMenu(vm, code, sim, chair, {.firstOnly = true});
    CHECK(menu.choices.size() == 1 && menu.choices[0].name == "Play");
    chair.data[0x57] = 1;
    CHECK(ts2::sim::pieMenu(vm, code, sim, chair).choices.empty());
}

// The cheat window's commands as the game's cheat system runs them: names without case, boolProp with
// true, on, false or off, help listing the commands not hidden, and "Unknown command" for any other.
void cheatWindowRunsTheGamesCommands() {
    using ts2::sim::CheatResult;
    using ts2::sim::runCheat;
    ts2::sim::CheatProperties properties;
    CHECK(!properties.testingCheats());
    auto result = runCheat(properties, nullptr, "boolProp testingCheatsEnabled true");
    CHECK(result.ok && result.output.empty() && properties.testingCheats());
    result = runCheat(properties, nullptr, "BOOLPROP testingcheatsenabled FALSE");
    CHECK(result.ok && !properties.testingCheats());
    CHECK(runCheat(properties, nullptr, "boolProp testingCheatsEnabled on").ok && properties.testingCheats());
    CHECK(runCheat(properties, nullptr, "boolProp testingCheatsEnabled Off").ok && !properties.testingCheats());
    // A value boolProp doesn't take changes nothing.
    properties.set("testingCheatsEnabled", true);
    CHECK(!runCheat(properties, nullptr, "boolProp testingCheatsEnabled maybe").ok && properties.testingCheats());
    CHECK(!runCheat(properties, nullptr, "boolProp testingCheatsEnabled").ok);
    const std::vector<std::string> help{
        "TSCheatSystem: expand              : Expands or contracts the output portion of the cheat console.",
        "TSCheatSystem: help                : [wildcard] [-all]  Logs this help output.",
        "TSCheatSystem: lockAspiration      : lockAspiration [on/off]: Locks/Unlocks the aspiration score of sims on the lot.",
        "TSCheatSystem: maxMotives          : Sets all the motives to maximum for all sims on the lot",
        "TSCheatSystem: motiveDecay         : motiveDecay [on/off]: Turns motive decay on/off for all sims on the lot ",
        "TSCheatSystem: setHour             : [0-23] sets the hour of the day"};
    result = runCheat(properties, nullptr, "help");
    CHECK(result.ok && result.output == help);
    const std::vector<std::string> unknown{"Unknown command"};
    result = runCheat(properties, nullptr, "motherlode");
    CHECK(!result.ok && result.output == unknown);
    CHECK(runCheat(properties, nullptr, "Expand").window == CheatResult::Window::Expand);
    CHECK(runCheat(properties, nullptr, "hide").window == CheatResult::Window::Hide);
    CHECK(runCheat(properties, nullptr, "   ").ok);
    CHECK(!runCheat(properties, nullptr, std::string(5000, 'a')).ok);
}

// maxMotives sets motives 0-15 to 100 on every Sim, all but 2 and 10-13. motiveDecay off sets person
// data 0x1D to 2 and on to 0. lockAspiration on sets person data 0xC1 to 1 and off to 0. Objects
// that aren't Sims are left alone, and without a lot nothing changes.
void lotCheatsSetEverySim() {
    using ts2::sim::runCheat;
    ts2::sim::CheatProperties properties;
    ts2::sim::World world;
    std::array<std::int16_t, 256> globals{};
    ts2::sim::CheatLot lot{world, globals};
    auto& first = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& fridge = world.create(kObjectGroup, typed(kFridgeGuid));
    auto& second = world.create(kObjectGroup, typed(0x51515152, ts2::sim::kPersonType));
    for (auto* sim : {&first, &second}) {
        sim->motives.fill(-50.0f);
    }
    CHECK(runCheat(properties, nullptr, "maxMotives").ok && first.motives[0] == -50.0f);
    CHECK(runCheat(properties, &lot, "MAXMOTIVES").ok);
    for (const auto* sim : {&first, &second}) {
        for (std::size_t motive = 0; motive < ts2::sim::kMotiveCount; ++motive) {
            const bool kept = motive == 2 || (motive >= 10 && motive <= 13);
            CHECK(sim->motives[motive] == (kept ? -50.0f : 100.0f));
        }
    }
    CHECK(runCheat(properties, &lot, "motiveDecay off").ok && first.personData[0x1D] == 2 && second.personData[0x1D] == 2);
    CHECK(fridge.personData[0x1D] == 0);
    CHECK(runCheat(properties, &lot, "motiveDecay true").ok && first.personData[0x1D] == 0);
    CHECK(runCheat(properties, &lot, "lockAspiration on").ok && first.personData[0xC1] == 1 && second.personData[0xC1] == 1);
    CHECK(runCheat(properties, &lot, "lockAspiration FALSE").ok && second.personData[0xC1] == 0);
    // Without a word the game does nothing and the line is done. A word it doesn't take fails.
    CHECK(runCheat(properties, &lot, "lockAspiration on").ok);
    CHECK(runCheat(properties, &lot, "lockAspiration").ok && first.personData[0xC1] == 1);
    CHECK(!runCheat(properties, &lot, "lockAspiration maybe").ok && first.personData[0xC1] == 1);
    // setHour sets the hour, minute 0, global 0x1D to 30 and the Time Of Day. 24 and up change nothing.
    globals[0x00] = 9;
    globals[0x05] = 41;
    globals[0x01] = 12;
    CHECK(runCheat(properties, &lot, "setHour 19").ok && globals[0x00] == 19 && globals[0x05] == 0 && globals[0x1D] == 30);
    CHECK(globals[0x04] == 2 && globals[0x01] == 12);
    CHECK(runCheat(properties, &lot, "sethour 6").ok && globals[0x00] == 6 && globals[0x04] == 3);
    CHECK(runCheat(properties, &lot, "setHour 24").ok && globals[0x00] == 6);
    CHECK(!runCheat(properties, &lot, "setHour noon").ok && globals[0x00] == 6);
    CHECK(runCheat(properties, nullptr, "setHour 3").ok);
}

// A Shift-click marks every part with the clicked object's lead. A debug line (interaction table flag
// 0x80) starts only when it isn't autonomy's, the object is marked and testingCheatsEnabled is on.
// Then no other line starts.
void debugLinesStartFromShiftClickedObjects() {
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& lead = world.create(kObjectGroup, typed(kFridgeGuid));
    auto& part = world.create(kObjectGroup, typed(kFridgeGuid));
    auto& other = world.create(kObjectGroup, typed(kFridgeGuid));
    lead.data[0x13] = lead.id;
    part.data[0x13] = lead.id;
    sim.personData[0x3A] = 30;  // an adult
    world.markShiftClick(part, true);
    CHECK(lead.shiftClicked && part.shiftClicked && !other.shiftClicked);

    ts2::ttab::Interaction plain;
    plain.flags = 0x40;  // adults
    ts2::ttab::Interaction debug = plain;
    debug.flags |= 0x80;
    CHECK(!ts2::sim::mayStartLine(debug, sim, lead, false, false));
    CHECK(ts2::sim::mayStartLine(plain, sim, lead, false, false));
    CHECK(ts2::sim::mayStartLine(debug, sim, lead, false, false, true));
    CHECK(!ts2::sim::mayStartLine(plain, sim, lead, false, false, true));
    world.markShiftClick(lead, false);
    CHECK(!lead.shiftClicked && !part.shiftClicked);
}

// A line with interaction table flag 0x4 (the lights' Turn On and Turn Off) runs at once when picked:
// as the Sim, on its object, with the menu's priority and not counted as autonomous meanwhile. The
// Sim's queue, temps and person data are as before. A line no longer offered does nothing.
void choicesFlaggedToRunAtOnceSkipTheQueue() {
    MemoryCodeSource code;
    auto lampType = typed(kFridgeGuid);
    lampType.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    ts2::ttab::InteractionTable table;
    table.interactions.resize(2);
    table.interactions[0].action = 0x1000;
    table.interactions[0].flags = 0x40 | 0x4;
    table.interactions[1].action = 0x1000;
    table.interactions[1].flags = 0x40;
    table.interactions[1].stringIndex = 1;
    code.addInteractionTable(kObjectGroup, 0x80, table);
    code.addInteractionStrings(kObjectGroup, 0x80, {"Turn Off", "Dispose"});
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kStackObjectData, 1, op::kAssign, owner::kMyPersonData, 0x21, 1, E),
                                         expr(owner::kStackObjectData, 2, op::kAssign, owner::kMyPersonData, 0xA6, 2, E),
                                         expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 9, T, E)}));

    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& lamp = world.create(kObjectGroup, lampType);
    sim.personData[0x3A] = 30;
    sim.personData[0x21] = 7;
    sim.personData[0xA6] = 1;
    sim.thread.temps[0] = 3;
    Vm vm(code, {}, &world);
    const auto menu = ts2::sim::pieMenu(vm, code, sim, lamp);
    CHECK(menu.choices.size() == 2);
    if (menu.choices.size() != 2) return;
    CHECK(ts2::sim::runsAtOnce(code, lamp, menu.choices[0]) && !ts2::sim::runsAtOnce(code, lamp, menu.choices[1]));
    const auto r = ts2::sim::runAtOnce(vm, code, world, sim, lamp, menu.choices[0]);
    CHECK(r.outcome == ts2::sim::Outcome::True);
    CHECK(lamp.data[1] == 50 && lamp.data[2] == 0);
    CHECK(sim.personData[0x21] == 7 && sim.personData[0xA6] == 1 && sim.thread.temps[0] == 3 && sim.queue.empty());

    lamp.data[1] = 0;
    lamp.data[0x57] = 1;  // the lamp's interactions are off now
    CHECK(ts2::sim::runAtOnce(vm, code, world, sim, lamp, menu.choices[0]).outcome == ts2::sim::Outcome::False && lamp.data[1] == 0);
}

// A multi-tile part whose definition names table 0xFFFF offers its master's table, with the texts
// of that table (a bed's foot shows "Relax" as its head does).
void multiTilePartsUseTheirMastersTexts() {
    constexpr std::uint32_t kPartGuid = 0x51515152;
    MemoryCodeSource code;
    auto headType = typed(kFridgeGuid);
    headType.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    auto footType = typed(kPartGuid);
    footType.fields[ts2::objd::field::kInteractionTableId] = 0xFFFF;
    code.addObjectType(kObjectGroup, headType);
    code.addObjectType(kObjectGroup, footType);
    code.setMultiTileMaster(kPartGuid, kFridgeGuid);
    ts2::ttab::InteractionTable table;
    table.interactions.resize(1);
    table.interactions[0].action = 0x1000;
    table.interactions[0].flags = 0x40;
    code.addInteractionTable(kObjectGroup, 0x80, table);
    code.addInteractionStrings(kObjectGroup, 0x80, {"Relax"});
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 1, T, E)}));

    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& head = world.create(kObjectGroup, headType);
    auto& foot = world.create(kObjectGroup, footType);
    sim.personData[0x3A] = 30;
    Vm vm(code, {}, &world);
    for (auto* part : {&head, &foot}) {
        const auto menu = ts2::sim::pieMenu(vm, code, sim, *part);
        CHECK(menu.choices.size() == 1 && menu.choices[0].name == "Relax");
        CHECK(ts2::sim::interactionName(code, *part, 0) == "Relax");
    }
}

// A check tree reads and changes the advertisement of the line it tests for the Sim's age group (a
// copy: the table stays as it is). The stack object's functions and slots by temp are read too.
void checkTreeOwnersReadTheTestedLine() {
    MemoryCodeSource code;
    ts2::ttab::Interaction line;
    line.motives.resize(2);
    line.motives[0].resize(3);
    line.motives[0][2] = {30, 5, 4};  // adults: min, range, personality type
    line.motives[1].resize(3);
    line.motives[1][2] = {60, 0, 0};  // children
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 1, op::kAssign, ts2::sim::owner::kCheckTreeAdMin, 2, 1, E),
        expr(owner::kTemp, 2, op::kAssign, ts2::sim::owner::kCheckTreeAdPersonality, 2, 2, E),
        expr(ts2::sim::owner::kCheckTreeAdRange, 9, op::kAssign, owner::kLiteral, 40, 3, E),
        expr(owner::kTemp, 3, op::kAssign, ts2::sim::owner::kCheckTreeAdRange, 9, 4, E),
        expr(owner::kTemp, 4, op::kAssign, ts2::sim::owner::kStackObjectHasFunction, 1, 5, E),
        expr(owner::kTemp, 5, op::kAssign, ts2::sim::owner::kStackObjectHasFunction, 0, 6, E),
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 1, 7, E),
        expr(owner::kTemp, 6, op::kAssign, ts2::sim::owner::kStackObjectSlotByTemp, 0, T, E),
    }));
    code.add(kObjectGroup, 0x1001, bhav({expr(owner::kTemp, 1, op::kAssign, ts2::sim::owner::kCheckTreeAdMin, 16, T, E)}));
    code.addFunctions(kObjectGroup, {{0x10, 0}, {0x11, 0x12}});
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& counter = world.create(kObjectGroup, typed(kFridgeGuid), 2);
    code.addObjectType(kObjectGroup, typed(kFridgeGuid));
    counter.slots[1] = sim.id;
    sim.personData[0x3A] = 30;
    Vm vm(code, {}, &world);
    std::vector<ts2::sim::OfferedAction> offered;
    CHECK(vm.runCheck(sim, kObjectGroup, 0x1000, counter.id, {}, line, offered).outcome == Outcome::True);
    const auto& t = sim.thread.temps;
    CHECK(t[1] == 30 && t[2] == 4 && t[3] == 40 && t[4] == 1 && t[5] == 0 && t[6] == sim.id);
    CHECK(line.motives[0].size() == 3);  // the table's own line is left alone
    sim.personData[0x3A] = 10;
    CHECK(vm.runCheck(sim, kObjectGroup, 0x1000, counter.id, {}, line, offered).outcome == Outcome::True && t[1] == 60);
    CHECK(vm.runCheck(sim, kObjectGroup, 0x1001, counter.id, {}, line, offered).reason == "advertisement motive out of range");
    CHECK(vm.runCheck(counter, kObjectGroup, 0x1000, counter.id, {}, line, offered).reason ==
          "advertisement for something that is not a human Sim");
}

// Add/Change the Action String: the text (counted from 1) from table 0x12E (own group, then
// semi-global, then global), or an icon. A missing text is an error.
void actionStringSetsTextAndIcon() {
    auto text = [](std::uint16_t index, std::uint8_t flags) {
        Instruction in;
        in.opcode = 0x0032;
        in.trueTarget = T;
        in.falseTarget = E;
        in.operands[2] = flags;
        in.operands[14] = static_cast<std::uint8_t>(index);
        return in;
    };
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x12E, {"Eat", "Sleep"});
    code.addStrings(kObjectGroup, 0x12E, {"", "Snack"});
    code.add(kObjectGroup, 0x1000, bhav({text(2, 0)}));
    code.add(kObjectGroup, 0x1001, bhav({text(1, 0)}));
    code.add(kObjectGroup, 0x1002, bhav({text(2, 0x04)}));
    code.add(kObjectGroup, 0x1003, bhav({text(8, 0)}));
    code.add(kObjectGroup, 0x1005, bhav({text(0, 0)}));
    code.add(kObjectGroup, 0x1004, bhav({[] {
        Instruction in;
        in.opcode = 0x0032;
        in.trueTarget = T;
        in.falseTarget = E;
        in.operands[9] = 1;
        in.operands[11] = owner::kLiteral;
        in.operands[12] = 2;
        return in;
    }()}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    world.create(kObjectGroup, typed(kFridgeGuid));
    Vm vm(code, {}, &world);
    // With nothing running there is nothing to change.
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::True && sim.actionString.empty());
    sim.running = ts2::sim::QueuedInteraction{};
    sim.running->id = 1;
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::True && sim.actionString == "Snack");
    CHECK(vm.run(sim, kObjectGroup, 0x1001).outcome == Outcome::True && sim.actionString == "Eat");
    CHECK(vm.run(sim, kObjectGroup, 0x1002).outcome == Outcome::True && sim.actionString == "Sleep");
    const RunResult missing = vm.run(sim, kObjectGroup, 0x1003);
    CHECK(missing.outcome == Outcome::Error && missing.reason == "missing action string in table");
    CHECK(vm.run(sim, kObjectGroup, 0x1005).reason == "missing action string in table");  // counted from 1
    CHECK(vm.run(sim, kObjectGroup, 0x1004).outcome == Outcome::True && sim.actionIconObject == 2);
    // A candidate's check tree sets the candidate's icon instead, picture 0 counting as 1.
    sim.actionIconObject = 0;
    ts2::sim::InteractionIcon icon;
    std::vector<ts2::sim::OfferedAction> offered;
    CHECK(vm.runCheck(sim, kObjectGroup, 0x1004, 0, {}, ts2::ttab::Interaction{}, offered, &icon).outcome == Outcome::True);
    CHECK(icon.object == 2 && icon.guid == 0 && icon.index == 1 && sim.actionIconObject == 0);
    // The text's tokens are filled in from the tree, also for a line a check tree offers.
    code.addStrings(kObjectGroup, 0x12E, {"", "Snack", "Eat $Local:0 $Unknown"});
    code.add(kObjectGroup, 0x1006, bhav({text(3, 0)}, 0, 1));
    CHECK(vm.run(sim, kObjectGroup, 0x1006).outcome == Outcome::True && sim.actionString == "Eat 0 $Unknown");
    offered.clear();
    CHECK(vm.runCheck(sim, kObjectGroup, 0x1006, 0, {}, ts2::ttab::Interaction{}, offered, &icon).outcome == Outcome::True);
    CHECK(offered.size() == 1 && offered[0].name == "Eat 0 $Unknown");
}

Instruction wantSatisfy(std::uint32_t guid, std::uint8_t argOwner, std::uint16_t arg, std::uint16_t value,
                        std::uint16_t t, std::uint16_t f) {
    Instruction in;
    in.opcode = 0x007C;
    in.trueTarget = t;
    in.falseTarget = f;
    in.operands[0] = owner::kStackObjectId;
    for (std::size_t i = 0; i < 4; ++i) {
        in.operands[3 + i] = static_cast<std::uint8_t>(guid >> (8 * i));
    }
    in.operands[7] = argOwner;
    in.operands[8] = static_cast<std::uint8_t>(arg);
    in.operands[9] = static_cast<std::uint8_t>(arg >> 8);
    in.operands[10] = owner::kLiteral;
    in.operands[11] = static_cast<std::uint8_t>(value);
    in.operands[12] = static_cast<std::uint8_t>(value >> 8);
    return in;
}

// The wants manager offers a Sim new wants from the want trees: a satisfied trigger want in the
// history leads to its node's visible children. A child's want takes the first empty place with
// its node's points, a Sim argument from the trigger, and a score between the node's least and most
// times how far its lifetime has run. A trigger older than the child's lifetime offers nothing.
void wantTreesOfferNewWants() {
    constexpr std::uint32_t kSimGuid = 0x51515151, kTrigger = 0x0DDD0001, kChild = 0x0DDD0002;
    MemoryCodeSource code;
    ts2::sim::World world;
    world.create(kObjectGroup, typed(kSimGuid, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {27, 31};
    hood->neighbors[27].guid = kSimGuid;
    hood->neighbors[31].guid = 0x52525252;
    ts2::swaf::Want held;
    held.id = 27;
    held.guid = 0x0DDD0009;
    held.flags = 1;
    hood->wants[27] = {held};
    ts2::swaf::Want trigger;
    trigger.id = 27;
    trigger.guid = kTrigger;
    trigger.type = 1;
    trigger.argument = 31;
    trigger.tail = {1000, 0, 0};
    hood->wantHistories[27].time = 1000;
    hood->wantHistories[27].records[kTrigger] = {trigger};
    world.setNeighborhood(hood);
    auto defs = std::make_shared<ts2::content::WantDefinitions>();
    using ts2::content::WantArgument;
    using ts2::content::WantOperator;
    defs->byGuid[kTrigger] = {kTrigger, "trigger", WantArgument::Sim, WantOperator::None, 0, ""};
    defs->byGuid[kChild] = {kChild, "talk", WantArgument::Sim, WantOperator::None, 750, ""};
    defs->byGuid[kChild].influence = 40;
    defs->byGuid[0x0DDD0009] = {0x0DDD0009, "held", WantArgument::None, WantOperator::None, 100, ""};
    world.setWantDefinitions(defs);
    auto trees = std::make_shared<std::vector<ts2::want_trees::Tree>>(1);
    auto& tree = trees->front();
    tree.id = 0x6DCE563C;
    tree.nodes.resize(2);
    tree.nodes[0].wantId = kTrigger;
    tree.nodes[0].children = {1};
    tree.nodes[1].wantId = kChild;
    tree.nodes[1].parent = 0;
    tree.nodes[1].visible = true;
    tree.nodes[1].inheritSubject = true;
    tree.nodes[1].lifetime = 2;  // hours
    tree.nodes[1].minScore = 10;
    tree.nodes[1].maxScore = 50;
    world.setWantTrees(trees);

    // One stage a tick: the pick, the candidates, the scores, the fill, then the end.
    ts2::sim::queueWantRefill(world, 27, 3);
    const auto& wants = world.neighborWants(27);
    for (int tick = 1; tick <= 3; ++tick) {
        ts2::sim::stepWants({world, code, vm, 1});
        CHECK((wants.size() < 2 || wants[1].guid == 0) && world.wantsState().queue.empty() && ts2::sim::wantRefillWaiting(world, 27));
    }
    ts2::sim::stepWants({world, code, vm, 1});
    CHECK(wants.size() == 4 && wants[0].guid == 0x0DDD0009 && wants[1].guid == kChild);
    CHECK(wants[1].type == 1 && wants[1].argument == 31 && wants[1].tail.size() == 3 && wants[1].tail[1] == 750 && wants[1].tail[2] == 40);
    CHECK(wants[2].guid == 0 && !ts2::sim::wantRefillWaiting(world, 27) && world.wantsState().clock == 8);
    // The next Sim is taken a tick after the fill.
    ts2::sim::queueWantRefill(world, 27, 3);
    ts2::sim::stepWants({world, code, vm, 1});
    CHECK(world.wantsState().queue.size() == 1 && !world.wantsState().refill);

    // Two hours and more after the trigger, the child is past its lifetime.
    world.neighborWants(27)[1] = {};
    ts2::sim::wantHistory(world, 27).time = 1000 + 2 * 3600;
    for (int tick = 0; tick < 5; ++tick) {
        ts2::sim::stepWants({world, code, vm, 1});
    }
    CHECK(world.neighborWants(27)[1].guid == 0);
}

// A want offered again goes back to its place in the lists saved when the Sim was queued, even when
// the lists changed since. Only a Sim not queued yet has them saved, and only for levels 0 and 1.
void wantRefillUsesTheSavedLists() {
    ts2::sim::World world;
    std::vector<ts2::swaf::Want> wants(4), fears(3);
    ts2::swaf::Want w;
    w.id = 27;
    w.guid = 0x0DDD0001;
    w.tail = {500, 0, 0};
    wants[2] = w;
    world.neighborWants(27) = wants;
    world.neighborFears(27) = fears;
    ts2::sim::queueWantRefill(world, 27, 3);
    CHECK(!world.wantsState().savedLists.contains(27));
    world.wantsState().queue.clear();
    ts2::sim::queueWantRefill(world, 27, 1);
    CHECK(world.wantsState().savedLists.at(27).first[2].guid == 0x0DDD0001);
    world.neighborWants(27)[2] = {};
    ts2::sim::queueWantRefill(world, 27, 0);
    CHECK(world.wantsState().savedLists.at(27).first[2].guid == 0x0DDD0001 && world.wantsState().queue.at(27) == 0);

    world.wantsState().queue.clear();
    world.neighborWants(27)[0] = w;
    ts2::sim::saveWantsForRefill(world, 27);
    CHECK(world.wantsState().savedLists.at(27).first[0].tail[0] == 500 && world.neighborWants(27)[0].tail[0] == 0);
}

// Want Satisfy matches the stack object's Sim's current wants: by want GUID, then by argument
// (0 in the want accepts any) and the definition's operator. Matched wants leave the list.
void wantSatisfyMatchesCurrentWants() {
    constexpr std::uint32_t kSimGuidA = 0x51515151;
    constexpr std::uint32_t kAnySimWant = 0xADA6814F, kSkillWant = 0x2DCA4468, kLevelWant = 0x0DDDDDDD,
                            kListWant = 0x0EEEEEEE;
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 1, 1, E),
        wantSatisfy(kAnySimWant, owner::kStackObjectId, 0, 0, T, F),
    }));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 1, 1, E),
        wantSatisfy(kSkillWant, owner::kLiteral, 15, 7, T, F),
    }));
    code.add(kObjectGroup, 0x1002, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 1, 1, E),
        wantSatisfy(kLevelWant, owner::kLiteral, 6, 0, T, F),
    }));
    code.add(kObjectGroup, 0x1003, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 1, 1, E),
        wantSatisfy(0x0BADF00D, owner::kLiteral, 0, 0, T, F),
    }));
    code.add(kObjectGroup, 0x1004, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 1, 1, E),
        wantSatisfy(kListWant, owner::kLiteral, 0, 0, T, F),
    }));

    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(kSimGuidA, ts2::sim::kPersonType));
    CHECK(sim.id == 1);
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::Unsupported);

    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {27};
    hood->neighbors[27].guid = kSimGuidA;
    auto want = [](std::uint32_t guid, std::uint8_t type, std::int32_t argument, std::int16_t value) {
        ts2::swaf::Want w;
        w.id = 27;
        w.guid = guid;
        w.type = type;
        w.argument = argument;
        w.value = value;
        return w;
    };
    hood->wants[27] = {want(kAnySimWant, 1, 0, 0), want(kSkillWant, 4, 15, 8), want(kLevelWant, 0, 0, 5)};
    world.setNeighborhood(hood);
    auto defs = std::make_shared<ts2::content::WantDefinitions>();
    using ts2::content::WantArgument;
    using ts2::content::WantOperator;
    defs->byGuid[kAnySimWant] = {kAnySimWant, "any Sim", WantArgument::Sim, WantOperator::None, 3000, ""};
    defs->byGuid[kSkillWant] = {kSkillWant, "skill", WantArgument::Skill, WantOperator::None, 2500, ""};
    defs->byGuid[kLevelWant] = {kLevelWant, "level", WantArgument::None, WantOperator::GreaterThanOrEquals, 500, ""};
    defs->byGuid[kListWant] = {kListWant, "family", WantArgument::None, WantOperator::None, 500, "CT - nID Array - Family"};
    world.setWantDefinitions(defs);

    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);   // a want for any Sim, satisfied by itself
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::False);  // ...and gone from the list
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::False);  // skill 15 but level 7, the want says 8
    CHECK(vm.run(sim, 0x1002).outcome == Outcome::True);   // 6 >= 5
    // Satisfied wants leave empty places (GUID 0) in the Sim's 4 slots, every satisfy goes into the
    // history, and a satisfied want queues the Sim for new wants.
    const auto filled = [](const std::vector<ts2::swaf::Want>& list) { return std::count_if(list.begin(), list.end(), [](const auto& w) { return w.guid != 0; }); };
    CHECK(world.neighborWants(27).size() == 4 && filled(world.neighborWants(27)) == 1 && hood->wants[27].size() == 3);
    CHECK(world.neighborWants(27)[1].guid == kSkillWant);
    const auto& history = ts2::sim::wantHistory(world, 27).records;
    CHECK(history.at(kAnySimWant).size() == 2 && history.at(kSkillWant).size() == 1 && history.at(kLevelWant).size() == 1);
    CHECK(ts2::sim::recordTime(history.at(kAnySimWant)[0]) < ts2::sim::recordTime(history.at(kAnySimWant)[1]));
    CHECK(history.at(kAnySimWant)[0].tail[1] == 0 && world.wantsState().queue.at(27) == 3);
    const RunResult unknown = vm.run(sim, 0x1003);
    CHECK(unknown.outcome == Outcome::Error && unknown.reason == "data number out of range");
    CHECK(vm.run(sim, 0x1004).outcome == Outcome::False);  // no Wants - Controller on the lot: no Sims
}

// The age levels of the want tuning: an age with an unknown name goes on with the previous age's table, a threshold
// with no known name is stored as the first one, a missing score is the lowest score, scores keep to the game's range
// and every table ends strictly increasing.
void wantTuningLevelsFollowTheGamesLoader() {
    const auto levels = ts2::content::parseAspirationLevels(
        "<levels><age name=\"Adult\">"
        "<threshold name=\"rock bottom\" score=\"-9999999\"/><threshold name=\"severe failure\" score=\"0x10\"/>"
        "<threshold name=\"neutral\" score=\"5\"/><threshold name=\"bronze\" score=\"3000\"/>"
        "<threshold name=\"silver\" score=\"4000\"/><threshold name=\"gold\" score=\"9999999\"/>"
        "<threshold name=\"perfect\" score=\"9999999\"/></age>"
        "<age name=\"Nobody\"><threshold name=\"neutral\" score=\"50\"/><threshold name=\"odd\" score=\"-4000\"/>"
        "<threshold name=\"perfect\"/></age></levels>");
    const auto& adult = levels.at(0);
    CHECK(adult[0] == -4000);  // the first threshold, then the unnamed one
    CHECK(adult[1] == 16 && adult[2] == 50 && adult[3] == 3000 && adult[4] == 4000);
    CHECK(adult[5] == 327670 && adult[6] == 327671);  // the range's top, then one above it
    CHECK(levels.size() == 1);

    // An age named twice keeps what its first element stored.
    const auto twice = ts2::content::parseAspirationLevels(
        "<levels><age name=\"Teen\"><threshold name=\"gold\" score=\"900\"/></age>"
        "<age name=\"Teen\"><threshold name=\"bronze\" score=\"100\"/></age></levels>");
    CHECK(twice.at(3)[3] == 100 && twice.at(3)[5] == 900);
}

// The game's sort puts the best scores first and keeps its own order for equal scores. Twenty equal scores come out
// in the reverse order, as the first split swaps the two ends inwards and the insertion sort leaves equals alone.
// Ranges of 16 or fewer keep the order of equal scores.
void gameSortKeepsTheGamesOrderOfEqualScores() {
    struct Entry {
        float score;
        int id;
    };
    const auto better = [](const Entry& a, const Entry& b) { return b.score < a.score; };

    std::vector<Entry> equal;
    for (int i = 0; i < 20; ++i) {
        equal.push_back({5.0f, i});
    }
    ts2::sim::gameSort(equal.begin(), equal.end(), better);
    bool reversed = true;
    for (int i = 0; i < 20; ++i) {
        reversed = reversed && equal[static_cast<std::size_t>(i)].id == 19 - i;
    }
    CHECK(reversed);

    // A list of 491 want candidates as the game held it, 441 of them with a score another one has, and the order the
    // game's own sort left them in.
    std::vector<Entry> captured;
    for (std::size_t i = 0; i < std::size(ts2::test_data::kWantScores); ++i) {
        captured.push_back({std::bit_cast<float>(ts2::test_data::kWantScores[i]), static_cast<int>(i)});
    }
    ts2::sim::gameSort(captured.begin(), captured.end(), better);
    bool sameAsGame = captured.size() == std::size(ts2::test_data::kWantsAfter);
    for (std::size_t i = 0; sameAsGame && i < captured.size(); ++i) {
        sameAsGame = captured[i].id == ts2::test_data::kWantsAfter[i];
    }
    CHECK(sameAsGame);

    std::vector<Entry> small;
    for (int i = 0; i < 16; ++i) {
        small.push_back({static_cast<float>(i % 3), i});
    }
    ts2::sim::gameSort(small.begin(), small.end(), better);
    bool stable = true;
    for (std::size_t i = 1; i < small.size(); ++i) {
        stable = stable && small[i - 1].score >= small[i].score &&
                 (small[i - 1].score != small[i].score || small[i - 1].id < small[i].id);
    }
    CHECK(stable);

    // Any size and any ties: sorted, nothing lost, and the same input gives the same order.
    std::uint32_t seed = 12345;
    for (int size : {0, 1, 2, 17, 33, 100, 500, 3000}) {
        std::vector<Entry> list;
        for (int i = 0; i < size; ++i) {
            seed = seed * 1664525u + 1013904223u;
            list.push_back({static_cast<float>((seed >> 16) % 7), i});
        }
        auto again = list;
        ts2::sim::gameSort(list.begin(), list.end(), better);
        ts2::sim::gameSort(again.begin(), again.end(), better);
        bool sorted = true, same = true;
        std::vector<int> seen(static_cast<std::size_t>(size), 0);
        for (std::size_t i = 0; i < list.size(); ++i) {
            sorted = sorted && (i == 0 || list[i - 1].score >= list[i].score);
            same = same && list[i].id == again[i].id;
            ++seen[static_cast<std::size_t>(list[i].id)];
        }
        CHECK(sorted && same && std::count(seen.begin(), seen.end(), 1) == size);
    }
}

// The sort of an autonomous Sim's choices is the game's quicksort. Three equal scores come out as 1, 2, 0, as it moves
// the last-going one to the end each time. Nine equal scores stay as they are, as it finds nothing to swap.
void gameQsortKeepsTheGamesOrderOfEqualScores() {
    struct Entry {
        float score;
        int id;
    };
    const auto order = [](const Entry& a, const Entry& b) { return ts2::sim::compareChoiceScores(a.score, b.score); };
    const auto ids = [](const std::vector<Entry>& list) {
        std::vector<int> out;
        for (const Entry& e : list) {
            out.push_back(e.id);
        }
        return out;
    };

    std::vector<Entry> three = {{2.0f, 0}, {2.0f, 1}, {2.0f, 2}};
    ts2::sim::gameQsort(three.begin(), three.end(), order);
    CHECK(ids(three) == (std::vector<int>{1, 2, 0}));

    std::vector<Entry> nine;
    for (int i = 0; i < 9; ++i) {
        nine.push_back({2.0f, i});
    }
    ts2::sim::gameQsort(nine.begin(), nine.end(), order);
    CHECK(ids(nine) == (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8}));

    // Two lists as the game held them, with their order after the game's own sort.
    for (const auto& captured : ts2::test_data::kSortedChoices) {
        std::vector<Entry> list;
        for (int i = 0; i < captured.count; ++i) {
            list.push_back({std::bit_cast<float>(captured.scores[i]), i});
        }
        ts2::sim::gameQsort(list.begin(), list.end(), order);
        bool sameAsGame = true;
        for (int i = 0; i < captured.count; ++i) {
            sameAsGame = sameAsGame && list[static_cast<std::size_t>(i)].id == captured.after[i];
        }
        CHECK(sameAsGame);
    }

    // Scores within a ten millionth count as equal, and the best come first otherwise.
    CHECK(ts2::sim::compareChoiceScores(0.0f, 5.0e-8f) == 0);
    CHECK(ts2::sim::compareChoiceScores(0.0f, 2.0e-7f) == 1);
    CHECK(ts2::sim::compareChoiceScores(2.0e-7f, 0.0f) == -1);
    std::vector<Entry> close = {{1.0f, 0}, {5.0f, 1}, {1.0f, 2}};
    ts2::sim::gameQsort(close.begin(), close.end(), order);
    CHECK(close[0].id == 1);

    // Any size and any ties: best first, nothing lost, and the same input gives the same order.
    std::uint32_t seed = 777;
    for (int size : {0, 1, 2, 8, 9, 17, 100, 700, 5000}) {
        std::vector<Entry> list;
        for (int i = 0; i < size; ++i) {
            seed = seed * 1664525u + 1013904223u;
            list.push_back({static_cast<float>((seed >> 16) % 9), i});
        }
        auto again = list;
        ts2::sim::gameQsort(list.begin(), list.end(), order);
        ts2::sim::gameQsort(again.begin(), again.end(), order);
        bool sorted = true, same = true;
        std::vector<int> seen(static_cast<std::size_t>(size), 0);
        for (std::size_t i = 0; i < list.size(); ++i) {
            sorted = sorted && (i == 0 || list[i - 1].score >= list[i].score);
            same = same && list[i].id == again[i].id;
            ++seen[static_cast<std::size_t>(list[i].id)];
        }
        CHECK(sorted && same && std::count(seen.begin(), seen.end(), 1) == size);
    }
}

// A satisfied want gives the Sim its aspiration points, within its age's levels, and its feedbackTree runs on the
// Wants - Controller with the Sim, the points and the score before them as params. It runs for a failed match too,
// with nothing given, and a person data 0xC1 holds the score but not the lifetime total.
void wantSatisfyGivesPointsAndRunsItsFeedback() {
    constexpr std::uint32_t kSimGuid = 0x51515151, kControllerGuid = 0x6D9E58F0, kCheckTreesGuid = 0x8D92DD2E;
    constexpr std::uint32_t kCheckTreesGroup = 0x7F739728, kWant = 0x0EEEEEEE;
    constexpr std::size_t kLifeStage = 0x3A, kLevel = 0xA0, kLifetime = 0xA1, kScore = 0xA2, kHeld = 0xC1;
    MemoryCodeSource code;
    code.addObjectType(kCheckTreesGroup, typed(kCheckTreesGuid));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(kSimGuid, ts2::sim::kPersonType));
    auto& controller = world.create(kObjectGroup, typed(kControllerGuid));
    sim.personData[kLifeStage] = 20;
    sim.personData[kScore] = 100;
    sim.personData[kLifetime] = 5;
    Behavior tree = bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kParam, 0, 1, E),
        expr(owner::kTemp, 1, op::kAssign, owner::kParam, 8, 2, E),
        expr(owner::kTemp, 2, op::kAssign, owner::kParam, 9, 3, E),
        expr(owner::kTemp, 3, op::kAssign, owner::kParam, 11, 4, E),
        expr(owner::kTemp, 4, op::kAssign, owner::kParam, 12, 5, E),
        expr(owner::kTemp, 5, op::kAssign, owner::kParam, 14, T, E),
    }, 15);
    tree.name = "CT - Handle Feedback";
    code.add(kCheckTreesGroup, 0x1003, tree);
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 1, 1, E),
        wantSatisfy(kWant, owner::kLiteral, 0, 0, T, F),
    }));

    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {27};
    hood->neighbors[27].guid = kSimGuid;
    ts2::swaf::Want slot;
    slot.id = 27;
    slot.guid = kWant;
    slot.tail = {0, 750, 40};
    hood->wants[27] = {slot};
    world.setNeighborhood(hood);
    auto defs = std::make_shared<ts2::content::WantDefinitions>();
    defs->byGuid[kWant] = {kWant, "want", ts2::content::WantArgument::None, ts2::content::WantOperator::None, 750, ""};
    defs->byGuid[kWant].feedbackTree = "CT - Handle Feedback";
    defs->aspirationLevels[ts2::sim::wantAgeCode(20)] = {-2000, -1000, 0, 500, 1000, 1500, 2000};
    world.setWantDefinitions(defs);

    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(sim.personData[kScore] == 175 && sim.personData[kLifetime] == 80);
    CHECK(sim.personData[kLevel] == 88);  // 1750 is half way from 1500 (75) to 2000 (100)
    const auto& t = controller.thread.temps;
    CHECK(t[0] == 27 && t[1] == 1 && t[2] == 750 && t[3] == 100 && t[5] == 40);
    CHECK(t[4] == 50);  // the level of the 1000 it had before

    // Nothing left to match: nothing is given, and the tree is told so.
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::False);
    CHECK(sim.personData[kScore] == 175 && t[1] == 0 && t[2] == 0);

    // The score is held but the lifetime total still grows.
    world.neighborWants(27)[0] = slot;
    sim.personData[kHeld] = 1;
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(sim.personData[kScore] == 175 && sim.personData[kLifetime] == 155);

    // The score stops at the age's highest level.
    world.neighborWants(27)[0] = slot;
    world.neighborWants(27)[0].tail[1] = 30000;
    sim.personData[kHeld] = 0;
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(sim.personData[kScore] == 200 && sim.personData[kLevel] == 100);
}

// A want with a Sim list tree: the tree (a "Wants - Check Trees" one, by name) runs on the Wants -
// Controller with the Sim as param 0 and leaves in temp 0 an object whose array 0 lists the Sims.
// Each listed Sim's matching wants are satisfied, and the primitive is false.
void wantSatisfyWithASimListSatisfiesTheListedSims() {
    constexpr std::uint32_t kSimGuid = 0x51515151, kControllerGuid = 0x6D9E58F0, kCheckTreesGuid = 0x8D92DD2E;
    constexpr std::uint32_t kCheckTreesGroup = 0x7F739728, kListWant = 0x0EEEEEEE;
    MemoryCodeSource code;
    code.addObjectType(kCheckTreesGroup, typed(kCheckTreesGuid));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(kSimGuid, ts2::sim::kPersonType));
    auto& controller = world.create(kObjectGroup, typed(kControllerGuid));
    auto& list = world.create(kObjectGroup, typed(0x0BADF00D));
    list.arrays = {{28, 29}};
    Behavior tree = bhav({
        expr(owner::kTemp, 1, op::kAssign, owner::kParam, 0, 1, E),
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, static_cast<std::uint16_t>(list.id), T, E),
    }, 1);
    tree.name = "CT - nID Array - Sims I Know";
    code.add(kCheckTreesGroup, 0x1096, tree);
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 1, 1, E),
        wantSatisfy(kListWant, owner::kLiteral, 0, 0, T, F),
    }));

    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {27, 28, 29, 30};
    hood->neighbors[27].guid = kSimGuid;
    // A Sim's wants are satisfied only when it has person data.
    for (const std::int16_t id : {std::int16_t{28}, std::int16_t{29}, std::int16_t{30}}) {
        hood->neighbors[id].guid = 0x52525200u + static_cast<std::uint32_t>(id);
    }
    auto want = [](std::int16_t id) {
        ts2::swaf::Want w;
        w.id = static_cast<std::uint16_t>(id);
        w.guid = kListWant;
        return w;
    };
    hood->wants[27] = {want(27)};
    hood->wants[28] = {want(28)};
    hood->wants[29] = {want(29)};
    hood->wants[30] = {want(30)};
    world.setNeighborhood(hood);
    auto defs = std::make_shared<ts2::content::WantDefinitions>();
    defs->byGuid[kListWant] = {kListWant, "met", ts2::content::WantArgument::None, ts2::content::WantOperator::None, 500,
                               "CT - nID Array - Sims I Know"};
    world.setWantDefinitions(defs);

    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::False);
    CHECK(controller.thread.temps[1] == 27);  // the tree ran on the controller with the Sim as param 0
    const auto filled = [](const std::vector<ts2::swaf::Want>& list) { return std::count_if(list.begin(), list.end(), [](const auto& w) { return w.guid != 0; }); };
    CHECK(filled(world.neighborWants(28)) == 0 && filled(world.neighborWants(29)) == 0);
    CHECK(filled(world.neighborWants(27)) == 1 && filled(world.neighborWants(30)) == 1);
}

void motivesAreClampedAndNeedAPerson() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kMyMotives, 5, op::kAssign, owner::kLiteral, 60, 1, E),
        expr(owner::kMyMotives, 5, op::kAdd, owner::kLiteral, 70, 2, E),
        expr(owner::kStackObjectMotives, 3, op::kAssign, owner::kLiteral, static_cast<std::uint16_t>(-500), 3, E),
        expr(owner::kTemp, 2, op::kAssign, owner::kLiteral, 3, 4, E),
        expr(owner::kTemp, 0, op::kAssign, owner::kStackObjectMotiveByTemp, 2, T, E),
    }));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& other = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    sim.thread.stackObjectId = other.id;
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(sim.motives[5] == 100.0f);
    CHECK(other.motives[3] == -100.0f);
    CHECK(sim.thread.temps[0] == -100);

    MemoryCodeSource bad;
    bad.add(kObjectGroup, 0x1000, bhav({expr(owner::kMyMotives, 16, op::kAssign, owner::kLiteral, 1, T, E)}));
    Vm badVm(bad, {}, &world);
    CHECK(badVm.run(sim, 0x1000).outcome == Outcome::Error);
}

} // namespace

std::vector<Test> vmInteractionTests() {
    return {
        {"set skin to the Sim's own is true", setSkinToTheSimsOwnIsTrue},
        {"recalc wants and Debug carry on", recalcWantsAndDebugCarryOn},
        {"Remove Family Tie ends the tie both ways", removeFamilyTieEndsTheTieBothWays},
        {"Unlink Character deletes its Sims", unlinkCharacterDeletesItsSims},
        {"Add Child ties the baby to its parents", addChildTiesTheBabyToItsParents},
        {"remapped neighbor id comes from the remap table", remappedNeighborIdComesFromTheRemapTable},
        {"Get Outfits Information asks the Sim's looks", outfitsInformationAsksTheSimsLooks},
        {"fits with footprint tests the other footprint", fitsWithFootprintTestsTheOtherFootprint},
        {"Build and Buy calls list the running object", buildBuyCallsListTheRunningObject},
        {"abort interactions tells every Sim", abortInteractionsTellsEverySim},
        {"multi-tile parts use their master's interaction table", multiTilePartsUseTheirMastersTable},
        {"advertisers list objects by motive", advertisersListObjectsByMotive},
        {"Find Best Interaction picks the happiest near line", findBestInteractionPicksTheHappiestNearLine},
        {"Find Best Interaction works for pets", findBestInteractionWorksForPets},
        {"Find Best Interaction keeps visitors to their lines", findBestInteractionKeepsVisitorsToTheirLines},
        {"Find Best Interaction remembers its picks", findBestInteractionRemembersItsPicks},
        {"Find Best Interaction avoids engine gaps for a while", findBestInteractionAvoidsEngineGaps},
        {"Find Best Interaction weighs the social bias", findBestInteractionWeighsTheSocialBias},
        {"Find Best Interaction weighs offered lines", findBestInteractionWeighsOfferedLines},
        {"Want Satisfy gives points and runs its feedback", wantSatisfyGivesPointsAndRunsItsFeedback},
        {"Want tuning levels follow the game's loader", wantTuningLevelsFollowTheGamesLoader},
        {"The game's sort keeps its order of equal scores", gameSortKeepsTheGamesOrderOfEqualScores},
        {"The game's quicksort keeps its order of equal scores", gameQsortKeepsTheGamesOrderOfEqualScores},
        {"check tree kinds find trees that change the line", checkTreeKindsFindTreesThatChangeTheLine},
        {"Find Best Interaction scores what check trees advertise", findBestInteractionScoresWhatCheckTreesAdvertise},
        {"Want Satisfy matches current wants", wantSatisfyMatchesCurrentWants},
        {"want refills use the saved lists", wantRefillUsesTheSavedLists},
        {"want trees offer new wants", wantTreesOfferNewWants},
        {"Want Satisfy with a Sim list satisfies the listed Sims", wantSatisfyWithASimListSatisfiesTheListedSims},
        {"Push Interaction queues for the Sim", pushInteractionQueuesForTheSim},
        {"Push Interaction with no such partner has no icon", pushWithNoSuchPartnerHasNoIcon},
        {"Push Interaction runs flag 0x4 lines at once", pushRunsFlag4LinesAtOnce},
        {"a pushed line that waits leaves the Sim to reset", pushedLineThatWaitsLeavesTheSimToReset},
        {"paired multi-tile parts hand their interactions on", pairedPartsHandTheirInteractionsOn},
        {"check trees nest only so deep", checkTreesNestOnlySoDeep},
        {"Idle for Input starts queued interactions", idleForInputStartsQueuedInteractions},
        {"Idle for Input tests what starts", idleForInputTestsWhatStarts},
        {"Push Interaction limits each queue", pushInteractionLimitsEachQueue},
        {"queued interactions show and cancel", queuedInteractionsShowAndCancel},
        {"linked interactions drop each other", linkedInteractionsDropEachOther},
        {"On Timer sets, changes and removes timers", onTimerSetsChangesAndRemovesTimers},
        {"Message runs the receiver's handler on me", messageRunsTheReceiversHandlerOnMe},
        {"Find Best Object picks the best rated nearest", findBestObjectPicksTheBestRatedNearest},
        {"Run Functional Tree calls the object's tree", runFunctionalTreeCallsTheObjectsTree},
        {"Add/Change the Action String sets text and icon", actionStringSetsTextAndIcon},
        {"pie menu offers what checks allow", pieMenuOffersWhatChecksAllow},
        {"cheat window runs the game's commands", cheatWindowRunsTheGamesCommands},
        {"lot cheats set every Sim's motives and fields, and the hour", lotCheatsSetEverySim},
        {"debug lines start from Shift-clicked objects", debugLinesStartFromShiftClickedObjects},
        {"choices flagged to run at once skip the queue", choicesFlaggedToRunAtOnceSkipTheQueue},
        {"multi-tile parts use their master's texts", multiTilePartsUseTheirMastersTexts},
        {"check tree owners read the tested line", checkTreeOwnersReadTheTestedLine},
        {"Run Tree by Name runs the stack object's tree", runTreeByNameRunsTheStackObjectsTree},
        {"Run Tree by Name takes false when the tree is missing", runTreeByNameTakesFalseWhenTheTreeIsMissing},
        {"Run Tree by Name can pass the caller's params", runTreeByNameCanPassTheCallersParams},
        {"Notify out of Idle ends a Sleep counted in param 0", notifyOutOfIdleEndsASleepCountedInParamZero},
        {"Run Tree by Name passes the temps without flags", runTreeByNamePassesTheTempsWithoutFlags},
        {"Run Tree by Name passes every temp the tree takes", runTreeByNamePassesEveryTempTheTreeTakes},
        {"Run Tree by Name reads the semi-global names", runTreeByNameReadsTheSemiGlobalNames},
        {"Run Tree by Name skips the groups its flags name", runTreeByNameSkipsTheGroupsItsFlagsName},
        {"Run Tree by Name runs on the stack object's stack", runTreeByNameRunsOnTheStackObjectsStack},
        {"Run Tree by Name other modes report themselves", runTreeByNameOtherModesReportThemselves},
        {"Run Tree by Name at once counts its own steps", runTreeByNameAtOnceCountsItsOwnSteps},
        {"Run Tree by Name in my stack runs at once", runTreeByNameInMyStackRunsAtOnce},
        {"message handlers count their own steps", messageHandlersCountTheirOwnSteps},
        {"Run Tree by Name on the stack object runs the tree it found", runTreeByNameOnTheStackObjectRunsTheTreeItFound},
        {"Run Tree by Name pushes onto my stack", runTreeByNamePushesOntoMyStack},
        {"motives are clamped and need a person", motivesAreClampedAndNeedAPerson},
        {"interaction runs object code in the Sim's thread", interactionRunsObjectCodeInTheSimsThread},
        {"presentation-only Generic Sims Calls continue", presentationOnlyGenericCallsContinue},
        {"other Generic Sims Calls report their id", otherGenericCallsReportTheirId},
    };
}
