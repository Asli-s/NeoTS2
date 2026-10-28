// The VM itself: instructions, calls, constants, object memory, limits and errors.
#include "test_support.hpp"

namespace {

Instruction callWithTemps(std::uint16_t opcode, std::uint16_t t, std::uint16_t f) {
    Instruction in = call(opcode, t, f);
    std::fill_n(in.operands.begin(), 8, std::uint8_t{0xFF});
    return in;
}

// A tree is looked up in the global group below 0x1000, the object's own below 0x2000, else its
// semi-global group. hasTree says whether it exists (object functions with none are skipped).
void treesAreFoundByRange() {
    MemoryCodeSource code;
    const auto tree = bhav({expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 5, T, E)});
    code.add(ts2::sim::kGlobalGroup, 0x0118, tree);
    code.add(kObjectGroup, 0x1000, tree);
    code.add(kSemiGroup, 0x2001, tree);
    Vm vm(code, {});
    CHECK(vm.hasTree(kObjectGroup, 0x0118) && vm.hasTree(kObjectGroup, 0x1000) && vm.hasTree(kObjectGroup, 0x2001));
    CHECK(!vm.hasTree(kObjectGroup, 0x1013) && !vm.hasTree(kObjectGroup, 0x0119) && !vm.hasTree(kSemiGroup, 0x1000));
}

void assignAndReturnTrue() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 5, T, E)}));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(r.outcome == Outcome::True);
    CHECK(t.temps[0] == 5);
    CHECK(r.steps == 1);
}

void comparisonTakesFalseBranch() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kLiteral, 3, op::kGreater, owner::kLiteral, 5, T, F)}));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::False);
}

void loopSumsOneToTen() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kLocal, 0, op::kAssign, owner::kLiteral, 0, 1, E),
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 0, 2, E),
        expr(owner::kLocal, 0, op::kIncrementAndLess, owner::kLiteral, 11, 3, T),
        expr(owner::kTemp, 0, op::kAdd, owner::kLocal, 0, 2, E),
    }, 0, 1));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(r.outcome == Outcome::True);
    CHECK(t.temps[0] == 55);
}

void callPassesLiteralArgumentsAndReturns() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kTemp, 1, op::kAssign, owner::kParam, 0, 1, E),
        expr(owner::kTemp, 1, op::kAdd, owner::kParam, 1, F, E),
    }, 2));
    code.add(kObjectGroup, 0x1000, bhav({
        call(0x1001, E, 1, {7, 8}),
        expr(owner::kTemp, 2, op::kAssign, owner::kLiteral, 1, T, E),
    }));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(r.outcome == Outcome::True);
    CHECK(t.temps[1] == 15);
    CHECK(t.temps[2] == 1);
}

void callPassesTempsWhenArgumentsAreAllFF() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kTemp, 5, op::kAssign, owner::kParam, 0, 1, E),
        expr(owner::kTemp, 5, op::kMultiply, owner::kParam, 1, T, E),
    }, 2));
    code.add(kObjectGroup, 0x1000, bhav({callWithTemps(0x1001, T, E)}));
    ThreadState t;
    t.temps[0] = 6;
    t.temps[1] = 7;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
    CHECK(t.temps[5] == 42);
}

void localsArePerFrame() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1001, bhav({expr(owner::kLocal, 0, op::kAssign, owner::kLiteral, 99, T, E)}, 0, 1));
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kLocal, 0, op::kAssign, owner::kLiteral, 1, 1, E),
        call(0x1001, 2, E),
        expr(owner::kLocal, 0, op::kEquals, owner::kLiteral, 1, T, F),
    }, 0, 1));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
}

void callsResolveGlobalPrivateAndSemiGlobalGroups() {
    MemoryCodeSource code;
    code.add(ts2::sim::kGlobalGroup, 0x0100, bhav({expr(owner::kTemp, 0, op::kAdd, owner::kLiteral, 1, T, E)}));
    code.add(kSemiGroup, 0x2000, bhav({expr(owner::kTemp, 0, op::kAdd, owner::kLiteral, 10, T, E)}));
    code.add(kObjectGroup, 0x1001, bhav({expr(owner::kTemp, 0, op::kAdd, owner::kLiteral, 100, T, E)}));
    code.add(kObjectGroup, 0x1000, bhav({call(0x0100, 1, E), call(0x2000, 2, E), call(0x1001, T, E)}));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
    CHECK(t.temps[0] == 111);
}

void constantsResolveByIdRange() {
    MemoryCodeSource code;
    code.addConstants(kObjectGroup, 0x1002, {0, 0, 7});
    code.addConstants(kSemiGroup, 0x2001, {30});
    code.addConstants(ts2::sim::kGlobalGroup, 0x0103, {0, 500});
    const std::uint16_t privateRef = (0x02 << 7) | 2;
    const std::uint16_t semiRef = ((0x40 + 1) << 7) | 0;
    const std::uint16_t globalRef = ((0x80 + 3) << 7) | 1;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kConstant, privateRef, 1, E),
        expr(owner::kTemp, 1, op::kAssign, owner::kConstant, semiRef, 2, E),
        expr(owner::kTemp, 2, op::kAssign, owner::kConstant, globalRef, T, E),
    }));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
    CHECK(t.temps[0] == 7);
    CHECK(t.temps[1] == 30);
    CHECK(t.temps[2] == 500);
}

