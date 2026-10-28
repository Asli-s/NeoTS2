// Interactions: queues, pie menus, autonomy's picks, wants, motives and running trees.
#include "test_support.hpp"

#include "content/animation_lists.hpp"

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
// go, and a Sim whose current interaction object it is gets reset.
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
    // A Sim that would need a reset while its own code runs stops instead.
    CHECK(vm.runEvent(user, kObjectGroup, 0x1000, 2, {}).outcome == Outcome::Unsupported);
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
    CHECK(vm.run(sim, 0x1100).outcome == Outcome::False && sim.data[0x14] == 5);  // once a tick
    world.countTick();
    sim.motives[7] = 100;  // nothing left to gain
    CHECK(vm.run(sim, 0x1100).outcome == Outcome::False && sim.data[0x14] == -1);
}

// A check tree autonomy runs before scoring offers lines with Add/Change the Action String. Each
// is weighed as a candidate of its own with the params it was offered with, even when the check
// then fails, and Gosub Found Action runs the line's tree with those params and that name.
void findBestInteractionWeighsOfferedLines() {
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
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 0, F, F),  // the line itself fails
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

void runTreeByNameOnTheStackObjectSharesTheStepLimit() {
    NamedTreeSetup s;
    s.code.addStrings(kSimGroup, 0x12F, {"Loop"});
    s.code.addStrings(kObjectGroup, 0x12F, {"Loop"});
    s.code.add(kObjectGroup, 0x1007, named("Loop", {runTreeByName(0, 0x00, 0x01, 0, 0)}));
    s.code.add(kSimGroup, 0x1000, named("Caller", {runTreeByName(0, 0x00, 0x01, 0, 0)}));
    Vm vm(s.code, {1000, 64}, &s.world);
    std::size_t steps = 0;
    vm.onStep = [&](const ts2::sim::StepInfo&) { ++steps; };
    CHECK(vm.run(*s.sim, 0x1000).outcome == Outcome::StepLimit);
    CHECK(steps == 1000);
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

// Mode 2 pushes the stack object's tree onto my stack like mode 0. Mode 3 runs a tree of the
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

    // Something queued above the Sim's Priority (person data 0x21): it starts, and afterwards the
    // primitive runs again and waits out its last tick, even though the interaction returned
    // false. At or below it, the Sim keeps waiting.
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
    CHECK(vm.resume(sim).outcome == Outcome::Sleeping && fridge.attributes[0] == 7);
    CHECK((sim.data[8] & 0x40) != 0);
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
    table.interactions[1].action = 0x1000;
    table.interactions[1].stringIndex = 1;
    table.interactions[1].flags = 0x8000;
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
    CHECK(vm.run(controller, kObjectGroup, 0x1100, twoTicksOnFridge).outcome == Outcome::True);
    CHECK(vm.run(controller, kObjectGroup, 0x1101, twoTicksOnFridge).outcome == Outcome::True);
    CHECK(vm.run(controller, kObjectGroup, 0x1101, twoTicksOnFridge).outcome == Outcome::True);
    CHECK(sim.queue.size() == 3);
    CHECK(sim.queue[0].iconGuid == 0x12345678 && sim.queue[0].iconIndex == 3 && !sim.queue[0].nested);
    CHECK(sim.queue[1].iconGuid == 0 && sim.queue[1].nested);
    CHECK(sim.queue[0].id == 1 && sim.queue[1].id == 2 && sim.queue[2].id == 3);

    // A waiting one leaves at once.
    CHECK(ts2::sim::cancelInteraction(sim, 2) && sim.queue.size() == 2 && sim.queue[1].id == 3);
    CHECK(!ts2::sim::cancelInteraction(sim, 2));
    CHECK(ts2::sim::cancelInteraction(sim, 3) && sim.queue.size() == 1);

    // The first starts: it runs until its tree returns, its icon the Sim's action icon.
    CHECK(vm.run(sim, kObjectGroup, 0x1102).outcome == Outcome::Sleeping);
    CHECK(sim.running && sim.running->id == 1 && sim.queue.empty());
    CHECK(sim.actionIconGuid == 0x12345678 && sim.actionIconIndex == 3 && sim.actionIconObject == fridge.id);
    CHECK(sim.actionString.empty());  // a pushed interaction has no name of its own
    sim.personData[0x21] = 100;
    CHECK(ts2::sim::cancelInteraction(sim, 1) && sim.running->cancelled && sim.personData[0x21] == 0);
    CHECK(!ts2::sim::cancelInteraction(sim, 1));
    // Its Idle for Input is true at once now, so its tree finishes.
    CHECK(vm.resume(sim).outcome == Outcome::True && !sim.running);

    // Started at once (byte 4 bit 0), it is the running sub-interaction until its tree returns.
    CHECK(vm.run(controller, kObjectGroup, 0x1100, twoTicksOnFridge).outcome == Outcome::True);
    CHECK(vm.run(sim, kObjectGroup, 0x1103).outcome == Outcome::Sleeping);
    CHECK(sim.runningSub && sim.runningSub->id == 4 && !sim.running);
    CHECK(vm.resume(sim).outcome == Outcome::Sleeping);
    CHECK(vm.resume(sim).outcome == Outcome::True && !sim.runningSub);
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

    // Debug lines alone, with the "*" mark, on a shift-click with testing cheats.
    menu = ts2::sim::pieMenu(vm, code, sim, chair, {.debug = true});
    CHECK(menu.choices.size() == 1 && menu.choices[0].name == "*Debug");

    // Children see the children's line. Nothing is offered by an object with its interactions off.
    sim.personData[0x3A] = 10;
    menu = ts2::sim::pieMenu(vm, code, sim, chair, {.firstOnly = true});
    CHECK(menu.choices.size() == 1 && menu.choices[0].name == "Play");
    chair.data[0x57] = 1;
    CHECK(ts2::sim::pieMenu(vm, code, sim, chair).choices.empty());
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
    CHECK(world.neighborWants(27).size() == 1 && hood->wants[27].size() == 3);
    const RunResult unknown = vm.run(sim, 0x1003);
    CHECK(unknown.outcome == Outcome::Error && unknown.reason == "data number out of range");
    CHECK(vm.run(sim, 0x1004).outcome == Outcome::False);  // no Wants - Controller on the lot: no Sims
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
    CHECK(world.neighborWants(28).empty() && world.neighborWants(29).empty());
    CHECK(world.neighborWants(27).size() == 1 && world.neighborWants(30).size() == 1);
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
        {"remapped neighbor id comes from the remap table", remappedNeighborIdComesFromTheRemapTable},
        {"Get Outfits Information asks the Sim's looks", outfitsInformationAsksTheSimsLooks},
        {"fits with footprint tests the other footprint", fitsWithFootprintTestsTheOtherFootprint},
        {"Build and Buy calls list the running object", buildBuyCallsListTheRunningObject},
        {"abort interactions tells every Sim", abortInteractionsTellsEverySim},
        {"multi-tile parts use their master's interaction table", multiTilePartsUseTheirMastersTable},
        {"advertisers list objects by motive", advertisersListObjectsByMotive},
        {"Find Best Interaction picks the happiest near line", findBestInteractionPicksTheHappiestNearLine},
        {"Find Best Interaction weighs offered lines", findBestInteractionWeighsOfferedLines},
        {"check tree kinds find trees that change the line", checkTreeKindsFindTreesThatChangeTheLine},
        {"Find Best Interaction scores what check trees advertise", findBestInteractionScoresWhatCheckTreesAdvertise},
        {"Want Satisfy matches current wants", wantSatisfyMatchesCurrentWants},
        {"Want Satisfy with a Sim list satisfies the listed Sims", wantSatisfyWithASimListSatisfiesTheListedSims},
        {"Push Interaction queues for the Sim", pushInteractionQueuesForTheSim},
        {"paired multi-tile parts hand their interactions on", pairedPartsHandTheirInteractionsOn},
        {"check trees nest only so deep", checkTreesNestOnlySoDeep},
        {"Idle for Input starts queued interactions", idleForInputStartsQueuedInteractions},
        {"queued interactions show and cancel", queuedInteractionsShowAndCancel},
        {"On Timer sets, changes and removes timers", onTimerSetsChangesAndRemovesTimers},
        {"Message runs the receiver's handler on me", messageRunsTheReceiversHandlerOnMe},
        {"Find Best Object picks the best rated nearest", findBestObjectPicksTheBestRatedNearest},
        {"Run Functional Tree calls the object's tree", runFunctionalTreeCallsTheObjectsTree},
        {"Add/Change the Action String sets text and icon", actionStringSetsTextAndIcon},
        {"pie menu offers what checks allow", pieMenuOffersWhatChecksAllow},
        {"choices flagged to run at once skip the queue", choicesFlaggedToRunAtOnceSkipTheQueue},
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
        {"Run Tree by Name on the stack object shares the step limit", runTreeByNameOnTheStackObjectSharesTheStepLimit},
        {"Run Tree by Name on the stack object runs the tree it found", runTreeByNameOnTheStackObjectRunsTheTreeItFound},
        {"Run Tree by Name pushes onto my stack", runTreeByNamePushesOntoMyStack},
        {"motives are clamped and need a person", motivesAreClampedAndNeedAPerson},
        {"interaction runs object code in the Sim's thread", interactionRunsObjectCodeInTheSimsThread},
        {"presentation-only Generic Sims Calls continue", presentationOnlyGenericCallsContinue},
        {"other Generic Sims Calls report their id", otherGenericCallsReportTheirId},
    };
}