// Bit 15 is bit 6 of the table number. Bits 13-14 = 3 is an invalid constant.
void constantBit15PicksTheUpperTables() {
    MemoryCodeSource code;
    code.addConstants(kObjectGroup, 0x1041, {0, 9});
    code.addConstants(ts2::sim::kGlobalGroup, 0x0141, {0, 0, 0, 0, 44});
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kConstant, 0x8081, 1, E),
        expr(owner::kTemp, 1, op::kAssign, owner::kConstant, 0xC084, T, E),
    }));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kConstant, 0x6000, T, E),
    }));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
    CHECK(t.temps[0] == 9);
    CHECK(t.temps[1] == 44);
    const RunResult invalid = runPrivate(code, t, 0x1001);
    CHECK(invalid.outcome == Outcome::Error && invalid.reason == "invalid constant");
}

void flagsAreOneBased() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kSetFlag, owner::kLiteral, 3, 1, E),
        expr(owner::kTemp, 0, op::kIsFlagSet, owner::kLiteral, 3, 2, F),
        expr(owner::kTemp, 0, op::kClearFlag, owner::kLiteral, 3, 3, E),
        expr(owner::kTemp, 0, op::kIsFlagSet, owner::kLiteral, 3, F, T),
    }));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
    CHECK(t.temps[0] == 0);
}

void arithmeticWrapsAt16Bits() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 32767, 1, E),
        expr(owner::kTemp, 0, op::kAdd, owner::kLiteral, 1, T, E),
    }));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
    CHECK(t.temps[0] == -32768);
}

void divisionByZeroIsAnError() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kTemp, 0, op::kDivide, owner::kLiteral, 0, T, E)}));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(r.outcome == Outcome::Error);
    CHECK(r.reason == "division by zero");
}

void writingALiteralIsAnError() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kLiteral, 0, op::kAssign, owner::kLiteral, 1, T, E)}));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::Error);
}

void outOfRangeTempIsAnError() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kTemp, 500, op::kAssign, owner::kLiteral, 1, T, E)}));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(r.outcome == Outcome::Error);
    CHECK(r.reason == "temp index out of range");
}

void infiniteLoopHitsStepLimit() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kLiteral, 1, op::kEquals, owner::kLiteral, 1, 0, E)}));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000, {1000, 64});
    CHECK(r.outcome == Outcome::StepLimit);
    CHECK(r.steps == 1000);
}

void infiniteRecursionHitsDepthLimit() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({call(0x1000, T, F)}));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000, {100000, 32}).outcome == Outcome::DepthLimit);
}

void missingCallTargetIsReported() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({call(0x1234, T, F)}));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(r.outcome == Outcome::MissingCode);
    CHECK(r.pc == 0);
}

void unimplementedPrimitiveStopsWithItsOpcode() {
    MemoryCodeSource code;
    Instruction unimplemented;
    unimplemented.opcode = 0x0004;
    unimplemented.trueTarget = T;
    unimplemented.falseTarget = F;
    code.add(kObjectGroup, 0x1000, bhav({unimplemented}));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(r.outcome == Outcome::Unsupported);
    CHECK(r.detail == "0x0004");
}

void jumpPastTheEndIsAnError() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 1, 7, E)}));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::Error);
}

void attributesBelongToEachObject() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kMyAttribute, 0, op::kAdd, owner::kLiteral, 5, T, E),
    }));
    ts2::sim::World world;
    auto& fridge = world.create(kObjectGroup, definition("Fridge", 600));
    auto& other = world.create(kObjectGroup, definition("Fridge", 600));
    Vm vm(code, {}, &world);
    CHECK(vm.run(fridge, 0x1000).outcome == Outcome::True);
    CHECK(vm.run(fridge, 0x1000).outcome == Outcome::True);
    CHECK(vm.run(other, 0x1000).outcome == Outcome::True);
    CHECK(fridge.attributes[0] == 10);
    CHECK(other.attributes[0] == 5);
}

void initObjectSetsDefaultsAndAttributesByParam() {
    MemoryCodeSource code;
    Instruction initObject = call(0x0001, 1, E);  // Generic Sims Call
    initObject.operands = {};
    initObject.operands[0] = 0x41;
    code.add(kObjectGroup, 0x1000, bhav({
        initObject,
        // The stack object's attribute picked by param 1 (here 3).
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kParam, 0, 2, E),
        expr(owner::kStackObjectAttributeByParam, 1, op::kAssign, owner::kLiteral, 7, T, E),
    }, 2));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kStackObjectAttributeByParam, 5, op::kAssign, owner::kLiteral, 7, T, E),  // no param 5
    }, 2));
    ts2::sim::World world;
    auto& me = world.create(kObjectGroup, definition("Me", 0));
    auto& other = world.create(kObjectGroup, definition("Other", 0));
    Vm vm(code, {}, &world);
    const std::int16_t args[] = {other.id, 3};
    CHECK(vm.run(me, 0x1000, args).outcome == Outcome::True);
    CHECK(me.data[0x04] == 1 && me.data[0x2A] == 3 && me.data[0x2B] == 0x0A && me.data[0x28] == 0x2020);
    CHECK(me.data[0x17] == 2 && me.data[0x31] == 1 && me.data[0x4E] == 5);
    CHECK(other.attributes[3] == 7);
    CHECK(vm.run(me, 0x1001, args).outcome == Outcome::Error);
}

void objectIdFieldMatchesTheObject() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kMyData, ts2::sim::kObjectIdField, T, E),
    }));
    ts2::sim::World world;
    world.create(kObjectGroup, definition("A", 1));
    auto& b = world.create(kObjectGroup, definition("B", 1));
    Vm vm(code, {}, &world);
    CHECK(vm.run(b, 0x1000).outcome == Outcome::True);
    CHECK(b.thread.temps[0] == b.id);
    CHECK(b.id == 2);
}

void stackObjectIsReachedThroughItsId() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kParam, 0, 1, E),
        expr(owner::kStackObjectAttribute, 3, op::kAssign, owner::kLiteral, 42, 2, E),
        expr(owner::kTemp, 0, op::kAssign, owner::kStackObjectDefinition,
             ts2::objd::field::kPrice, T, E),
    }, 1));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, definition("Sim", 0));
    auto& fridge = world.create(kObjectGroup, definition("Fridge", 600));
    Vm vm(code, {}, &world);
    const std::int16_t args[] = {fridge.id};
    CHECK(vm.run(sim, 0x1000, args).outcome == Outcome::True);
    CHECK(fridge.attributes[3] == 42);
    CHECK(sim.attributes[3] == 0);
    CHECK(sim.thread.temps[0] == 600);
    CHECK(sim.thread.stackObjectId == fridge.id);
}

void eachFrameKeepsItsOwnStackObject() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 2, 1, E),
        call(0x1001, 2, E),
        expr(owner::kTemp, 0, op::kAssign, owner::kStackObjectId, 0, T, E),
    }));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kTemp, 1, op::kAssign, owner::kStackObjectId, 0, 1, E),
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 3, T, E),
    }));
    ts2::sim::World world;
    auto& me = world.create(kObjectGroup, definition("Me", 0));
    Vm vm(code, {}, &world);
    CHECK(vm.run(me, 0x1000).outcome == Outcome::True);
    CHECK(me.thread.temps[1] == 2);
    CHECK(me.thread.temps[0] == 2);
}

void missingStackObjectIsAnError() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectAttribute, 0, op::kAssign, owner::kLiteral, 1, T, E),
    }));
    ts2::sim::World world;
    auto& me = world.create(kObjectGroup, definition("Me", 0));
    me.thread.stackObjectId = 99;
    Vm vm(code, {}, &world);
    const RunResult r = vm.run(me, 0x1000);
    CHECK(r.outcome == Outcome::Error);
    CHECK(r.reason == "stack object does not exist");
}

void definitionWritesChangeOnlyThatObjectsCopy() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectDefinition, 0x3B, op::kAssign, owner::kLiteral, 1, T, E),
    }));
    ts2::sim::World world;
    auto& a = world.create(kObjectGroup, definition("A", 0));
    auto& b = world.create(kObjectGroup, definition("B", 0));
    a.thread.stackObjectId = a.id;
    Vm vm(code, {}, &world);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::True);
    CHECK(a.definition.field(0x3B) == 1);
    CHECK(b.definition.field(0x3B) == 0);
}

void assign32CopiesTwoWords() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 2, op::kAssign32, owner::kStackObjectDefinition, ts2::objd::field::kGuid1, T, E),
    }));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kTemp, 7, op::kAssign32, owner::kTemp, 0, T, E),
    }));
    code.add(kObjectGroup, 0x1002, bhav({
        expr(owner::kMyData, 0, op::kAssign32, owner::kTemp, 0, T, E),
    }));
    ts2::sim::World world;
    auto& me = world.create(kObjectGroup, definition("Me", 0));
    auto& fridge = world.create(kObjectGroup, typed(kFridgeGuid));
    me.thread.stackObjectId = fridge.id;
    Vm vm(code, {}, &world);
    CHECK(vm.run(me, 0x1000).outcome == Outcome::True);
    CHECK(static_cast<std::uint16_t>(me.thread.temps[2]) == (kFridgeGuid & 0xFFFF));
    CHECK(static_cast<std::uint16_t>(me.thread.temps[3]) == kFridgeGuid >> 16);

    // A side without a second word is an error before anything is written.
    me.thread.temps[0] = 5;
    CHECK(vm.run(me, 0x1001).outcome == Outcome::Error);
    CHECK(me.thread.temps[7] == 0);
    CHECK(vm.run(me, 0x1002).outcome == Outcome::Unsupported);
}

void stackObjectTempsAreThatObjectsTemps() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kStackObjectTemp, 3, 1, E),
        expr(owner::kStackObjectTemp, 5, op::kAssign, owner::kLiteral, 77, 2, E),
        expr(owner::kStackObjectTemp, 8, op::kAssign, owner::kLiteral, 1, T, E),
    }));
    ts2::sim::World world;
    auto& me = world.create(kObjectGroup, definition("Me", 0));
    auto& door = world.create(kObjectGroup, definition("Door", 0));
    door.thread.temps[3] = 12;
    me.thread.stackObjectId = door.id;
    Vm vm(code, {}, &world);
    const RunResult r = vm.run(me, 0x1000);
    CHECK(r.outcome == Outcome::Error);
    CHECK(r.reason == "temp index out of range");
    CHECK(me.thread.temps[0] == 12);
    CHECK(door.thread.temps[5] == 77);
    CHECK(me.thread.temps[5] == 0);
}

void masterDefinitionIsTheMultiTileMastersForParts() {
    constexpr std::uint32_t kPartGuid = 0x1111, kMasterGuid = 0x2222;
    auto part = typed(kPartGuid);
    part.fields[ts2::objd::field::kMultiTileMasterId] = 7;
    part.fields[ts2::objd::field::kPrice] = 0;
    auto master = typed(kMasterGuid);
    master.fields[ts2::objd::field::kMultiTileMasterId] = 7;
    master.fields[ts2::objd::field::kPrice] = 900;
    MemoryCodeSource code;
    code.addObjectType(kObjectGroup, part);
    code.addObjectType(kObjectGroup, master);
    code.setMultiTileMaster(kPartGuid, kMasterGuid);
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kStackObjectMasterDefinition, ts2::objd::field::kPrice, T, E),
    }));
    ts2::sim::World world;
    auto& me = world.create(kObjectGroup, definition("Me", 0));
    auto& fridge = world.create(kObjectGroup, definition("Fridge", 600));
    auto& tile = world.create(kObjectGroup, part);
    Vm vm(code, {}, &world);
    me.thread.stackObjectId = fridge.id;
    CHECK(vm.run(me, 0x1000).outcome == Outcome::True);
    CHECK(me.thread.temps[0] == 600);
    me.thread.stackObjectId = tile.id;
    CHECK(vm.run(me, 0x1000).outcome == Outcome::True);
    CHECK(me.thread.temps[0] == 900);

    code.setMultiTileMaster(kPartGuid, 0xDEAD);
    const RunResult missing = vm.run(me, 0x1000);
    CHECK(missing.outcome == Outcome::Error);
    CHECK(missing.reason == "multi-tile object without a master");
}

void scopedArgumentsBeyondCalleeArgCountAreIgnored() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kParam, 0, 1, E),
        expr(owner::kTemp, 0, op::kAdd, owner::kParam, 1, T, E),
    }, 2));
    Instruction c = call(0x1001, T, E);
    const std::uint8_t operands[13] = {owner::kLiteral, 3, 0, owner::kLiteral, 4, 0,
                                       0x00, 0x00, 0x02, 0x00, 0xFE, 0x0B, 0x01};
    std::copy(std::begin(operands), std::end(operands), c.operands.begin());
    code.add(kObjectGroup, 0x1000, bhav({c}));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
    CHECK(t.temps[0] == 7);
}

void callArgumentFormIsPickedByByte12Bits() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kParam, 0, 1, E),
        expr(owner::kTemp, 0, op::kAdd, owner::kParam, 1, T, E),
    }, 2));
    // Bit 0 (with stale high bits): (owner, data) pairs.
    Instruction pairs = call(0x1001, T, E);
    const std::uint8_t pairOperands[13] = {owner::kLiteral, 3, 0, owner::kLiteral, 4, 0, 0, 0, 0, 0, 0, 0, 0x37};
    std::copy(std::begin(pairOperands), std::end(pairOperands), pairs.operands.begin());
    code.add(kObjectGroup, 0x1000, bhav({pairs}));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
    CHECK(t.temps[0] == 7);

    // Bit 1 alone: the caller's own params.
    Instruction passOn = call(0x1001, T, E);
    passOn.operands[12] = 0xFE;
    code.add(kObjectGroup, 0x1002, bhav({passOn}, 2));
    code.add(kObjectGroup, 0x1003, bhav({call(0x1002, T, E, {5, 6})}));
    ThreadState u;
    CHECK(runPrivate(code, u, 0x1003).outcome == Outcome::True);
    CHECK(u.temps[0] == 11);
}

void attributesNeedAMeObject() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kMyAttribute, 0, op::kAssign, owner::kLiteral, 1, T, E),
    }));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::Unsupported);
}

Instruction setMotiveChange(std::uint8_t motive, std::uint8_t flags, std::int16_t perHour, std::int16_t limit,
                            std::uint16_t t) {
    Instruction in;
    in.opcode = 0x001D;
    in.trueTarget = t;
    in.falseTarget = E;
    in.operands[0] = owner::kLiteral;
    in.operands[1] = owner::kLiteral;
    in.operands[2] = motive;
    in.operands[3] = flags;
    in.operands[4] = static_cast<std::uint8_t>(perHour);
    in.operands[5] = static_cast<std::uint8_t>(perHour >> 8);
    in.operands[6] = static_cast<std::uint8_t>(limit);
    in.operands[7] = static_cast<std::uint8_t>(limit >> 8);
    return in;
}

// Set Motive Change adds a (motive, rate per tick, limit) entry. Flag bit 0 clears them all and
// bit 1 zeroes the motive's daily rate. Motives other than 0, 1, 5-9, 14, 15 are error 0x1A.
void setMotiveChangeKeepsChanges() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        setMotiveChange(5, 0, 900, 80, 1),
        setMotiveChange(5, 0, -1800, -50, T),
    }));
    code.add(kObjectGroup, 0x1001, bhav({setMotiveChange(0, 0x01, 0, 0, T)}));
    code.add(kObjectGroup, 0x1002, bhav({setMotiveChange(2, 0, 10, 10, T)}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& chair = world.create(kObjectGroup, typed(kChairGuid));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(sim.motiveChanges.size() == 2);
    CHECK(sim.motiveChanges[0].motive == 5 && sim.motiveChanges[0].perTick == 0.5f &&
          sim.motiveChanges[0].limit == 80.0f);
    CHECK(sim.motiveChanges[1].perTick == -1.0f && sim.motiveChanges[1].limit == -50.0f);
    // Bit 1 also stops the motive's own decay: energy's daily rate (person data 0x60) becomes 0.
    code.add(kObjectGroup, 0x1003, bhav({setMotiveChange(5, 0x02, 60, 100, T)}));
    sim.personData[0x60] = -500;
    CHECK(vm.run(sim, 0x1003).outcome == Outcome::True && sim.personData[0x60] == 0);
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::True && sim.motiveChanges.empty());
    const RunResult bad = vm.run(sim, 0x1002);
    CHECK(bad.outcome == Outcome::Error && bad.reason == "motive number out of range");
    CHECK(vm.run(chair, 0x1000).outcome == Outcome::Unsupported);
}

void setToNextWithoutTargetFlagUsesStackObject() {
    MemoryCodeSource code;
    addSmallWorldTypes(code);
    Instruction search = setToNext(0x04, owner::kLocal, 1, kChairGuid, T, F);
    search.operands[4] &= 0x7F;
    code.add(kObjectGroup, 0x1000, bhav({search}, 0, 2));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(w.controller->thread.stackObjectId == 3);
}

void setToNextFindsTheJobObjectOfTheSim() {
    MemoryCodeSource code;
    addSmallWorldTypes(code);
    Instruction search = setToNext(0x0E, owner::kLocal, 1, 0, T, F);
    search.operands[4] &= 0x7F;
    code.add(kObjectGroup, 0x1000, bhav({search}, 0, 2));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    ts2::sim::SimObject& sim = *w.world.find(5);
    w.controller->thread.stackObjectId = 5;
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::False);
    CHECK(w.controller->thread.stackObjectId == 5);
    sim.personData[0x5A] = static_cast<std::int16_t>(kFridgeGuid >> 16);
    sim.personData[0x59] = static_cast<std::int16_t>(kFridgeGuid & 0xFFFF);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(w.controller->thread.stackObjectId == 2);
    w.controller->thread.stackObjectId = 3;
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::Error);
}

void findWorstMotivesWritesTheFiveLowest() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({primitiveCall(0x0021, T, E)}));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    ts2::sim::SimObject& sim = *w.world.find(5);
    sim.motives.fill(50.0f);
    sim.motives[0x0E] = -40.0f;
    sim.motives[7] = -20.0f;
    sim.motives[9] = -20.005f;
    sim.motives[6] = 10.0f;
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    const auto& temps = sim.thread.temps;
    CHECK(temps[0] == 0x0E && temps[1] == 7 && temps[2] == 9 && temps[3] == 6 && temps[4] == 5);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::Error);
}

Instruction randomNumber(std::uint8_t destOwner, std::uint16_t destData, std::uint8_t rangeOwner,
                         std::uint16_t rangeData, std::uint16_t t) {
    Instruction in = primitiveCall(0x0008, t, E);
    in.operands[0] = static_cast<std::uint8_t>(destData);
    in.operands[2] = destOwner;
    in.operands[4] = static_cast<std::uint8_t>(rangeData);
    in.operands[5] = static_cast<std::uint8_t>(rangeData >> 8);
    in.operands[6] = rangeOwner;
    in.operands[8] = 1;
    return in;
}

void randomNumberStaysInRange() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({randomNumber(owner::kTemp, 0, owner::kLiteral, 10, T)}));
    Vm vm(code);
    std::array<int, 10> seen{};
    bool inRange = true;
    for (int i = 0; i < 500; ++i) {
        ThreadState t;
        CHECK(vm.run(t, kObjectGroup, 0x1000).outcome == Outcome::True);
        if (t.temps[0] < 0 || t.temps[0] > 9) {
            inRange = false;
        } else {
            ++seen[static_cast<std::size_t>(t.temps[0])];
        }
    }
    CHECK(inRange);
    CHECK(std::all_of(seen.begin(), seen.end(), [](int n) { return n > 0; }));
}

void randomNumberRangeCanComeFromALocal() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kLocal, 1, op::kAssign, owner::kLiteral, 1, 1, E),
        randomNumber(owner::kLocal, 0, owner::kLocal, 1, 2),
        expr(owner::kLocal, 0, op::kEquals, owner::kLiteral, 0, T, F),
    }, 0, 2));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
}

// The simulator's random numbers step as the game's do: three states in a row taken from the game.
void simRandomStepsAsTheGameDoes() {
    ts2::sim::SimRandom random(48557956);
    random();
    CHECK(random.state() == 3230425965u);
    const std::uint32_t draw = random();
    CHECK(random.state() == 1432337314u);
    CHECK(draw == static_cast<std::uint32_t>((3230425965ull * 0x41C64E6Du + 12345u) >> 16));
}

void randomNumberWithZeroRangeWritesZero() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 5, 1, E),
        randomNumber(owner::kTemp, 0, owner::kLiteral, 0, T),
    }));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
    CHECK(t.temps[0] == 0);
}

Instruction breakPoint(std::uint16_t literalCondition, std::uint8_t flags) {
    Instruction in = primitiveCall(0x000F, T, E);
    in.operands[0] = static_cast<std::uint8_t>(literalCondition);
    in.operands[2] = owner::kLiteral;
    in.operands[4] = flags;
    return in;
}

void breakPointIgnoredInReleaseContinues() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({breakPoint(1, 0x01)}));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);

    Instruction unconditional = primitiveCall(0x000F, T, E);
    std::fill_n(unconditional.operands.begin(), 8, std::uint8_t{0xFF});
    code.add(kObjectGroup, 0x1001, bhav({unconditional}));
    CHECK(runPrivate(code, t, 0x1001).outcome == Outcome::True);
}

void breakPointWithFalseConditionContinues() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({breakPoint(0, 0x00)}));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
}

void breakPointWithTrueConditionIsAnError() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({breakPoint(1, 0x00)}));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(r.outcome == Outcome::Error);
    CHECK(r.reason == "break point hit");
}

void expressionIgnoresStaleOperandBytes() {
    MemoryCodeSource code;
    Instruction assign = expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 9, T, E);
    assign.operands[4] = 0xFF;
    for (std::size_t k = 8; k < 16; ++k) {
        assign.operands[k] = static_cast<std::uint8_t>(0xA0 + k);
    }
    code.add(kObjectGroup, 0x1000, bhav({assign}));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
    CHECK(t.temps[0] == 9);
}

void sleepYieldsInsideACallAndResumes() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1001, bhav({
        sleepFor(0, 1),
        expr(owner::kTemp, 0, op::kAdd, owner::kLiteral, 1, T, E),
    }, 1));
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 5, 1, E),
        call(0x1001, 2, E, {30}),
        expr(owner::kTemp, 1, op::kAssign, owner::kTemp, 0, T, E),
    }));
    ThreadState t;
    Vm vm(code);
    const RunResult first = vm.run(t, kObjectGroup, 0x1000);
    CHECK(first.outcome == Outcome::Sleeping);
    CHECK(t.sleepTicks == 30);
    CHECK(t.temps[0] == 5);
    CHECK(t.isRunning());
    CHECK(t.frames.size() == 2);

    const RunResult second = vm.resume(t, kObjectGroup);
    CHECK(second.outcome == Outcome::True);
    CHECK(t.temps[1] == 6);
    CHECK(!t.isRunning());
}

void sleepAsLastInstructionCompletesOnResume() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({sleepFor(0, T)}, 1));
    ThreadState t;
    Vm vm(code);
    const std::int16_t args[] = {3};
    CHECK(vm.run(t, kObjectGroup, 0x1000, args).outcome == Outcome::Sleeping);
    CHECK(vm.resume(t, kObjectGroup).outcome == Outcome::True);
    CHECK(!t.isRunning());
}

void sleepOfNoTicksGoesOnAtOnce() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({sleepFor(0, T)}, 1));
    ThreadState t;
    Vm vm(code);
    const std::int16_t args[] = {0};
    CHECK(vm.run(t, kObjectGroup, 0x1000, args).outcome == Outcome::True);
}

void runOnABusyThreadIsAnError() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({sleepFor(0, T)}, 1));
    ThreadState t;
    Vm vm(code);
    const std::int16_t args[] = {3};
    CHECK(vm.run(t, kObjectGroup, 0x1000, args).outcome == Outcome::Sleeping);
    CHECK(vm.run(t, kObjectGroup, 0x1000, args).outcome == Outcome::Error);
}

void resumeWithNothingToRunIsAnError() {
    MemoryCodeSource code;
    ThreadState t;
    Vm vm(code);
    CHECK(vm.resume(t, kObjectGroup).outcome == Outcome::Error);
}

void errorsLeaveTheThreadIdle() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1001, bhav({expr(owner::kTemp, 0, op::kDivide, owner::kLiteral, 0, T, E)}));
    code.add(kObjectGroup, 0x1000, bhav({call(0x1001, T, F)}));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(r.outcome == Outcome::Error);
    CHECK(r.instance == 0x1001);
    CHECK(!t.isRunning());
}

void personDataBelongsToEachSim() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kMyPersonData, 0x0A, op::kAdd, owner::kLiteral, 100, 1, E),
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kParam, 0, 2, E),
        expr(owner::kStackObjectPersonData, 0x0A, op::kAssign, owner::kMyPersonData, 0x0A, T, E),
    }, 1));
    ts2::sim::World world;
    auto& cook = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& learner = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    const std::int16_t args[] = {learner.id};
    CHECK(vm.run(cook, 0x1000, args).outcome == Outcome::True);
    CHECK(cook.personData[0x0A] == 100);
    CHECK(learner.personData[0x0A] == 100);
}

void personDataByTempUsesATempAsTheIndex() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 1, op::kAssign, owner::kLiteral, 200, 1, E),
        expr(owner::kMyPersonDataByTemp, 1, op::kAssign, owner::kLiteral, 7, T, E),
    }));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(sim.personData[200] == 7);
}

void personDataOnANonPersonIsAnError() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kMyPersonData, 0, T, E),
    }));
    ts2::sim::World world;
    auto& fridge = world.create(kObjectGroup, typed(kFridgeGuid));
    Vm vm(code, {}, &world);
    const RunResult r = vm.run(fridge, 0x1000);
    CHECK(r.outcome == Outcome::Error);
    CHECK(r.reason == "person data on an object that is not a person");
}

void createObjectMakesItTheStackObject() {
    MemoryCodeSource code;
    code.addObjectType(kLampGroup, typed(kLampGuid));
    code.add(kObjectGroup, 0x1000, bhav({
        createObject(kLampGuid, 0x06, 0x00, 1, F),
        expr(owner::kStackObjectAttribute, 2, op::kAssign, owner::kLiteral, 9, T, E),
    }));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(world.size() == 2);
    const ts2::sim::SimObject* lamp = world.find(sim.thread.stackObjectId);
    CHECK(lamp != nullptr && lamp != &sim);
    CHECK(lamp != nullptr && lamp->group == kLampGroup && lamp->attributes[2] == 9);
}

// Get Direction To: from the object in temp 1 to the stack object, as 8 directions or in degrees.
void getDirectionToMeasuresTowardsStackObject() {
    auto direction = [](bool degrees) {
        Instruction in;
        in.opcode = 0x000C;
        in.trueTarget = T;
        in.falseTarget = E;
        in.operands[0] = 0;  // result: temp 0
        in.operands[2] = owner::kTemp;
        in.operands[4] = 0x01;
        in.operands[5] = owner::kTemp;
        in.operands[6] = 1;
        in.operands[8] = degrees ? 0x02 : 0x00;
        return in;
    };
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({direction(false)}));
    code.add(kObjectGroup, 0x1001, bhav({direction(true)}));
    SmallWorld w;
    auto place = [&](std::int16_t id, float x, float y) {
        w.world.find(id)->x = static_cast<std::int32_t>(x * 65536);
        w.world.find(id)->y = static_cast<std::int32_t>(y * 65536);
    };
    place(2, 10.5f, 10.5f);
    Vm vm(code, {}, &w.world);
    auto measure = [&](std::uint16_t tree, float x, float y) {
        place(3, x, y);
        w.controller->thread.stackObjectId = 3;
        w.controller->thread.temps[1] = 2;
        CHECK(vm.run(*w.controller, tree).outcome == Outcome::True);
        return w.controller->thread.temps[0];
    };
    CHECK(measure(0x1000, 12.5f, 10.5f) == 2);
    CHECK(measure(0x1000, 10.5f, 12.5f) == 4);
    CHECK(measure(0x1000, 8.5f, 8.5f) == 7);
    CHECK(measure(0x1000, 10.5f, 8.5f) == 0);
    CHECK(measure(0x1001, 12.5f, 10.5f) == 90);
    CHECK(measure(0x1001, 10.5f, 12.5f) == 180);
    CHECK(measure(0x1001, 8.5f, 8.5f) == 315);
}

// Owner 0x20 "neighbor's person data": the stack object id names a neighbor. Writes change this
// world's copy, not the saved neighborhood.
void neighborPersonDataReadsAndWrites() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 9, 1, E),
        expr(owner::kTemp, 0, op::kAssign, ts2::sim::owner::kNeighborPersonData, 0x3A, 2, E),
        expr(ts2::sim::owner::kNeighborPersonData, 0xE6, op::kAssign, owner::kLiteral, 77, 3, E),
        expr(owner::kTemp, 1, op::kAssign, ts2::sim::owner::kNeighborPersonData, 0xE6, T, E),
    }));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 9, 1, E),
        expr(owner::kTemp, 0, op::kAssign, ts2::sim::owner::kNeighborPersonData, 0xE7, T, E),
    }));
    code.add(kObjectGroup, 0x1002, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 4, 1, E),
        expr(owner::kTemp, 0, op::kAssign, ts2::sim::owner::kNeighborPersonData, 0, T, E),
    }));
    ts2::sim::World world;
    auto& a = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::Unsupported);

    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {9};
    hood->neighbors[9].personData.assign(0x40, 0);
    hood->neighbors[9].personData[0x3A] = 3;
    world.setNeighborhood(hood);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::True);
    CHECK(a.thread.temps[0] == 3 && a.thread.temps[1] == 77);
    CHECK(hood->neighbors[9].personData.size() == 0x40);
    const RunResult range = vm.run(a, 0x1001);
    CHECK(range.outcome == Outcome::Error && range.reason == "person data field is out of range");
    const RunResult missing = vm.run(a, 0x1002);
    CHECK(missing.outcome == Outcome::Error && missing.reason == "missing neighbor for data access");
}

void constantsByTempIndexWithATemp() {
    MemoryCodeSource code;
    code.addConstants(ts2::sim::kGlobalGroup, 0x0103, {10, 20, 30, 40});
    code.addConstants(kObjectGroup, 0x1002, {7, 8, 9});
    const std::uint16_t table = (0x800 + 3) << 3;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 2, 1, E),
        expr(owner::kTemp, 1, op::kAssign, ts2::sim::owner::kConstantByTemp, table, 2, E),
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 9, 3, E),
        expr(owner::kTemp, 2, op::kAssign, ts2::sim::owner::kConstantByTemp, table, T, E),
    }));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(t.temps[1] == 30);
    CHECK(r.outcome == Outcome::Error);
    CHECK(r.reason == "constant index out of range");

    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 1, 1, E),
        expr(owner::kTemp, 3, op::kAssign, ts2::sim::owner::kConstantByTemp, 2 << 3, 2, E),
        expr(owner::kTemp, 2, op::kAssign, owner::kLiteral, 2, 3, E),
        expr(owner::kTemp, 4, op::kAssign, ts2::sim::owner::kConstantByTemp, 2 << 3 | 2, T, E),  // index from temp 2
    }));
    ThreadState t2;
    CHECK(runPrivate(code, t2, 0x1001).outcome == Outcome::True);
    CHECK(t2.temps[3] == 8 && t2.temps[4] == 9);
}

void removeObjectRemovesTheStackObject() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        removeObject(1, 0x00, 1, F),
        setToNext(0x04, owner::kStackObjectId, 0, kChairGuid, F, T),
    }));
    SmallWorld w;
    w.controller->thread.stackObjectId = 3;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(w.world.find(3) == nullptr);
    CHECK(w.world.size() == 6);
    w.world.purge();
    CHECK(w.world.size() == 6);
}

// A relationship towards an object that isn't there is dropped at the next purge, even when no
// object was removed. Relationships between objects that are there stay.
void purgeDropsRelationshipsWithoutTheirObjects() {
    SmallWorld w;
    w.world.relationship(1, 2) = {7};
    w.world.purge();
    w.world.relationship(1, 99) = {5};
    w.world.purge();
    CHECK(w.world.relationship(1, 99).empty());
    w.world.purge();
    CHECK(w.world.relationship(1, 2) == std::vector<std::int16_t>{7});
}

} // namespace

std::vector<Test> vmCoreTests() {
    return {
        {"assign and return true", assignAndReturnTrue},
        {"trees are found by range", treesAreFoundByRange},
        {"purge drops relationships without their objects", purgeDropsRelationshipsWithoutTheirObjects},
        {"comparison takes false branch", comparisonTakesFalseBranch},
        {"loop sums 1..10", loopSumsOneToTen},
        {"call passes literal arguments and returns", callPassesLiteralArgumentsAndReturns},
        {"call passes temps when arguments are all 0xFF", callPassesTempsWhenArgumentsAreAllFF},
        {"locals are per frame", localsArePerFrame},
        {"calls resolve global/private/semi-global groups", callsResolveGlobalPrivateAndSemiGlobalGroups},
        {"constants resolve by id range", constantsResolveByIdRange},
        {"constant bit 15 picks the upper tables", constantBit15PicksTheUpperTables},
        {"flags are one-based", flagsAreOneBased},
        {"arithmetic wraps at 16 bits", arithmeticWrapsAt16Bits},
        {"division by zero is an error", divisionByZeroIsAnError},
        {"writing a literal is an error", writingALiteralIsAnError},
        {"out-of-range temp is an error", outOfRangeTempIsAnError},
        {"infinite loop hits step limit", infiniteLoopHitsStepLimit},
        {"infinite recursion hits depth limit", infiniteRecursionHitsDepthLimit},
        {"missing call target is reported", missingCallTargetIsReported},
        {"unimplemented primitive stops with its opcode", unimplementedPrimitiveStopsWithItsOpcode},
        {"jump past the end is an error", jumpPastTheEndIsAnError},
        {"attributes belong to each object", attributesBelongToEachObject},
        {"Init - Object sets defaults; attributes by param", initObjectSetsDefaultsAndAttributesByParam},
        {"object id field matches the object", objectIdFieldMatchesTheObject},
        {"stack object is reached through its id", stackObjectIsReachedThroughItsId},
        {"each frame keeps its own stack object", eachFrameKeepsItsOwnStackObject},
        {"missing stack object is an error", missingStackObjectIsAnError},
        {"definition writes change only that object's copy", definitionWritesChangeOnlyThatObjectsCopy},
        {"master definition is the multi-tile master's for parts", masterDefinitionIsTheMultiTileMastersForParts},
        {"stack object temps are that object's temps", stackObjectTempsAreThatObjectsTemps},
        {"Assign 32bit Value copies two words", assign32CopiesTwoWords},
        {"scoped arguments beyond callee arg count are ignored", scopedArgumentsBeyondCalleeArgCountAreIgnored},
        {"call argument form is picked by byte 12 bits", callArgumentFormIsPickedByByte12Bits},
        {"Set Motive Change keeps changes", setMotiveChangeKeepsChanges},
        {"neighbor person data reads and writes", neighborPersonDataReadsAndWrites},
        {"Get Direction To measures towards the stack object", getDirectionToMeasuresTowardsStackObject},
        {"Set to Next without target flag uses stack object", setToNextWithoutTargetFlagUsesStackObject},
        {"Create Object makes it the stack object", createObjectMakesItTheStackObject},
        {"Constants [temp] index with a temp", constantsByTempIndexWithATemp},
        {"Remove Object removes the stack object", removeObjectRemovesTheStackObject},
        {"Random Number stays in range", randomNumberStaysInRange},
        {"Random Number range can come from a local", randomNumberRangeCanComeFromALocal},
        {"Random Number with zero range writes 0", randomNumberWithZeroRangeWritesZero},
        {"simulator random numbers step as the game's do", simRandomStepsAsTheGameDoes},
        {"Set to Next 0x0E finds the Sim's job object", setToNextFindsTheJobObjectOfTheSim},
        {"Find 5 Worst Motives writes the five lowest", findWorstMotivesWritesTheFiveLowest},
        {"Break Point ignored in release continues", breakPointIgnoredInReleaseContinues},
        {"Break Point with false condition continues", breakPointWithFalseConditionContinues},
        {"Break Point with true condition is an error", breakPointWithTrueConditionIsAnError},
        {"Expression ignores stale operand bytes", expressionIgnoresStaleOperandBytes},
        {"Sleep yields inside a call and resumes", sleepYieldsInsideACallAndResumes},
        {"Sleep as last instruction completes on resume", sleepAsLastInstructionCompletesOnResume},
        {"a Sleep of no ticks goes on at once", sleepOfNoTicksGoesOnAtOnce},
        {"run on a busy thread is an error", runOnABusyThreadIsAnError},
        {"resume with nothing to run is an error", resumeWithNothingToRunIsAnError},
        {"errors leave the thread idle", errorsLeaveTheThreadIdle},
        {"person data belongs to each Sim", personDataBelongsToEachSim},
        {"person data [temp] uses a temp as the index", personDataByTempUsesATempAsTheIndex},
        {"person data on a non-person is an error", personDataOnANonPersonIsAnError},
        {"attributes need a 'me' object", attributesNeedAMeObject},
    };
}
